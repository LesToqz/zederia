// The live link (plan 4.2, 4.8): how zederia-mcp reaches Zederia instances running in a host.
// Each instance with Agent Link enabled opens a local pipe that only the current user can open,
// writes a discovery file (user-only) naming the pipe and a random token, and answers one JSON
// request per line. The server lists the discovery files as live targets.
//
// Shared by the plug-in and the server. No plug-in framework and no engine code here.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace zed::link {

inline constexpr int kLinkProtocol = 1;
inline constexpr std::size_t kMaxLineBytes = 64u * 1024u * 1024u;

// --- Folders (plan 5.6) ---------------------------------------------------------------------------
// %LOCALAPPDATA%\Zederia (cache, logs, link discovery) and %APPDATA%\Zederia (settings), as UTF-8.
// ZEDERIA_LOCAL_DIR and ZEDERIA_SETTINGS_DIR override them (tests).
std::string localDir();
std::string settingsDir();
std::string discoveryDir(); // localDir()/link

// Random bytes from the operating system as lowercase hex.
std::string randomHex(int bytes);

// --- Discovery files --------------------------------------------------------------------------------
struct Discovery {
    std::string instanceId; // random, 16 hex digits
    std::string pipe;       // full pipe path
    std::string token;      // must accompany every request
    std::string name;       // what the instance calls itself ("Zederia 1 - Init")
    std::string host;       // the host program
    std::string version;
    int pid = 0;
    std::int64_t started = 0; // seconds since 1970
};
nlohmann::json toJson(const Discovery& d);
bool fromJson(const nlohmann::json& j, Discovery& d);
// Writes discoveryDir()/<instanceId>.json readable and writable by the current user only.
bool writeDiscovery(const Discovery& d, std::string& error);
void removeDiscovery(const std::string& instanceId);
// Every discovery file whose process still runs; files of processes that ended are removed.
std::vector<Discovery> listDiscoveries();
bool processAlive(int pid);

// --- The pipe -----------------------------------------------------------------------------------------
// A pipe name for a new instance: \\.\pipe\zederia-link-<random>.
std::string newPipeName();

// The instance's side. The link thread accepts one connection at a time and answers each line
// with the handler's line. The handler runs on the link thread; it must not touch the engine.
class PipeServer {
public:
    using Handler = std::function<std::string(const std::string& line)>;
    PipeServer();
    ~PipeServer();
    PipeServer(const PipeServer&) = delete;
    PipeServer& operator=(const PipeServer&) = delete;
    bool start(const std::string& pipeName, Handler handler, std::string& error);
    void stop(); // joins the link thread
    bool running() const { return running_.load(); }
    int connections() const { return connections_.load(); } // accepted since start

private:
    void run();
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    Handler handler_;
    std::atomic<bool> running_{false};
    std::atomic<int> connections_{0};
};

// The server's side: one connection, requests answered in order.
class PipeClient {
public:
    PipeClient();
    ~PipeClient();
    PipeClient(const PipeClient&) = delete;
    PipeClient& operator=(const PipeClient&) = delete;
    bool connect(const std::string& pipeName, int timeoutMs, std::string& error);
    bool request(const std::string& line, std::string& response, int timeoutMs, std::string& error);
    void close();
    bool connected() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// --- Agent permissions (plan 4.8), shared by the AGENT page and the server ----------------------------
// Kept in settingsDir()/agent.json; the plug-in writes it, the server reads it on every check.
struct Permissions {
    bool readPatch = true;
    bool editPatch = true;
    bool writeZederia = true;    // inside Zederia's own folders
    bool writeElsewhere = false; // only inside writeFolders
    bool audition = true;        // through the user's speakers, level-capped
    double auditionCapDb = -12.0;
    bool readSamples = true;     // only inside readFolders (and Zederia's folders)
    std::vector<std::string> writeFolders;
    std::vector<std::string> readFolders;
    bool operator==(const Permissions&) const = default;
};
nlohmann::json toJson(const Permissions& p);
Permissions permissionsFromJson(const nlohmann::json& j);
std::string permissionsPath();
Permissions loadPermissions();
bool savePermissions(const Permissions& p, std::string& error);

// Writes a file that only the current user can read or write (replacing it whole).
bool writePrivateFile(const std::string& path, const std::string& text, std::string& error);

// --- Text helpers ------------------------------------------------------------------------------------
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& wide);

} // namespace zed::link
