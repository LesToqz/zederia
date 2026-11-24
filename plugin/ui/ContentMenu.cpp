#include "ContentMenu.h"

#include "Context.h"
#include "../PluginProcessor.h"

namespace zui {
namespace {

// The files of one folder and its sub-folders, sub-folders first as submenus, all in name order.
int addFolder(juce::PopupMenu& menu, const juce::File& folder, const juce::String& wildcard,
              const std::function<void(const juce::File&)>& pick, int depth) {
    if (depth > 4)
        return 0;
    int added = 0;
    auto folders = folder.findChildFiles(juce::File::findDirectories, false);
    folders.sort();
    for (const juce::File& sub : folders) {
        juce::PopupMenu inner;
        const int count = addFolder(inner, sub, wildcard, pick, depth + 1);
        if (count > 0) {
            menu.addSubMenu(sub.getFileName(), inner);
            added += count;
        }
    }
    auto files = folder.findChildFiles(juce::File::findFiles, false, wildcard);
    files.sort();
    for (const juce::File& file : files) {
        menu.addItem(file.getFileNameWithoutExtension(), [pick, file] { pick(file); });
        ++added;
    }
    return added;
}

} // namespace

ContentSections contentSections(const Context& ctx, const juce::String& kind) {
    ContentSections sections;
    const std::string factory = ctx.processor.library().factoryDir();
    if (!factory.empty()) {
        const juce::File folder = juce::File(juce::String::fromUTF8(factory.c_str())).getChildFile(kind);
        if (folder.isDirectory())
            sections.emplace_back("Factory", folder);
    }
    const juce::File user = ctx.userFolder().getChildFile(kind);
    if (user.isDirectory())
        sections.emplace_back("Yours", user);
    return sections;
}

int addContentFiles(juce::PopupMenu& menu, const ContentSections& sections, const juce::String& wildcard,
                    std::function<void(const juce::File&)> pick) {
    int added = 0;
    for (const auto& [heading, folder] : sections) {
        juce::PopupMenu part;
        const int count = addFolder(part, folder, wildcard, pick, 0);
        if (count == 0)
            continue;
        menu.addSectionHeader(heading);
        for (juce::PopupMenu::MenuItemIterator it(part); it.next();)
            menu.addItem(it.getItem());
        added += count;
    }
    return added;
}

} // namespace zui
