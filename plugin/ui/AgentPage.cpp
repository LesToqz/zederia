#include "AgentPage.h"

#include "Audit.h"

#include "../AgentLink.h"
#include "../PluginProcessor.h"
#include "../Session.h"

#include "zederia/link/Link.h"

namespace zui {

namespace {

// A line shortened with an ellipsis until it fits `width` at `size`.
juce::String fitLine(juce::String text, float width, float size) {
    if (textWidth(text, size) <= width)
        return text;
    const juce::String dots = juce::String::fromUTF8("\xe2\x80\xa6");
    while (text.length() > 1 && textWidth(text + dots, size) > width)
        text = text.dropLastCharacters(1);
    return text + dots;
}

// A configuration written on several lines, shown on one.
juce::String oneLine(const juce::String& text) {
    juce::StringArray words = juce::StringArray::fromTokens(text, " \r\n\t", "\"");
    words.removeEmptyStrings();
    return words.joinIntoString(" ");
}

} // namespace

// A card that shows lines of text below its header and parts placed by its owner; it repaints when
// the owner's provider gives new lines.
class AgentPage::LinesCard final : public Card {
public:
    LinesCard(juce::String title, std::function<juce::StringArray()> provider)
        : Card(std::move(title), Family::Neutral), provider_(std::move(provider)) {}
    void draw(juce::Graphics& g) override {
        Card::draw(g);
        auto area = body().toFloat();
        area.removeFromTop(static_cast<float>(topReserve));
        const bool bright = emphasize && emphasize();
        for (const auto& line : lines_) {
            if (area.getHeight() < 16.0f)
                break;
            drawText(g, fitLine(line, area.getWidth() - static_cast<float>(rightReserve), kTextSmall + 0.5f), area.removeFromTop(17.0f),
                     kTextSmall + 0.5f, bright ? theme().text : theme().textDim);
            area.removeFromTop(1.0f);
        }
    }
    bool tick() override {
        const bool header = Card::tick();
        if (!provider_)
            return header;
        auto next = provider_();
        if (next == lines_)
            return header;
        lines_ = std::move(next);
        return true;
    }
    juce::RectangleList<int> dirtyArea() override { return getLocalBounds(); }
    std::function<bool()> emphasize; // the lines are entries (bright), not a note about the card (dim)
    void refreshNow() {
        if (provider_)
            lines_ = provider_();
        repaint();
    }
    int topReserve = 0;   // space for parts at the top of the body
    int rightReserve = 0; // space for parts along the right edge

private:
    std::function<juce::StringArray()> provider_;
    juce::StringArray lines_;
};

// The Connect panel: one block per agent client with its configuration and a prompt to copy.
class AgentPage::ConnectPanel final : public Card {
public:
    struct Client {
        juce::String name, where;
        std::function<juce::String()> config, prompt;
    };

    explicit ConnectPanel(AgentPage& owner) : Card("Connect an agent", Family::Neutral), owner_(owner) {
        markOverlay(*this);
        screws = false;
        clients_ = {{"Claude Code", "Config: save it as .mcp.json in the folder you start Claude Code in. The prompt adds Zederia for every project.", claudeJson,
                     claudePrompt},
                    {"Codex", "Config: add it to config.toml in the .codex folder of your user profile (%USERPROFILE%\\.codex\\config.toml).", codexToml,
                     codexPrompt},
                    {"Antigravity", "Config: add it to mcp_config.json (the /mcp command shows where that file is).", antigravityJson, antigravityPrompt}};
        auto& close = parts_.add<Button>(*this, "", Glyph::Close);
        close.ghost = true;
        close.setTitle("Close the Connect panel");
        close.setTooltip("Close");
        close.setBounds(0, 0, 24, 22);
        close.onClick = [this] { owner_.showConnect(false); };
        headerRight.push_back(&close);
        for (const auto& c : clients_) {
            auto& config = parts_.add<Button>(*this, "Copy config", Glyph::Copy);
            config.textSize = kTextSmall + 0.5f;
            config.setTitle("Copy the " + c.name + " config");
            config.setHelp("Copy config", "Copies the " + c.name + " configuration for this computer's zederia-mcp to the clipboard.");
            const auto text = c.config;
            const juce::String name = c.name;
            config.onClick = [this, text, name] {
                owner_.ctx_.session.copyText(text());
                if (owner_.ctx_.notify)
                    owner_.ctx_.notify("Copied the " + name + " config. Paste it into the file named beside it, then restart " + name + ".");
            };
            auto& prompt = parts_.add<Button>(*this, "Copy prompt", Glyph::Wand);
            prompt.textSize = kTextSmall + 0.5f;
            prompt.accentStyle = true;
            prompt.setTitle("Copy the prompt for " + c.name);
            prompt.setHelp("Copy prompt", "Copies a request to paste into " + c.name + ": it checks the server, adds Zederia to its own settings and tells you when to restart.");
            const auto request = c.prompt;
            prompt.onClick = [this, request, name] {
                owner_.ctx_.session.copyText(request());
                if (owner_.ctx_.notify)
                    owner_.ctx_.notify("Copied the prompt. Paste it into " + name + ": it sets Zederia up itself.");
            };
            config_.push_back(&config);
            prompt_.push_back(&prompt);
        }
        auto& link = parts_.add<Button>(*this, "Link off");
        link.switchStyle = true;
        link.textSize = kTextSmall + 0.5f;
        link.setTitle("Agent link for this instance");
        link.setHelp("Agent link", "Lets agents reach this window's sound (live:... in list_targets). Not needed for headless sessions.");
        link.onClick = [this] { owner_.toggleLink(); };
        link_ = &link;
    }

    void setLinkState(bool on) {
        link_->setToggled(on);
        link_->setText(on ? "Link on" : "Link off");
    }

    void resized() override {
        Card::resized();
        auto area = body();
        area.removeFromTop(kIntroHeight);
        for (std::size_t i = 0; i < clients_.size(); ++i) {
            auto block = area.removeFromTop(kBlockHeight);
            auto buttons = block.removeFromBottom(26).withTrimmedBottom(0);
            config_[i]->setBounds(buttons.removeFromLeft(130));
            buttons.removeFromLeft(8);
            prompt_[i]->setBounds(buttons.removeFromLeft(130));
            area.removeFromTop(kBlockGap);
        }
        link_->setBounds(area.removeFromTop(26).removeFromLeft(130));
    }

    void draw(juce::Graphics& g) override {
        Card::draw(g);
        const Theme& t = theme();
        auto area = body().toFloat();
        auto intro = area.removeFromTop(static_cast<float>(kIntroHeight));
        drawText(g, "Pick your agent. Copy prompt: paste it into the agent and it sets Zederia up by itself.", intro.removeFromTop(17.0f), kTextSmall + 0.5f, t.text);
        drawText(g, "Copy config: paste it into the agent's settings file yourself. Restart the agent afterwards so Zederia's tools load.",
                 intro.removeFromTop(17.0f), kTextSmall + 0.5f, t.textDim);
        const bool found = juce::File(serverPath()).existsAsFile();
        drawText(g, fitLine((found ? "Server: " : "Not found yet: ") + serverPath() + (found ? juce::String() : juce::String("  (install Zederia first)")),
                            intro.getWidth(), kTextSmall + 0.5f),
                 intro.removeFromTop(17.0f), kTextSmall + 0.5f, found ? t.textDim : t.warning);
        for (const auto& c : clients_) {
            auto block = area.removeFromTop(static_cast<float>(kBlockHeight));
            drawText(g, c.name, block.removeFromTop(20.0f), kTextTitle, t.text, juce::Justification::centredLeft, true);
            drawText(g, fitLine(c.where, block.getWidth(), kTextSmall + 0.5f), block.removeFromTop(17.0f), kTextSmall + 0.5f, t.textDim);
            block.removeFromTop(2.0f);
            const auto well = block.removeFromTop(24.0f);
            g.setColour(t.sunken);
            g.fillRoundedRectangle(well, 4.0f);
            g.setColour(t.line);
            g.drawRoundedRectangle(well.reduced(0.5f), 4.0f, 1.0f);
            drawText(g, fitLine(oneLine(c.config()), well.getWidth() - 16.0f, kTextSmall), well.reduced(8.0f, 0.0f), kTextSmall, t.text);
            area.removeFromTop(static_cast<float>(kBlockGap));
        }
        drawText(g, "Agents reach this window's sound only while the link is on; their own sessions need no link.",
                 area.removeFromTop(26.0f).withTrimmedLeft(140.0f), kTextSmall + 0.5f, t.textDim);
    }

    static constexpr int kIntroHeight = 58, kBlockHeight = 96, kBlockGap = 10;
    static constexpr int kPanelHeight = 28 + 12 + kIntroHeight + 3 * (kBlockHeight + kBlockGap) + 26 + 6;

private:
    AgentPage& owner_;
    Parts parts_;
    std::vector<Client> clients_;
    std::vector<Button*> config_, prompt_;
    Button* link_ = nullptr;
};

// --- What the buttons copy -------------------------------------------------------------------------------

juce::String AgentPage::serverPath() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getParentDirectory()
        .getChildFile("Local")
        .getChildFile("Programs")
        .getChildFile("Zederia")
        .getChildFile("zederia-mcp.exe")
        .getFullPathName()
        .replaceCharacter('\\', '/');
}

juce::String AgentPage::claudeJson() {
    return "{\n  \"mcpServers\": {\n    \"zederia\": {\n      \"command\": \"" + serverPath() + "\",\n      \"args\": [\"--stdio\"]\n    }\n  }\n}\n";
}
juce::String AgentPage::claudeCommand() { return "claude mcp add zederia -s user -- \"" + serverPath() + "\" --stdio"; }
juce::String AgentPage::codexToml() {
    return "[mcp_servers.zederia]\ncommand = \"" + serverPath() + "\"\nargs = [\"--stdio\"]\n";
}
juce::String AgentPage::antigravityJson() { return claudeJson(); }

namespace {

juce::String promptHead(const juce::String& client) {
    return "Set up the Zederia MCP server for me in " + client + ".\n\n"
           "Zederia is a wavetable synthesizer. Its MCP server is a program on this computer:\n  " + AgentPage::serverPath() +
           "\nIt speaks MCP over stdio when it is started with the argument --stdio.\n\n"
           "1. Check that the file exists. If it does not, stop and tell me that Zederia has to be installed first.\n";
}

juce::String promptTail(const juce::String& restart) {
    return "Then tell me what you did, and that I need to " + restart +
           " for the Zederia tools to appear. Once they are there, read the resource zederia://guide/agent before using them.\n\n"
           "Do not change any other settings.\n";
}

} // namespace

juce::String AgentPage::claudePrompt() {
    return promptHead("Claude Code") + "2. Add it for my user account, so every project has it, by running:\n   " + claudeCommand() +
           "\n   If it says a server named zederia already exists, run `claude mcp remove zederia -s user` and then the command again.\n"
           "3. Run `claude mcp list` and check that zederia is listed as connected.\n" +
           promptTail("start a new Claude Code session");
}

juce::String AgentPage::codexPrompt() {
    return promptHead("Codex") +
           "2. Add it to my Codex configuration: the file config.toml in the .codex folder of my user profile "
           "(%USERPROFILE%\\.codex\\config.toml; create the file if it does not exist). If the file already has a [mcp_servers.zederia] table, "
           "replace that table; otherwise add this at the end. Leave everything else in the file as it is:\n\n" +
           codexToml() + "\n   (If your version has `codex mcp add`, the command `codex mcp add zederia -- \"" + serverPath() +
           "\" --stdio` does the same.)\n" + promptTail("restart Codex");
}

juce::String AgentPage::antigravityPrompt() {
    return promptHead("Antigravity") +
           "2. Find your MCP configuration file, mcp_config.json (the /mcp command shows where it is). Inside its \"mcpServers\" object add this "
           "entry, keeping every other server as it is:\n\n"
           "\"zederia\": { \"command\": \"" + serverPath() + "\", \"args\": [\"--stdio\"] }\n\n"
           "   If the file does not exist yet, create it with:\n\n" + antigravityJson() + "\n" +
           promptTail("restart Antigravity (or reload its MCP servers)");
}

// --- The page --------------------------------------------------------------------------------------------

AgentPage::AgentPage(Context& ctx) : Page(ctx) {
    setTitle("AGENT");
    auto& link = ctx_.processor.agentLink();
    link_ = std::make_unique<LinesCard>("Agent link", [&link] {
        if (!link.enabled())
            return juce::StringArray{"Off. When it is on, AI agents on this computer (Claude Code, Codex, the Antigravity CLI and other",
                                     "Model Context Protocol clients) can edit this instance, play through it, render and analyze.",
                                     "Only you can open its link, and every agent edit is one undo step with the client's name."};
        return juce::StringArray{"On. Agents reach this instance as live:" + juce::String(link.discovery().instanceId) + " (list_targets).",
                                 "The link is a local pipe only your user account can open; requests need the instance's random token.",
                                 "Switch it off to close the pipe at once."};
    });
    clients_ = std::make_unique<LinesCard>("Connected clients", [&link] {
        juce::StringArray lines;
        for (const auto& c : link.clients())
            lines.add(juce::String(c.name) + ": last request " + c.lastSeen.toString(false, true, true, true) + ", " + juce::String(c.requests) +
                      " requests, " + juce::String(c.edits) + " edits");
        if (lines.isEmpty())
            lines.add(link.enabled() ? "No client has connected yet." : "None (Agent Link is off).");
        return lines;
    });
    log_ = std::make_unique<LinesCard>("Activity", [this, &link] {
        // The header's controls follow the link (an agent's first edit makes the session revertible).
        if (link.generation() != seenGeneration_ && revert_ != nullptr && linkSwitch_ != nullptr) {
            seenGeneration_ = link.generation();
            revert_->setEnabled(link.canRevert());
            showLinkState();
        }
        juce::StringArray lines;
        const auto entries = link.activity();
        for (auto it = entries.rbegin(); it != entries.rend() && lines.size() < 40; ++it) {
            const juce::String client(it->client), action(it->action);
            const juce::String what = client.isEmpty() || action.startsWith(client + ":") ? action : client + " " + action;
            lines.add(it->time.toString(false, true, true, true) + "   " + what);
        }
        if (lines.isEmpty())
            lines.add("Every action an agent takes is listed here, newest first. Undo (Ctrl+Z) or the history list takes it back.");
        return lines;
    });
    log_->emphasize = [&link] { return !link.activity().empty(); }; // entries in full ink, the empty note dim
    // The permissions are not this instance's: the heading says they hold for every agent.
    permissions_ = std::make_unique<LinesCard>("Permissions for every agent on this computer", [] {
        const auto p = zed::link::loadPermissions();
        juce::StringArray lines;
        lines.add("Saved in " + juce::String(zed::link::permissionsPath()) + "; they apply whether this instance's link is on or off.");
        juce::String w = "Allowed write folders: ";
        for (const auto& f : p.writeFolders)
            w += juce::String(f) + "; ";
        lines.add(p.writeFolders.empty() ? juce::String("Allowed write folders: none") : w.dropLastCharacters(2));
        juce::String r = "Allowed read folders: ";
        for (const auto& f : p.readFolders)
            r += juce::String(f) + "; ";
        lines.add(p.readFolders.empty() ? juce::String("Allowed read folders: none (Zederia's own folders are always readable)") : r.dropLastCharacters(2));
        return lines;
    });
    setup_ = std::make_unique<LinesCard>("Setup", [] {
        return juce::StringArray{"Connect Claude Code, Codex or Antigravity to Zederia: copy a prompt for the agent, or its config."};
    });
    for (auto* c : {link_.get(), clients_.get(), log_.get(), permissions_.get(), setup_.get()})
        addAndMakeVisible(*c);

    // The link is this instance's: a switch in its card's header, its state written beside it.
    auto& linkSwitch = parts_.add<Button>(*link_, link.enabled() ? "This instance: on" : "This instance: off");
    linkSwitch.switchStyle = true;
    linkSwitch.textSize = kTextSmall + 0.5f;
    linkSwitch.setToggled(link.enabled());
    linkSwitch.setTitle("Agent link for this instance");
    linkSwitch.setHelp("Agent link", "Turns this instance's local link for AI agents on or off. Off by default; the top bar shows when it is on.");
    linkSwitch.setBounds(0, 0, 150, 22);
    linkSwitch.onClick = [this] { toggleLink(); };
    linkSwitch_ = &linkSwitch;
    link_->headerRight.push_back(&linkSwitch);

    auto& revert = parts_.add<Button>(*log_, "Revert agent session");
    revert.setHelp("Revert agent session", "Restores the sound from before the first agent edit since Agent Link was switched on (as one undo step).");
    revert.setBounds(0, 0, 160, 24);
    revert.onClick = [this] {
        if (ctx_.processor.agentLink().revertAgentSession() && ctx_.notify)
            ctx_.notify("Reverted the agent session.");
    };
    revert_ = &revert;
    log_->headerRight.push_back(&revert);

    // Permissions: one switch each.
    static const char* labels[] = {"Read the patch", "Edit the patch", "Write in Zederia's folders", "Write in allowed folders",
                                   "Audition through speakers", "Read samples in allowed folders"};
    static const char* helps[] = {
        "Agents may read this instance's sound.", "Agents may change this instance's sound (every change is an undo step).",
        "Agents may save presets, tables, clips, renders and packs in your Zederia folders.",
        "Agents may also write in the folders you allow below.", "Agents may play notes through this instance, held below the cap.",
        "Agents may read audio and images in the folders you allow below."};
    for (int i = 0; i < 6; ++i) { // labelled checkboxes: states that are saved, not commands
        auto& b = parts_.add<Button>(*permissions_, labels[i]);
        b.checkbox = true;
        b.textSize = kTextSmall + 0.5f;
        b.setHelp(labels[i], helps[i]);
        b.onClick = [this, i] {
            permButtons_[static_cast<std::size_t>(i)]->setToggled(!permButtons_[static_cast<std::size_t>(i)]->toggled());
            savePermissions();
        };
        permButtons_.push_back(&b);
    }
    auto& cap = parts_.add<Button>(*permissions_, "Audition cap -12 dB");
    cap.setHelp("Audition cap", "The highest peak an agent's audition may reach. Click to step through -6, -12, -18 and -24 dB.");
    cap.onClick = [this] {
        auto p = zed::link::loadPermissions();
        p.auditionCapDb = p.auditionCapDb >= -6.0 ? -12.0 : p.auditionCapDb >= -12.0 ? -18.0 : p.auditionCapDb >= -18.0 ? -24.0 : -6.0;
        std::string error;
        zed::link::savePermissions(p, error);
        refreshPermissions();
    };
    cap_ = &cap;
    for (const auto& [text, write] : std::vector<std::pair<const char*, int>>{{"Allow a write folder...", 1}, {"Allow a read folder...", 0}, {"Clear folders", -1}}) {
        auto& b = parts_.add<Button>(*permissions_, text);
        const int w = write;
        b.setHelp(text, w < 0 ? "Removes every allowed folder." : "Chooses a folder agents may also use.");
        b.onClick = [this, w] {
            if (w < 0) {
                auto p = zed::link::loadPermissions();
                p.writeFolders.clear();
                p.readFolders.clear();
                std::string error;
                zed::link::savePermissions(p, error);
                refreshPermissions();
            } else {
                addFolder(w == 1);
            }
        };
        folderButtons_.push_back(&b);
    }

    // Client setup: an action that opens the panel with each client's config and prompt (it
    // switches nothing on: the link has its own switch).
    auto& connect = parts_.add<Button>(*setup_, "Client setup", Glyph::Link);
    connect.textSize = kTextSmall + 0.5f;
    connect.setHelp("Client setup", "Shows how to connect Claude Code, Codex or Antigravity: copy a prompt the agent follows to set Zederia up, or copy its config.");
    connect.onClick = [this] { showConnect(!connectShown()); };
    connectButton_ = &connect;
    connect_ = std::make_unique<ConnectPanel>(*this);
    addChildComponent(*connect_);

    refreshPermissions();
    showLinkState();
    seenGeneration_ = link.generation();
}

AgentPage::~AgentPage() = default;

void AgentPage::showConnect(bool shown) {
    connect_->setVisible(shown);
    if (shown)
        connect_->toFront(false);
    connectButton_->setToggled(shown);
}

bool AgentPage::connectShown() const { return connect_ != nullptr && connect_->isVisible(); }

void AgentPage::toggleLink() {
    auto& l = ctx_.processor.agentLink();
    std::string error;
    if (!l.setEnabled(!l.enabled(), error) && ctx_.notify)
        ctx_.notify("Agent Link could not start: " + juce::String(error));
    showLinkState();
    link_->refreshNow();
    clients_->refreshNow();
}

void AgentPage::showLinkState() {
    const bool on = ctx_.processor.agentLink().enabled();
    linkSwitch_->setToggled(on);
    linkSwitch_->setText(on ? "This instance: on" : "This instance: off");
    if (connect_ != nullptr)
        connect_->setLinkState(on);
}

void AgentPage::refreshPermissions() {
    const auto p = zed::link::loadPermissions();
    const bool values[] = {p.readPatch, p.editPatch, p.writeZederia, p.writeElsewhere, p.audition, p.readSamples};
    for (std::size_t i = 0; i < permButtons_.size(); ++i)
        permButtons_[i]->setToggled(values[i]);
    cap_->setText("Audition cap " + juce::String(static_cast<int>(p.auditionCapDb)) + " dB");
    permissions_->refreshNow();
    revert_->setEnabled(ctx_.processor.agentLink().canRevert());
}

void AgentPage::savePermissions() {
    auto p = zed::link::loadPermissions();
    bool* fields[] = {&p.readPatch, &p.editPatch, &p.writeZederia, &p.writeElsewhere, &p.audition, &p.readSamples};
    for (std::size_t i = 0; i < permButtons_.size(); ++i)
        *fields[i] = permButtons_[i]->toggled();
    std::string error;
    if (!zed::link::savePermissions(p, error) && ctx_.notify)
        ctx_.notify("The permissions could not be saved: " + juce::String(error));
    refreshPermissions();
}

void AgentPage::addFolder(bool write) {
    chooser_ = std::make_unique<juce::FileChooser>(write ? "A folder agents may write in" : "A folder agents may read audio and images from",
                                                   juce::File::getSpecialLocation(juce::File::userDocumentsDirectory));
    juce::Component::SafePointer<AgentPage> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe, write](const juce::FileChooser& fc) {
        if (safe == nullptr || fc.getResult() == juce::File())
            return;
        auto p = zed::link::loadPermissions();
        (write ? p.writeFolders : p.readFolders).push_back(fc.getResult().getFullPathName().toStdString());
        if (write)
            p.writeElsewhere = true;
        else
            p.readSamples = true;
        std::string error;
        zed::link::savePermissions(p, error);
        safe->refreshPermissions();
    });
}

void AgentPage::paint(juce::Graphics&) {} // the chassis shows through

void AgentPage::resized() {
    auto area = getLocalBounds();
    connect_->setBounds(juce::Rectangle<int>(780, ConnectPanel::kPanelHeight).withCentre(area.getCentre()).constrainedWithin(area));
    auto left = area.removeFromLeft(area.getWidth() / 2);
    link_->setBounds(left.removeFromTop(100));
    clients_->setBounds(left.removeFromTop(100));
    permissions_->setBounds(left);
    // One labelled checkbox per row at the top of the card, the audition cap and the folders below.
    {
        auto body = permissions_->body().reduced(0, 2);
        for (std::size_t i = 0; i < permButtons_.size(); ++i)
            permButtons_[i]->setBounds(body.getX(), body.getY() + static_cast<int>(i) * 26, juce::jmin(body.getWidth(), 320), 22);
        const int y = body.getY() + static_cast<int>(permButtons_.size()) * 26 + 6;
        cap_->setBounds(body.getX(), y, 150, 24);
        int x = body.getX() + 158;
        for (std::size_t i = 0; i < 3; ++i) {
            const int w = i == 2 ? 104 : 150;
            folderButtons_[i]->setBounds(x, y, w, 24);
            x += w + 8;
        }
        permissions_->topReserve = static_cast<int>(permButtons_.size()) * 26 + 6 + 24 + 10;
    }
    log_->setBounds(area.removeFromTop(area.getHeight() - 92));
    setup_->setBounds(area);
    {
        auto body = setup_->body();
        connectButton_->setBounds(body.getX(), body.getBottom() - 28, 150, 26);
    }
}

} // namespace zui
