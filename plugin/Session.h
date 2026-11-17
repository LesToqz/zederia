// The editing session of one instance (plan 3.11), on the message thread: named undo and redo over
// everything that changes sound, A/B compare, locks, Randomize / Mutate / Blend, presets, and copy
// and paste of modules and shapes. It belongs to the processor, so it outlives the editor window.
//
// Every change from the interface goes through here: either a parameter gesture (begin, moves,
// end: one step) or an edit() that changes a copy of the patch, hands the result to the processor
// and records it as one step.
#pragma once

#include "zederia/patch/History.h"
#include "zederia/patch/Variation.h"

#include <functional>
#include <juce_events/juce_events.h>
#include <map>

class ZederiaProcessor;

class Session final : public juce::ChangeBroadcaster {
public:
    explicit Session(ZederiaProcessor& processor);

    // --- History -------------------------------------------------------------------------------
    const zed::History& history() const { return history_; }
    void resetHistory(const juce::String& label);
    bool undo();
    bool redo();
    bool jumpTo(int step);
    // Records the processor's current state as a step (after a change made on it directly). With
    // a merge key equal to the last step's, recent repeats (wheel turns, arrow keys) amend that
    // step instead of adding one.
    void commit(const juce::String& label, const juce::String& mergeKey = {});
    // A change to the patch as one step: `change` edits a copy (return false to cancel), the
    // processor takes the result. Returns false when nothing changed.
    bool edit(const juce::String& label, const std::function<bool(zed::Patch&)>& change);
    // A change the processor makes itself (rack edits that keep tails, table imports): the state
    // before and after becomes one step. `action` returns false when nothing happened.
    bool perform(const juce::String& label, const std::function<bool()>& action);
    // Increments whenever the history or the patch's structure changes through the session.
    int generation() const { return generation_; }

    // --- Parameters ----------------------------------------------------------------------------
    void beginGesture(int index);
    void setFromGesture(int index, double plain); // while the pointer moves
    void endGesture(int index);
    // One step: a menu choice, a switch, a typed value.
    void setValue(int index, double plain);
    // Small repeated changes (wheel, arrow keys): merged into one step while they keep coming.
    void nudgeValue(int index, double plain);
    void resetValue(int index);
    juce::String describeChange(int index, double from, double to) const;

    // --- Routes --------------------------------------------------------------------------------
    // Adds a route from a source to a parameter; returns its number, or -1 when the matrix is full
    // or the parameter cannot be modulated. An existing route from the same source is reused.
    int addRoute(const juce::String& source, int destIndex, double amount);
    void removeRoute(int route);
    void flipPolarity(int route);
    void setRouteMute(int route, bool mute);
    void setRouteSolo(int route, bool solo);
    void setRouteEnabled(int route, bool enabled);
    void setRouteSource(int route, const juce::String& source);
    void setRouteVia(int route, const juce::String& via, bool invert);
    void setRouteCurve(int route, int curve);
    void setRouteCurveShape(int route, const zed::Shape& shape);
    void setRouteDest(int route, int destIndex);
    // Bulk source replacement on the MOD page.
    void replaceSource(const std::vector<int>& routes, const juce::String& source);
    // Routes whose destination is `index`, in matrix order.
    std::vector<int> routesTo(int index) const;

    // --- A/B -----------------------------------------------------------------------------------
    int abSide() const { return abSide_; }
    void switchAB();
    void copyToOtherSide();
    std::string abStateJson() const;
    void setAbStateJson(const std::string& json);

    // --- Presets -------------------------------------------------------------------------------
    bool loadPresetFile(const juce::File& file, juce::String& error);
    bool savePresetFile(const juce::File& file, const zed::PatchMeta& meta, juce::String& error);
    void initPatch();
    juce::File currentPreset() const { return presetFile_; }
    juce::String presetName() const;
    // True when the sound differs from the preset as it was loaded or saved.
    bool modified() const;

    // --- Variation (respects locks) -------------------------------------------------------------
    void randomize(const zed::VariationScope& scope, double amount);
    void mutate(double amount);
    bool blend(const juce::Array<juce::File>& presets, juce::String& error);

    // --- Locks ---------------------------------------------------------------------------------
    void setLocked(int index, bool locked);
    void setModuleLocked(const juce::String& module, bool locked);
    bool moduleLocked(const juce::String& module) const;

    // --- Copy and paste --------------------------------------------------------------------------
    bool copyModule(const juce::String& moduleId);
    bool pasteModule(const juce::String& moduleId, juce::String& error);
    juce::String clipboardClass() const; // the class of module or shape on the clipboard, "" when none
    bool copyText(const juce::String& text); // to the system clipboard (and kept here as well)
    juce::String clipboardText() const;

private:
    void apply(const zed::Patch& state, const juce::String& label, const juce::String& mergeKey = {});
    void changed();

    ZederiaProcessor& p_;
    zed::History history_;
    std::map<int, double> gestureStart_;
    juce::String lastMergeKey_;
    double lastMergeTime_ = 0.0;
    int generation_ = 0;
    // A/B: the side that is not playing.
    int abSide_ = 0;
    std::unique_ptr<zed::Patch> abOther_;
    // Presets
    juce::File presetFile_;
    zed::Patch presetState_;
    std::uint64_t seed_ = 1;
    juce::String clipboard_; // also on the system clipboard; kept for hosts that block it
};
