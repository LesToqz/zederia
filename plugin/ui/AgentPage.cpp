#include "AgentPage.h"

namespace zui {

namespace {

class TextCard final : public Card {
public:
    TextCard(juce::String title, juce::StringArray lines) : Card(std::move(title), Family::Neutral), lines_(std::move(lines)) {}
    void draw(juce::Graphics& g) override {
        Card::draw(g);
        auto area = body().toFloat();
        for (const auto& line : lines_) {
            drawText(g, line, area.removeFromTop(18.0f), fittingSize(line, area.getWidth(), kTextSmall + 0.5f), theme().textDim);
            area.removeFromTop(2.0f);
        }
    }

private:
    juce::StringArray lines_;
};

juce::String serverPath() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getParentDirectory()
        .getChildFile("Local")
        .getChildFile("Programs")
        .getChildFile("Zederia")
        .getChildFile("zederia-mcp.exe")
        .getFullPathName()
        .replaceCharacter('\\', '/');
}

} // namespace

AgentPage::AgentPage(Context& ctx) : Page(ctx) {
    setTitle("AGENT");
    link_ = std::make_unique<TextCard>("Agent link", juce::StringArray{
        "Off. The agent bridge arrives with the next milestone.",
        "When it is on, AI agents on this computer can edit this instance, render and analyze.",
        "It is off by default for every instance, and the top bar shows when it is on."});
    clients_ = std::make_unique<TextCard>("Connected clients", juce::StringArray{"No clients."});
    log_ = std::make_unique<TextCard>("Activity", juce::StringArray{"Every action an agent takes will be listed here, and can be undone."});
    permissions_ = std::make_unique<TextCard>("Permissions", juce::StringArray{
        "Read the patch: on", "Edit the patch: on", "Write inside Zederia's folders: on", "Write elsewhere: off (allow-listed folders only)",
        "Audition through your speakers: on, level-capped", "Read samples from disk: allow-listed folders"});
    setup_ = std::make_unique<TextCard>("Client setup", juce::StringArray{
        "Claude Code:  claude mcp add zederia -- \"" + serverPath() + "\" --stdio",
        "Codex:  [mcp_servers.zederia] command = \"" + serverPath() + "\"  args = [\"--stdio\"]",
        "Antigravity CLI:  the same mcpServers entry in its mcp_config.json",
        "The server program is installed with the agent bridge."});
    for (auto* c : {link_.get(), clients_.get(), log_.get(), permissions_.get(), setup_.get()})
        addAndMakeVisible(*c);
    auto& linkSwitch = parts_.add<Button>(*link_, "Agent link off");
    linkSwitch.toggleRole = true;
    linkSwitch.setEnabled(false);
    linkSwitch.setHelp("Agent link", "Turns the local link for agents on or off for this instance (with the agent bridge).");
    linkSwitch.setBounds(0, 0, 140, 24);
    link_->headerRight.push_back(&linkSwitch);
    auto& revert = parts_.add<Button>(*log_, "Revert agent session");
    revert.setEnabled(false);
    revert.setHelp("Revert agent session", "Restores the sound from before an agent connected (with the agent bridge).");
    revert.setBounds(0, 0, 170, 24);
    log_->headerRight.push_back(&revert);
    auto& copy = parts_.add<Button>(*setup_, "Copy Claude Code command", Glyph::Copy);
    copy.setHelp("Copy", "Copies the command that registers the server with Claude Code.");
    copy.onClick = [this] {
        ctx_.session.copyText("claude mcp add zederia -- \"" + serverPath() + "\" --stdio");
        if (ctx_.notify)
            ctx_.notify("Copied.");
    };
    copy.setBounds(0, 0, 210, 24);
    setup_->headerRight.push_back(&copy);
}

void AgentPage::paint(juce::Graphics& g) { g.fillAll(theme().bg); }

void AgentPage::resized() {
    auto area = getLocalBounds().reduced(8);
    auto left = area.removeFromLeft(area.getWidth() / 2 - 4);
    area.removeFromLeft(8);
    link_->setBounds(left.removeFromTop(120));
    left.removeFromTop(8);
    clients_->setBounds(left.removeFromTop(120));
    left.removeFromTop(8);
    permissions_->setBounds(left);
    log_->setBounds(area.removeFromTop(area.getHeight() - 200));
    area.removeFromTop(8);
    setup_->setBounds(area);
}

} // namespace zui
