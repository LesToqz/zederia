// The Model Context Protocol layer (plan 4.3), for both generations in use:
//   - the stateless generation, revision 2026-07-28: every request carries its protocol version
//     and client capabilities in _meta; server/discover; resultType on every result; cache hints
//     on list results; no protocol sessions;
//   - the handshake generation, revisions 2025-11-25 and 2025-06-18: initialize first, then
//     requests in the negotiated revision for the life of the channel (a stdio process or an HTTP
//     session).
// The transports (Stdio.cpp, Http.cpp) feed messages in and carry answers and notifications out.
#pragma once

#include "Tools.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace zmcp {

inline constexpr const char* kModernVersion = "2026-07-28";
const std::vector<std::string>& legacyVersions(); // newest first
const std::vector<std::string>& supportedVersions(); // modern first

// JSON-RPC and MCP error codes.
inline constexpr int kParseError = -32700, kInvalidRequest = -32600, kMethodNotFound = -32601, kInvalidParams = -32602,
                     kInternalError = -32603, kHeaderMismatch = -32020, kMissingCapability = -32021, kUnsupportedVersion = -32022;

// The state of one handshake-generation channel.
struct LegacyChannel {
    std::mutex mutex;
    bool initialized = false;
    std::string version;
    std::string clientName;
};

// What a request carries for its handler.
struct RequestInfo {
    bool modern = false;
    std::string version;
    std::string clientName = "agent";
    J progressToken; // null when the client did not ask for progress
    std::function<void(const J& notification)> notify = [](const J&) {};
    std::function<bool()> cancelled = [] { return false; };
};

class Server {
public:
    explicit Server(Bridge& bridge);

    // Handles one JSON-RPC message from a client. `legacy` is the channel's handshake state (null
    // for an HTTP request without a session). Returns the response, or null for a notification or
    // a cancelled request. `httpStatus` receives the status an HTTP transport should use.
    J handle(const J& message, LegacyChannel* legacy, RequestInfo& info, int* httpStatus = nullptr);

    // Classifies a request before handling it (the HTTP transport needs this for its headers).
    enum class Era { Modern, Legacy, Unknown };
    static Era eraOf(const J& message);

    const std::vector<ToolDef>& tools() const { return tools_; }
    const ToolDef* findTool(const std::string& name) const;
    J toolList() const;
    J serverInfo() const;
    std::string instructions() const;
    Bridge& bridge() { return bridge_; }

    static J error(const J& id, int code, const std::string& message, J data = nullptr);

    // Runs a tool as tools/call would (for tests and the self-check): validation, handler, output check.
    J callTool(const std::string& name, const J& arguments, RequestInfo& info);
    // Problems with the structured results the server produced (should stay empty).
    std::vector<std::string> outputSchemaFailures() const;

private:
    J dispatch(const std::string& method, const J& params, RequestInfo& info, const J& id, int& status);
    J resourcesList() const;
    J resourceTemplates() const;
    J readResource(const std::string& uri, const J& id, int& status);
    J promptsList() const;
    J getPrompt(const J& params, const J& id, int& status);

    Bridge& bridge_;
    std::vector<ToolDef> tools_;
    mutable std::mutex failuresMutex_;
    std::vector<std::string> outputFailures_;
};

// The agent guide (docs/AGENT_GUIDE.md), compiled into the server.
const std::string& agentGuide();

} // namespace zmcp
