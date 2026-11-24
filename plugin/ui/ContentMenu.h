// Menus of content files: the factory's and the user's shapes, racks, effect presets and clips,
// offered where they are used (the Shapes menus, Load rack, Load effect preset, Import MIDI).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <utility>
#include <vector>

namespace zui {

class Context;

// A heading and the folder whose files go under it.
using ContentSections = std::vector<std::pair<juce::String, juce::File>>;

// The factory folder and the user folder of one kind of content ("Shapes", "Racks",
// "Racks/Effects", "Clips"), as "Factory" and "Yours"; folders that do not exist are left out.
ContentSections contentSections(const Context& ctx, const juce::String& kind);

// Adds every file matching `wildcard` ("*.zshape", "*.mid;*.midi") in the sections to `menu`: a
// heading per section, the files in name order, sub-folders as submenus. `pick` gets the file
// chosen. Returns the number of files added.
int addContentFiles(juce::PopupMenu& menu, const ContentSections& sections, const juce::String& wildcard,
                    std::function<void(const juce::File&)> pick);

} // namespace zui
