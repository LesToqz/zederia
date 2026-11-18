#include "AgentPage.h"

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
        for (const auto& line : lines_) {
            if (area.getHeight() < 16.0f)
                break;
            drawText(g, fitLine(line, area.getWidth() - static_cast<float>(rightReserve), kTextSmall + 0.5f), area.removeFromTop(17.0f),
                     kTextSmall + 0.5f, theme().textDim);
            area.removeFromTop(1.0f);
        }
    }
    bool tick() override {
        if (!provider_)
            return false;
        auto next = provider_();
        if (next == lines_)
            return false;
        lines_ = std::move(next);
        return true;
    }
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
    return "{ \"mcpServers\": { \"zederia\": { \"command\": \"" + serverPath() + "\", \"args\": [\"--stdio\"] } } }";
}
juce::String AgentPage::claudeCommand() { return "claude mcp add zederia -- \"" + serverPath() + "\" --stdio"; }
juce::String AgentPage::codexToml() {
    return "[mcp_servers.zederia]\ncommand = \"" + serverPath() + "\"\nargs = [\"--stdio\"]\n";
}
juce::String AgentPage::antigravityJson() { return claudeJson(); }

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
            linkSwitch_->setToggled(link.enabled());
            linkSwitch_->setText(link.enabled() ? "Agent link on" : "Agent link off");
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
    permissions_ = std::make_unique<LinesCard>("Permissions", [] {
        const auto p = zed::link::loadPermissions();
        juce::StringArray lines;
        lines.add("For every agent on this computer (" + juce::String(zed::link::permissionsPath()) + ").");
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
    setup_ = std::make_unique<LinesCard>("Client setup", [] {
        return juce::StringArray{"Claude Code:  " + claudeCommand(), "   or in a project's .mcp.json:  " + claudeJson(),
                                 "Codex (~/.codex/config.toml):  [mcp_servers.zederia]  command = \"" + serverPath() + "\"  args = [\"--stdio\"]",
                                 "Antigravity CLI (mcp_config.json; /mcp shows where):  the same mcpServers entry as Claude Code"};
    });
    for (auto* c : {link_.get(), clients_.get(), log_.get(), permissions_.get(), setup_.get()})
        addAndMakeVisible(*c);

    auto& linkSwitch = parts_.add<Button>(*link_, link.enabled() ? "Agent link on" : "Agent link off");
    linkSwitch.toggleRole = true;
    linkSwitch.setToggled(link.enabled());
    linkSwitch.setHelp("Agent link", "Turns this instance's local link for AI agents on or off. Off by default; the top bar shows when it is on.");
    linkSwitch.setBounds(0, 0, 130, 24);
    linkSwitch.onClick = [this] {
        auto& l = ctx_.processor.agentLink();
        std::string error;
        if (!l.setEnabled(!l.enabled(), error) && ctx_.notify)
            ctx_.notify("Agent Link could not start: " + juce::String(error));
        linkSwitch_->setToggled(l.enabled());
        linkSwitch_->setText(l.enabled() ? "Agent link on" : "Agent link off");
        link_->refreshNow();
        clients_->refreshNow();
    };
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
    for (int i = 0; i < 6; ++i) {
        auto& b = parts_.add<Button>(*permissions_, labels[i]);
        b.toggleRole = true;
        b.leftAlign = true;
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

    // Copy buttons for the client snippets.
    const std::vector<std::pair<juce::String, std::function<juce::String()>>> copies = {
        {"Copy claude mcp add", claudeCommand}, {"Copy .mcp.json", claudeJson}, {"Copy Codex", codexToml}, {"Copy Antigravity", antigravityJson}};
    for (const auto& [label, text] : copies) {
        auto& b = parts_.add<Button>(*setup_, label, Glyph::Copy);
        b.setHelp(label, "Copies the client configuration for this computer's zederia-mcp to the clipboard.");
        auto make = text;
        b.onClick = [this, make] {
            ctx_.session.copyText(make());
            if (ctx_.notify)
                ctx_.notify("Copied.");
        };
        folderButtons_.push_back(&b);
    }
    refreshPermissions();
    seenGeneration_ = link.generation();
}

AgentPage::~AgentPage() = default;

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
    auto area = getLocalBounds().reduced(8);
    auto left = area.removeFromLeft(area.getWidth() / 2 - 4);
    area.removeFromLeft(8);
    link_->setBounds(left.removeFromTop(100));
    left.removeFromTop(8);
    clients_->setBounds(left.removeFromTop(100));
    left.removeFromTop(8);
    permissions_->setBounds(left);
    // Permission switches in two columns at the top of the card, folder buttons below them.
    {
        auto body = permissions_->body().reduced(0, 2);
        const int colW = (body.getWidth() - 8) / 2;
        for (std::size_t i = 0; i < permButtons_.size(); ++i) {
            const int row = static_cast<int>(i) / 2, col = static_cast<int>(i) % 2;
            permButtons_[i]->setBounds(body.getX() + col * (colW + 8), body.getY() + row * 28, colW, 24);
        }
        const int y = body.getY() + 3 * 28;
        cap_->setBounds(body.getX(), y, 150, 24);
        int x = body.getX() + 158;
        for (std::size_t i = 0; i < 3; ++i) {
            const int w = i == 2 ? 104 : 150;
            folderButtons_[i]->setBounds(x, y, w, 24);
            x += w + 8;
        }
        permissions_->topReserve = 4 * 28 + 4;
    }
    log_->setBounds(area.removeFromTop(area.getHeight() - 168));
    area.removeFromTop(8);
    setup_->setBounds(area);
    {
        auto body = setup_->body();
        const int w = (body.getWidth() - 3 * 8) / 4;
        for (std::size_t i = 0; i < 4; ++i)
            folderButtons_[3 + i]->setBounds(body.getX() + static_cast<int>(i) * (w + 8), body.getBottom() - 26, w, 24);
    }
}

} // namespace zui
