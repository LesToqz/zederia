// The preset library (plan 3.11): what the factory and user folders hold, searched with free text
// and `field:value` filters, faceted and sorted. The LIBRARY page and the agent tools both use it.
// Scanning reads files, so it runs off the audio thread (a worker or the message thread).
#pragma once

#include "zederia/patch/Patch.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

struct PresetInfo {
    std::string path;        // the .zpreset file
    bool factory = false;
    PatchMeta meta;
    std::uint64_t soundHash = 0; // the sound without its metadata: equal for duplicates
    std::int64_t fileTime = 0;   // last write, seconds since 1970
    std::string folder;          // sub-folder inside Presets ("Basses", "My Pack/Leads"), "" at the top
};

// Reads one preset's metadata and sound hash. False with a message when the file is unreadable or
// not a patch (the library lists it as broken instead of failing).
bool readPresetInfo(const std::string& path, PresetInfo& out, std::string& error);
// The hash of a patch's sound: everything but `meta`.
std::uint64_t soundHash(const Patch& patch);

// Every *.zpreset under <factory>/Presets and <user>/Presets. Unreadable files are reported in
// `problems` (one line each) and left out.
std::vector<PresetInfo> scanPresets(const std::string& factoryDir, const std::string& userDir,
                                    std::vector<std::string>* problems = nullptr);

// A search as typed in the browser: free words plus filters.
//   tag:warm  type:bass  genre:house  pack:"Night Drive"  author:me  rating:3 (at least 3)
//   fav (favourites only)  factory  user
// Free words must all appear (any order, any case) in the name, author, pack, type, tags, genres
// or notes.
struct PresetQuery {
    std::vector<std::string> words;
    std::vector<std::string> tags, types, genres, packs, authors;
    int minRating = 0;
    bool favouritesOnly = false;
    int origin = 0; // 0 any, 1 factory only, 2 user only
};
PresetQuery parsePresetQuery(std::string_view text);
bool presetMatches(const PresetInfo& preset, const PresetQuery& query);

enum class PresetSort { Name, Type, Author, Pack, Rating, Modified, Origin };
// Sorts `items` (indices into `presets`) by a key and then by a second key; names break ties.
void sortPresets(const std::vector<PresetInfo>& presets, std::vector<int>& items, PresetSort key, bool descending,
                 PresetSort secondary = PresetSort::Name);

// How many of `items` carry each value of a facet: "type", "tag", "genre", "pack", "author".
std::map<std::string, int> presetFacet(const std::vector<PresetInfo>& presets, const std::vector<int>& items,
                                       std::string_view facet);

// Groups of presets whose sounds are identical (indices into `presets`, two or more per group).
std::vector<std::vector<int>> duplicatePresets(const std::vector<PresetInfo>& presets);

} // namespace zed
