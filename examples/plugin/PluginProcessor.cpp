#include "PluginProcessor.h"
#include "PluginEditor.h"

juce::AudioProcessorValueTreeState::ParameterLayout
PulpEmbedJuceProcessor::createParameterLayout() {
    // Parameter IDs double as the embedded design's control keys: a design
    // control whose key is "gain" / "mix" binds to these. Demonstrates the
    // adapter's host parameter bridge (PulpEmbedComponent's processor ctor).
    return {
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"gain", 1}, "Gain",
            juce::NormalisableRange<float>(0.0f, 1.0f), 0.8f),
        std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"mix", 1}, "Dry/Wet",
            juce::NormalisableRange<float>(0.0f, 1.0f), 1.0f),
    };
}

PulpEmbedJuceProcessor::PulpEmbedJuceProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, juce::Identifier("PARAMS"), createParameterLayout()) {}

void PulpEmbedJuceProcessor::prepareToPlay(double, int) {}

bool PulpEmbedJuceProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    // Match input and output layouts; allow mono or stereo.
    const auto& out = layouts.getMainOutputChannelSet();
    if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
        return false;
    return layouts.getMainInputChannelSet() == out;
}

void PulpEmbedJuceProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                          juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    // Silent passthrough — clear any extra output channels, leave the rest as a
    // pass of input (a real plugin would process here). Silent keeps auval and
    // any host scan from producing surprise audio.
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear(ch, 0, buffer.getNumSamples());
}

juce::AudioProcessorEditor* PulpEmbedJuceProcessor::createEditor() {
    return new PulpEmbedJuceEditor(*this);
}

// This creates the plugin instances for the format wrappers.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new PulpEmbedJuceProcessor();
}
