// The AGENT page (plan 4.8, 4.9): the instance's Agent Link switch, the clients that reached it,
// every action they took (each one an undo step), "Revert agent session", the permissions agents
// have on this computer, and a client setup panel for Claude Code, Codex and the Antigravity CLI.
// The selected client's configuration and setup prompt use this computer's zederia-mcp path.
#pragma once

#include "Shell.h"

namespace zui {

class AgentPage final : public Page {
public:
    explicit AgentPage(Context& ctx);
    ~AgentPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;

    // What the copy buttons put on the clipboard (plan 4.9), for this computer's server path.
    static juce::String serverPath();
    // Where the installer puts the server (C:\Program Files\Zederia, or the folder it recorded in
    // the registry) and where scripts/install-dev.ps1 puts it (%LOCALAPPDATA%\Programs\Zederia).
    static juce::File installedServer();
    static juce::File developmentServer();
    // The server the client setup names: the one that belongs to this copy of Zederia (`self`, the
    // running plug-in or Standalone; a development copy runs from %LOCALAPPDATA%\Programs), else
    // whichever exists, the installed one first; with neither, the installed location. Forward slashes.
    static juce::String chooseServer(const juce::File& self, const juce::File& installed, const juce::File& development);
    static juce::String claudeJson();    // .mcp.json
    static juce::String claudeCommand(); // claude mcp add, for every project of this user
    static juce::String codexToml();     // config.toml
    static juce::String antigravityJson(); // mcp_config.json
    static juce::String claudePrompt();
    static juce::String codexPrompt();
    static juce::String antigravityPrompt();

    // The Connect panel (tests and tools).
    void showConnect(bool shown);
    bool connectShown() const;

private:
    class LinesCard;
    class ConnectPanel;
    void refreshPermissions();
    void savePermissions();
    void addFolder(bool write);
    void toggleLink();
    void showLinkState();

    Parts parts_;
    std::unique_ptr<LinesCard> link_, clients_, log_, permissions_, setup_;
    std::unique_ptr<ConnectPanel> connect_;
    std::unique_ptr<juce::Component> connectBackdrop_;
    Button *linkSwitch_ = nullptr, *revert_ = nullptr, *cap_ = nullptr, *connectButton_ = nullptr;
    std::vector<Button*> permButtons_;
    std::vector<Button*> folderButtons_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int seenGeneration_ = -1;
};

} // namespace zui
