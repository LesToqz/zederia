// The AGENT page: a shell that milestone 6 fills (plan 4.8, 4.9). It shows where the link switch,
// connected clients, activity log, permissions, revert and the client setup snippets will be;
// nothing here connects to anything yet.
#pragma once

#include "Shell.h"

namespace zui {

class AgentPage final : public Page {
public:
    explicit AgentPage(Context& ctx);
    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    Parts parts_;
    std::unique_ptr<Card> link_, clients_, log_, permissions_, setup_;
};

} // namespace zui
