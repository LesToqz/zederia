#include "AgentLink.h"

#include "PluginProcessor.h"
#include "Session.h"

#include "zederia/Version.h"
#include "zederia/link/JsonLimits.h"
#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

using J = nlohmann::json;

namespace {

bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty())
        return false;
    unsigned char d = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

J failure(const std::string& message, const std::string& fix, const std::string& path = "/target") {
    return {{"ok", false}, {"error", {{"message", message}, {"fix", fix}, {"path", path}}}};
}

std::string hashOf(const std::string& text) {
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(zed::fnv1a(text.data(), text.size())));
    return buf;
}

// A string field, or "" when it is missing or not a string (never throws).
std::string textOf(const J& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// One line of text for the AGENT page: control characters become spaces, and a long text is cut at
// a character boundary (the JSON parser has already checked that it is UTF-8).
std::string plainText(const std::string& s, std::size_t maxBytes) {
    std::string out = s;
    for (auto& c : out)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
            c = ' ';
    if (out.size() > maxBytes) {
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
            --cut;
        out.resize(cut);
        out += "...";
    }
    return out;
}

} // namespace

AgentLink::AgentLink(ZederiaProcessor& processor) : p_(processor) {}

AgentLink::~AgentLink() {
    alive_->store(false);
    server_.stop();
    if (!discovery_.instanceId.empty())
        zed::link::removeDiscovery(discovery_.instanceId);
    std::lock_guard<std::mutex> lock(jobsMutex_);
    for (auto& j : jobs_)
        j->abandoned = true;
    jobDone_.notify_all();
}

bool AgentLink::setEnabled(bool on, std::string& error) {
    if (on == enabled())
        return true;
    if (!on) {
        server_.stop();
        zed::link::removeDiscovery(discovery_.instanceId);
        {
            std::lock_guard<std::mutex> lock(infoMutex_);
            activity_.push_back({juce::Time::getCurrentTime(), "", "Agent Link switched off"});
            clients_.clear();
            preAgent_.reset();
        }
        touched();
        return true;
    }
    zed::link::Discovery d;
    d.instanceId = zed::link::randomHex(8);
    d.pipe = zed::link::newPipeName();
    if (!zed::link::secureRandomHex(24, d.token)) {
        error = "this computer gave no secure random numbers for the link's token";
        return false;
    }
    d.version = zed::kVersion;
    d.pid = zed::link::currentProcessId();
    d.started = static_cast<std::int64_t>(std::time(nullptr));
    d.host = juce::PluginHostType().getHostDescription();
    if (juce::PluginHostType::getPluginLoadedAs() == juce::AudioProcessor::wrapperType_Standalone)
        d.host = "Standalone";
    d.name = "Zederia in " + d.host;
    // Set before the pipe opens: its thread reads the token from the first request on, and the
    // pipe thread is stopped (joined) whenever this changes.
    discovery_ = d;
    if (!server_.start(d.pipe, [this](const std::string& line) { return handleLine(line); }, error)) {
        discovery_ = {};
        return false;
    }
    if (!zed::link::writeDiscovery(d, error)) {
        server_.stop();
        discovery_ = {};
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        activity_.push_back({juce::Time::getCurrentTime(), "", "Agent Link switched on"});
    }
    touched();
    return true;
}

void AgentLink::touched() { generation_.fetch_add(1); }

std::string AgentLink::handleLine(const std::string& line) {
    if (!zed::jsonNestingAllowed(line)) // refused before parsing: nesting that deep could exhaust the stack
        return failure("the request is nested too deeply", "send a request of the link protocol").dump();
    const J request = J::parse(line, nullptr, false);
    if (!request.is_object())
        return failure("the request is not JSON", "update zederia-mcp to this instance's version").dump();
    // Checked by type: a field of the wrong type is refused like a wrong value, never thrown (this
    // runs on the pipe thread, where an exception would end the host).
    const auto v = request.find("v");
    if (v == request.end() || !v->is_number_integer() || v->get<long long>() != zed::link::kLinkProtocol)
        return failure("the link protocol versions differ", "use zederia-mcp from the same Zederia version").dump();
    const auto token = request.find("token");
    if (token == request.end() || !token->is_string() || !sameToken(token->get<std::string>(), discovery_.token))
        return failure("the token is wrong", "read the instance's current discovery file (list_targets)").dump();
    auto job = std::make_shared<Job>();
    job->requestText = line;
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        jobs_.push_back(job);
    }
    // Ask the message thread to run it now; the processor's timer runs it too, should this be lost.
    auto alive = alive_;
    AgentLink* self = this;
    juce::MessageManager::callAsync([alive, self] {
        if (alive->load())
            self->service();
    });
    std::unique_lock<std::mutex> lock(jobsMutex_);
    if (!jobDone_.wait_for(lock, std::chrono::seconds(8), [&] { return job->done || job->abandoned; })) {
        job->abandoned = true;
        return failure("the instance is busy and did not answer in time", "try again in a moment").dump();
    }
    if (!job->done)
        return failure("the instance is closing", "list_targets to find another one").dump();
    return job->reply;
}

void AgentLink::service() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            if (jobs_.empty())
                return;
            job = jobs_.front();
            jobs_.pop_front();
            if (job->abandoned)
                continue;
        }
        std::string reply;
        try {
            const J request = J::parse(job->requestText);
            J r = J::parse(run(request));
            r["id"] = request.value("id", J());
            reply = r.dump();
        } catch (const std::exception& e) {
            reply = failure(std::string("the instance could not do that: ") + e.what(), "report this problem").dump();
        }
        std::lock_guard<std::mutex> lock(jobsMutex_);
        job->reply = std::move(reply);
        job->done = true;
        jobDone_.notify_all();
    }
}

AgentLink::Client& AgentLink::seen(const std::string& client, juce::Time when) {
    auto it = std::find_if(clients_.begin(), clients_.end(), [&](const Client& c) { return c.name == client; });
    if (it == clients_.end()) {
        clients_.push_back({client, when, when});
        activity_.push_back({when, client, "connected"});
        return clients_.back();
    }
    if (when > it->lastSeen)
        it->lastSeen = when;
    return *it;
}

void AgentLink::note(const std::string& client, const std::string& action, bool edit) {
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        const auto now = juce::Time::getCurrentTime();
        auto& c = seen(client, now);
        ++c.requests;
        if (edit)
            ++c.edits;
        if (!action.empty())
            activity_.push_back({now, client, action});
        while (activity_.size() > 200)
            activity_.pop_front();
    }
    touched();
}

void AgentLink::noteElsewhere(const std::string& client, const std::string& action, juce::Time when) {
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        ++seen(client, when).reported;
        activity_.push_back({when, client, action});
        while (activity_.size() > 200)
            activity_.pop_front();
    }
    touched();
}

std::vector<AgentLink::Client> AgentLink::clients() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return clients_;
}

std::vector<AgentLink::Entry> AgentLink::activity() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return {activity_.begin(), activity_.end()};
}

bool AgentLink::revertAgentSession() {
    std::unique_ptr<zed::Patch> state;
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        state = std::move(preAgent_);
    }
    if (!state)
        return false;
    p_.session().applyAgentState("Revert agent session", "", *state, false);
    {
        std::lock_guard<std::mutex> lock(infoMutex_);
        activity_.push_back({juce::Time::getCurrentTime(), "", "Reverted the agent session"});
    }
    touched();
    return true;
}

std::string AgentLink::run(const J& request) {
    const std::string method = request.value("method", "");
    std::string client = request.value("client", "agent");
    if (client.empty())
        client = "agent";
    const J params = request.value("params", J::object());
    const auto perms = zed::link::loadPermissions();
    auto& session = p_.session();
    const auto ok = [](J result) { return J{{"ok", true}, {"result", std::move(result)}}.dump(); };
    const auto historyJson = [&] {
        const auto& h = session.history();
        J steps = J::array();
        for (int i = std::max(0, h.size() - 50); i < h.size(); ++i)
            steps.push_back({{"step", i}, {"label", h.label(i)}, {"origin", h.origin(i)}, {"current", i == h.current()}});
        return J{{"current", h.current()}, {"can_undo", h.canUndo()}, {"can_redo", h.canRedo()}, {"undo", h.undoLabel()},
                 {"redo", h.redoLabel()}, {"steps", steps}, {"revertible", canRevert()}};
    };

    if (method == "status") {
        note(client, "", false);
        return ok({{"name", discovery_.name},
                   {"patch", p_.meta().name},
                   {"bpm", p_.hostBpm()},
                   {"playing", p_.hostPlaying()},
                   {"cpu_percent", p_.cpuAverage()},
                   {"voices", p_.activeVoices()},
                   {"sample_rate", p_.currentSampleRate()},
                   {"undo_steps", session.history().current()},
                   {"revertible", canRevert()}});
    }
    if (method == "get_patch") {
        if (!perms.readPatch)
            return failure("reading the patch is switched off", "the user can allow it on the AGENT page").dump();
        note(client, "", false);
        const std::string text = zed::savePatch(p_.snapshot());
        return ok({{"patch", text}, {"hash", hashOf(text)}});
    }
    if (method == "put_patch") {
        if (!perms.editPatch)
            return failure("editing the patch is switched off", "the user can allow it on the AGENT page").dump();
        const zed::Patch current = p_.snapshot();
        const std::string expect = params.value("expect_hash", "");
        if (!expect.empty() && expect != hashOf(zed::savePatch(current)))
            return failure("the instance's patch changed meanwhile", "read it again and repeat the edit").dump();
        auto loaded = zed::loadPatch(params.value("patch", std::string()));
        if (!loaded.ok())
            return failure("the patch has errors", "validate it with apply_patch dry_run first", "/patch").dump();
        {
            std::lock_guard<std::mutex> lock(infoMutex_);
            if (!preAgent_)
                preAgent_ = std::make_unique<zed::Patch>(current);
        }
        const std::string label = params.value("label", client + ": edit");
        const bool changed = session.applyAgentState(label, client, loaded.patch, params.value("preset", false));
        note(client, label, true);
        return ok({{"applied", changed}, {"hash", hashOf(zed::savePatch(p_.snapshot()))}});
    }
    if (method == "history") {
        const std::string action = params.value("action", "list");
        if (action != "list" && !perms.editPatch)
            return failure("editing the patch is switched off", "the user can allow it on the AGENT page").dump();
        bool done = true;
        if (action == "undo")
            done = session.undo();
        else if (action == "redo")
            done = session.redo();
        else if (action == "jump")
            done = session.jumpTo(params.value("step", -1));
        else if (action == "revert_agent_session")
            done = revertAgentSession();
        else if (action != "list")
            return failure("unknown history action", "use list, undo, redo, jump or revert_agent_session", "/action").dump();
        if (!done)
            return failure(action == "revert_agent_session" ? "there is no agent session to revert" : "there is no step to go to",
                           "call history with action list", "/action").dump();
        note(client, action == "list" ? "" : client + ": " + action, action != "list");
        return ok(historyJson());
    }
    if (method == "audition") {
        if (!perms.audition)
            return failure("auditioning is switched off", "the user can allow it on the AGENT page").dump();
        std::vector<zed::NoteSpec> notes;
        for (const auto& n : params.value("notes", J::array()))
            notes.push_back({std::clamp(n.value("note", 60), 0, 127), std::clamp(n.value("start", 0.0), 0.0, 30.0),
                             std::clamp(n.value("duration", 0.5), 0.01, 30.0), std::clamp(n.value("velocity", 0.8), 0.0, 1.0)});
        if (notes.empty() || notes.size() > 512)
            return failure("an audition needs 1 to 512 notes", "send fewer notes", "/notes").dump();
        // A client may ask for a lower cap, never a higher one than the user set on the AGENT page.
        const double cap = std::min(perms.auditionCapDb, params.value("cap_db", perms.auditionCapDb));
        p_.audition(notes, static_cast<float>(zed::dbToGain(std::clamp(cap, -60.0, 0.0))));
        note(client, client + ": audition (" + std::to_string(notes.size()) + " notes)", false);
        return ok({{"played", notes.size()}});
    }
    if (method == "note") {
        // What agents did outside this instance (work in headless sessions, files they saved), reported
        // by zederia-mcp so this page lists every agent action on the computer. It only adds lines.
        const auto entries = params.is_object() ? params.find("entries") : params.end();
        if (!params.is_object() || entries == params.end() || !entries->is_array() || entries->empty() || entries->size() > 200)
            return failure("a note needs 1 to 200 entries", "use zederia-mcp from the same Zederia version", "/entries").dump();
        const std::string self = "live:" + discovery_.instanceId;
        int noted = 0;
        for (const auto& e : *entries) {
            if (!e.is_object())
                continue;
            std::string who = plainText(textOf(e, "client"), 64);
            if (who.empty())
                who = "agent";
            const std::string what = plainText(textOf(e, "action"), 240);
            if (what.empty())
                continue;
            const std::string where = textOf(e, "target");
            std::string line = who + ": " + what;
            if (where.rfind("session:", 0) == 0)
                line += " (headless " + plainText(where, 40) + ")";
            else if (where.rfind("live:", 0) == 0 && where != self)
                line += " (another instance)";
            auto when = juce::Time::getCurrentTime();
            const auto at = e.find("at");
            if (at != e.end() && at->is_number_integer()) {
                const juce::Time t(at->get<juce::int64>());
                if (std::abs((t - when).inSeconds()) < 120.0) // the server's clock, when it is plausible
                    when = t;
            }
            noteElsewhere(who, line, when);
            ++noted;
        }
        return ok({{"noted", noted}});
    }
    return failure("unknown request \"" + method + "\"", "use zederia-mcp from the same Zederia version").dump();
}
