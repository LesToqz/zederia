// Finds the wavetables and textures a patch refers to (plan 4.5, 5.6) and loads them into the
// form the engine plays. A reference is one of
//   ""                          the built-in table or noise
//   "factory:Basics/Saw Morph"  <factory>/Wavetables/Basics/Saw Morph.wav (Textures for samples)
//   "user:My Tables/Growl"      <user>/Wavetables/My Tables/Growl.wav
//   a file path                 absolute, or relative to the folder of the preset that names it
// Loading touches files, so nothing here is for the audio thread.
#pragma once

#include "zederia/engine/Engine.h"
#include "zederia/engine/SampleData.h"
#include "zederia/patch/Patch.h"
#include "zederia/table/TableData.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

class ContentLibrary {
public:
    // The usual folders (plan 5.6): the environment variable ZEDERIA_FACTORY when set, otherwise
    // the machine-wide factory folder and its per-user fallback; user content under Documents.
    static ContentLibrary standard();

    void setFactoryDir(std::string dir) { factory_ = std::move(dir); }
    void setUserDir(std::string dir) { user_ = std::move(dir); }
    const std::string& factoryDir() const { return factory_; }
    const std::string& userDir() const { return user_; }

    // The file a reference points to; "" for the built-in reference or one that cannot be mapped.
    std::string tablePath(std::string_view reference, std::string_view presetDir = {}) const;
    std::string texturePath(std::string_view reference, std::string_view presetDir = {}) const;

    // Null with a message when the file is missing or unreadable. The empty reference gives the
    // built-in object.
    std::shared_ptr<const TableData> loadTable(std::string_view reference, std::string& error,
                                               std::string_view presetDir = {}) const;
    std::shared_ptr<const SampleData> loadTexture(std::string_view reference, std::string& error,
                                                  std::string_view presetDir = {}) const;

    // References to everything found in the factory and user folders, sorted.
    std::vector<std::string> listTables() const;
    std::vector<std::string> listTextures() const;

private:
    std::string path(std::string_view reference, std::string_view kindFolder, std::string_view presetDir) const;
    std::vector<std::string> list(std::string_view kindFolder) const;
    std::string factory_, user_;
};

// Loads every table and the texture sample a patch names and hands them to the engine. What
// cannot be loaded falls back to the built-in object; one message per problem is returned.
std::vector<std::string> applyPatchAssets(Engine& engine, const Patch& patch, const ContentLibrary& library,
                                          std::string_view presetDir = {});

} // namespace zed
