// The AGENT page (plan 4.8, 4.9): the instance's Agent Link switch, the clients that reached it,
// every action they took (each one an undo step), "Revert agent session", the permissions agents
// have on this computer, and copy buttons for each client's configuration.
#pragma once

#include "Shell.h"

namespace zui {

class AgentPage final : public Page {
public:
    explicit AgentPage(Context& ctx);
    ~AgentPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;

    // The snippets the copy buttons put on the clipboard (plan 4.9), for this computer's server path.
    static juce::String serverPath();
    static juce::String claudeJson();
    static juce::String claudeCommand();
    static juce::String codexToml();
    static juce::String antigravityJson();

private:
    class LinesCard;
    void refreshPermissions();
    void savePermissions();
    void addFolder(bool write);

    Parts parts_;
    std::unique_ptr<LinesCard> link_, clients_, log_, permissions_, setup_;
    Button *linkSwitch_ = nullptr, *revert_ = nullptr, *cap_ = nullptr;
    std::vector<Button*> permButtons_;
    std::vector<Button*> folderButtons_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int seenGeneration_ = -1;
};

} // namespace zui
