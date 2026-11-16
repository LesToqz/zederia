// What the effects rack needs inside the engine (plan 3.8). Included inside the engine's
// anonymous namespace, after EngineTypes.inl.

constexpr int kRackSlots = kNumFxLanes * kNumFxSlots;
constexpr int kFxPool = 2 * kRackSlots; // the running effects plus the ones fading out
constexpr int kSlotFields = 2 + kNumFxSlotParams; // on, mix, p1..p16
constexpr double kFxFadeSeconds = 0.01;           // insert, remove, move, bypass and FX off fade this long

// A rack layout built off the audio thread. The audio thread takes it at the start of a block,
// moves the new effects into its pool and hands the empty plan back to be freed.
struct RackPlan {
    std::uint32_t uid[kRackSlots] = {}; // the effect instance at each position; 0 = empty
    int type[kRackSlots];
    int count[kNumFxLanes] = {};
    std::unique_ptr<fx::Effect> fresh[kRackSlots]; // new instances, by position
    double values[kRackSlots][kSlotFields] = {};
    bool hasCurve[kRackSlots][2] = {};
    float curves[kRackSlots][2][fx::kCurveSize + 1];
    RackPlan() { std::fill(std::begin(type), std::end(type), -1); }
};

// One effect instance on the audio thread.
struct FxInstance {
    fx::Effect* effect = nullptr; // owned by the pool; handed back to be deleted elsewhere
    std::uint32_t uid = 0;
    int type = -1, params = 0;
    int lane = -1, pos = -1;
    float g = 0.0f, gTarget = 0.0f; // presence: fades the effect in and out of the chain
    bool leaving = false;           // removed: retired once faded out
    bool moving = false;            // has to move: fades out, moves, fades back in
    bool needsClear = false, needsSet = true, snap = true;
    bool idle = false, ran = false;
    int silent = 0;
    fx::Ramp mix;
    double cached[kNumFxSlotParams] = {};
};

// Objects the audio thread gives back to be deleted on another thread.
template <class T, int N>
struct RetireBin {
    std::array<std::atomic<T*>, N> slots;
    RetireBin() {
        for (auto& s : slots)
            s.store(nullptr, std::memory_order_relaxed);
    }
    ~RetireBin() { collect(); }
    RetireBin(const RetireBin&) = delete;
    RetireBin& operator=(const RetireBin&) = delete;
    bool put(T* object) noexcept {
        for (auto& s : slots) {
            T* expected = nullptr;
            if (s.compare_exchange_strong(expected, object, std::memory_order_acq_rel))
                return true;
        }
        return false;
    }
    void collect() {
        for (auto& s : slots)
            delete s.exchange(nullptr, std::memory_order_acq_rel);
    }
};

// Length of the longest common subsequence of two short lists, with the members of one such
// subsequence marked in `keepA` (positions of `a`).
inline void commonOrder(const std::uint32_t* a, int na, const std::uint32_t* b, int nb, bool* keepA) {
    int table[kNumFxSlots + 1][kNumFxSlots + 1] = {};
    for (int i = na - 1; i >= 0; --i)
        for (int j = nb - 1; j >= 0; --j)
            table[i][j] = a[i] == b[j] ? table[i + 1][j + 1] + 1 : std::max(table[i + 1][j], table[i][j + 1]);
    for (int i = 0; i < na; ++i)
        keepA[i] = false;
    int i = 0, j = 0;
    while (i < na && j < nb) {
        if (a[i] == b[j]) {
            keepA[i] = true;
            ++i;
            ++j;
        } else if (table[i + 1][j] >= table[i][j + 1]) {
            ++i;
        } else {
            ++j;
        }
    }
}

// The oversampled soft clipper of the output protection: below half scale it passes the signal
// untouched; above, it bends smoothly toward full scale and never passes it.
inline float softClip(float x) noexcept {
    const float a = std::abs(x);
    if (a <= 0.5f)
        return x;
    const float y = 0.5f + 0.5f * std::tanh((a - 0.5f) * 2.0f);
    return x < 0.0f ? -y : y;
}
