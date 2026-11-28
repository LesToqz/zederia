#include "zederia/content/PresetIndex.h"

#include "zederia/patch/PatchIO.h"
#include "zederia/render/OfflineRenderer.h"
#include "zederia/util/Math.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <unordered_map>

namespace zed {
namespace {

namespace fs = std::filesystem;

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool contains(std::string_view haystack, std::string_view needleLower) {
    return lower(haystack).find(needleLower) != std::string::npos;
}

bool anyEquals(const std::vector<std::string>& values, std::string_view wantLower) {
    for (const auto& v : values)
        if (lower(v) == wantLower)
            return true;
    return false;
}

std::int64_t timeOf(const fs::path& file) {
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    if (ec)
        return 0;
    const auto system = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return std::chrono::duration_cast<std::chrono::seconds>(system.time_since_epoch()).count();
}

// Splits on spaces, keeping "quoted phrases" and field:"quoted values" together.
std::vector<std::string> tokens(std::string_view text) {
    std::vector<std::string> out;
    std::string current;
    bool quoted = false;
    for (const char c : text) {
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty())
                out.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty())
        out.push_back(std::move(current));
    return out;
}

const std::string& field(const PresetInfo& p, PresetSort key) {
    switch (key) {
    case PresetSort::Type: return p.meta.type;
    case PresetSort::Author: return p.meta.author;
    case PresetSort::Pack: return p.meta.pack;
    default: return p.meta.name;
    }
}

int compare(const PresetInfo& a, const PresetInfo& b, PresetSort key) {
    switch (key) {
    case PresetSort::Rating: return (a.meta.rating > b.meta.rating) - (a.meta.rating < b.meta.rating);
    case PresetSort::Modified: return (a.fileTime > b.fileTime) - (a.fileTime < b.fileTime);
    case PresetSort::Origin: return static_cast<int>(a.factory) - static_cast<int>(b.factory);
    default: {
        const std::string x = lower(field(a, key)), y = lower(field(b, key));
        return x < y ? -1 : x > y ? 1 : 0;
    }
    }
}

} // namespace

std::uint64_t soundHash(const Patch& patch) {
    Patch sound = patch;
    sound.meta = PatchMeta{};
    const std::string text = savePatch(sound);
    return fnv1a(text.data(), text.size());
}

bool readPresetInfo(const std::string& path, PresetInfo& out, std::string& error) {
    std::vector<std::uint8_t> bytes;
    if (!readFile(path, bytes, error))
        return false;
    const LoadResult loaded = loadPatch(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    if (!loaded.ok()) {
        error = loaded.issues.empty() ? "not a patch" : loaded.issues.front().message;
        return false;
    }
    out.path = path;
    out.meta = loaded.patch.meta;
    out.soundHash = soundHash(loaded.patch);
    out.fileTime = timeOf(fs::path(path));
    return true;
}

std::vector<PresetInfo> scanPresets(const std::string& factoryDir, const std::string& userDir, std::vector<std::string>* problems) {
    std::vector<PresetInfo> out;
    const auto scan = [&](const std::string& root, bool factory) {
        if (root.empty())
            return;
        const fs::path base = fs::path(root) / "Presets";
        std::error_code ec;
        if (!fs::is_directory(base, ec))
            return;
        std::vector<fs::path> files;
        for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && lower(it->path().extension().string()) == ".zpreset")
                files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            PresetInfo info;
            std::string error;
            if (!readPresetInfo(file.string(), info, error)) {
                if (problems != nullptr)
                    problems->push_back(file.string() + ": " + error);
                continue;
            }
            info.factory = factory;
            const fs::path rel = fs::relative(file.parent_path(), base, ec);
            info.folder = ec || rel == "." ? std::string() : rel.generic_string();
            out.push_back(std::move(info));
        }
    };
    scan(factoryDir, true);
    // The installer also copies the factory presets into the user's Presets folder, into the same
    // folders, so they can be seen in Explorer. Such a copy is the factory preset, listed once: a
    // user file in the same place as a factory preset with the same sound is left out. One whose
    // sound was changed is the user's.
    const auto place = [](const PresetInfo& p) { return lower(p.folder + "/" + fs::path(p.path).filename().string()); };
    std::unordered_map<std::string, std::uint64_t> factorySounds;
    for (const auto& p : out)
        factorySounds.emplace(place(p), p.soundHash);
    const std::size_t factoryCount = out.size();
    scan(userDir, false);
    if (!factorySounds.empty())
        out.erase(std::remove_if(out.begin() + static_cast<std::ptrdiff_t>(factoryCount), out.end(),
                                 [&](const PresetInfo& p) {
                                     const auto it = factorySounds.find(place(p));
                                     return it != factorySounds.end() && it->second == p.soundHash;
                                 }),
                  out.end());
    return out;
}

PresetQuery parsePresetQuery(std::string_view text) {
    PresetQuery q;
    for (const std::string& token : tokens(text)) {
        const auto colon = token.find(':');
        const std::string key = lower(colon == std::string::npos ? token : token.substr(0, colon));
        const std::string value = colon == std::string::npos ? std::string() : lower(token.substr(colon + 1));
        if (colon != std::string::npos && !value.empty()) {
            if (key == "tag" || key == "character") { q.tags.push_back(value); continue; }
            if (key == "type") { q.types.push_back(value); continue; }
            if (key == "genre") { q.genres.push_back(value); continue; }
            if (key == "pack") { q.packs.push_back(value); continue; }
            if (key == "author" || key == "by") { q.authors.push_back(value); continue; }
            if (key == "rating" || key == "stars") { q.minRating = std::clamp(std::atoi(value.c_str()), 0, 5); continue; }
        }
        if (colon == std::string::npos) {
            if (key == "fav" || key == "favourite" || key == "favourites" || key == "favorite") { q.favouritesOnly = true; continue; }
            if (key == "factory") { q.origin = 1; continue; }
            if (key == "user") { q.origin = 2; continue; }
        }
        q.words.push_back(lower(token));
    }
    return q;
}

bool presetMatches(const PresetInfo& p, const PresetQuery& q) {
    const PatchMeta& m = p.meta;
    if (q.favouritesOnly && !m.favourite) return false;
    if (m.rating < q.minRating) return false;
    if (q.origin == 1 && !p.factory) return false;
    if (q.origin == 2 && p.factory) return false;
    for (const auto& t : q.tags) if (!anyEquals(m.tags, t)) return false;
    for (const auto& g : q.genres) if (!anyEquals(m.genres, g)) return false;
    for (const auto& t : q.types) if (lower(m.type) != t) return false;
    for (const auto& pk : q.packs) if (lower(m.pack) != pk) return false;
    for (const auto& a : q.authors) if (!contains(m.author, a)) return false;
    for (const auto& w : q.words) {
        bool found = contains(m.name, w) || contains(m.author, w) || contains(m.pack, w) || contains(m.type, w) ||
                     contains(m.notes, w) || contains(p.folder, w);
        for (const auto& t : m.tags) found = found || contains(t, w);
        for (const auto& g : m.genres) found = found || contains(g, w);
        if (!found)
            return false;
    }
    return true;
}

void sortPresets(const std::vector<PresetInfo>& presets, std::vector<int>& items, PresetSort key, bool descending,
                 PresetSort secondary) {
    std::stable_sort(items.begin(), items.end(), [&](int ia, int ib) {
        const PresetInfo& a = presets[static_cast<std::size_t>(ia)];
        const PresetInfo& b = presets[static_cast<std::size_t>(ib)];
        int c = compare(a, b, key);
        if (descending)
            c = -c;
        if (c == 0)
            c = compare(a, b, secondary);
        if (c == 0)
            c = compare(a, b, PresetSort::Name);
        return c < 0;
    });
}

std::map<std::string, int> presetFacet(const std::vector<PresetInfo>& presets, const std::vector<int>& items, std::string_view facet) {
    std::map<std::string, int> counts;
    for (const int i : items) {
        const PatchMeta& m = presets[static_cast<std::size_t>(i)].meta;
        if (facet == "type") { if (!m.type.empty()) ++counts[m.type]; }
        else if (facet == "pack") { if (!m.pack.empty()) ++counts[m.pack]; }
        else if (facet == "author") { if (!m.author.empty()) ++counts[m.author]; }
        else if (facet == "tag") { for (const auto& t : m.tags) ++counts[t]; }
        else if (facet == "genre") { for (const auto& g : m.genres) ++counts[g]; }
    }
    return counts;
}

std::vector<std::vector<int>> duplicatePresets(const std::vector<PresetInfo>& presets) {
    std::unordered_map<std::uint64_t, std::vector<int>> byHash;
    for (int i = 0; i < static_cast<int>(presets.size()); ++i)
        byHash[presets[static_cast<std::size_t>(i)].soundHash].push_back(i);
    std::vector<std::vector<int>> groups;
    for (auto& [hash, list] : byHash)
        if (list.size() > 1)
            groups.push_back(std::move(list));
    std::sort(groups.begin(), groups.end());
    return groups;
}

} // namespace zed
