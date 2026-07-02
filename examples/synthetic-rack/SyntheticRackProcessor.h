// SyntheticRackProcessor — the JUCE side of the canonical fixture. A real
// juce::AudioProcessor whose APVTS carries:
//
//   • two STATIC params    — "input.gain", "mix"
//   • a paged EFFECT RACK   — kSlotCount slots × {gain, cutoff}, each a real
//                             AudioParameterFloat keyed "slot<N>.<id>" (the
//                             EffectHost shape).
//
// The parameter IDs ARE the SyntheticRackView element keys, so the embed binds
// by key==paramID: the static knobs bind at create, and each rack slot's params
// exist for the plugin's whole life, so hostHasParam("slot2.cutoff") is true
// regardless of which slot the view is currently paged to. Display text comes
// from AudioProcessorParameter::getText via the adapter's hostParamDisplayText.
//
// DSP is silent passthrough — this fixture is about the UI runtime surfaces, not
// audio. Keeping it silent means auval / a host scan never emits surprise audio.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "SyntheticRackView.h"

class SyntheticRackProcessor final : public juce::AudioProcessor {
public:
    SyntheticRackProcessor()
        : AudioProcessor(BusesProperties()
                             .withInput("Input", juce::AudioChannelSet::stereo(), true)
                             .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
          apvts(*this, nullptr, juce::Identifier("SYNTHRACK"), createLayout()) {}

    // ── EffectHost-style accessors by (slot, id) key ─────────────────────────
    // A real paged rack would resolve these dynamically; here every slot param is
    // a live APVTS parameter, so a key always resolves. Provided so a consumer can
    // read/write a slot without knowing the flat APVTS index.
    juce::RangedAudioParameter* slotParam(int slot, int paramIdx) {
        return apvts.getParameter(synth_rack::slotKey(slot, paramIdx).c_str());
    }
    juce::String slotDisplayText(int slot, int paramIdx, float normalized) {
        if (auto* p = slotParam(slot, paramIdx))
            return p->getText(normalized, 0);
        return {};
    }

    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override {
        const auto& out = layouts.getMainOutputChannelSet();
        if (out != juce::AudioChannelSet::mono() && out != juce::AudioChannelSet::stereo())
            return false;
        return layouts.getMainInputChannelSet() == out;
    }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override {
        juce::ScopedNoDenormals noDenormals;
        for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
            buffer.clear(ch, 0, buffer.getNumSamples());  // silent passthrough
    }

    juce::AudioProcessorEditor* createEditor() override;  // in PluginMain.cpp
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "SyntheticRack"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock& dest) override {
        if (auto state = apvts.copyState(); state.isValid())
            if (auto xml = state.createXml())
                copyXmlToBinary(*xml, dest);
    }
    void setStateInformation(const void* data, int size) override {
        if (auto xml = getXmlFromBinary(data, size))
            apvts.replaceState(juce::ValueTree::fromXml(*xml));
    }

    juce::AudioProcessorValueTreeState apvts;

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout() {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;
        const auto range = juce::NormalisableRange<float>(0.0f, 1.0f);
        // Static params — keys match the view's static knobs.
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"input.gain", 1}, "Input Gain", range, 0.8f));
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{"mix", 1}, "Dry/Wet", range, 1.0f));
        // Paged rack params — one real parameter per (slot, id). These all exist
        // for the plugin's life; the view pages which two it shows.
        for (int slot = 0; slot < synth_rack::kSlotCount; ++slot)
            for (int p = 0; p < synth_rack::kSlotParamCount; ++p) {
                const juce::String key{synth_rack::slotKey(slot, p).c_str()};
                layout.add(std::make_unique<juce::AudioParameterFloat>(
                    juce::ParameterID{key, 1}, key, range, 0.5f));
            }
        return layout;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SyntheticRackProcessor)
};
