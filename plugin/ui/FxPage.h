// The FX page (plan 3.8): lanes A, B and Master side by side, slots dragged to reorder or move
// between lanes, the selected slot's controls and display, the bus balances, FX off, the rack lock
// and rack and slot presets.
#pragma once

#include "Shell.h"

namespace zui {

class LaneColumn;
class SlotDetail;

class FxPage final : public Page {
public:
    explicit FxPage(Context& ctx);
    ~FxPage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool reveal(const juce::String& module) override;
    void structureChanged() override;
    void applySimpleView() override;
    void saveState(nlohmann::json& state) const override;
    void loadState(const nlohmann::json& state) override;

    void select(int lane, int slot);
    int selectedLane() const { return lane_; }
    int selectedSlot() const { return slot_; }
    // Rack edits, each one undo step; they fade in the engine, so none of them clicks.
    bool insertEffect(int lane, int position, int type);
    bool removeEffect(int lane, int slot);
    bool moveEffect(int fromLane, int fromSlot, int toLane, int toPosition);
    int slotCount(int lane) const;
    int slotType(int lane, int slot) const;
    SlotDetail& detail() { return *detail_; }

private:
    std::vector<std::unique_ptr<LaneColumn>> lanes_;
    std::unique_ptr<SlotDetail> detail_;
    std::unique_ptr<juce::Component> rackCard_;
    std::unique_ptr<juce::Component> ticker_;
    juce::String layout_;
    int lane_ = 0, slot_ = 0;
};

} // namespace zui
