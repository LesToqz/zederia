#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <cmath>
class P : public juce::AudioProcessor {
public:
  P() : AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true)) {}
  const juce::String getName() const override { return "ZedSmoke"; }
  void prepareToPlay(double sr, int) override { rate = sr; }
  void releaseResources() override {}
  void processBlock(juce::AudioBuffer<float>& b, juce::MidiBuffer&) override {
    for (int i = 0; i < b.getNumSamples(); ++i) {
      float s = 0.1f * std::sin(ph); ph += 6.2831853f * 440.0f / (float) rate; if (ph > 6.2831853f) ph -= 6.2831853f;
      for (int c = 0; c < b.getNumChannels(); ++c) b.setSample(c, i, s);
    }
  }
  juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }
  bool hasEditor() const override { return true; }
  bool acceptsMidi() const override { return true; }
  bool producesMidi() const override { return false; }
  double getTailLengthSeconds() const override { return 0; }
  int getNumPrograms() override { return 1; } int getCurrentProgram() override { return 0; }
  void setCurrentProgram(int) override {} const juce::String getProgramName(int) override { return {}; }
  void changeProgramName(int, const juce::String&) override {}
  void getStateInformation(juce::MemoryBlock&) override {} void setStateInformation(const void*, int) override {}
private: double rate = 44100; float ph = 0;
};
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new P(); }
