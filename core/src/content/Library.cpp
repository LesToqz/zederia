#include "zederia/content/Library.h"

#include "zederia/audio/WavFile.h"
#include "zederia/render/OfflineRenderer.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace zed {
namespace {

namespace fs = std::filesystem;

// One of the few places that asks the operating system anything.
std::string environment(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr)
        return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
#endif
}

bool isDirectory(const std::string& path) {
    std::error_code ec;
    return !path.empty() && fs::is_directory(fs::path(path), ec);
}

// Keeps a reference inside its root: no absolute parts, no "..".
bool safeRelative(std::string_view relative) {
    if (relative.empty() || relative.front() == '/' || relative.front() == '\\' || relative.find(':') != std::string_view::npos)
        return false;
    std::size_t start = 0;
    while (start <= relative.size()) {
        const std::size_t end = std::min(relative.find_first_of("/\\", start), relative.size());
        if (relative.substr(start, end - start) == "..")
            return false;
        start = end + 1;
    }
    return true;
}

std::string join(const std::string& a, std::string_view b) {
    return (fs::path(a) / fs::path(std::string(b))).string();
}

} // namespace

ContentLibrary ContentLibrary::standard() {
    ContentLibrary library;
    const std::string chosen = environment("ZEDERIA_FACTORY");
    if (isDirectory(chosen)) {
        library.factory_ = chosen;
    } else {
        const std::string machine = environment("PROGRAMDATA"), local = environment("LOCALAPPDATA");
        const std::string machineWide = machine.empty() ? std::string() : join(machine, "Zederia/Factory");
        const std::string perUser = local.empty() ? std::string() : join(local, "Zederia/Factory");
        library.factory_ = isDirectory(machineWide) ? machineWide : perUser;
    }
    const std::string home = environment("USERPROFILE").empty() ? environment("HOME") : environment("USERPROFILE");
    if (!home.empty())
        library.user_ = join(home, "Documents/Zederia");
    return library;
}

std::string ContentLibrary::path(std::string_view reference, std::string_view kindFolder, std::string_view presetDir) const {
    if (reference.empty())
        return {};
    const auto inRoot = [&](const std::string& root, std::string_view relative) {
        if (root.empty() || !safeRelative(relative))
            return std::string();
        std::string file = join(join(root, kindFolder), relative);
        if (file.size() < 4 || file.compare(file.size() - 4, 4, ".wav") != 0)
            file += ".wav";
        return file;
    };
    if (reference.substr(0, 8) == "factory:")
        return inRoot(factory_, reference.substr(8));
    if (reference.substr(0, 5) == "user:")
        return inRoot(user_, reference.substr(5));
    const fs::path given = fs::path(std::string(reference));
    if (given.is_absolute() || presetDir.empty())
        return given.string();
    return (fs::path(std::string(presetDir)) / given).string();
}

std::string ContentLibrary::tablePath(std::string_view reference, std::string_view presetDir) const {
    return path(reference, "Wavetables", presetDir);
}
std::string ContentLibrary::texturePath(std::string_view reference, std::string_view presetDir) const {
    return path(reference, "Textures", presetDir);
}

std::shared_ptr<const TableData> ContentLibrary::loadTable(std::string_view reference, std::string& error,
                                                           std::string_view presetDir) const {
    if (reference.empty())
        return TableData::builtin();
    const std::string file = tablePath(reference, presetDir);
    if (file.empty()) {
        error = "the wavetable reference \"" + std::string(reference) + "\" cannot be resolved (no such folder is set up)";
        return nullptr;
    }
    std::vector<std::uint8_t> bytes;
    Wavetable table;
    if (!readFile(file, bytes, error) || !decodeWavetable(bytes, table, error)) {
        error = "wavetable \"" + std::string(reference) + "\": " + error;
        return nullptr;
    }
    if (table.name.empty())
        table.name = fs::path(file).stem().string();
    return TableData::build(table);
}

std::shared_ptr<const SampleData> ContentLibrary::loadTexture(std::string_view reference, std::string& error,
                                                              std::string_view presetDir) const {
    if (reference.empty())
        return SampleData::builtin();
    const std::string file = texturePath(reference, presetDir);
    if (file.empty()) {
        error = "the sample reference \"" + std::string(reference) + "\" cannot be resolved (no such folder is set up)";
        return nullptr;
    }
    std::vector<std::uint8_t> bytes;
    WavData wav;
    if (!readFile(file, bytes, error) || !decodeWav(bytes, wav, error)) {
        error = "sample \"" + std::string(reference) + "\": " + error;
        return nullptr;
    }
    if (wav.frames() < 2) {
        error = "sample \"" + std::string(reference) + "\" holds no audio";
        return nullptr;
    }
    return SampleData::fromWav(wav, fs::path(file).stem().string());
}

std::vector<std::string> ContentLibrary::list(std::string_view kindFolder) const {
    std::vector<std::string> found;
    const auto scan = [&](const std::string& root, const char* prefix) {
        if (root.empty())
            return;
        const fs::path base = fs::path(join(root, kindFolder));
        std::error_code ec;
        if (!fs::is_directory(base, ec))
            return;
        for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
             it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            try {
                std::string extension = it->path().extension().string();
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
                if (extension != ".wav")
                    continue;
                fs::path relative = fs::relative(it->path(), base, ec);
                relative.replace_extension();
                found.push_back(prefix + relative.generic_string());
            } catch (const std::exception&) {
                // A name that cannot be written in this system's narrow encoding is skipped.
            }
        }
    };
    scan(factory_, "factory:");
    scan(user_, "user:");
    std::sort(found.begin(), found.end());
    return found;
}

std::vector<std::string> ContentLibrary::listTables() const { return list("Wavetables"); }
std::vector<std::string> ContentLibrary::listTextures() const { return list("Textures"); }

std::vector<std::string> applyPatchAssets(Engine& engine, const Patch& patch, const ContentLibrary& library,
                                          std::string_view presetDir) {
    std::vector<std::string> problems;
    for (int i = 0; i < kNumOscs; ++i) {
        if (const auto& data = patch.tableData[static_cast<std::size_t>(i)]) { // kept in the patch
            engine.setTable(i, TableData::build(*data.table));
            continue;
        }
        std::string error;
        auto table = library.loadTable(patch.tables[static_cast<std::size_t>(i)], error, presetDir);
        if (table == nullptr)
            problems.push_back("osc" + std::to_string(i + 1) + ": " + error + "; the built-in table is used instead");
        engine.setTable(i, std::move(table));
    }
    std::string error;
    auto sample = library.loadTexture(patch.textureSample, error, presetDir);
    if (sample == nullptr)
        problems.push_back("texture: " + error + "; the built-in noise is used instead");
    engine.setTexture(std::move(sample));
    return problems;
}

} // namespace zed
