// The TABLE page: the wavetable workshop (plan 3.10). Frames strip, wave view with drawing tools,
// harmonic, spectral and formula views, process, morph, sort, resynthesis and export, and the
// import wizard. It edits the table of one oscillator; every action is one undo step, and the
// oscillator plays each change at once (the table builds on a worker thread, plan 5.8).
#pragma once

#include "Shell.h"

#include "zederia/table/Toolkit.h"

#include <set>

namespace zui {

class FrameStrip;
class WaveEditor;
class HarmonicEditor;
class SpectralEditor;
class ImportWizard;

class TablePage final : public Page {
public:
    explicit TablePage(Context& ctx);
    ~TablePage() override;
    void paint(juce::Graphics& g) override;
    void resized() override;
    bool reveal(const juce::String& module) override;
    void structureChanged() override;
    void saveState(nlohmann::json& state) const override;
    void loadState(const nlohmann::json& state) override;

    // --- The table being edited --------------------------------------------------------------------
    void setOsc(int osc);
    int osc() const { return osc_; }
    const zed::Wavetable& table() const { return *table_; }
    int frame() const { return frame_; }
    void setFrame(int frame, bool extendSelection = false, bool toggle = false);
    const std::set<int>& selection() const { return selection_; }
    zed::FrameSelection range() const; // the selection as one run (first to last selected)
    // Applies a change to a copy of the table, plays it and records it as one undo step. While
    // `live`, the change is played but not recorded (a stroke in progress; finish() records it).
    void apply(const juce::String& label, const std::function<bool(zed::Wavetable&)>& change, bool live = false);
    void finish(const juce::String& label);
    // Replaces the whole table (imports): one undo step.
    void replaceTable(std::shared_ptr<zed::Wavetable> table, const juce::String& label);

    // For tests: a straight line drawn on the current frame from (x0, y0) to (x1, y1), x 0..1 of the
    // cycle, y -1..1, as a stroke of `steps` pointer moves.
    void drawStroke(double x0, double y0, double x1, double y1, int steps);
    WaveEditor& waveEditor() { return *wave_; }
    ImportWizard& importWizard() { return *wizard_; }
    void showImport();

private:
    void reload();
    void showProcessMenu();
    void showMorphMenu();
    void showSortMenu();
    void showResynthMenu();
    void showExportMenu();
    void saveTableAs();
    void applyFormula();
    void layoutRightTab();

    std::shared_ptr<zed::Wavetable> table_;
    std::shared_ptr<const zed::Wavetable> loaded_; // what the processor had when we last looked
    int osc_ = 0, frame_ = 0;
    std::set<int> selection_{0};
    Parts parts_;
    Button* oscButtons_[3] = {nullptr, nullptr, nullptr};
    Caption* name_ = nullptr;
    std::unique_ptr<FrameStrip> strip_;
    juce::Viewport stripView_;
    std::unique_ptr<WaveEditor> wave_;
    std::unique_ptr<HarmonicEditor> harmonics_;
    std::unique_ptr<SpectralEditor> spectral_;
    std::unique_ptr<ImportWizard> wizard_;
    TabStrip* rightTabs_ = nullptr;
    std::vector<juce::Component*> toolbar_, frameButtons_, waveTools_, formulaParts_;
    TextField formula_{"Formula, for example sin(tau*t) * (1 - y) + saw(t) * y", "Formula"};
    TextField phaseFormula_{"Phase formula (harmonics, optional)", "Phase formula"};
    Caption formulaError_{"", kTextSmall, true};
    int formulaDomain_ = 0, formulaScope_ = 2;
    juce::String pendingLabel_;
};

} // namespace zui
