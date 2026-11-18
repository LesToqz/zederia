#include "Bridge.h"

#include "zederia/Version.h"
#include "zederia/patch/Shape.h"
#include "zederia/table/TableData.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace zmcp {

using zed::Patch;

void fail(std::string path, std::string problem, std::string fix) {
    throw ToolError{{Problem{std::move(path), std::move(problem), std::move(fix)}}};
}

fs::path pathFromUtf8(const std::string& s) { return fs::path(zed::link::widen(s)); }
std::string utf8(const fs::path& p) {
#ifdef _WIN32
    return zed::link::narrow(p.wstring());
#else
    return p.string();
#endif
}

// --- Sandbox ----------------------------------------------------------------------------------------

namespace {

std::wstring lowerW(std::wstring s) {
    for (auto& c : s)
        c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool reservedName(const std::wstring& component) {
    std::wstring stem = lowerW(component.substr(0, component.find(L'.')));
    while (!stem.empty() && (stem.back() == L' '))
        stem.pop_back();
    static const wchar_t* names[] = {L"con", L"prn", L"aux", L"nul", L"conin$", L"conout$"};
    for (const auto* n : names)
        if (stem == n)
            return true;
    if (stem.size() == 4 && (stem.rfind(L"com", 0) == 0 || stem.rfind(L"lpt", 0) == 0) && stem[3] >= L'0' && stem[3] <= L'9')
        return true;
    return false;
}

} // namespace

bool insideRoot(const fs::path& path, const fs::path& root) {
    auto p = path.begin(), r = root.begin();
    for (; r != root.end(); ++r, ++p) {
        if (r->empty())
            continue;
        if (p == path.end())
            return false;
#ifdef _WIN32
        if (lowerW(p->wstring()) != lowerW(r->wstring()))
#else
        if (*p != *r)
#endif
            return false;
    }
    return true;
}

SandboxResult sandboxPath(const std::string& input, const fs::path& base, const std::vector<fs::path>& roots,
                          const std::vector<std::string>& extensions) {
    SandboxResult r;
    const auto refuse = [&](std::string problem, std::string fix) {
        r.ok = false;
        r.problem = std::move(problem);
        r.fix = std::move(fix);
        return r;
    };
    if (input.empty())
        return refuse("the path is empty", "give a file name or a path inside an allowed folder");
    if (input.find('\0') != std::string::npos || input.size() > 1024)
        return refuse("the path is not a usable path", "give a plain file path");
    std::string unified = input;
    std::replace(unified.begin(), unified.end(), '\\', '/');
    if (unified.rfind("//", 0) == 0)
        return refuse("network (UNC) and device paths are not allowed", "use a folder on this computer that the user allowed");
    // ':' only as a drive letter's colon at the start ("C:/..."); anything else is a stream or a device.
    for (std::size_t i = 0; i < unified.size(); ++i)
        if (unified[i] == ':' && !(i == 1 && std::isalpha(static_cast<unsigned char>(unified[0]))))
            return refuse("\":\" is only allowed after a drive letter", "remove the \":\" (alternate data streams and devices are not allowed)");
    const fs::path given = pathFromUtf8(unified);
    for (const auto& part : given) {
        const std::wstring c = part.wstring();
        if (c == L"..")
            return refuse("\"..\" is not allowed in paths", "name the file inside an allowed folder directly");
        if (c != L"/" && !c.empty() && (c.back() == L'.' || c.back() == L' ') && c != L".")
            return refuse("a path part may not end with a dot or a space", "remove the trailing dot or space");
        if (reservedName(c))
            return refuse("\"" + utf8(part) + "\" is a reserved device name", "choose another file name");
    }
    fs::path full = given.is_absolute() ? given : base / given;
    std::error_code ec;
    fs::path canonical = fs::weakly_canonical(full, ec);
    if (ec)
        return refuse("the path cannot be resolved", "check the folder exists and the name is right");
    canonical = canonical.lexically_normal();
    if (!extensions.empty()) {
        std::string ext = utf8(canonical.extension());
        for (auto& c : ext)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end()) {
            std::string list;
            for (const auto& e : extensions)
                list += (list.empty() ? "" : ", ") + e;
            return refuse("files of type \"" + (ext.empty() ? std::string("(none)") : ext) + "\" are not allowed here", "use one of: " + list);
        }
    }
    for (const auto& root : roots) {
        std::error_code rec;
        fs::path rc = fs::weakly_canonical(root, rec);
        if (rec)
            continue;
        if (insideRoot(canonical, rc.lexically_normal())) {
            r.ok = true;
            r.path = canonical;
            return r;
        }
    }
    std::string allowed;
    for (const auto& root : roots)
        allowed += (allowed.empty() ? "" : "; ") + utf8(root);
    return refuse("\"" + input + "\" is outside the folders the agent may use",
                  "use a path inside: " + allowed + ". The user can allow more folders on the AGENT page or with --allow-read/--allow-write");
}

// --- Bridge -----------------------------------------------------------------------------------------

struct Bridge::Session {
    std::mutex mutex;
    std::string name;
    Patch current, start;
    zed::History history;
    std::chrono::steady_clock::time_point lastUsed = std::chrono::steady_clock::now();
    std::int64_t created = 0;
    std::string client;
};

Bridge::Bridge(Config config) : config_(std::move(config)), library_(zed::ContentLibrary::standard()) {
    if (!config_.userDir.empty())
        library_.setUserDir(config_.userDir);
    if (!config_.factoryDir.empty())
        library_.setFactoryDir(config_.factoryDir);
}

Bridge::~Bridge() = default;

fs::path Bridge::userDir() const { return pathFromUtf8(library_.userDir()); }

fs::path Bridge::userFolder(const std::string& sub) const {
    fs::path p = userDir() / pathFromUtf8(sub);
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

zed::link::Permissions Bridge::permissions() const {
    auto p = zed::link::loadPermissions();
    for (const auto& f : config_.allowRead)
        p.readFolders.push_back(f);
    return p;
}

std::vector<fs::path> Bridge::readRoots() const {
    const auto p = permissions();
    std::vector<fs::path> roots = {userDir()};
    if (!library_.factoryDir().empty())
        roots.push_back(pathFromUtf8(library_.factoryDir()));
    if (p.readSamples)
        for (const auto& f : p.readFolders)
            roots.push_back(pathFromUtf8(f));
    for (const auto& f : config_.allowWrite) // a folder the agent may write it may read back
        roots.push_back(pathFromUtf8(f));
    return roots;
}

std::vector<fs::path> Bridge::writeRoots() const {
    const auto p = permissions();
    std::vector<fs::path> roots;
    if (p.writeZederia)
        roots.push_back(userDir());
    if (p.writeElsewhere)
        for (const auto& f : p.writeFolders)
            roots.push_back(pathFromUtf8(f));
    for (const auto& f : config_.allowWrite) // given on the command line: the user's explicit choice
        roots.push_back(pathFromUtf8(f));
    return roots;
}

fs::path Bridge::resolveRead(const std::string& input, const std::vector<std::string>& extensions, const std::string& argPath,
                             const fs::path& base) const {
    const auto r = sandboxPath(input, base.empty() ? userDir() : base, readRoots(), extensions);
    if (!r.ok)
        fail(argPath, r.problem, r.fix);
    std::error_code ec;
    if (!fs::is_regular_file(r.path, ec))
        fail(argPath, "there is no file at \"" + input + "\"", "check the name; paths are relative to " + utf8(base.empty() ? userDir() : base));
    return r.path;
}

fs::path Bridge::resolveWrite(const std::string& input, const std::vector<std::string>& extensions, const std::string& argPath,
                              const fs::path& base) const {
    const auto roots = writeRoots();
    if (roots.empty())
        fail(argPath, "writing files is switched off for agents", "the user can allow it on the AGENT page (Permissions)");
    const auto r = sandboxPath(input, base, roots, extensions);
    if (!r.ok)
        fail(argPath, r.problem, r.fix);
    std::error_code ec;
    fs::create_directories(r.path.parent_path(), ec);
    return r.path;
}

void Bridge::prune() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (now - it->second->lastUsed > kSessionIdle)
            it = sessions_.erase(it);
        else
            ++it;
    }
}

std::string Bridge::openSession(const Patch& start, const std::string& name, const Call& call) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune();
    if (static_cast<int>(sessions_.size()) >= kMaxSessions)
        fail("", "there are already " + std::to_string(kMaxSessions) + " headless sessions open", "close one with close_session first");
    std::string id;
    do
        id = "session:" + zed::link::randomHex(4);
    while (sessions_.count(id));
    auto s = std::make_unique<Session>();
    s->name = name.empty() ? start.meta.name : name;
    s->current = start;
    s->start = start;
    s->client = call.client;
    s->created = static_cast<std::int64_t>(std::time(nullptr));
    s->history.reset(start, call.client + ": Open session");
    sessions_[id] = std::move(s);
    return id;
}

void Bridge::checkTarget(const std::string& target, const std::string& argPath) const {
    if (isSession(target)) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessions_.count(target))
            fail(argPath, "no session \"" + target + "\" (sessions end when the server restarts or after 2 hours unused)",
                 "open a new one with open_session, or call list_targets");
        return;
    }
    if (isLive(target)) {
        zed::link::Discovery d;
        if (!config_.live)
            fail(argPath, "live targets are switched off for this server", "start zederia-mcp without --no-live");
        if (!findLive(target, d))
            fail(argPath, "no live instance \"" + target + "\"", "call list_targets; in the host, open Zederia's AGENT page and switch Agent Link on");
        return;
    }
    fail(argPath, "\"" + target + "\" is not a target", "use a handle from open_session (\"session:...\") or list_targets (\"live:...\")");
}

void Bridge::closeSession(const std::string& target) {
    if (!isSession(target))
        fail("/target", "only headless sessions can be closed", "pass a \"session:...\" handle; live instances stay open in their host");
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.erase(target) == 0)
        fail("/target", "no session \"" + target + "\"", "call list_targets to see what is open");
}

Bridge::Session* Bridge::findSession(const std::string& target, std::unique_lock<std::mutex>& lock) {
    Session* s = nullptr;
    {
        std::lock_guard<std::mutex> mapLock(mutex_);
        auto it = sessions_.find(target);
        if (it != sessions_.end())
            s = it->second.get();
    }
    if (!s)
        fail("/target", "no session \"" + target + "\" (sessions end when the server restarts or after 2 hours unused)",
             "open a new one with open_session");
    lock = std::unique_lock<std::mutex>(s->mutex);
    s->lastUsed = std::chrono::steady_clock::now();
    return s;
}

bool Bridge::findLive(const std::string& target, zed::link::Discovery& out) const {
    if (!config_.live || !isLive(target))
        return false;
    const std::string id = target.substr(5);
    for (auto& d : zed::link::listDiscoveries())
        if (d.instanceId == id) {
            out = d;
            return true;
        }
    return false;
}

J Bridge::liveCall(const zed::link::Discovery& d, const std::string& method, const J& params, const Call& call, int timeoutMs) {
    zed::link::PipeClient client;
    std::string error;
    if (!client.connect(d.pipe, 2000, error))
        fail("/target", "cannot reach the instance: " + error, "check that the host still runs and Agent Link is on (AGENT page)");
    static std::atomic<int> counter{0};
    J request = {{"v", zed::link::kLinkProtocol}, {"id", ++counter}, {"token", d.token}, {"client", call.client}, {"method", method},
                 {"params", params}};
    std::string response;
    if (!client.request(request.dump(), response, timeoutMs, error))
        fail("/target", "the instance did not answer: " + error, "try again; if the host is busy, wait a moment");
    const J reply = J::parse(response, nullptr, false);
    if (reply.is_discarded() || !reply.is_object())
        fail("/target", "the instance sent an unreadable answer", "update the plug-in and the server to the same version");
    if (!reply.value("ok", false)) {
        const J err = reply.value("error", J::object());
        throw ToolError{{Problem{err.value("path", "/target"), err.value("message", "the instance refused the request"),
                                 err.value("fix", "see the instance's AGENT page")}}};
    }
    return reply.value("result", J::object());
}

J Bridge::liveRequest(const std::string& target, const std::string& method, J params, const Call& call, int timeoutMs) {
    zed::link::Discovery d;
    if (!findLive(target, d))
        fail("/target", "no live instance \"" + target + "\"", "call list_targets; switch Agent Link on in the instance's AGENT page");
    return liveCall(d, method, params, call, timeoutMs);
}

J Bridge::liveTargets(bool withStatus) {
    J out = J::array();
    if (!config_.live)
        return out;
    for (const auto& d : zed::link::listDiscoveries()) {
        J t = {{"target", "live:" + d.instanceId}, {"kind", "live"}, {"name", d.name}, {"host", d.host}, {"version", d.version}};
        if (withStatus) {
            try {
                Call quiet;
                quiet.client = "zederia-mcp";
                t["status"] = liveCall(d, "status", J::object(), quiet, 1500);
            } catch (const ToolError& e) {
                t["status"] = {{"reachable", false}, {"problem", e.problems.empty() ? "" : e.problems.front().problem}};
            }
        }
        out.push_back(std::move(t));
    }
    return out;
}

J Bridge::listTargets() {
    J out = J::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        prune();
        for (const auto& [id, s] : sessions_) {
            std::lock_guard<std::mutex> sl(s->mutex);
            out.push_back({{"target", id},
                           {"kind", "session"},
                           {"name", s->name},
                           {"patch_name", s->current.meta.name},
                           {"opened_by", s->client},
                           {"undo_steps", s->history.current()}});
        }
    }
    for (auto& t : liveTargets(false))
        out.push_back(std::move(t));
    return out;
}

Patch Bridge::patch(const std::string& target, const Call& call) {
    if (isSession(target)) {
        std::unique_lock<std::mutex> lock;
        Session* s = findSession(target, lock);
        return s->current;
    }
    if (isLive(target)) {
        if (!permissions().readPatch)
            fail("/target", "reading the instance's patch is switched off", "the user can allow it on the AGENT page (Permissions)");
        const J r = liveRequest(target, "get_patch", J::object(), call);
        auto loaded = zed::loadPatch(r.value("patch", std::string()));
        if (!loaded.ok())
            fail("/target", "the instance's patch could not be read", "update the plug-in and the server to the same version");
        return loaded.patch;
    }
    checkTarget(target);
    return {};
}

Bridge::EditResult Bridge::edit(const std::string& target, const Call& call, const std::string& label,
                                const std::function<void(Patch&)>& change, bool dryRun, bool asPreset) {
    EditResult r;
    const std::string stepLabel = call.client + ": " + label;
    if (isSession(target)) {
        std::unique_lock<std::mutex> lock;
        Session* s = findSession(target, lock);
        r.before = s->current;
        r.after = s->current;
        change(r.after);
        if (!dryRun && !(r.after == r.before)) {
            s->history.push(stepLabel, r.after, call.client);
            s->current = r.after;
            r.applied = true;
        }
        return r;
    }
    if (!isLive(target))
        checkTarget(target);
    const auto perms = permissions();
    if (!perms.readPatch || (!dryRun && !perms.editPatch))
        fail("/target", "editing the instance's patch is switched off", "the user can allow it on the AGENT page (Permissions)");
    for (int attempt = 0; attempt < 3; ++attempt) {
        const J got = liveRequest(target, "get_patch", J::object(), call);
        auto loaded = zed::loadPatch(got.value("patch", std::string()));
        if (!loaded.ok())
            fail("/target", "the instance's patch could not be read", "update the plug-in and the server to the same version");
        r.before = loaded.patch;
        r.after = loaded.patch;
        change(r.after);
        if (dryRun || r.after == r.before)
            return r;
        try {
            liveRequest(target, "put_patch",
                        {{"patch", zed::savePatch(r.after)}, {"label", stepLabel}, {"expect_hash", got.value("hash", "")}, {"preset", asPreset}},
                        call);
            r.applied = true;
            return r;
        } catch (const ToolError& e) {
            if (e.problems.empty() || e.problems.front().problem.find("changed meanwhile") == std::string::npos || attempt == 2)
                throw;
        }
    }
    return r;
}

J Bridge::history(const std::string& target, const Call& call, const std::string& action, int step) {
    if (isLive(target)) {
        if (action != "list" && !permissions().editPatch)
            fail("/target", "editing the instance's patch is switched off", "the user can allow it on the AGENT page (Permissions)");
        return liveRequest(target, "history", {{"action", action}, {"step", step}}, call);
    }
    std::unique_lock<std::mutex> lock;
    Session* s = findSession(target, lock);
    auto& h = s->history;
    if (action == "undo") {
        if (const Patch* p = h.undo())
            s->current = *p;
        else
            fail("/action", "there is nothing to undo", "call history with action \"list\" to see the steps");
    } else if (action == "redo") {
        if (const Patch* p = h.redo())
            s->current = *p;
        else
            fail("/action", "there is nothing to redo", "call history with action \"list\" to see the steps");
    } else if (action == "jump") {
        if (step < 0 || step >= h.size())
            fail("/step", "there is no step " + std::to_string(step), "use a step index from 0 to " + std::to_string(h.size() - 1));
        if (const Patch* p = h.jumpTo(step))
            s->current = *p;
    } else if (action == "revert_agent_session") {
        if (h.push(call.client + ": Revert agent session", s->start, call.client))
            s->current = s->start;
    } else if (action != "list") {
        fail("/action", "unknown action \"" + action + "\"", "use list, undo, redo, jump or revert_agent_session");
    }
    J steps = J::array();
    const int first = std::max(0, h.size() - 50);
    for (int i = first; i < h.size(); ++i)
        steps.push_back({{"step", i}, {"label", h.label(i)}, {"origin", h.origin(i)}, {"current", i == h.current()}});
    return {{"target", target}, {"current", h.current()}, {"can_undo", h.canUndo()}, {"can_redo", h.canRedo()},
            {"undo", h.undoLabel()}, {"redo", h.redoLabel()}, {"steps", steps}};
}

zed::RenderSettings Bridge::renderSettings(const Patch& patch, std::vector<std::string>& problems) {
    zed::RenderSettings settings;
    for (int i = 0; i < zed::kNumOscs; ++i) {
        const auto idx = static_cast<std::size_t>(i);
        if (const auto& data = patch.tableData[idx]) {
            settings.tables[idx] = zed::TableData::build(*data.table);
            continue;
        }
        const std::string& ref = patch.tables[idx];
        if (ref.empty())
            continue;
        std::lock_guard<std::mutex> lock(tableMutex_);
        auto it = tableCache_.find(ref);
        if (it == tableCache_.end()) {
            std::string error;
            auto t = library_.loadTable(ref, error);
            if (!t) {
                problems.push_back("osc" + std::to_string(i + 1) + " table \"" + ref + "\": " + error + "; the built-in table plays instead");
                continue;
            }
            it = tableCache_.emplace(ref, std::move(t)).first;
        }
        settings.tables[idx] = it->second;
    }
    if (!patch.textureSample.empty()) {
        std::lock_guard<std::mutex> lock(tableMutex_);
        auto it = textureCache_.find(patch.textureSample);
        if (it == textureCache_.end()) {
            std::string error;
            auto t = library_.loadTexture(patch.textureSample, error);
            if (t)
                it = textureCache_.emplace(patch.textureSample, std::move(t)).first;
            else
                problems.push_back("texture \"" + patch.textureSample + "\": " + error + "; the built-in noise plays instead");
        }
        if (it != textureCache_.end())
            settings.texture = it->second;
    }
    return settings;
}

// --- Clips ------------------------------------------------------------------------------------------

namespace {
J clipJson(const zed::Clip& c) {
    J notes = J::array();
    for (const auto& n : c.notes)
        notes.push_back({{"start", n.start}, {"length", n.length}, {"key", n.key}, {"velocity", n.velocity}});
    return {{"format", "zederia.clip"}, {"name", c.name}, {"bars", c.bars}, {"beats_per_bar", c.beatsPerBar}, {"tempo", c.tempo},
            {"key", c.key}, {"root", c.root}, {"notes", notes}};
}
} // namespace

std::string Bridge::saveClip(const zed::Clip& clip, const std::string& id) {
    std::string use = id;
    if (use.empty())
        use = "clip:" + zed::link::randomHex(4);
    const fs::path file = userFolder("Clips/Agent") / pathFromUtf8(use.substr(5) + ".json");
    std::ofstream f(file, std::ios::binary);
    f << clipJson(clip).dump(1);
    if (!f)
        fail("", "the clip could not be saved", "check that " + utf8(file.parent_path()) + " is writable");
    return use;
}

bool Bridge::loadClip(const std::string& id, zed::Clip& out) const {
    if (id.rfind("clip:", 0) != 0 || id.size() != 13)
        return false;
    for (std::size_t i = 5; i < id.size(); ++i)
        if (!std::isxdigit(static_cast<unsigned char>(id[i])))
            return false;
    std::ifstream f(userDir() / "Clips" / "Agent" / pathFromUtf8(id.substr(5) + ".json"), std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    const J j = J::parse(ss.str(), nullptr, false);
    if (!j.is_object())
        return false;
    out = zed::Clip{};
    out.name = j.value("name", "");
    out.bars = j.value("bars", 1.0);
    out.beatsPerBar = j.value("beats_per_bar", 4);
    out.tempo = j.value("tempo", 120.0);
    out.key = j.value("key", "");
    out.root = j.value("root", 60);
    for (const auto& n : j.value("notes", J::array()))
        out.notes.push_back({n.value("start", 0.0), n.value("length", 0.25), n.value("key", 60), n.value("velocity", 0.8)});
    return true;
}

// --- Presets ------------------------------------------------------------------------------------------

fs::path Bridge::presetsDir(bool factory) const {
    return factory ? pathFromUtf8(library_.factoryDir()) / "Presets" : userDir() / "Presets";
}

fs::path Bridge::presetPath(const std::string& id) const {
    const bool factory = id.rfind("factory:", 0) == 0;
    const bool user = id.rfind("user:", 0) == 0;
    if (!factory && !user)
        return {};
    const std::string rel = id.substr(factory ? 8 : 5) + ".zpreset";
    const fs::path dir = presetsDir(factory);
    const auto r = sandboxPath(rel, dir, {dir}, {".zpreset"});
    return r.ok ? r.path : fs::path();
}

std::string Bridge::presetId(const fs::path& file) const {
    for (const bool factory : {false, true}) {
        const fs::path dir = presetsDir(factory);
        std::error_code ec;
        const fs::path canonicalDir = fs::weakly_canonical(dir, ec);
        if (ec || !insideRoot(fs::weakly_canonical(file, ec), canonicalDir))
            continue;
        fs::path rel = fs::weakly_canonical(file, ec).lexically_relative(canonicalDir);
        rel.replace_extension();
        std::string s = utf8(rel);
        std::replace(s.begin(), s.end(), '\\', '/');
        return (factory ? "factory:" : "user:") + s;
    }
    return utf8(file);
}

// --- Patch helpers --------------------------------------------------------------------------------------

J patchJson(const Patch& patch, bool compact) {
    zed::SaveOptions o;
    o.compact = compact;
    return J::parse(zed::savePatch(patch, o), nullptr, false);
}

void issuesToProblems(const std::vector<zed::Issue>& issues, std::vector<Problem>& errors, J& warnings) {
    if (!warnings.is_array())
        warnings = J::array();
    for (const auto& i : issues) {
        if (i.severity == zed::Severity::Error)
            errors.push_back({i.path, i.message, i.fix});
        else
            warnings.push_back({{"path", i.path}, {"code", i.code}, {"problem", i.message}, {"fix", i.fix}});
    }
}

namespace {
J shortValue(const J& v) {
    if (v.is_string() && v.get<std::string>().size() > 160)
        return "<" + std::to_string(v.get<std::string>().size()) + " characters of data>";
    const std::string d = v.dump();
    if ((v.is_object() || v.is_array()) && d.size() > 400)
        return "<" + std::string(v.is_object() ? "object" : "list") + ", " + std::to_string(d.size()) + " characters>";
    return v;
}
} // namespace

J patchDiff(const Patch& a, const Patch& b, std::size_t limit) {
    const J ja = patchJson(a), jb = patchJson(b);
    const J ops = J::diff(ja, jb);
    J out = J::array();
    for (const auto& op : ops) {
        if (out.size() >= limit) {
            out.push_back({{"path", ""}, {"from", nullptr}, {"to", "... " + std::to_string(ops.size() - limit) + " more changes"}});
            break;
        }
        const std::string path = op.value("path", "");
        const std::string kind = op.value("op", "");
        J from = nullptr, to = nullptr;
        try {
            if (kind != "add")
                from = ja.at(J::json_pointer(path));
        } catch (...) {
        }
        if (kind != "remove")
            to = op.value("value", J());
        out.push_back({{"path", path}, {"from", shortValue(from)}, {"to", shortValue(to)}});
    }
    return out;
}

J patchSummary(const Patch& p) {
    const auto& reg = zed::Registry::get();
    const auto val = [&](const std::string& id) -> std::string {
        const auto* def = reg.find(id);
        return def ? def->format(p.get(id)) : std::string();
    };
    J sources = J::array();
    for (int i = 1; i <= zed::kNumOscs; ++i) {
        const std::string m = "osc" + std::to_string(i);
        if (p.get(m + ".on") < 0.5)
            continue;
        J s = {{"module", m}, {"engine", val(m + ".engine")}, {"level", val(m + ".level")}, {"position", val(m + ".position")},
               {"unison", val(m + ".unison.voices")}};
        const auto idx = static_cast<std::size_t>(i - 1);
        s["table"] = p.tableData[idx] ? "(edited table kept in the patch)" : (p.tables[idx].empty() ? "(built-in)" : p.tables[idx]);
        sources.push_back(std::move(s));
    }
    if (p.get("sub.on") > 0.5)
        sources.push_back({{"module", "sub"}});
    if (p.get("texture.on") > 0.5)
        sources.push_back({{"module", "texture"}, {"sample", p.textureSample.empty() ? "(built-in noise)" : p.textureSample}});
    J filters = J::array();
    for (int i = 1; i <= zed::kNumFilters; ++i) {
        const std::string m = "filter" + std::to_string(i);
        if (p.get(m + ".on") > 0.5)
            filters.push_back({{"module", m}, {"type", val(m + ".type")}, {"cutoff", val(m + ".cutoff")}, {"resonance", val(m + ".resonance")}});
    }
    J routes = J::array();
    for (std::size_t i = 0; i < p.routes.size(); ++i) {
        const auto& r = p.routes[i];
        routes.push_back(r.source + (r.via.empty() ? "" : " (via " + r.via + ")") + " -> " + r.dest + " " +
                         reg.at(reg.modAmount(static_cast<int>(i))).format(p.get(reg.modAmount(static_cast<int>(i)))) +
                         (r.enabled && !r.mute ? "" : " (off)"));
    }
    J fx = J::object();
    for (int lane = 0; lane < zed::kNumFxLanes; ++lane) {
        J list = J::array();
        for (int s = 0; s < p.fxSlotCount(lane); ++s) {
            const int t = p.fxSlotType(lane, s);
            if (t >= 0)
                list.push_back(reg.effectTypes()[static_cast<std::size_t>(t)].id);
        }
        fx[std::string(zed::fxLaneKey(lane))] = list;
    }
    J macros = J::array();
    for (int m = 0; m < zed::kNumMacros; ++m)
        if (!p.macroNames[static_cast<std::size_t>(m)].empty())
            macros.push_back({{"macro", m + 1}, {"name", p.macroNames[static_cast<std::size_t>(m)]}, {"value", val("macro" + std::to_string(m + 1) + ".value")}});
    int clips = 0;
    for (const auto& c : p.clips)
        clips += c.empty() ? 0 : 1;
    return {{"name", p.meta.name},
            {"type", p.meta.type},
            {"tags", p.meta.tags},
            {"voice", {{"mode", val("global.mode")}, {"polyphony", val("global.polyphony")}, {"master", val("global.master")}}},
            {"sources", sources},
            {"filters", filters},
            {"amp_envelope", {{"attack", val("env1.attack")}, {"decay", val("env1.decay")}, {"sustain", val("env1.sustain")}, {"release", val("env1.release")}}},
            {"routes", routes},
            {"fx", fx},
            {"macros", macros},
            {"clips", clips}};
}

std::string clipToText(const zed::Clip& c) {
    return c.name + ": " + std::to_string(c.notes.size()) + " notes, " + std::to_string(c.bars) + " bars of " + std::to_string(c.beatsPerBar) +
           " beats at " + std::to_string(c.tempo) + " bpm" + (c.key.empty() ? "" : ", " + c.key);
}

} // namespace zmcp
