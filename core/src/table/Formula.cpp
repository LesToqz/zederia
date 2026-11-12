#include "zederia/table/Formula.h"

#include "zederia/util/Math.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstring>

namespace zed {
namespace {

enum Op : std::uint8_t {
    kConst, kVar, kAdd, kSub, kMul, kDiv, kPow, kMod, kNeg, kNot,
    kLt, kLe, kGt, kGe, kEq, kNe, kAnd, kOr, kSelect, kCall,
};

enum Var : std::uint8_t { kT, kX, kY, kZ, kH, kIn, kSel, kRand, kVarCount };

struct Named {
    std::string_view name;
    double value;
};
constexpr std::array<std::string_view, kVarCount> kVarNames = {"t", "x", "y", "z", "h", "in", "sel", "rand"};
constexpr std::array<Named, 3> kConstants = {{{"pi", kPi}, {"tau", 2.0 * kPi}, {"e", 2.71828182845904523536}}};

enum Fn : std::uint8_t {
    fSin, fCos, fTan, fAsin, fAcos, fAtan, fSinh, fCosh, fTanh, fExp, fLn, fLog2, fLog10, fSqrt, fAbs, fSign,
    fFloor, fCeil, fRound, fFrac, fMin, fMax, fClamp, fMix, fPow, fMod, fStep, fSmoothstep, fSaw, fSquare, fTri,
    fNoise, fCount,
};
struct Function {
    std::string_view name;
    int arity;
};
constexpr std::array<Function, fCount> kFunctions = {{
    {"sin", 1},   {"cos", 1},   {"tan", 1},   {"asin", 1},  {"acos", 1},  {"atan", 1}, {"sinh", 1},       {"cosh", 1},
    {"tanh", 1},  {"exp", 1},   {"ln", 1},    {"log2", 1},  {"log10", 1}, {"sqrt", 1}, {"abs", 1},        {"sign", 1},
    {"floor", 1}, {"ceil", 1},  {"round", 1}, {"frac", 1},  {"min", 2},   {"max", 2},  {"clamp", 3},      {"mix", 3},
    {"pow", 2},   {"mod", 2},   {"step", 2},  {"smoothstep", 3}, {"saw", 1}, {"square", 1}, {"tri", 1},   {"noise", 1},
}};

double fracOf(double v) { return v - std::floor(v); }
double modOf(double a, double b) { return b == 0.0 ? 0.0 : a - b * std::floor(a / b); }

// Deterministic noise in -1..1 from a value and a seed.
double hashNoise(double value, std::uint64_t seed) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    Rng rng(bits ^ (seed * 0x9E3779B97F4A7C15ull));
    return rng.uniform() * 2.0 - 1.0;
}

double call(std::uint8_t fn, const double* a, std::uint64_t seed) {
    switch (fn) {
    case fSin: return std::sin(a[0]);
    case fCos: return std::cos(a[0]);
    case fTan: return std::tan(a[0]);
    case fAsin: return std::asin(a[0]);
    case fAcos: return std::acos(a[0]);
    case fAtan: return std::atan(a[0]);
    case fSinh: return std::sinh(a[0]);
    case fCosh: return std::cosh(a[0]);
    case fTanh: return std::tanh(a[0]);
    case fExp: return std::exp(a[0]);
    case fLn: return std::log(a[0]);
    case fLog2: return std::log2(a[0]);
    case fLog10: return std::log10(a[0]);
    case fSqrt: return std::sqrt(a[0]);
    case fAbs: return std::abs(a[0]);
    case fSign: return a[0] > 0.0 ? 1.0 : a[0] < 0.0 ? -1.0 : 0.0;
    case fFloor: return std::floor(a[0]);
    case fCeil: return std::ceil(a[0]);
    case fRound: return std::round(a[0]);
    case fFrac: return fracOf(a[0]);
    case fMin: return std::min(a[0], a[1]);
    case fMax: return std::max(a[0], a[1]);
    case fClamp: return std::min(std::max(a[0], a[1]), a[2]);
    case fMix: return a[0] + (a[1] - a[0]) * a[2];
    case fPow: return std::pow(a[0], a[1]);
    case fMod: return modOf(a[0], a[1]);
    case fStep: return a[1] < a[0] ? 0.0 : 1.0;
    case fSmoothstep: {
        if (a[0] == a[1])
            return a[2] < a[0] ? 0.0 : 1.0;
        const double u = std::clamp((a[2] - a[0]) / (a[1] - a[0]), 0.0, 1.0);
        return u * u * (3.0 - 2.0 * u);
    }
    case fSaw: return 2.0 * fracOf(a[0]) - 1.0;
    case fSquare: return fracOf(a[0]) < 0.5 ? 1.0 : -1.0;
    case fTri: return 1.0 - 4.0 * std::abs(fracOf(a[0] + 0.25) - 0.5);
    case fNoise: return hashNoise(a[0], seed);
    default: return 0.0;
    }
}

// How many single-character edits (insert, delete, replace, swap neighbours) turn a into b.
int editDistance(std::string_view a, std::string_view b) {
    const std::size_t rows = a.size() + 1, columns = b.size() + 1;
    std::vector<int> d(rows * columns);
    const auto at = [&](std::size_t i, std::size_t j) -> int& { return d[i * columns + j]; };
    for (std::size_t i = 0; i < rows; ++i)
        at(i, 0) = static_cast<int>(i);
    for (std::size_t j = 0; j < columns; ++j)
        at(0, j) = static_cast<int>(j);
    for (std::size_t i = 1; i < rows; ++i) {
        for (std::size_t j = 1; j < columns; ++j) {
            at(i, j) = std::min({at(i - 1, j) + 1, at(i, j - 1) + 1, at(i - 1, j - 1) + (a[i - 1] == b[j - 1] ? 0 : 1)});
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                at(i, j) = std::min(at(i, j), at(i - 2, j - 2) + 1);
        }
    }
    return at(a.size(), b.size());
}

std::string suggestion(std::string_view name) {
    std::string_view best;
    int bestDistance = 3;
    const auto consider = [&](std::string_view candidate) {
        const int d = editDistance(name, candidate);
        if (d < bestDistance) {
            bestDistance = d;
            best = candidate;
        }
    };
    for (const auto& f : kFunctions)
        consider(f.name);
    for (const auto v : kVarNames)
        consider(v);
    for (const auto& c : kConstants)
        consider(c.name);
    return best.empty() ? std::string() : "; did you mean \"" + std::string(best) + "\"?";
}

} // namespace

// Recursive descent; emits postfix bytecode as it goes.
class FormulaParser {
public:
    FormulaParser(std::string_view text, Formula& out) : text_(text), out_(out) {}

    bool run(FormulaError& error) {
        if (text_.size() > static_cast<std::size_t>(Formula::kMaxLength))
            return fail(Formula::kMaxLength, "the formula is longer than " + std::to_string(Formula::kMaxLength) + " characters", error);
        skipSpace();
        if (pos_ >= text_.size())
            return fail(0, "the formula is empty", error);
        if (!ternary() || failed_) {
            error = error_;
            return false;
        }
        skipSpace();
        if (pos_ < text_.size()) {
            const std::string rest(1, text_[pos_]);
            return fail(static_cast<int>(pos_),
                        rest == ")" ? "this \")\" has no matching \"(\""
                                    : "unexpected \"" + rest + "\" after the end of the expression; an operator is missing", error);
        }
        out_.nodes_ = static_cast<int>(out_.code_.size());
        out_.stackDepth_ = maxStack_;
        return true;
    }

private:
    bool fail(int position, std::string message, FormulaError& error) {
        error.position = position;
        error.message = std::move(message);
        return false;
    }
    bool fail(std::size_t position, std::string message) {
        if (!failed_) {
            failed_ = true;
            error_.position = static_cast<int>(position);
            error_.message = std::move(message);
        }
        return false;
    }
    void skipSpace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r'))
            ++pos_;
    }
    bool match(std::string_view token) {
        skipSpace();
        if (text_.substr(pos_, token.size()) != token)
            return false;
        pos_ += token.size();
        return true;
    }
    bool peek(std::string_view token) {
        skipSpace();
        return text_.substr(pos_, token.size()) == token;
    }
    bool emit(std::uint8_t op, int stackChange, std::uint8_t arg = 0, double value = 0.0) {
        if (static_cast<int>(out_.code_.size()) >= Formula::kMaxNodes)
            return fail(pos_, "the formula is too complex: it has more than " + std::to_string(Formula::kMaxNodes) + " parts");
        out_.code_.push_back({op, arg, value});
        stack_ += stackChange;
        maxStack_ = std::max(maxStack_, stack_);
        return true;
    }
    struct Depth {
        FormulaParser& p;
        bool ok;
        explicit Depth(FormulaParser& parser) : p(parser), ok(++parser.depth_ <= Formula::kMaxDepth) {
            if (!ok)
                p.fail(p.pos_, "the formula is nested too deeply (more than " + std::to_string(Formula::kMaxDepth) + " levels)");
        }
        ~Depth() { --p.depth_; }
    };

    bool ternary() {
        const Depth depth(*this);
        if (!depth.ok || !logicalOr())
            return false;
        skipSpace();
        const std::size_t question = pos_;
        if (!match("?"))
            return true;
        if (!ternary())
            return false;
        if (!match(":"))
            return fail(pos_, "the \"?\" at column " + std::to_string(question + 1) + " needs a \":\" and a second value");
        return ternary() && emit(kSelect, -2);
    }
    bool logicalOr() {
        if (!logicalAnd())
            return false;
        while (match("||"))
            if (!logicalAnd() || !emit(kOr, -1))
                return false;
        return true;
    }
    bool logicalAnd() {
        if (!equality())
            return false;
        while (match("&&"))
            if (!equality() || !emit(kAnd, -1))
                return false;
        return true;
    }
    bool equality() {
        if (!comparison())
            return false;
        for (;;) {
            if (match("==")) {
                if (!comparison() || !emit(kEq, -1))
                    return false;
            } else if (match("!=")) {
                if (!comparison() || !emit(kNe, -1))
                    return false;
            } else {
                return true;
            }
        }
    }
    bool comparison() {
        if (!additive())
            return false;
        for (;;) {
            std::uint8_t op = 0;
            if (match("<="))
                op = kLe;
            else if (match(">="))
                op = kGe;
            else if (match("<"))
                op = kLt;
            else if (match(">"))
                op = kGt;
            else
                return true;
            if (!additive() || !emit(op, -1))
                return false;
        }
    }
    bool additive() {
        if (!multiplicative())
            return false;
        for (;;) {
            if (match("+")) {
                if (!multiplicative() || !emit(kAdd, -1))
                    return false;
            } else if (match("-")) {
                if (!multiplicative() || !emit(kSub, -1))
                    return false;
            } else {
                return true;
            }
        }
    }
    bool multiplicative() {
        if (!unary())
            return false;
        for (;;) {
            std::uint8_t op = 0;
            if (match("*"))
                op = kMul;
            else if (match("/"))
                op = kDiv;
            else if (match("%"))
                op = kMod;
            else
                return true;
            if (!unary() || !emit(op, -1))
                return false;
        }
    }
    bool unary() {
        const Depth depth(*this);
        if (!depth.ok)
            return false;
        if (match("-"))
            return unary() && emit(kNeg, 0);
        if (match("+"))
            return unary();
        if (peek("!") && !peek("!="))
            return match("!") && unary() && emit(kNot, 0);
        return power();
    }
    bool power() { // binds tighter than a leading minus and groups to the right: -2^2 is -4, 2^3^2 is 2^9
        if (!primary())
            return false;
        if (!match("^"))
            return true;
        return unary() && emit(kPow, -1);
    }
    bool primary() {
        skipSpace();
        if (pos_ >= text_.size())
            return fail(pos_, "the formula ends here, but a value is expected");
        const char c = text_[pos_];
        if (c == '(') {
            const std::size_t open = pos_++;
            if (!ternary())
                return false;
            if (!match(")"))
                return fail(pos_, "missing \")\" to close the \"(\" at column " + std::to_string(open + 1));
            return true;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            double value = 0.0;
            const auto r = std::from_chars(text_.data() + pos_, text_.data() + text_.size(), value);
            if (r.ec != std::errc() || !std::isfinite(value))
                return fail(pos_, "this is not a readable number");
            pos_ = static_cast<std::size_t>(r.ptr - text_.data());
            return emit(kConst, 1, 0, value);
        }
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            const std::size_t start = pos_;
            while (pos_ < text_.size() && ((text_[pos_] >= 'a' && text_[pos_] <= 'z') || (text_[pos_] >= 'A' && text_[pos_] <= 'Z') ||
                                           (text_[pos_] >= '0' && text_[pos_] <= '9') || text_[pos_] == '_'))
                ++pos_;
            const std::string_view name = text_.substr(start, pos_ - start);
            const bool called = peek("(");
            for (std::size_t f = 0; f < kFunctions.size(); ++f) {
                if (kFunctions[f].name != name)
                    continue;
                if (!called)
                    return fail(start, "\"" + std::string(name) + "\" is a function: write " + std::string(name) + "(...)");
                return arguments(static_cast<std::uint8_t>(f), start);
            }
            for (std::size_t v = 0; v < kVarNames.size(); ++v) {
                if (kVarNames[v] != name)
                    continue;
                if (called)
                    return fail(start, "\"" + std::string(name) + "\" is a value, not a function");
                return emit(kVar, 1, static_cast<std::uint8_t>(v));
            }
            for (const auto& constant : kConstants) {
                if (constant.name != name)
                    continue;
                if (called)
                    return fail(start, "\"" + std::string(name) + "\" is a value, not a function");
                return emit(kConst, 1, 0, constant.value);
            }
            return fail(start, std::string(called ? "unknown function \"" : "unknown name \"") + std::string(name) + "\"" +
                                   suggestion(name));
        }
        if (c == ')')
            return fail(pos_, "a value is expected before this \")\"");
        return fail(pos_, "unexpected character \"" + std::string(1, c) + "\"");
    }
    bool arguments(std::uint8_t function, std::size_t namePosition) {
        match("(");
        int count = 0;
        if (!peek(")")) {
            do {
                if (!ternary())
                    return false;
                ++count;
            } while (match(","));
        }
        if (!match(")"))
            return fail(pos_, "missing \")\" after the arguments of " + std::string(kFunctions[function].name));
        const int wanted = kFunctions[function].arity;
        if (count != wanted)
            return fail(namePosition, std::string(kFunctions[function].name) + " takes " + std::to_string(wanted) +
                                          (wanted == 1 ? " argument" : " arguments") + ", but " + std::to_string(count) +
                                          (count == 1 ? " was given" : " were given"));
        return emit(kCall, 1 - wanted, function);
    }

    std::string_view text_;
    Formula& out_;
    std::size_t pos_ = 0;
    int depth_ = 0, stack_ = 0, maxStack_ = 0;
    bool failed_ = false;
    FormulaError error_;
};

bool Formula::compile(std::string_view text, Formula& out, FormulaError& error) {
    out = Formula();
    FormulaParser parser(text, out);
    if (parser.run(error))
        return true;
    out = Formula();
    return false;
}

double Formula::evaluate(const FormulaVars& vars) const {
    if (code_.empty())
        return 0.0;
    // The parser limits nesting, so the stack depth is bounded well below this size.
    double stack[kMaxNodes + 4];
    int top = 0; // number of values on the stack
    const double variables[kVarCount] = {vars.t, vars.x, vars.y, vars.z, vars.h, vars.in, vars.sel, vars.rand};
    const std::uint64_t seed = vars.seed;
    for (const Instruction& i : code_) {
        switch (i.op) {
        case kConst: stack[top++] = i.value; break;
        case kVar: stack[top++] = variables[i.arg]; break;
        case kAdd: --top; stack[top - 1] += stack[top]; break;
        case kSub: --top; stack[top - 1] -= stack[top]; break;
        case kMul: --top; stack[top - 1] *= stack[top]; break;
        case kDiv: --top; stack[top - 1] /= stack[top]; break;
        case kPow: --top; stack[top - 1] = std::pow(stack[top - 1], stack[top]); break;
        case kMod: --top; stack[top - 1] = modOf(stack[top - 1], stack[top]); break;
        case kNeg: stack[top - 1] = -stack[top - 1]; break;
        case kNot: stack[top - 1] = stack[top - 1] == 0.0 ? 1.0 : 0.0; break;
        case kLt: --top; stack[top - 1] = stack[top - 1] < stack[top] ? 1.0 : 0.0; break;
        case kLe: --top; stack[top - 1] = stack[top - 1] <= stack[top] ? 1.0 : 0.0; break;
        case kGt: --top; stack[top - 1] = stack[top - 1] > stack[top] ? 1.0 : 0.0; break;
        case kGe: --top; stack[top - 1] = stack[top - 1] >= stack[top] ? 1.0 : 0.0; break;
        case kEq: --top; stack[top - 1] = stack[top - 1] == stack[top] ? 1.0 : 0.0; break;
        case kNe: --top; stack[top - 1] = stack[top - 1] != stack[top] ? 1.0 : 0.0; break;
        case kAnd: --top; stack[top - 1] = stack[top - 1] != 0.0 && stack[top] != 0.0 ? 1.0 : 0.0; break;
        case kOr: --top; stack[top - 1] = stack[top - 1] != 0.0 || stack[top] != 0.0 ? 1.0 : 0.0; break;
        case kSelect:
            top -= 2;
            stack[top - 1] = stack[top - 1] != 0.0 ? stack[top] : stack[top + 1];
            break;
        case kCall: {
            const int arity = kFunctions[i.arg].arity;
            top -= arity;
            stack[top] = call(i.arg, stack + top, seed);
            ++top;
            break;
        }
        default: break;
        }
    }
    return top > 0 ? stack[top - 1] : 0.0;
}

bool applyFormula(Wavetable& table, const FormulaRequest& request, std::string& error) {
    Formula value, phase;
    FormulaError problem;
    if (!Formula::compile(request.expression, value, problem)) {
        error = problem.describe();
        return false;
    }
    const bool hasPhase = request.domain == FormulaDomain::Harmonics && !request.phaseExpression.empty();
    if (hasPhase && !Formula::compile(request.phaseExpression, phase, problem)) {
        error = "phase expression, " + problem.describe();
        return false;
    }
    if (table.frames.empty())
        table.frames.push_back(Frame{});
    const int count = table.size();
    const int first = std::max(request.scope.first, 0);
    const int last = request.scope.last < 0 ? count - 1 : std::min(request.scope.last, count - 1);
    const Frame selected = table.frames[static_cast<std::size_t>(std::clamp(request.selectedFrame, 0, count - 1))];
    const auto clean = [](double v, double limit) { return std::isfinite(v) ? std::clamp(v, -limit, limit) : 0.0; };
    // `rand` is noise fixed for each position: the same in every frame, different per seed.
    const auto noiseAt = [&](int index) {
        Rng rng(request.seed * 0x9E3779B97F4A7C15ull + static_cast<std::uint64_t>(index) * 0xD1B54A32D192ED03ull);
        return rng.uniform() * 2.0 - 1.0;
    };

    if (request.domain == FormulaDomain::Wave) {
        for (int f = first; f <= last; ++f) {
            Frame& frame = table.frames[static_cast<std::size_t>(f)];
            FormulaVars v;
            v.seed = request.seed;
            v.y = count > 1 ? static_cast<double>(f) / (count - 1) : 0.0;
            v.z = 2.0 * v.y - 1.0;
            Frame out;
            for (int n = 0; n < kFrameSize; ++n) {
                v.t = static_cast<double>(n) / kFrameSize;
                v.x = 2.0 * v.t - 1.0;
                v.in = frame[static_cast<std::size_t>(n)];
                v.sel = selected[static_cast<std::size_t>(n)];
                v.rand = noiseAt(n);
                out[static_cast<std::size_t>(n)] = static_cast<float>(clean(value.evaluate(v), 16.0));
            }
            frame = out;
        }
    } else {
        const Harmonics selectedHarmonics = analyzeFrame(selected);
        for (int f = first; f <= last; ++f) {
            Frame& frame = table.frames[static_cast<std::size_t>(f)];
            const Harmonics existing = analyzeFrame(frame);
            Harmonics out;
            FormulaVars v;
            v.seed = request.seed;
            v.y = count > 1 ? static_cast<double>(f) / (count - 1) : 0.0;
            v.z = 2.0 * v.y - 1.0;
            for (int k = 1; k <= kMaxHarmonics; ++k) {
                const auto at = static_cast<std::size_t>(k);
                v.h = k;
                v.t = static_cast<double>(k - 1) / (kMaxHarmonics - 1); // along the harmonic axis
                v.x = 2.0 * v.t - 1.0;
                v.in = existing.magnitude[at];
                v.sel = selectedHarmonics.magnitude[at];
                v.rand = noiseAt(k);
                double level = clean(value.evaluate(v), 1.0e6);
                double angle = existing.magnitude[at] > 1e-12 ? existing.phase[at] : 0.0;
                if (k == kMaxHarmonics && !hasPhase)
                    angle = existing.magnitude[at] > 1e-12 ? existing.phase[at] : kPi / 2.0; // only a cosine exists up there
                if (hasPhase) {
                    v.in = existing.phase[at];
                    v.sel = selectedHarmonics.phase[at];
                    angle = clean(phase.evaluate(v), 1.0e6);
                }
                if (level < 0.0) { // a negative level is the same harmonic turned upside down
                    level = -level;
                    angle += kPi;
                }
                out.magnitude[at] = level;
                out.phase[at] = angle;
            }
            // The top harmonic can only be a cosine: keep what the expression asked for as far as that allows.
            const double top = out.magnitude[kMaxHarmonics] * std::sin(out.phase[kMaxHarmonics]);
            out.magnitude[kMaxHarmonics] = std::abs(top);
            out.phase[kMaxHarmonics] = top < 0.0 ? -kPi / 2.0 : kPi / 2.0;
            frame = synthesizeFrame(out);
        }
    }
    if (request.normalize)
        processFrames(table, {first, last}, ProcessOp::NormalizeEach);
    return true;
}

} // namespace zed
