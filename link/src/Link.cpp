#include "zederia/link/Link.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")
#else
#include <random>
#include <signal.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using J = nlohmann::json;

namespace zed::link {

// --- Text -------------------------------------------------------------------------------------------

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}
std::string narrow(const std::wstring& w) {
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
#else
std::wstring widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string narrow(const std::wstring& w) { return std::string(w.begin(), w.end()); }
#endif

namespace {

fs::path pathOf(const std::string& utf8) { return fs::path(widen(utf8)); }
std::string utf8Of(const fs::path& p) {
#ifdef _WIN32
    return narrow(p.wstring());
#else
    return p.string();
#endif
}

std::string envUtf8(const char* name) {
#ifdef _WIN32
    const std::wstring wname = widen(name);
    wchar_t* value = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&value, &len, wname.c_str()) != 0 || value == nullptr)
        return {};
    std::string out = narrow(value);
    std::free(value);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

#ifdef _WIN32
// "D:P(A;;GA;;;<sid of the current user>)": a protected list that admits only the current user.
std::wstring userOnlySddl() {
    static const std::wstring sddl = [] {
        std::wstring result;
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return result;
        DWORD size = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buf(size);
        if (size > 0 && GetTokenInformation(token, TokenUser, buf.data(), size, &size)) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid, &sid)) {
                result = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
                LocalFree(sid);
            }
        }
        CloseHandle(token);
        return result;
    }();
    return sddl;
}

struct UserOnlySecurity {
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR sd = nullptr;
    bool ok = false;
    UserOnlySecurity() {
        sa.nLength = sizeof sa;
        sa.bInheritHandle = FALSE;
        const std::wstring sddl = userOnlySddl();
        if (!sddl.empty() && ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
            sa.lpSecurityDescriptor = sd;
            ok = true;
        }
    }
    ~UserOnlySecurity() {
        if (sd)
            LocalFree(sd);
    }
};
#endif

} // namespace

bool writePrivateFile(const std::string& path, const std::string& text, std::string& error) {
    std::error_code ec;
    fs::create_directories(pathOf(path).parent_path(), ec);
    const std::string temp = path + ".tmp" + randomHex(4);
#ifdef _WIN32
    UserOnlySecurity security;
    if (!security.ok) {
        error = "could not build a user-only security descriptor";
        return false;
    }
    HANDLE h = CreateFileW(widen(temp).c_str(), GENERIC_WRITE, 0, &security.sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = "cannot create " + temp;
        return false;
    }
    DWORD written = 0;
    const BOOL wrote = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(h);
    if (!wrote || written != text.size()) {
        DeleteFileW(widen(temp).c_str());
        error = "cannot write " + temp;
        return false;
    }
    if (!MoveFileExW(widen(temp).c_str(), widen(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(widen(temp).c_str());
        error = "cannot replace " + path;
        return false;
    }
    return true;
#else
    {
        std::ofstream f(temp, std::ios::binary);
        f << text;
        if (!f) {
            error = "cannot write " + temp;
            return false;
        }
    }
    fs::permissions(temp, fs::perms::owner_read | fs::perms::owner_write, ec);
    fs::rename(temp, path, ec);
    if (ec) {
        error = "cannot replace " + path;
        return false;
    }
    return true;
#endif
}

namespace {

bool readText(const fs::path& p, std::string& out, std::size_t limit = 1u << 20) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return out.size() <= limit;
}

} // namespace

// --- Folders ----------------------------------------------------------------------------------------

std::string localDir() {
    if (auto v = envUtf8("ZEDERIA_LOCAL_DIR"); !v.empty())
        return v;
#ifdef _WIN32
    return utf8Of(pathOf(envUtf8("LOCALAPPDATA")) / L"Zederia");
#else
    return envUtf8("HOME") + "/.local/share/Zederia";
#endif
}

std::string settingsDir() {
    if (auto v = envUtf8("ZEDERIA_SETTINGS_DIR"); !v.empty())
        return v;
#ifdef _WIN32
    return utf8Of(pathOf(envUtf8("APPDATA")) / L"Zederia");
#else
    return envUtf8("HOME") + "/.config/Zederia";
#endif
}

std::string discoveryDir() { return utf8Of(pathOf(localDir()) / "link"); }

std::string randomHex(int bytes) {
    std::vector<unsigned char> buf(static_cast<std::size_t>(std::max(1, bytes)));
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        for (auto& b : buf) // never expected; still not predictable across processes
            b = static_cast<unsigned char>(GetTickCount64() ^ GetCurrentProcessId() ^ reinterpret_cast<std::uintptr_t>(&b));
#else
    std::random_device rd;
    for (auto& b : buf)
        b = static_cast<unsigned char>(rd());
#endif
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : buf) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

// --- Discovery --------------------------------------------------------------------------------------

J toJson(const Discovery& d) {
    return {{"format", "zederia.link"}, {"protocol", kLinkProtocol}, {"instance_id", d.instanceId}, {"pipe", d.pipe},
            {"token", d.token},         {"name", d.name},             {"host", d.host},               {"version", d.version},
            {"pid", d.pid},             {"started", d.started}};
}

bool fromJson(const J& j, Discovery& d) {
    if (!j.is_object() || j.value("format", "") != "zederia.link")
        return false;
    d.instanceId = j.value("instance_id", "");
    d.pipe = j.value("pipe", "");
    d.token = j.value("token", "");
    d.name = j.value("name", "");
    d.host = j.value("host", "");
    d.version = j.value("version", "");
    d.pid = j.value("pid", 0);
    d.started = j.value("started", std::int64_t{0});
    return !d.instanceId.empty() && !d.pipe.empty() && !d.token.empty();
}

bool writeDiscovery(const Discovery& d, std::string& error) {
    return writePrivateFile(utf8Of(pathOf(discoveryDir()) / pathOf(d.instanceId + ".json")), toJson(d).dump(2), error);
}

void removeDiscovery(const std::string& instanceId) {
    std::error_code ec;
    fs::remove(pathOf(discoveryDir()) / pathOf(instanceId + ".json"), ec);
}

bool processAlive(int pid) {
    if (pid <= 0)
        return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return GetLastError() == ERROR_ACCESS_DENIED; // exists, owned by someone else
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(pid, 0) == 0;
#endif
}

std::vector<Discovery> listDiscoveries() {
    std::vector<Discovery> out;
    std::error_code ec;
    const fs::path dir = pathOf(discoveryDir());
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".json")
            continue;
        std::string text;
        if (!readText(it->path(), text))
            continue;
        Discovery d;
        const J j = J::parse(text, nullptr, false);
        if (j.is_discarded() || !fromJson(j, d))
            continue;
        if (!processAlive(d.pid)) {
            fs::remove(it->path(), ec);
            ec.clear();
            continue;
        }
        out.push_back(std::move(d));
    }
    std::sort(out.begin(), out.end(), [](const Discovery& a, const Discovery& b) {
        return a.started != b.started ? a.started < b.started : a.instanceId < b.instanceId;
    });
    return out;
}

std::string newPipeName() {
#ifdef _WIN32
    return "\\\\.\\pipe\\zederia-link-" + randomHex(12);
#else
    return "/tmp/zederia-link-" + randomHex(12);
#endif
}

// --- Pipe server ------------------------------------------------------------------------------------

#ifdef _WIN32
struct PipeServer::Impl {
    std::wstring name;
    HANDLE stop = nullptr;
    HANDLE listening = INVALID_HANDLE_VALUE;
    bool first = true;
};

namespace {

HANDLE createInstance(const std::wstring& name, bool first) {
    UserOnlySecurity security;
    if (!security.ok)
        return INVALID_HANDLE_VALUE;
    return CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, PIPE_UNLIMITED_INSTANCES,
                            1 << 16, 1 << 16, 0, &security.sa);
}

// Waits for an overlapped operation; false on stop, timeout or failure.
bool waitIo(HANDLE h, OVERLAPPED& ov, HANDLE stop, DWORD timeoutMs, DWORD& transferred) {
    HANDLE events[2] = {ov.hEvent, stop};
    const DWORD r = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeoutMs);
    if (r != WAIT_OBJECT_0) {
        CancelIoEx(h, &ov);
        GetOverlappedResult(h, &ov, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(h, &ov, &transferred, FALSE) != FALSE;
}

bool writeAll(HANDLE h, const std::string& data, HANDLE stop) {
    std::size_t done = 0;
    while (done < data.size()) {
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 16));
        BOOL ok = WriteFile(h, data.data() + done, chunk, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = waitIo(h, ov, stop, 10000, n);
        CloseHandle(ov.hEvent);
        if (!ok || n == 0)
            return false;
        done += n;
    }
    return true;
}

} // namespace

PipeServer::PipeServer() : impl_(std::make_unique<Impl>()) {}
PipeServer::~PipeServer() { stop(); }

bool PipeServer::start(const std::string& pipeName, Handler handler, std::string& error) {
    stop();
    impl_->name = widen(pipeName);
    impl_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->listening = createInstance(impl_->name, true);
    if (impl_->listening == INVALID_HANDLE_VALUE) {
        error = "cannot open the link pipe (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(impl_->stop);
        impl_->stop = nullptr;
        return false;
    }
    handler_ = std::move(handler);
    running_ = true;
    thread_ = std::thread([this] { run(); });
    return true;
}

void PipeServer::stop() {
    if (!thread_.joinable())
        return;
    SetEvent(impl_->stop);
    thread_.join();
    if (impl_->listening != INVALID_HANDLE_VALUE)
        CloseHandle(impl_->listening);
    impl_->listening = INVALID_HANDLE_VALUE;
    CloseHandle(impl_->stop);
    impl_->stop = nullptr;
    running_ = false;
}

void PipeServer::run() {
    while (WaitForSingleObject(impl_->stop, 0) != WAIT_OBJECT_0) {
        HANDLE pipe = impl_->listening;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool connected = ConnectNamedPipe(pipe, &ov) != FALSE;
        if (!connected) {
            const DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (e == ERROR_IO_PENDING) {
                DWORD n = 0;
                connected = waitIo(pipe, ov, impl_->stop, INFINITE, n);
            }
        }
        CloseHandle(ov.hEvent);
        if (!connected) {
            if (WaitForSingleObject(impl_->stop, 0) == WAIT_OBJECT_0)
                break;
            DisconnectNamedPipe(pipe);
            continue;
        }
        // Keep an instance listening while this one is served, so the name is never free.
        impl_->listening = createInstance(impl_->name, false);
        ++connections_;
        std::string buffer;
        char chunk[1 << 14];
        bool alive = true;
        while (alive && WaitForSingleObject(impl_->stop, 0) != WAIT_OBJECT_0) {
            OVERLAPPED rov{};
            rov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            DWORD n = 0;
            BOOL ok = ReadFile(pipe, chunk, sizeof chunk, &n, &rov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                ok = waitIo(pipe, rov, impl_->stop, 60000, n);
            CloseHandle(rov.hEvent);
            if (!ok || n == 0)
                break;
            buffer.append(chunk, n);
            if (buffer.size() > kMaxLineBytes)
                break;
            for (std::size_t nl; (nl = buffer.find('\n')) != std::string::npos;) {
                std::string line = buffer.substr(0, nl);
                buffer.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty())
                    continue;
                std::string reply = handler_(line);
                reply.push_back('\n');
                if (!writeAll(pipe, reply, impl_->stop)) {
                    alive = false;
                    break;
                }
            }
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        if (impl_->listening == INVALID_HANDLE_VALUE)
            break;
    }
    running_ = false;
}

// --- Pipe client ------------------------------------------------------------------------------------

struct PipeClient::Impl {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::string pending;
};

PipeClient::PipeClient() : impl_(std::make_unique<Impl>()) {}
PipeClient::~PipeClient() { close(); }

bool PipeClient::connected() const { return impl_->h != INVALID_HANDLE_VALUE; }

void PipeClient::close() {
    if (impl_->h != INVALID_HANDLE_VALUE)
        CloseHandle(impl_->h);
    impl_->h = INVALID_HANDLE_VALUE;
    impl_->pending.clear();
}

bool PipeClient::connect(const std::string& pipeName, int timeoutMs, std::string& error) {
    close();
    const std::wstring name = widen(pipeName);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            impl_->h = h;
            return true;
        }
        const DWORD e = GetLastError();
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0 || (e != ERROR_PIPE_BUSY && e != ERROR_FILE_NOT_FOUND)) {
            error = e == ERROR_FILE_NOT_FOUND ? "the instance is not listening (Agent Link off or the host closed)"
                    : e == ERROR_ACCESS_DENIED ? "access to the instance's pipe was denied"
                                               : "cannot connect to the instance (error " + std::to_string(e) + ")";
            return false;
        }
        if (e == ERROR_PIPE_BUSY)
            WaitNamedPipeW(name.c_str(), static_cast<DWORD>(std::min<long long>(left, 1000)));
        else
            Sleep(20);
    }
}

bool PipeClient::request(const std::string& line, std::string& response, int timeoutMs, std::string& error) {
    if (impl_->h == INVALID_HANDLE_VALUE) {
        error = "not connected";
        return false;
    }
    if (!writeAll(impl_->h, line + "\n", nullptr)) {
        error = "the instance closed the link";
        close();
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    char chunk[1 << 14];
    for (;;) {
        if (const std::size_t nl = impl_->pending.find('\n'); nl != std::string::npos) {
            response = impl_->pending.substr(0, nl);
            impl_->pending.erase(0, nl + 1);
            return true;
        }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            error = "the instance did not answer in time";
            close();
            return false;
        }
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD n = 0;
        BOOL ok = ReadFile(impl_->h, chunk, sizeof chunk, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = waitIo(impl_->h, ov, nullptr, static_cast<DWORD>(left), n);
        CloseHandle(ov.hEvent);
        if (!ok || n == 0) {
            error = "the instance closed the link or did not answer";
            close();
            return false;
        }
        impl_->pending.append(chunk, n);
        if (impl_->pending.size() > kMaxLineBytes) {
            error = "the instance's answer is too large";
            close();
            return false;
        }
    }
}

#else // not Windows: the live link is not available in this version.

struct PipeServer::Impl {};
PipeServer::PipeServer() : impl_(std::make_unique<Impl>()) {}
PipeServer::~PipeServer() = default;
bool PipeServer::start(const std::string&, Handler, std::string& error) {
    error = "the live link is available on Windows only in this version";
    return false;
}
void PipeServer::stop() {}
void PipeServer::run() {}
struct PipeClient::Impl {};
PipeClient::PipeClient() : impl_(std::make_unique<Impl>()) {}
PipeClient::~PipeClient() = default;
bool PipeClient::connect(const std::string&, int, std::string& error) {
    error = "the live link is available on Windows only in this version";
    return false;
}
bool PipeClient::request(const std::string&, std::string&, int, std::string& error) {
    error = "not connected";
    return false;
}
void PipeClient::close() {}
bool PipeClient::connected() const { return false; }
#endif

// --- Permissions ------------------------------------------------------------------------------------

J toJson(const Permissions& p) {
    return {{"format", "zederia.agent"},
            {"read_patch", p.readPatch},
            {"edit_patch", p.editPatch},
            {"write_zederia", p.writeZederia},
            {"write_elsewhere", p.writeElsewhere},
            {"audition", p.audition},
            {"audition_cap_db", p.auditionCapDb},
            {"read_samples", p.readSamples},
            {"write_folders", p.writeFolders},
            {"read_folders", p.readFolders}};
}

Permissions permissionsFromJson(const J& j) {
    Permissions p;
    if (!j.is_object())
        return p;
    const auto flag = [&](const char* key, bool& v) {
        if (auto it = j.find(key); it != j.end() && it->is_boolean())
            v = it->get<bool>();
    };
    flag("read_patch", p.readPatch);
    flag("edit_patch", p.editPatch);
    flag("write_zederia", p.writeZederia);
    flag("write_elsewhere", p.writeElsewhere);
    flag("audition", p.audition);
    flag("read_samples", p.readSamples);
    if (auto it = j.find("audition_cap_db"); it != j.end() && it->is_number())
        p.auditionCapDb = std::clamp(it->get<double>(), -60.0, 0.0);
    const auto list = [&](const char* key, std::vector<std::string>& v) {
        if (auto it = j.find(key); it != j.end() && it->is_array())
            for (const auto& e : *it)
                if (e.is_string() && !e.get<std::string>().empty())
                    v.push_back(e.get<std::string>());
    };
    list("write_folders", p.writeFolders);
    list("read_folders", p.readFolders);
    return p;
}

std::string permissionsPath() { return utf8Of(pathOf(settingsDir()) / "agent.json"); }

Permissions loadPermissions() {
    std::string text;
    if (!readText(pathOf(permissionsPath()), text))
        return Permissions{};
    const J j = J::parse(text, nullptr, false);
    return j.is_discarded() ? Permissions{} : permissionsFromJson(j);
}

bool savePermissions(const Permissions& p, std::string& error) {
    return writePrivateFile(permissionsPath(), toJson(p).dump(2), error);
}

} // namespace zed::link
