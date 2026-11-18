// What the tools work on (plan 4.2): headless sessions (the engine library in this process) and
// live instances reached through the local pipe. A target is an explicit handle passed as an
// ordinary tool argument ("session:3f9a1c2e", "live:8b2d..."); the server keeps no hidden
// per-connection state. Also here: Zederia's folders, the agent permissions, the file sandbox and
// the clip store. Every function may be called from several request threads at once.
#pragma once

#include "Schema.h"

#include "zederia/content/Library.h"
#include "zederia/link/Link.h"
#include "zederia/patch/History.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace zmcp {

namespace fs = std::filesystem;

// Thrown by tool code: becomes a tool result with isError and the problems, each with a path and a fix.
struct ToolError {
    std::vector<Problem> problems;
};
[[noreturn]] void fail(std::string path, std::string problem, std::string fix);

// Thrown when the client cancelled the request; no response is sent.
struct Cancelled {};

// One tool call: who is calling, progress reporting and cancellation.
struct Call {
    std::string client = "agent"; // shown in undo labels and the instance's activity log
    std::function<void(double fraction, const std::string& message)> progress = [](double, const std::string&) {};
    std::function<bool()> cancelled = [] { return false; };
    void checkCancelled() const {
        if (cancelled())
            throw Cancelled{};
    }
};

// --- Limits (plan 4.8) ----------------------------------------------------------------------------
inline constexpr double kMaxRenderSeconds = 120.0;   // audio per render call (plus its tail)
inline constexpr double kMaxTailSeconds = 30.0;
inline constexpr int kMaxSessions = 16;
inline constexpr int kMaxPackFiles = 256;
inline constexpr double kMaxPackSeconds = 1800.0;
inline constexpr int kMaxRenderNotes = 4096;
inline constexpr std::size_t kMaxRequestBytes = 16u * 1024u * 1024u;
inline constexpr std::chrono::minutes kSessionIdle{120};

// --- File sandbox (plan 4.8) -----------------------------------------------------------------------
// Paths are canonicalized (symbolic links and junctions resolved), traversal ("..") is rejected,
// and only paths inside the allowed roots with a known extension pass. Device names, UNC and
// device paths, alternate data streams and trailing dots or spaces are refused outright.
struct SandboxResult {
    bool ok = false;
    fs::path path;
    std::string problem, fix;
};
SandboxResult sandboxPath(const std::string& input, const fs::path& base, const std::vector<fs::path>& roots,
                          const std::vector<std::string>& extensions);
bool insideRoot(const fs::path& path, const fs::path& root);
fs::path pathFromUtf8(const std::string& s);
std::string utf8(const fs::path& p);

class Bridge {
public:
    struct Config {
        std::string userDir;    // Documents/Zederia unless given
        std::string factoryDir; // the standard factory folder unless given
        std::vector<std::string> allowRead, allowWrite; // added by command-line flags
        bool live = true;       // list and reach live instances
        std::string serverVersion;
    };
    explicit Bridge(Config config);
    ~Bridge();

    const Config& config() const { return config_; }
    const zed::ContentLibrary& library() const { return library_; }
    fs::path userDir() const;
    fs::path userFolder(const std::string& sub) const; // created when missing
    zed::link::Permissions permissions() const;       // the AGENT page's file plus the flags

    // Files the agent names. `argPath` is the argument's JSON pointer for error messages.
    fs::path resolveRead(const std::string& input, const std::vector<std::string>& extensions, const std::string& argPath,
                         const fs::path& base = {}) const;
    fs::path resolveWrite(const std::string& input, const std::vector<std::string>& extensions, const std::string& argPath,
                          const fs::path& base) const;
    std::vector<fs::path> readRoots() const;
    std::vector<fs::path> writeRoots() const;

    // --- Targets --------------------------------------------------------------------------------------
    std::string openSession(const zed::Patch& start, const std::string& name, const Call& call);
    void closeSession(const std::string& target);
    J listTargets();
    J liveTargets(bool withStatus);
    static bool isSession(const std::string& target) { return target.rfind("session:", 0) == 0; }
    static bool isLive(const std::string& target) { return target.rfind("live:", 0) == 0; }
    void checkTarget(const std::string& target, const std::string& argPath = "/target") const;

    zed::Patch patch(const std::string& target, const Call& call);
    struct EditResult {
        zed::Patch before, after;
        bool applied = false;
    };
    // One transaction and, when something changed, one undo step labelled "<client>: <label>".
    // `asPreset` loads the result as a preset (a live instance keeps its locked parameters).
    EditResult edit(const std::string& target, const Call& call, const std::string& label,
                    const std::function<void(zed::Patch&)>& change, bool dryRun = false, bool asPreset = false);
    J history(const std::string& target, const Call& call, const std::string& action, int step);
    J liveRequest(const std::string& target, const std::string& method, J params, const Call& call, int timeoutMs = 8000);

    // --- Rendering support --------------------------------------------------------------------------
    // Tables and the texture the patch names, built for the renderer (cached by reference).
    zed::RenderSettings renderSettings(const zed::Patch& patch, std::vector<std::string>& problems);

    // --- Clips --------------------------------------------------------------------------------------
    std::string saveClip(const zed::Clip& clip, const std::string& id); // "" makes a new id
    bool loadClip(const std::string& id, zed::Clip& out) const;

    // --- Presets ------------------------------------------------------------------------------------
    // "factory:Basses/Deep" or "user:Agent/Glass Pluck" -> file; "" when it does not map.
    fs::path presetPath(const std::string& id) const;
    std::string presetId(const fs::path& file) const;
    fs::path presetsDir(bool factory) const;

private:
    struct Session;
    Session* findSession(const std::string& target, std::unique_lock<std::mutex>& lock);
    void prune();
    J liveCall(const zed::link::Discovery& d, const std::string& method, const J& params, const Call& call, int timeoutMs);
    bool findLive(const std::string& target, zed::link::Discovery& out) const;

    Config config_;
    zed::ContentLibrary library_;
    mutable std::mutex mutex_;
    std::map<std::string, std::unique_ptr<Session>> sessions_;
    std::mutex tableMutex_;
    std::map<std::string, std::shared_ptr<const zed::TableData>> tableCache_;
    std::map<std::string, std::shared_ptr<const zed::SampleData>> textureCache_;
};

// --- Patch helpers --------------------------------------------------------------------------------------
J patchJson(const zed::Patch& patch, bool compact = false);
// Problems from a patch load, split into errors and warnings.
void issuesToProblems(const std::vector<zed::Issue>& issues, std::vector<Problem>& errors, J& warnings);
// What changed from `a` to `b`: [{path, from, to}], at most `limit` entries.
J patchDiff(const zed::Patch& a, const zed::Patch& b, std::size_t limit = 200);
// A short description of a patch: name, sources, filters, envelopes, routes, effects, macros.
J patchSummary(const zed::Patch& patch);
std::string clipToText(const zed::Clip& clip);

} // namespace zmcp
