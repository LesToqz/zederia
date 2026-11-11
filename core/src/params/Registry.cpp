// Declares every parameter. Order is permanent: add new parameters at the end of declareAll()
// (after the last module) and never remove or reorder existing ones; a retired parameter stays
// as a hidden placeholder (automatable = false).
#include "zederia/params/Registry.h"

#include <cassert>
#include <charconv>

namespace zed {
namespace {

using Strings = std::vector<std::string>;

const Strings& filterTypes() {
    static const Strings types = [] {
        Strings t = {
            // State-variable
            "svf_lp6", "svf_lp12", "svf_lp18", "svf_lp24", "svf_hp6", "svf_hp12", "svf_hp18", "svf_hp24",
            "svf_bp12", "svf_bp24", "svf_notch12", "svf_notch24", "svf_peak12", "svf_peak24",
            // Ladder
            "ladder_lp6", "ladder_lp12", "ladder_lp18", "ladder_lp24",
            // Dual
            "dual_lp_hp", "dual_lp_bp", "dual_lp_peak", "dual_lp_notch", "dual_hp_bp", "dual_hp_peak",
            "dual_hp_notch", "dual_bp_peak", "dual_bp_notch", "dual_peak_peak", "dual_peak_notch", "dual_notch_notch",
            // Morph
            "morph_lbh12", "morph_lbh24", "morph_lph12", "morph_lph24", "morph_lnh12", "morph_lnh24",
            "morph_bpn12", "morph_bpn24",
        };
        // Comb, flange and phase: feedback sign and the damping filter in the loop.
        for (const char* family : {"comb", "flange", "phase"})
            for (const char* sign : {"pos", "neg"})
                for (const char* damp : {"lp", "hp", "bp"})
                    t.push_back(std::string(family) + "_" + sign + "_" + damp);
        for (const char* rest : {"formant_a", "formant_b", "formant_c", "low_shelf", "high_shelf", "bell",
                                 "ring_mod", "sample_hold", "allpass_cluster", "diffuser", "band_reject",
                                 "howl_lp", "howl_bp", "grit_comb"})
            t.emplace_back(rest);
        return t;
    }();
    return types;
}

const Strings kWarpModes = {"off",        "sync",       "soft_sync", "bend_plus",  "bend_minus", "bend_both",
                            "pulse_width", "skew_plus",  "skew_minus", "skew_both", "mirror",     "flip",
                            "quantize",   "curve_full", "curve_half", "curve_sine", "curve_quad", "fold",
                            "drive",      "pm",         "am",         "rm"};
const Strings kSources = {"osc1", "osc2", "osc3", "sub", "texture"};

// Builds the parameter list one module at a time.
struct Builder {
    std::vector<ParamDef>& params;
    std::vector<ModuleInfo>& modules;
    std::string moduleId, moduleName, base;
    Scope scope = Scope::Global;

    void begin(std::string id, std::string name, std::string description, std::string pointerBase,
               Scope moduleScope, ModuleKind kind = ModuleKind::Plain) {
        moduleId = id;
        moduleName = name;
        base = std::move(pointerBase);
        scope = moduleScope;
        modules.push_back({std::move(id), std::move(name), std::move(description), kind,
                           static_cast<int>(params.size()), 0});
    }
    void end() { modules.back().numParams = static_cast<int>(params.size()) - modules.back().firstParam; }

    ParamDef& add(const std::string& key, const std::string& name, ParamType type, double min, double max,
                  double def, Unit unit, std::string description) {
        ParamDef p;
        p.id = moduleId + "." + key;
        p.name = moduleName.empty() ? name : moduleName + " " + name;
        p.module = moduleId;
        std::string path = key;
        for (char& c : path)
            if (c == '.')
                c = '/';
        p.pointer = base + "/" + path + std::string(unitKeySuffix(unit));
        p.description = std::move(description);
        p.type = type;
        p.min = min;
        p.max = max;
        p.def = def;
        p.unit = unit;
        p.scope = scope;
        const bool continuous = type == ParamType::Float;
        p.smoothing = continuous ? Smoothing::Control : Smoothing::None;
        p.modulatable = continuous;
        assert(def >= min && def <= max);
        params.push_back(std::move(p));
        return params.back();
    }

    ParamDef& toggle(const std::string& key, const std::string& name, bool def, std::string d) {
        return add(key, name, ParamType::Bool, 0, 1, def ? 1 : 0, Unit::None, std::move(d));
    }
    ParamDef& choice(const std::string& key, const std::string& name, const Strings& choices,
                     std::string_view def, std::string d) {
        auto& p = add(key, name, ParamType::Choice, 0, static_cast<double>(choices.size() - 1), 0, Unit::None,
                      std::move(d));
        p.choices = choices;
        const int i = p.choiceIndex(def);
        assert(i >= 0);
        p.def = i;
        return p;
    }
    ParamDef& integer(const std::string& key, const std::string& name, int min, int max, int def, Unit unit,
                      std::string d) {
        return add(key, name, ParamType::Int, min, max, def, unit, std::move(d));
    }
    ParamDef& real(const std::string& key, const std::string& name, double min, double max, double def,
                   Unit unit, std::string d) {
        return add(key, name, ParamType::Float, min, max, def, unit, std::move(d));
    }
    // 0..1, shown as a percentage.
    ParamDef& amount(const std::string& key, const std::string& name, double def, std::string d) {
        return real(key, name, 0, 1, def, Unit::Percent, std::move(d));
    }
    // -1..1, shown as a percentage.
    ParamDef& bipolar(const std::string& key, const std::string& name, double def, std::string d) {
        return real(key, name, -1, 1, def, Unit::Percent, std::move(d));
    }
    ParamDef& level(const std::string& key, const std::string& name, double def, std::string d) {
        return real(key, name, kSilenceDb, 6, def, Unit::Db, std::move(d)).smooth(Smoothing::Sample);
    }
    ParamDef& freq(const std::string& key, const std::string& name, double min, double max, double def,
                   std::string d) {
        auto& p = real(key, name, min, max, def, Unit::Hz, std::move(d));
        p.skew = Skew::Log;
        return p;
    }
    // 0..max milliseconds with fine resolution near zero.
    ParamDef& time(const std::string& key, const std::string& name, double max, double def, std::string d) {
        auto& p = real(key, name, 0, max, def, Unit::Ms, std::move(d));
        p.skew = Skew::Power;
        p.skewExp = 4.0;
        return p;
    }
    // min..max milliseconds on a logarithmic scale (min > 0).
    ParamDef& timeLog(const std::string& key, const std::string& name, double min, double max, double def,
                      std::string d) {
        auto& p = real(key, name, min, max, def, Unit::Ms, std::move(d));
        p.skew = Skew::Log;
        return p;
    }
    ParamDef& division(const std::string& key, const std::string& name, std::string_view def, std::string d) {
        return choice(key, name, tempoDivisions(), def, std::move(d));
    }
};

void declareGlobal(Builder& b) {
    b.begin("global", "Global", "Instrument-wide performance, tuning, quality and output settings.", "/global",
            Scope::Global);
    b.choice("mode", "Voice Mode", {"poly", "mono", "legato"}, "poly",
             "Poly plays chords; Mono plays one note and retriggers; Legato plays one note and keeps the "
             "envelopes running while keys overlap.").noAuto();
    b.integer("polyphony", "Polyphony", 1, kMaxPolyphony, 16, Unit::None,
              "Largest number of notes sounding at once; also the voice limit.").noAuto();
    b.time("glide", "Glide Time", 5000, 0, "Time for pitch to slide from the previous note to the new one.");
    b.bipolar("glide_curve", "Glide Curve", 0,
              "Shape of the slide: 0 is linear in pitch, positive arrives quickly then eases, negative starts slowly.");
    b.choice("glide_mode", "Glide Mode", {"always", "legato"}, "always",
             "Slide on every note, or only when a new key is pressed while another is held.");
    b.toggle("glide_scaled", "Glide Scaled", false,
             "When on, the glide time is per octave, so larger intervals take longer.");
    b.real("bend_up", "Bend Up", 0, 48, 2, Unit::Semitones, "Pitch-bend range upward.")
        .at("/global/bend_range/0").noMod();
    b.real("bend_down", "Bend Down", 0, 48, 2, Unit::Semitones, "Pitch-bend range downward.")
        .at("/global/bend_range/1").noMod();
    b.level("master", "Master Volume", -6, "Output level after the effects.");
    b.real("master_tune", "Master Tune", -100, 100, 0, Unit::Cents,
           "Fine tuning of the whole instrument, to 0.1 cent.").smooth(Smoothing::Sample);
    b.real("reference", "Reference Pitch", 400, 480, 440, Unit::Hz, "Frequency of the A above middle C.")
        .noMod().smooth(Smoothing::Sample);
    b.choice("quality", "Quality", {"draft", "standard", "high", "ultra"}, "standard",
             "Oversampling used while playing: Draft none, Standard 2x, High 4x, Ultra 8x on warped and "
             "nonlinear paths.").noAuto();
    b.choice("quality_offline", "Offline Quality", {"draft", "standard", "high", "ultra"}, "high",
             "Quality used when the host renders faster than real time and for offline renders.").noAuto();
    b.choice("limiter", "Output Protection", {"off", "soft_clip", "limiter"}, "off",
             "Optional soft clipper or limiter on the final output.").noAuto();
    b.toggle("dc_filter", "DC Filter", true, "Removes any constant offset from the output.").noAuto();
    b.toggle("fx_off", "FX Off", false, "Bypasses the whole effects rack.");
    b.toggle("midi_out", "MIDI Out", true,
             "Sends notes made by the arpeggiator, chord tools and clip player to the host.").noAuto();
    b.toggle("midi_thru", "MIDI Thru", false, "Passes incoming MIDI on to the host unchanged.").noAuto();
    b.toggle("mpe", "MPE", false, "Treats each MIDI channel as one note with its own bend, pressure and slide.")
        .noAuto();
    b.real("mpe_bend_range", "MPE Bend Range", 0, 96, 48, Unit::Semitones, "Per-note pitch-bend range in MPE mode.")
        .noAuto().noMod();
    b.choice("follower.source", "Follower Source", {"internal", "sidechain"}, "internal",
             "Signal the envelope follower listens to: the instrument before its effects, or the side-chain input.")
        .noAuto();
    b.time("follower.attack", "Follower Attack", 1000, 10, "How quickly the envelope follower rises.").noMod();
    b.time("follower.release", "Follower Release", 5000, 150, "How quickly the envelope follower falls.").noMod();
    b.end();
}

void declareVoice(Builder& b) {
    b.begin("voice", "Voice", "Voice management, filter routing and analog-style character.", "/voice", Scope::Voice);
    b.choice("steal", "Stealing", {"oldest", "quietest", "release"}, "oldest",
             "Which voice gives way when the limit is reached: the oldest, the quietest, or one that is "
             "already releasing (falling back to the oldest).").noAuto();
    b.choice("priority", "Note Priority", {"last", "low", "high"}, "last",
             "Which held key sounds in Mono and Legato modes.").noAuto();
    b.amount("velocity_sens", "Velocity Sensitivity", 0.5, "How strongly key velocity scales the level.").noMod();
    b.amount("filter_routing", "Filter Routing", 0,
             "Blend between the filters in series (0 %: Filter 1 into Filter 2) and in parallel (100 %).");
    b.amount("drift", "Drift", 0, "Slow random pitch wander, different for every voice.");
    b.amount("variance.cutoff", "Cutoff Variance", 0, "Random filter cutoff offset chosen per note.").noMod();
    b.amount("variance.env", "Envelope Variance", 0, "Random scaling of envelope times chosen per note.").noMod();
    b.amount("variance.pan", "Pan Variance", 0, "Random pan offset chosen per note.").noMod();
    b.amount("variance.level", "Level Variance", 0, "Random level offset chosen per note.").noMod();
    b.amount("warmth", "Warmth", 0, "Gentle saturation before the filters with a soft high-frequency tilt.");
    b.amount("noise_floor", "Noise Floor", 0, "A low bed of analog-style noise under each voice.");
    b.end();
}

void declareOsc(Builder& b, int n) {
    const std::string id = "osc" + std::to_string(n);
    b.begin(id, "Osc " + std::to_string(n),
            "Oscillator with a wavetable engine and a low-cost classic engine, two warp slots and unison.",
            "/" + id, Scope::Voice);
    b.toggle("on", "On", n == 1, "Power switch; an oscillator that is off costs nothing.");
    b.choice("engine", "Engine", {"wavetable", "classic"}, "wavetable",
             "Wavetable plays the loaded table; Classic is a virtual-analog saw, square, triangle and sine.")
        .noAuto();
    b.amount("position", "Position", 0, "Where in the wavetable the oscillator plays, continuous across frames.");
    b.integer("octave", "Octave", -4, 4, 0, Unit::Octaves, "Pitch offset in octaves.");
    b.integer("semi", "Semitones", -12, 12, 0, Unit::Semitones, "Pitch offset in semitones.").at("/" + id + "/semi");
    b.real("fine", "Fine", -100, 100, 0, Unit::Cents, "Fine pitch offset.").smooth(Smoothing::Sample);
    b.real("coarse", "Coarse", -64, 64, 0, Unit::Semitones,
           "Continuous pitch offset, intended as a modulation destination.").smooth(Smoothing::Sample);
    b.level("level", "Level", -6, "Output level of the oscillator.");
    b.bipolar("pan", "Pan", 0, "Stereo position.").smooth(Smoothing::Sample);
    b.real("phase", "Phase", 0, 360, 0, Unit::Degrees, "Start phase of each note.").noMod();
    b.choice("phase_mode", "Phase Mode", {"reset", "continue"}, "reset",
             "Reset starts every note at the start phase; Continue carries on from the last note.").noAuto();
    b.amount("phase_random", "Phase Random", 1, "How much the start phase is randomized per note.").noMod();
    b.toggle("keytrack", "Key Tracking", true,
             "On follows the keyboard; off plays a fixed pitch for drones, percussion and modulator duty.").noAuto();
    b.amount("classic.shape", "Classic Shape", 0, "Classic engine: morphs saw, square, triangle and sine.");
    b.amount("classic.pulse_width", "Classic Pulse Width", 0.5, "Classic engine: width of the square's high half.");
    for (const char* slot : {"warp_a", "warp_b"}) {
        const std::string key = slot;
        const std::string label = key == "warp_a" ? "Warp A" : "Warp B";
        b.choice(key + ".mode", label + " Mode", kWarpModes, "off",
                 "What this warp slot does to the cycle; pm, am and rm take another source as the modulator.");
        b.amount(key + ".amount", label + " Amount", 0,
                 "Strength of the warp; two-sided modes (bend_both, skew_both) are neutral at 50 %.");
        b.choice(key + ".source", label + " Source", kSources, n == 1 ? "osc2" : "osc1",
                 "The source used as modulator by the pm, am and rm modes; it may be silent in the mix.").noAuto();
    }
    b.integer("unison.voices", "Unison Voices", 1, 16, 1, Unit::None, "Number of stacked detuned copies.");
    b.amount("unison.detune", "Unison Detune", 0.2, "Detune spread as a share of the unison range.");
    b.real("unison.range", "Unison Range", 0, 48, 2, Unit::Semitones, "Largest detune spread.").noMod();
    b.amount("unison.blend", "Unison Blend", 0.7, "Level of the side voices against the centre voice.");
    b.amount("unison.width", "Unison Width", 0.8, "Stereo spread of the unison voices.");
    b.bipolar("unison.position_spread", "Unison Position Spread", 0, "Spreads wavetable position across the voices.");
    b.bipolar("unison.warp_spread", "Unison Warp Spread", 0, "Spreads the warp amount across the voices.");
    b.choice("unison.law", "Unison Spread Law", {"linear", "ensemble", "exponential", "inverse", "random"},
             "linear", "How detune is distributed: evenly, denser near the centre, exponential, inverse, or random per note.")
        .noAuto();
    b.choice("unison.stack", "Unison Stack",
             {"off", "octave", "two_octaves", "fifths", "fifths_octaves", "center_down_12", "center_down_24"}, "off",
             "Transposes alternate voices: +12, +12/+24, fifths, fifths and octaves, or the centre voice down.")
        .noAuto();
    b.choice("unison.phase", "Unison Start Phase", {"free", "random", "consistent"}, "random",
             "Free keeps running, Random differs per note, Consistent reuses one stored set of random phases.")
        .noAuto();
    b.amount("sends.filter_balance", "Filter Balance", 0, "Share sent to Filter 1 (0 %) or Filter 2 (100 %).");
    b.amount("sends.direct", "Direct", 0, "Share that skips both filters.");
    b.end();
}

void declareSub(Builder& b) {
    b.begin("sub", "Sub", "A simple oscillator below the note, also usable as a modulator.", "/sub", Scope::Voice);
    b.toggle("on", "On", false, "Power switch.");
    b.choice("shape", "Shape", {"sine", "triangle", "saw", "square", "pulse25", "rounded_square"}, "sine",
             "Waveform of the sub oscillator.");
    b.integer("octave", "Octave", -4, 0, -1, Unit::Octaves, "Octave below the played note.");
    b.real("coarse", "Coarse", -48, 48, 0, Unit::Semitones,
           "Continuous pitch offset, so the sub can modulate at any ratio.").smooth(Smoothing::Sample);
    b.real("fine", "Fine", -100, 100, 0, Unit::Cents, "Fine pitch offset.").smooth(Smoothing::Sample);
    b.level("level", "Level", -6, "Output level.");
    b.bipolar("pan", "Pan", 0, "Stereo position.").smooth(Smoothing::Sample);
    b.toggle("direct", "Direct", false, "Skips the filters.");
    b.amount("sends.filter_balance", "Filter Balance", 0, "Share sent to Filter 1 (0 %) or Filter 2 (100 %).");
    b.end();
}

void declareTexture(Builder& b) {
    b.begin("texture", "Texture", "A stereo sample player for noise beds and attack layers.", "/texture", Scope::Voice);
    b.toggle("on", "On", false, "Power switch.");
    b.choice("mode", "Mode", {"one_shot", "loop"}, "loop",
             "One-shot plays the sample once; Loop repeats it, using loop points stored in the file when present.")
        .noAuto();
    b.amount("start", "Start", 0, "Where in the sample playback begins.");
    b.amount("random_start", "Random Start", 0, "How much the start position is randomized per note.").noMod();
    b.toggle("keytrack", "Key Tracking", false, "When on, the sample is transposed with the keyboard.").noAuto();
    b.real("pitch", "Pitch", -48, 48, 0, Unit::Semitones, "Transposition.").smooth(Smoothing::Sample);
    b.real("fine", "Fine", -100, 100, 0, Unit::Cents, "Fine pitch offset.").smooth(Smoothing::Sample);
    b.level("level", "Level", -12, "Output level.");
    b.bipolar("pan", "Pan", 0, "Stereo position.").smooth(Smoothing::Sample);
    b.toggle("direct", "Direct", false, "Skips the filters.");
    b.amount("sends.filter_balance", "Filter Balance", 0, "Share sent to Filter 1 (0 %) or Filter 2 (100 %).");
    b.end();
}

void declareFilter(Builder& b, int n) {
    const std::string id = "filter" + std::to_string(n);
    b.begin(id, "Filter " + std::to_string(n), "One of the two per-voice filters.", "/" + id, Scope::Voice);
    b.toggle("on", "On", false, "Power switch; a filter that is off passes the signal unchanged.");
    b.choice("type", "Type", filterTypes(), "svf_lp12", "Filter family and response.");
    b.freq("cutoff", "Cutoff", 8, 22000, 2000, "Cutoff or centre frequency.").smooth(Smoothing::Sample);
    b.amount("resonance", "Resonance", 0, "Emphasis around the cutoff; some types self-oscillate at the top.");
    b.amount("drive", "Drive", 0, "Input gain into the filter's nonlinear stages.");
    b.amount("character", "Character", 0.5,
             "Type-dependent control (body, heat, second cutoff, morph position, damping, formant shift, gain); "
             "two-sided controls are neutral at 50 %.");
    b.bipolar("stereo_offset", "Stereo Offset", 0, "Moves the left and right cutoffs apart.");
    b.amount("keytrack", "Key Tracking", 0, "How far the cutoff follows the keyboard.").noMod();
    b.amount("mix", "Mix", 1, "Dry/wet mix, or output level when Mix Mode is Level.").smooth(Smoothing::Sample);
    b.choice("mix_mode", "Mix Mode", {"mix", "level"}, "mix",
             "Mix blends dry and filtered signal; Level trims the filtered output with no dry signal.").noAuto();
    b.end();
}

void declareEnv(Builder& b, int n) {
    const std::string id = "env" + std::to_string(n);
    b.begin(id, "Env " + std::to_string(n),
            n == 1 ? "The amp envelope; also a modulation source." : "A modulation envelope.", "/" + id,
            Scope::Voice);
    b.time("delay", "Delay", 30000, 0, "Wait before the attack starts.");
    b.time("attack", "Attack", 30000, 1, "Time to rise to full level.");
    b.time("hold", "Hold", 30000, 0, "Time held at full level before the decay.");
    b.time("decay", "Decay", 30000, 300, "Time to fall to the sustain level.");
    b.amount("sustain", "Sustain", 1, "Level held while the key is down.");
    b.time("release", "Release", 30000, 50, "Time to fall to silence after the key is released.");
    const char* curveHelp = ": 0 is linear, positive moves fast at first then eases, negative the reverse.";
    b.bipolar("attack_curve", "Attack Curve", 0, std::string("Shape of the attack") + curveHelp).noMod();
    b.bipolar("decay_curve", "Decay Curve", 0.5, std::string("Shape of the decay") + curveHelp).noMod();
    b.bipolar("release_curve", "Release Curve", 0.5, std::string("Shape of the release") + curveHelp).noMod();
    if (n > 1)
        b.choice("mode", "Mode", {"standard", "multi"}, "standard",
                 "Standard is delay-attack-hold-decay-sustain-release; Multi is a drawn multi-segment "
                 "envelope with a sustain point and loop region.").noAuto();
    b.end();
}

void declareLfo(Builder& b, int n) {
    const std::string id = "lfo" + std::to_string(n);
    b.begin(id, "LFO " + std::to_string(n), "A drawn-shape low-frequency oscillator.", "/" + id, Scope::Voice);
    b.choice("mode", "Mode", {"free", "retrigger", "one_shot", "loop_point"}, "retrigger",
             "Free ignores notes, Retrigger restarts on each note, One-shot plays once like an envelope, "
             "Loop-point plays through then loops from the shape's loop marker.").noAuto();
    b.freq("rate", "Rate", 0.01, 2000, 2, "Free rate; values above 100 Hz take effect with audio-rate mode on.");
    b.toggle("sync", "Sync", false, "Locks the rate to the host tempo using the division.");
    b.division("division", "Division", "1/4", "Length of one cycle when synced.");
    b.toggle("anchor", "Anchor", false, "Keeps a synced LFO locked to song position when its rate changes.").noAuto();
    b.real("phase", "Phase", 0, 360, 0, Unit::Degrees, "Start phase.");
    b.time("delay", "Delay", 10000, 0, "Wait after note-on before the LFO starts.").noMod();
    b.time("rise", "Rise", 10000, 0, "Fade-in time of the LFO depth.").noMod();
    b.amount("smooth", "Smooth", 0, "Rounds off steps and corners in the shape.");
    b.choice("polarity", "Polarity", {"bipolar", "unipolar"}, "bipolar", "Output range: -1..1 or 0..1.").noAuto();
    b.choice("scope", "Scope", {"voice", "global"}, "voice",
             "Per-voice LFOs run separately for every note; a global LFO is shared by all.").noAuto();
    b.amount("keytrack", "Rate Key Tracking", 0, "How far the rate follows the keyboard.").noMod();
    b.toggle("audio_rate", "Audio Rate", false, "Evaluates the LFO every sample so it can run up to 2 kHz.").noAuto();
    b.end();
}

void declareFlux(Builder& b, int n) {
    const std::string id = "flux" + std::to_string(n);
    b.begin(id, "Flux " + std::to_string(n), "A random and chaos generator.", "/" + id, Scope::Global);
    b.choice("type", "Type", {"orbit", "burst", "stepped", "smooth"}, "orbit",
             "Orbit is smooth and loosely periodic, Burst restarts irregularly, Stepped is sample-and-hold, "
             "Smooth is filtered noise.").noAuto();
    b.freq("rate", "Rate", 0.01, 100, 1, "Free rate.");
    b.toggle("sync", "Sync", false, "Locks the rate to the host tempo using the division.");
    b.division("division", "Division", "1/4", "Length of one step or cycle when synced.");
    b.choice("scope", "Scope", {"voice", "global"}, "global", "One generator per voice, or one shared by all.").noAuto();
    b.end();
}

void declareMacro(Builder& b, int n) {
    const std::string id = "macro" + std::to_string(n);
    b.begin(id, "Macro " + std::to_string(n), "A named performance control assignable by drag.",
            "/macros/" + std::to_string(n - 1), Scope::Global, ModuleKind::Macro);
    b.amount("value", "Value", 0, "Position of the macro.").noMod();
    b.toggle("bipolar", "Bipolar", false, "When on, the macro's output runs from -1 to 1 with 50 % as zero.").noAuto();
    b.end();
}

void declareModRoute(Builder& b, int n) {
    b.begin("mod." + std::to_string(n), "Mod " + std::to_string(n), "One modulation route.",
            "/mod/" + std::to_string(n - 1), Scope::Global, ModuleKind::ModRoute);
    b.bipolar("amount", "Amount", 0, "Depth of the route; negative values invert it.");
    b.end();
}

void declareFx(Builder& b) {
    b.begin("fx", "FX", "Routing of the three-lane effects rack.", "/fx", Scope::Global, ModuleKind::FxRack);
    b.choice("b_routing", "Lane B Routing", {"parallel", "series"}, "parallel",
             "Lane B runs beside lane A, or after it.").noAuto();
    b.amount("filter1_balance", "Filter 1 Send", 0, "Share of Filter 1's output sent to lane A (0 %) or lane B (100 %).");
    b.amount("filter2_balance", "Filter 2 Send", 0, "Share of Filter 2's output sent to lane A (0 %) or lane B (100 %).");
    b.amount("direct_balance", "Direct Send", 0, "Share of the direct signal sent to lane A (0 %) or lane B (100 %).");
    b.end();
    for (int lane = 0; lane < kNumFxLanes; ++lane) {
        for (int slot = 1; slot <= kNumFxSlots; ++slot) {
            const std::string prefix(fxLaneIdPrefix(lane));
            std::string laneName = prefix == "m" ? "Master" : prefix == "a" ? "A" : "B";
            const std::string base = "/fx/" + std::string(fxLaneKey(lane)) + "/" + std::to_string(slot - 1);
            b.begin("fx." + prefix + std::to_string(slot), "FX " + laneName + std::to_string(slot),
                    "One slot of effects lane " + laneName + ".", base, Scope::Global, ModuleKind::FxSlot);
            b.toggle("on", "On", true, "Power switch of the slot.");
            b.amount("mix", "Mix", 1, "Dry/wet mix of the slot.").smooth(Smoothing::Sample);
            for (int k = 1; k <= kNumFxSlotParams; ++k)
                b.amount("p" + std::to_string(k), "Param " + std::to_string(k), 0,
                         "Generic slot parameter " + std::to_string(k) +
                             ": takes the name, range and unit of the loaded effect's parameter number " +
                             std::to_string(k) + ".").at("");
            b.end();
        }
    }
}

void declarePlay(Builder& b) {
    b.begin("arp", "Arp", "The arpeggiator.", "/arp", Scope::Global);
    b.toggle("on", "On", false, "Power switch.");
    b.choice("mode", "Mode", {"up", "down", "up_down", "up_down_repeat", "random", "walk", "as_played", "chord"},
             "up", "Order in which held notes are played.");
    b.integer("octaves", "Octaves", 1, 4, 1, Unit::None, "Number of octaves the pattern spans.");
    b.division("rate", "Rate", "1/16", "Length of one step.");
    b.real("gate", "Gate", 0.01, 2, 0.5, Unit::Percent, "Note length as a share of the step.");
    b.amount("swing", "Swing", 0, "Delays every second step.");
    b.toggle("latch", "Latch", false, "Keeps the pattern running after the keys are released.");
    b.toggle("restart", "Restart", true, "Restarts the pattern when a new note starts from silence.").noAuto();
    b.integer("steps", "Steps", 1, 32, 16, Unit::None, "Length of the step lane.").noAuto();
    b.integer("pattern", "Pattern", 1, 12, 1, Unit::None, "Which of the twelve pattern slots plays.");
    b.end();

    b.begin("chord", "Chord", "Chord memory, scale lock and strum.", "/chord", Scope::Global);
    b.toggle("memory", "Memory", false, "Plays the stored chord from a single key.");
    b.toggle("scale_lock", "Scale Lock", false, "Keeps played notes inside the chosen key and scale.");
    b.choice("key", "Key", {"c", "c_sharp", "d", "d_sharp", "e", "f", "f_sharp", "g", "g_sharp", "a", "a_sharp", "b"},
             "c", "Root note of the scale.").noAuto();
    b.choice("scale", "Scale",
             {"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian", "harmonic_minor",
              "melodic_minor", "major_pentatonic", "minor_pentatonic", "blues", "whole_tone", "chromatic"},
             "major", "Scale used by scale lock.").noAuto();
    b.choice("lock_mode", "Lock Mode", {"snap", "filter"}, "snap",
             "Snap moves outside notes to the nearest scale note; Filter drops them.").noAuto();
    b.real("strum", "Strum", 0, 500, 0, Unit::Ms, "Delay between successive notes of a chord.").noMod();
    b.choice("strum_direction", "Strum Direction", {"up", "down", "alternate"}, "up",
             "Order in which the notes of a chord are strummed.").noAuto();
    b.end();

    b.begin("clip", "Clip", "The clip player.", "/clip", Scope::Global);
    b.toggle("on", "On", false, "Power switch.");
    b.integer("slot", "Slot", 1, 8, 1, Unit::None, "Which of the eight clips plays.");
    b.choice("trigger", "Trigger", {"transport", "key"}, "key",
             "Transport follows the host; Key starts the clip from a played note and transposes it.").noAuto();
    b.toggle("latch", "Latch", false, "Keeps the clip playing after the key is released.");
    b.toggle("loop", "Loop", true, "Repeats the clip.");
    b.end();
}

// --- Effects -----------------------------------------------------------------------------------

std::vector<EffectType> declareEffects() {
    std::vector<EffectType> effects;
    std::vector<ModuleInfo> unused;
    const auto make = [&](const char* id, const char* name, const char* description, auto&& declare) {
        EffectType e{id, name, description, {}};
        Builder b{e.params, unused};
        b.begin(id, "", "", "", Scope::Global);
        declare(b);
        for (std::size_t i = 0; i < e.params.size(); ++i) {
            auto& p = e.params[i];
            p.index = static_cast<int>(i);
            p.pointer.erase(0, 1); // effect parameters are plain keys inside the slot object
        }
        assert(e.params.size() <= static_cast<std::size_t>(kNumFxSlotParams));
        effects.push_back(std::move(e));
    };

    make("drive", "Drive", "Distortion with fifteen modes and a pre or post filter.", [](Builder& b) {
        b.choice("mode", "Mode",
                 {"tube", "soft_clip", "hard_clip", "diode_a", "diode_b", "linear_fold", "sine_fold", "rectify",
                  "asymmetric", "bit_crush", "downsample", "sine_shaper", "fuzz", "custom_curve", "dual_curve"},
                 "tube", "Distortion character.");
        b.amount("amount", "Amount", 0.3, "How hard the signal is driven.");
        b.choice("filter_position", "Filter Position", {"off", "pre", "post"}, "off",
                 "Places the filter before or after the distortion.");
        b.amount("filter_morph", "Filter Morph", 0, "Filter response from low-pass through band-pass to high-pass.");
        b.freq("filter_freq", "Filter Frequency", 20, 20000, 2000, "Filter frequency.");
        b.amount("filter_q", "Filter Q", 0.2, "Filter resonance.");
        b.amount("curve_morph", "Curve Morph", 0, "Dual curve mode: blend between the two drawn curves.");
        b.choice("curve_symmetry", "Curve Symmetry", {"symmetric", "asymmetric"}, "symmetric",
                 "Whether drawn curves treat positive and negative halves alike.");
        b.real("level", "Level", -24, 24, 0, Unit::Db, "Output trim.");
    });
    make("compressor", "Compressor", "Single-band compressor with a three-band upward and downward mode.",
         [](Builder& b) {
             b.choice("mode", "Mode", {"single", "multiband"}, "single", "One band, or three bands.");
             b.real("threshold", "Threshold", -60, 0, -18, Unit::Db, "Level above which gain is reduced.");
             b.real("ratio", "Ratio", 1, 20, 4, Unit::None, "Compression ratio; the maximum acts as a limiter.").skew = Skew::Log;
             b.timeLog("attack", "Attack", 0.1, 500, 10, "Attack time.");
             b.timeLog("release", "Release", 5, 2000, 120, "Release time.");
             b.real("makeup", "Make-up", -12, 36, 0, Unit::Db, "Gain after compression.");
             b.amount("upward", "Upward", 0.5, "Multiband: strength of upward compression below each threshold.");
             for (const char* band : {"low", "mid", "high"}) {
                 const std::string k = band;
                 std::string label = k;
                 label[0] = static_cast<char>(label[0] - 'a' + 'A');
                 b.real(k + "_threshold", label + " Threshold", -60, 0, -24, Unit::Db,
                        "Multiband: threshold of the " + k + " band.");
                 b.amount(k + "_amount", label + " Amount", 0.5, "Multiband: compression depth of the " + k + " band.");
             }
             b.freq("crossover_low", "Low Crossover", 40, 1000, 120, "Multiband: frequency between low and mid.");
             b.freq("crossover_high", "High Crossover", 1000, 12000, 2500, "Multiband: frequency between mid and high.");
         });
    make("eq", "EQ", "Four-band equalizer.", [](Builder& b) {
        b.choice("low_type", "Low Type", {"shelf", "bell", "high_pass"}, "shelf", "Shape of the low band.");
        b.freq("low_freq", "Low Frequency", 20, 2000, 100, "Frequency of the low band.");
        b.real("low_gain", "Low Gain", -24, 24, 0, Unit::Db, "Gain of the low band.");
        b.amount("low_q", "Low Q", 0.3, "Width or slope of the low band.");
        for (const char* band : {"mid1", "mid2"}) {
            const std::string k = band;
            const std::string label = k == "mid1" ? "Mid 1" : "Mid 2";
            b.freq(k + "_freq", label + " Frequency", 40, 16000, k == "mid1" ? 500 : 2500, "Frequency of the bell.");
            b.real(k + "_gain", label + " Gain", -24, 24, 0, Unit::Db, "Gain of the bell.");
            b.amount(k + "_q", label + " Q", 0.3, "Width of the bell.");
        }
        b.choice("high_type", "High Type", {"shelf", "bell", "low_pass"}, "shelf", "Shape of the high band.");
        b.freq("high_freq", "High Frequency", 500, 20000, 8000, "Frequency of the high band.");
        b.real("high_gain", "High Gain", -24, 24, 0, Unit::Db, "Gain of the high band.");
        b.amount("high_q", "High Q", 0.3, "Width or slope of the high band.");
    });
    make("filter", "Filter", "Any voice filter type, in stereo.", [](Builder& b) {
        b.choice("filter_type", "Filter Type", filterTypes(), "svf_lp12", "Filter family and response.");
        b.freq("cutoff", "Cutoff", 8, 22000, 2000, "Cutoff or centre frequency.").smooth(Smoothing::Sample);
        b.amount("resonance", "Resonance", 0, "Emphasis around the cutoff.");
        b.amount("drive", "Drive", 0, "Input gain into the nonlinear stages.");
        b.amount("character", "Character", 0.5, "Type-dependent control, as for the voice filters.");
        b.bipolar("stereo_offset", "Stereo Offset", 0, "Moves the left and right cutoffs apart.");
    });
    make("chorus", "Chorus", "Multi-voice chorus.", [](Builder& b) {
        b.integer("voices", "Voices", 2, 6, 4, Unit::None, "Number of chorus voices.");
        b.freq("rate", "Rate", 0.01, 20, 0.6, "Modulation rate.");
        b.toggle("sync", "Sync", false, "Locks the rate to the host tempo.");
        b.division("division", "Division", "1/1", "Length of one modulation cycle when synced.");
        b.timeLog("delay1", "Delay 1", 0.5, 50, 6, "First delay time.");
        b.timeLog("delay2", "Delay 2", 0.5, 50, 14, "Second delay time.");
        b.amount("depth", "Depth", 0.5, "Modulation depth.");
        b.amount("feedback", "Feedback", 0.1, "Amount fed back into the delays.");
        b.choice("filter_type", "Wet Filter", {"low_pass", "high_pass"}, "low_pass", "Filter on the wet signal.");
        b.freq("filter_cutoff", "Wet Filter Cutoff", 20, 20000, 20000, "Cutoff of the wet-signal filter.");
    });
    make("flanger", "Flanger", "Flanger with positive or negative feedback.", [](Builder& b) {
        b.freq("rate", "Rate", 0.01, 20, 0.3, "Modulation rate.");
        b.toggle("sync", "Sync", false, "Locks the rate to the host tempo.");
        b.division("division", "Division", "1/1", "Length of one modulation cycle when synced.");
        b.amount("depth", "Depth", 0.6, "Modulation depth.");
        b.bipolar("feedback", "Feedback", 0.4, "Feedback amount; negative values invert it.");
        b.real("stereo_phase", "Stereo Phase", 0, 360, 90, Unit::Degrees, "Phase offset between left and right.");
    });
    make("phaser", "Phaser", "Phaser with 2 to 12 stages.", [](Builder& b) {
        b.integer("stages", "Stages", 2, 12, 6, Unit::None, "Number of all-pass stages.");
        b.freq("rate", "Rate", 0.01, 20, 0.4, "Modulation rate.");
        b.toggle("sync", "Sync", false, "Locks the rate to the host tempo.");
        b.division("division", "Division", "1/1", "Length of one modulation cycle when synced.");
        b.amount("depth", "Depth", 0.6, "Modulation depth.");
        b.freq("center", "Centre", 40, 12000, 800, "Centre frequency of the sweep.");
        b.bipolar("feedback", "Feedback", 0.3, "Feedback amount; negative values invert it.");
        b.real("stereo_phase", "Stereo Phase", 0, 360, 90, Unit::Degrees, "Phase offset between left and right.");
    });
    make("delay", "Delay", "Dual, ping-pong or pre-tap delay.", [](Builder& b) {
        b.choice("mode", "Mode", {"dual", "ping_pong", "pre_tap"}, "dual", "Delay topology.");
        b.toggle("sync", "Sync", true, "Uses the divisions instead of the times in milliseconds.");
        b.timeLog("time_l", "Time L", 1, 4000, 375, "Left delay time.");
        b.timeLog("time_r", "Time R", 1, 4000, 375, "Right delay time.");
        b.division("division_l", "Division L", "1/8D", "Left delay length when synced.");
        b.division("division_r", "Division R", "1/8D", "Right delay length when synced.");
        b.bipolar("offset_l", "Offset L", 0, "Fine offset of the left synced time.");
        b.bipolar("offset_r", "Offset R", 0, "Fine offset of the right synced time.");
        b.toggle("link", "Link", true, "Makes the right side follow the left.");
        b.amount("feedback", "Feedback", 0.35, "Amount fed back into the delay.");
        b.freq("band_center", "Feedback Band Centre", 40, 16000, 1200, "Centre of the band-pass in the feedback path.");
        b.amount("band_width", "Feedback Band Width", 1, "Width of the band-pass in the feedback path.");
        b.amount("saturation", "Saturation", 0, "Saturation in the feedback path.");
        b.amount("modulation", "Modulation", 0, "Slow modulation of the delay time.");
        b.amount("ducking", "Ducking", 0, "Lowers the echoes while the input is loud.");
    });
    make("reverb", "Reverb", "Plate, hall and room reverb.", [](Builder& b) {
        b.choice("mode", "Mode", {"plate", "hall", "room"}, "hall", "Reverb character.");
        b.amount("size", "Size", 0.5, "Size of the space.");
        b.timeLog("decay", "Decay", 100, 60000, 2000, "Time for the tail to fall by 60 dB.");
        b.real("predelay", "Pre-delay", 0, 500, 10, Unit::Ms, "Gap before the reverb starts.");
        b.freq("low_cut", "Low Cut", 20, 2000, 80, "Removes lows from the reverb.");
        b.freq("high_cut", "High Cut", 500, 20000, 12000, "Removes highs from the reverb.");
        b.amount("damping", "Damping", 0.4, "How quickly highs decay inside the tail.");
        b.amount("modulation", "Modulation", 0.3, "Movement of the delay lines.");
        b.amount("width", "Width", 1, "Stereo width of the tail.");
        b.toggle("freeze", "Freeze", false, "Holds the current tail indefinitely.");
    });
    make("swarm", "Swarm", "Micro-detune ensemble.", [](Builder& b) {
        b.integer("voices", "Voices", 1, 7, 5, Unit::None, "Number of detuned voices.");
        b.freq("rate", "Rate", 0.01, 20, 0.5, "Speed of the detune movement.");
        b.amount("detune", "Detune", 0.3, "Depth of the detune.");
        b.toggle("restart", "Restart", false, "Restarts the movement on each note.");
    });
    make("widener", "Widener", "Four-tap stereo decorrelation.", [](Builder& b) {
        b.amount("size", "Size", 0.4, "Spacing of the taps.");
    });
    make("utility", "Utility", "Gain, pan, width, bass mono, polarity and DC filter.", [](Builder& b) {
        b.real("gain", "Gain", -36, 36, 0, Unit::Db, "Level change.").smooth(Smoothing::Sample);
        b.bipolar("pan", "Pan", 0, "Stereo position.");
        b.real("width", "Width", 0, 2, 1, Unit::Percent, "Stereo width; 0 % is mono.");
        b.real("mono_below", "Mono Below", 0, 500, 0, Unit::Hz, "Makes the signal mono below this frequency; 0 is off.");
        b.choice("polarity", "Polarity", {"normal", "invert_left", "invert_right", "invert_both"}, "normal",
                 "Flips the polarity of one or both channels.");
        b.toggle("dc_filter", "DC Filter", false, "Removes any constant offset.");
    });
    make("limiter", "Limiter", "Look-ahead-free peak limiter.", [](Builder& b) {
        b.real("ceiling", "Ceiling", -24, 0, -0.3, Unit::Db, "Highest output level.");
        b.timeLog("release", "Release", 1, 1000, 80, "Release time.");
    });
    return effects;
}

std::vector<ModSourceDef> declareSources() {
    std::vector<ModSourceDef> s;
    for (int i = 1; i <= kNumEnvs; ++i)
        s.push_back({"env" + std::to_string(i), "Env " + std::to_string(i), Scope::Voice, "Envelope output, 0..1."});
    for (int i = 1; i <= kNumLfos; ++i)
        s.push_back({"lfo" + std::to_string(i), "LFO " + std::to_string(i), Scope::Voice,
                     "LFO output; per voice unless the LFO's scope is global."});
    for (int i = 1; i <= kNumFlux; ++i)
        s.push_back({"flux" + std::to_string(i), "Flux " + std::to_string(i), Scope::Global,
                     "Random or chaos generator output."});
    for (int i = 1; i <= kNumMacros; ++i)
        s.push_back({"macro" + std::to_string(i), "Macro " + std::to_string(i), Scope::Global, "Macro position."});
    const auto add = [&](const char* id, const char* name, Scope scope, const char* description) {
        s.push_back({id, name, scope, description});
    };
    add("velocity", "Velocity", Scope::Voice, "Note-on velocity.");
    add("release_velocity", "Release Velocity", Scope::Voice, "Note-off velocity.");
    add("note", "Note", Scope::Voice, "Key number, centred on middle C.");
    add("pitchbend", "Pitch Bend", Scope::Global, "Pitch wheel position, -1..1.");
    add("modwheel", "Mod Wheel", Scope::Global, "Modulation wheel (controller 1).");
    add("aftertouch", "Aftertouch", Scope::Global, "Channel pressure.");
    add("poly_aftertouch", "Poly Aftertouch", Scope::Voice, "Per-key pressure.");
    add("mpe_pressure", "MPE Pressure", Scope::Voice, "Per-note pressure in MPE mode.");
    add("mpe_slide", "MPE Slide", Scope::Voice, "Per-note slide (controller 74) in MPE mode.");
    add("expression", "Expression", Scope::Global, "Expression pedal (controller 11).");
    add("breath", "Breath", Scope::Global, "Breath controller (controller 2).");
    add("sustain", "Sustain Pedal", Scope::Global, "Sustain pedal (controller 64).");
    add("random1", "Random 1", Scope::Voice, "A new random value for every note.");
    add("random2", "Random 2", Scope::Voice, "A second, independent random value for every note.");
    add("alternate", "Alternate", Scope::Voice, "Flips between -1 and 1 on each note.");
    add("voice_index", "Voice Index", Scope::Voice, "Position of the voice among the sounding voices.");
    add("unison_index", "Unison Index", Scope::Voice, "Position of a unison voice within its stack.");
    add("constant", "Constant", Scope::Global, "Always 1; scales a destination by the route amount.");
    add("osc1_audio", "Osc 1 Audio", Scope::Voice, "Audio-rate output of oscillator 1.");
    add("osc2_audio", "Osc 2 Audio", Scope::Voice, "Audio-rate output of oscillator 2.");
    add("osc3_audio", "Osc 3 Audio", Scope::Voice, "Audio-rate output of oscillator 3.");
    add("sub_audio", "Sub Audio", Scope::Voice, "Audio-rate output of the sub oscillator.");
    add("texture_audio", "Texture Audio", Scope::Voice, "Audio-rate output of the texture player.");
    add("follower", "Envelope Follower", Scope::Global, "Level of the followed signal.");
    add("bar_phase", "Bar Phase", Scope::Global, "Position inside the host's current bar, 0..1.");
    return s;
}

} // namespace

std::string_view fxLaneIdPrefix(int lane) { return lane == 0 ? "a" : lane == 1 ? "b" : "m"; }
std::string_view fxLaneKey(int lane) { return lane == 0 ? "a" : lane == 1 ? "b" : "master"; }

const std::vector<std::string>& tempoDivisions() {
    static const std::vector<std::string> divisions = [] {
        std::vector<std::string> d = {"8/1", "4/1", "2/1"};
        for (int n : {1, 2, 4, 8, 16, 32, 64}) {
            const std::string base = "1/" + std::to_string(n);
            d.push_back(base + "D");
            d.push_back(base);
            d.push_back(base + "T");
        }
        d.emplace_back("1/128");
        return d;
    }();
    return divisions;
}

double divisionInBeats(std::string_view token) {
    double scale = 1.0;
    if (!token.empty() && token.back() == 'D') {
        scale = 1.5;
        token.remove_suffix(1);
    } else if (!token.empty() && token.back() == 'T') {
        scale = 2.0 / 3.0;
        token.remove_suffix(1);
    }
    const auto slash = token.find('/');
    if (slash == std::string_view::npos)
        return 0.0;
    int num = 0, den = 0;
    const auto a = std::from_chars(token.data(), token.data() + slash, num);
    const auto b = std::from_chars(token.data() + slash + 1, token.data() + token.size(), den);
    if (a.ec != std::errc() || b.ec != std::errc() || a.ptr != token.data() + slash ||
        b.ptr != token.data() + token.size() || num <= 0 || den <= 0)
        return 0.0;
    return 4.0 * num / den * scale;
}

Registry::Registry() {
    Builder b{params_, modules_};
    declareGlobal(b);
    declareVoice(b);
    for (int i = 1; i <= kNumOscs; ++i)
        declareOsc(b, i);
    declareSub(b);
    declareTexture(b);
    for (int i = 1; i <= kNumFilters; ++i)
        declareFilter(b, i);
    for (int i = 1; i <= kNumEnvs; ++i)
        declareEnv(b, i);
    for (int i = 1; i <= kNumLfos; ++i)
        declareLfo(b, i);
    for (int i = 1; i <= kNumFlux; ++i)
        declareFlux(b, i);
    macroFirst_ = static_cast<int>(params_.size());
    for (int i = 1; i <= kNumMacros; ++i)
        declareMacro(b, i);
    modFirst_ = static_cast<int>(params_.size());
    for (int i = 1; i <= kNumModRoutes; ++i)
        declareModRoute(b, i);
    declareFx(b);
    declarePlay(b);
    // New parameters are appended here, in new declare calls, never inserted above.

    effects_ = declareEffects();
    sources_ = declareSources();

    for (std::size_t i = 0; i < params_.size(); ++i) {
        auto& p = params_[i];
        p.index = static_cast<int>(i);
        p.hostId = hostIdFor(p.id);
    }
    for (const auto& p : params_) {
        byId_.emplace(p.id, p.index);
        byHostId_.emplace(p.hostId, p.index);
    }
    fxFirst_ = indexOf("fx.a1.on");
    fxStride_ = 2 + kNumFxSlotParams;
}

const Registry& Registry::get() {
    static const Registry instance;
    return instance;
}

int Registry::indexOf(std::string_view id) const {
    const auto it = byId_.find(id);
    return it == byId_.end() ? -1 : it->second;
}

const ParamDef* Registry::find(std::string_view id) const {
    const int i = indexOf(id);
    return i < 0 ? nullptr : &params_[static_cast<std::size_t>(i)];
}

const ParamDef* Registry::findByHostId(std::uint32_t hostId) const {
    const auto it = byHostId_.find(hostId);
    return it == byHostId_.end() ? nullptr : &params_[static_cast<std::size_t>(it->second)];
}

int Registry::effectIndex(std::string_view id) const {
    for (std::size_t i = 0; i < effects_.size(); ++i)
        if (effects_[i].id == id)
            return static_cast<int>(i);
    return -1;
}

bool Registry::isModSource(std::string_view id) const {
    for (const auto& s : sources_)
        if (s.id == id)
            return true;
    if (id.size() >= 3 && id.size() <= 5 && id.substr(0, 2) == "cc") {
        int n = -1;
        const auto r = std::from_chars(id.data() + 2, id.data() + id.size(), n);
        return r.ec == std::errc() && r.ptr == id.data() + id.size() && n >= 0 && n <= 127 &&
               (id.size() == 3 || id[2] != '0');
    }
    return false;
}

bool Registry::fxSlotOf(int index, int& lane, int& slot, int& field) const {
    const int rel = index - fxFirst_;
    if (rel < 0 || rel >= kNumFxLanes * kNumFxSlots * fxStride_)
        return false;
    lane = rel / fxStride_ / kNumFxSlots;
    slot = rel / fxStride_ % kNumFxSlots;
    field = rel % fxStride_;
    return true;
}

bool Registry::fxSlotParamAt(int index, int& lane, int& slot, int& k) const {
    int field = 0;
    if (!fxSlotOf(index, lane, slot, field) || field < 2)
        return false;
    k = field - 2;
    return true;
}

} // namespace zed
