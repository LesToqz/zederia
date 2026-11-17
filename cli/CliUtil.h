// Argument handling shared by the command-line tool's source files.
#pragma once

#include "zederia/render/OfflineRenderer.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace cli {

inline constexpr int kOk = 0, kFailed = 1, kUsage = 2;

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    bool has(const std::string& key) const { return options.count(key) != 0; }
    std::string get(const std::string& key, const std::string& fallback = {}) const {
        const auto it = options.find(key);
        return it == options.end() ? fallback : it->second;
    }
};

// Options that take no value.
inline bool isFlag(const std::string& name) {
    static const char* flags[] = {"json", "print", "compact", "list", "update-baseline", "report-only", "profile", "help", "normalize", "descending",
                                  "snap", "average", "unipolar", "live-quality", "audit", "trace"};
    return std::any_of(std::begin(flags), std::end(flags), [&](const char* f) { return name == f; });
}

int cmdUiSnapshot(const Args& args); // UiSnapshot.cpp, or a stub when the plug-in is not built

inline bool parseArgs(int argc, char** argv, int first, Args& out) {
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--", 0) != 0) {
            out.positional.push_back(a);
            continue;
        }
        const std::string name = a.substr(2);
        if (isFlag(name)) {
            out.options[name] = "1";
        } else if (i + 1 < argc) {
            out.options[name] = argv[++i];
        } else {
            std::fprintf(stderr, "error: --%s needs a value\n", name.c_str());
            return false;
        }
    }
    return true;
}

inline bool toNumber(const std::string& text, double& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc() && r.ptr == text.data() + text.size() && std::isfinite(out);
}

inline bool numberOption(const Args& args, const char* key, double min, double max, double& value) {
    if (!args.has(key))
        return true;
    if (!toNumber(args.get(key), value) || value < min || value > max) {
        std::fprintf(stderr, "error: --%s must be a number from %g to %g\n", key, min, max);
        return false;
    }
    return true;
}

inline bool writeBytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::string error;
    if (zed::writeFile(path, bytes, error))
        return true;
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
}

inline bool writeText(const std::string& path, const std::string& text) {
    std::string error;
    const auto* data = reinterpret_cast<const std::uint8_t*>(text.data());
    if (zed::writeFile(path, {data, text.size()}, error))
        return true;
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
}

inline bool readBytes(const std::string& path, std::vector<std::uint8_t>& bytes) {
    std::string error;
    if (zed::readFile(path, bytes, error))
        return true;
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return false;
}

// The table, texture and content commands (TableCommands.cpp).
int cmdTable(const Args& args);
int cmdTexture(const Args& args);
int cmdContent(const Args& args);

} // namespace cli
