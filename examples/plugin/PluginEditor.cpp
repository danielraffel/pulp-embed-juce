#include "PluginEditor.h"

#include <cstdlib>

#ifndef PULP_EMBED_DEMO_IR
 #define PULP_EMBED_DEMO_IR ""
#endif

namespace {
constexpr int kW = 1000;
constexpr int kH = 600;

juce::File outDir() {
    if (auto* e = std::getenv("PULP_EMBED_OUT"); e && *e)
        return juce::File(juce::String::fromUTF8(e));
    return juce::File("/tmp");
}
bool wantSelfCheck() {
    auto* e = std::getenv("PULP_EMBED_SELFCHECK");
    return e && *e && juce::String(e) != "0";
}
}

PulpEmbedJuceEditor::PulpEmbedJuceEditor(PulpEmbedJuceProcessor& p)
    : juce::AudioProcessorEditor(&p) {
    // Bind the embedded design's controls to this plugin's APVTS parameters
    // (control key == parameter ID == the control's widget id). A dragged knob
    // writes the host param; host automation pushes values back into the UI.
    // Controls without a matching parameter id stay visual-only.
    //
    // PULP_EMBED_BUNDLE overrides the baked-in demo bundle, so a self-check or
    // dev run can point at a param-bound bundle (one whose control ids match
    // "gain"/"mix") and see boundParameterCount() > 0. The bundled figma demo is
    // visual-only, so it binds 0 — that is expected, not a failure.
    juce::File source{PULP_EMBED_DEMO_IR};
    if (auto* b = std::getenv("PULP_EMBED_BUNDLE"); b && *b)
        source = juce::File(juce::String::fromUTF8(b));
    embed_ = std::make_unique<pulp_juce::PulpEmbedComponent>(source, kW, kH, p);
    if (!embed_->isValid())
        juce::Logger::writeToLog("Pulp embed failed: " + embed_->lastError());
    addAndMakeVisible(*embed_);
    setSize(kW, kH);

    selfCheck_ = wantSelfCheck();
    if (selfCheck_)
        startTimer(250);  // poll until attached, then capture + quit
}

PulpEmbedJuceEditor::~PulpEmbedJuceEditor() { stopTimer(); }

void PulpEmbedJuceEditor::resized() {
    if (embed_) embed_->setBounds(getLocalBounds());
}

void PulpEmbedJuceEditor::timerCallback() {
    if (embed_ == nullptr) { stopTimer(); return; }
    // Wait until the embed has attached and drawn a few live frames.
    if (!embed_->isOpened()) { if (++waits_ < 40) return; }
    if (++frames_ < 6) return;  // ~1.5s of live frames after attach

    const auto dir = outDir();
    const bool live = embed_->writeCapturePng(dir.getChildFile("juce-plugin-live-capture.png"));
    const bool det  = embed_->writeRenderPng(dir.getChildFile("juce-plugin-render.png"), kW, kH);
    const int bound = embed_->boundParameterCount();
    juce::Logger::writeToLog(juce::String("SELFCHECK opened=") + (embed_->isOpened() ? "1" : "0")
        + " gpu=" + (embed_->isGpuBacked() ? "1" : "0")
        + " liveCapture=" + (live ? "ok" : "FAIL")
        + " deterministic=" + (det ? "ok" : "FAIL")
        + " boundParams=" + juce::String(bound));
    fprintf(stderr, "SELFCHECK opened=%d gpu=%d liveCapture=%s deterministic=%s boundParams=%d\n",
            embed_->isOpened() ? 1 : 0, embed_->isGpuBacked() ? 1 : 0,
            live ? "ok" : "FAIL", det ? "ok" : "FAIL", bound);
    stopTimer();
    juce::JUCEApplicationBase::quit();
}
