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

class SetupBackdrop final : public juce::Component {
public:
    SetupBackdrop(AgentPage& owner, juce::Component& panel) : owner_(owner), panel_(panel) {
        markOverlay(*this);
        setAccessible(false);
    }
    void paint(juce::Graphics& g) override {
        g.fillAll(theme().bg.withAlpha(0.76f));
        juce::DropShadow(theme().bg.darker(1.0f).withAlpha(0.66f), 12, {0, 4}).drawForRectangle(g, panel_.getBounds());
    }
    void mouseUp(const juce::MouseEvent& e) override {
        if (!e.mouseWasDraggedSinceMouseDown())
            owner_.showConnect(false);
    }

private:
    AgentPage& owner_;
    juce::Component& panel_;
};

class ConfigPreview final : public juce::TextEditor {
public:
    ConfigPreview() {
        setMultiLine(true, true);
        setReadOnly(true);
        setCaretVisible(false);
        setScrollbarsShown(true);
        setIndents(12, 7);
        setJustification(juce::Justification::topLeft);
    }
    void lookAndFeelChanged() override {
        juce::TextEditor::lookAndFeelChanged();
        applyFontToAllText(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), (kTextSmall + 0.5f) * textScale(), juce::Font::plain)));
        applyColourToAllText(findColour(textColourId), false);
    }
    bool keyPressed(const juce::KeyPress& key) override {
        // JUCE's read-only editor handles Copy and Select All. Allow navigation and
        // Shift-selection as well, including Page Down when larger text wraps a path.
        for (const int code : {juce::KeyPress::leftKey, juce::KeyPress::rightKey, juce::KeyPress::upKey, juce::KeyPress::downKey,
                               juce::KeyPress::homeKey, juce::KeyPress::endKey, juce::KeyPress::pageUpKey, juce::KeyPress::pageDownKey})
            if (key.isKeyCode(code))
                return juce::TextEditorKeyMapper<juce::TextEditor>::invokeKeyFunction(*this, key);
        return juce::TextEditor::keyPressed(key);
    }
    void paintOverChildren(juce::Graphics& g) override {
        juce::TextEditor::paintOverChildren(g);
        if (hasKeyboardFocus(false)) {
            g.setColour(theme().focus);
            g.drawRect(getLocalBounds());
        }
    }
};

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
        if (journal) {
            drawWell(g, area);
            area.reduce(10.0f, 5.0f);
            auto head = area.removeFromTop(22.0f);
            drawText(g, "Time", head.removeFromLeft(92.0f), kTextBody, theme().text, juce::Justification::centredLeft, true);
            drawText(g, "Action", head, kTextBody, theme().text, juce::Justification::centredLeft, true);
            g.setColour(theme().line);
            g.fillRect(area.removeFromTop(1.0f));
        }
        for (const auto& line : lines_) {
            const float height = journal ? 23.0f : 17.0f;
            if (area.getHeight() < height)
                break;
            auto row = area.removeFromTop(height);
            const int split = journal ? line.indexOfChar('\t') : -1;
            if (split >= 0) {
                drawText(g, line.substring(0, split), row.removeFromLeft(92.0f), kTextSmall + 0.5f, theme().textDim);
                drawText(g, fitLine(line.substring(split + 1), row.getWidth(), kTextBody), row, kTextBody, theme().text);
            } else {
                drawText(g, fitLine(line, row.getWidth() - static_cast<float>(rightReserve), kTextSmall + 0.5f), row, kTextSmall + 0.5f, theme().textDim);
            }
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
    bool journal = false;
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

// The selected client owns the instructions, preview and both clipboard actions.
class AgentPage::ConnectPanel final : public Card, private juce::Timer {
public:
    static constexpr int kPanelWidth = 780, kPanelHeight = 462;

    explicit ConnectPanel(AgentPage& owner) : Card("Client setup", Family::Neutral), owner_(owner) {
        markOverlay(*this);
        setFocusContainerType(FocusContainerType::keyboardFocusContainer);
        headerHeight = 32;
        for (juce::Component* c : std::initializer_list<juce::Component*>{&tabs_, &prompt_, &config_, &preview_, &link_, &close_})
            addAndMakeVisible(*c);

        tabs_.setTitle("Agent client");
        tabs_.large = true;
        tabs_.onChange = [this](int) {
            selectClient();
            focusClient();
        };
        prompt_.accentStyle = true;
        prompt_.onClick = [this] {
            copy(prompt_, client().prompt(), "Paste the copied prompt into " + name() + ".");
        };
        config_.onClick = [this] {
            copy(config_, preview_.getText(), client().where);
        };
        link_.switchStyle = true;
        link_.setTitle("Agent link for this instance");
        link_.setHelp("Agent link", "Let connected agents play and edit this instance. Agents can also run their own separate sessions.");
        link_.onClick = [this] {
            owner_.toggleLink();
            if (isShowing()) link_.grabKeyboardFocus();
        };

        close_.ghost = true;
        close_.setTitle("Close client setup");
        close_.setTooltip("Close client setup (Escape)");
        close_.setBounds(0, 0, 28, 24);
        close_.onClick = [this] { owner_.showConnect(false); };
        headerRight.push_back(&close_);
        selectClient();
    }

    void opened() {
        serverFound_ = juce::File(serverPath()).existsAsFile();
        setDescription(serverFound_ ? "The local Zederia server is installed." : "The local server is missing. Install Zederia first.");
        resetFeedback();
        focusClient();
        repaint();
    }

    void setLinkState(bool on) {
        link_.setToggled(on);
        link_.setText(on ? "Link on" : "Link off");
    }

    bool keyPressed(const juce::KeyPress& key) override {
        if (key != juce::KeyPress::escapeKey) return false;
        owner_.showConnect(false);
        return true;
    }

    void resized() override {
        Card::resized();
        auto area = body().reduced(12, 2);
        tabs_.setBounds(area.removeFromTop(32));
        area.removeFromTop(12);

        auto guided = area.removeFromTop(50);
        prompt_.setBounds(guided.removeFromRight(136).withSizeKeepingCentre(136, 32));
        guided.removeFromRight(14);
        promptArea_ = guided;
        area.removeFromTop(10);

        footer_ = area.removeFromBottom(44);
        auto linkArea = footer_.withTrimmedTop(6);
        link_.setBounds(linkArea.removeFromRight(116).withSizeKeepingCentre(116, 28));
        area.removeFromBottom(10);

        manualArea_ = area.removeFromTop(55);
        fileHeader_ = area.removeFromTop(30);
        config_.setBounds(fileHeader_.withTrimmedTop(2).withTrimmedBottom(2).withTrimmedRight(2).removeFromRight(120));
        preview_.setBounds(area);
    }

    void draw(juce::Graphics& g) override {
        Card::draw(g);
        const auto& t = theme();
        const juce::String statusText = serverFound_ ? "Local server installed" : "Server missing. Install Zederia first.";
        const auto ink = serverFound_ ? t.textDim : t.warning;
        auto status = header().withRight(close_.getX() - 10).toFloat().removeFromRight(textWidth(statusText, kTextSmall) + 15.0f);
        g.setColour(serverFound_ ? t.ok : t.warning);
        g.fillEllipse(status.removeFromLeft(6.0f).withSizeKeepingCentre(6.0f, 6.0f));
        status.removeFromLeft(7.0f);
        drawText(g, statusText, status, kTextSmall, ink);

        auto guided = promptArea_.toFloat();
        drawText(g, "Set up with a prompt", guided.removeFromTop(26.0f), kTextLarge, t.text, juce::Justification::centredLeft, true);
        drawText(g, client().instruction, guided, kTextBody, t.textDim);

        auto manual = manualArea_.toFloat();
        g.setColour(t.line);
        g.fillRect(manual.removeFromTop(1.0f));
        manual.removeFromTop(7.0f);
        drawText(g, "Manual setup", manual.removeFromTop(22.0f), kTextTitle, t.text, juce::Justification::centredLeft, true);
        drawText(g, client().where, manual, kTextBody, t.textDim);

        drawPanel(g, fileHeader_.toFloat(), t.raised);
        auto file = fileHeader_.withRight(config_.getX()).toFloat().reduced(12.0f, 0.0f);
        drawText(g, client().file, file, kTextBody, t.text, juce::Justification::centredLeft, true);

        auto foot = footer_.toFloat();
        g.setColour(t.line);
        g.fillRect(foot.removeFromTop(1.0f));
        foot.removeFromTop(4.0f);
        foot = foot.withRight(static_cast<float>(link_.getX()) - 12.0f);
        drawText(g, "Agent link", foot.removeFromTop(20.0f), kTextBody, t.text, juce::Justification::centredLeft, true);
        drawText(g, "Allow connected agents to play and edit this instance.", foot, kTextSmall + 0.5f, t.textDim);
    }

private:
    struct Client {
        const char *instruction, *where, *file;
        juce::String (*config)();
        juce::String (*prompt)();
    };
    static constexpr Client clients_[] = {
        {"Paste the prompt into Claude Code to add Zederia for all your projects.",
         "Merge the Zederia entry into your project's .mcp.json, then start a new Claude Code session.",
         ".mcp.json", claudeJson, claudePrompt},
        {"Paste the prompt into Codex to add Zederia to your local settings.",
         "Add the table to %USERPROFILE%\\.codex\\config.toml, then restart Codex.",
         "config.toml", codexToml, codexPrompt},
        {"Paste the prompt into Antigravity to connect Zederia.",
         "Use /mcp to locate mcp_config.json, merge the Zederia entry, then reload MCP servers.",
         "mcp_config.json", antigravityJson, antigravityPrompt}
    };

    const Client& client() const { return clients_[tabs_.selected()]; }
    juce::String name() { return tabs_.tab(tabs_.selected()).text(); }

    void selectClient() {
        resetFeedback();
        preview_.setTitle(name() + " configuration");
        preview_.setDescription(client().where);
        preview_.setText(client().config(), false);
        preview_.setCaretPosition(0);
        prompt_.setTitle("Copy the prompt for " + name());
        prompt_.setHelp("Copy prompt", client().instruction);
        config_.setTitle("Copy the " + name() + " config");
        config_.setHelp("Copy config", client().where);
        repaint();
    }

    void focusClient() {
        if (isShowing())
            tabs_.tab(tabs_.selected()).grabKeyboardFocus();
    }

    void copy(Button& action, const juce::String& content, const juce::String& nextStep) {
        owner_.ctx_.session.copyText(content);
        resetFeedback();
        action.setText("Copied");
        action.glyph = Glyph::Check;
        if (isShowing()) action.grabKeyboardFocus();
        startTimer(1800);
        if (owner_.ctx_.notify)
            owner_.ctx_.notify(nextStep);
    }

    void timerCallback() override { resetFeedback(); }
    void resetFeedback() {
        stopTimer();
        prompt_.glyph = config_.glyph = Glyph::Copy;
        prompt_.setText("Copy prompt");
        config_.setText("Copy config");
    }

    AgentPage& owner_;
    TabStrip tabs_{{"Claude Code", "Codex", "Antigravity"}};
    Button prompt_{"Copy prompt", Glyph::Copy}, config_{"Copy config", Glyph::Copy};
    ConfigPreview preview_;
    Button link_{"Link off"}, close_{"", Glyph::Close};
    juce::Rectangle<int> promptArea_, manualArea_, fileHeader_, footer_;
    bool serverFound_ = false;
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
            lines.add(it->time.toString(false, true, true, true) + "\t" + what);
        }
        if (lines.isEmpty())
            lines.add("Every action an agent takes is listed here, newest first. Undo (Ctrl+Z) or the history list takes it back.");
        return lines;
    });
    log_->journal = true;
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
    setup_ = std::make_unique<LinesCard>("Agent clients", [] {
        return juce::StringArray{"Connect Claude Code, Codex or Antigravity to Zederia."};
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
    connect.setHelp("Client setup", "Choose your agent and copy a setup prompt or its configuration file entry.");
    connect.onClick = [this] { showConnect(!connectShown()); };
    connectButton_ = &connect;
    connect_ = std::make_unique<ConnectPanel>(*this);
    connectBackdrop_ = std::make_unique<SetupBackdrop>(*this, *connect_);
    addChildComponent(*connectBackdrop_);
    addChildComponent(*connect_);

    refreshPermissions();
    showLinkState();
    seenGeneration_ = link.generation();
}

AgentPage::~AgentPage() = default;

void AgentPage::showConnect(bool shown) {
    if (connectShown() == shown)
        return;
    for (auto* card : {link_.get(), clients_.get(), log_.get(), permissions_.get(), setup_.get()})
        card->setEnabled(!shown);
    connectBackdrop_->setVisible(shown);
    connect_->setVisible(shown);
    connectButton_->setToggled(shown);
    if (shown)
        connect_->opened();
    else if (connectButton_->isShowing())
        connectButton_->grabKeyboardFocus();
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
    connectBackdrop_->setBounds(area);
    connect_->setBounds(juce::Rectangle<int>(ConnectPanel::kPanelWidth, ConnectPanel::kPanelHeight).withCentre(area.getCentre()).constrainedWithin(area));
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
