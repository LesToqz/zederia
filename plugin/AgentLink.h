// Agent Link for one instance (plan 4.2, 4.8): when switched on, the instance opens a local pipe
// only the current user can open, writes a user-only discovery file with a random token, and
// answers zederia-mcp's requests. The link thread only checks the token and queues each request;
// the message thread carries it out through the editing session, so every agent edit is one undo
// step labelled with the client's name and reaches the audio thread the way the interface's do
// (the parameter store and hand-overs, applied at the start of a block). Renders happen in the
// server on a copy of the patch, never here.
#pragma once

#include "zederia/link/Link.h"
#include "zederia/patch/Patch.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class ZederiaProcessor;

class AgentLink {
public:
    explicit AgentLink(ZederiaProcessor& processor);
    ~AgentLink();

    // Message thread.
    bool setEnabled(bool on, std::string& error);
    bool enabled() const { return server_.running(); }
    const zed::link::Discovery& discovery() const { return discovery_; }

    // Carries out queued requests (message thread; the processor's timer and an async call run it).
    void service();

    struct Client {
        std::string name;
        juce::Time firstSeen, lastSeen;
        int requests = 0, edits = 0;
    };
    struct Entry {
        juce::Time time;
        std::string client, action;
    };
    std::vector<Client> clients() const;   // seen since the link was switched on
    std::vector<Entry> activity() const;   // newest last, at most 200
    bool canRevert() const { return preAgent_ != nullptr; }
    // Restores the sound from before the first agent edit since the link was switched on (or the last revert).
    bool revertAgentSession();
    int generation() const { return generation_.load(); } // changes whenever clients or activity change

    // For tests: the request handler as the link thread runs it.
    std::string handleLine(const std::string& line);

private:
    struct Job {
        std::string requestText;
        std::string reply;
        bool done = false, abandoned = false;
    };
    std::string run(const nlohmann::json& request);
    void note(const std::string& client, const std::string& action, bool edit);
    void touched();

    ZederiaProcessor& p_;
    zed::link::PipeServer server_;
    zed::link::Discovery discovery_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    std::mutex jobsMutex_;
    std::condition_variable jobDone_;
    std::deque<std::shared_ptr<Job>> jobs_;

    mutable std::mutex infoMutex_;
    std::vector<Client> clients_;
    std::deque<Entry> activity_;
    std::unique_ptr<zed::Patch> preAgent_;
    std::atomic<int> generation_{0};
};
