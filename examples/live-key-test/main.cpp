// The host->UI pump against a LIVE key set, and the host step-count seam.
//
// A paged/tabbed control re-keys its elements from inside the view — no host
// call, no reload — so a pump that walks a mount-time snapshot of the key set
// keeps pushing the OLD keys and the re-keyed control silently stops tracking
// automation. These tests drive the real thing: re-key the view, then prove the
// pump re-resolves and the re-keyed element receives its new parameter's value.
//
// They also prove the gate: an idle UI must NOT re-enumerate the key set every
// tick (keyResolveCount holds constant), and a re-key must fire it exactly once.
//
// The step-count seam is exercised through PulpEmbedComponent's public accessor,
// which the v10 host_param_steps callback trampolines into — so proving the seam
// proves the callback, without needing the v10 runtime present.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PulpEmbedComponent.h"

#include <pulp/view/design_frame_view.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool approx(double a, double b) { return std::fabs(a - b) < 1e-3; }

// Two rack slots plus a continuous and a choice parameter, so the tests can
// re-key between real parameters and read real step counts.
class RackProc : public juce::AudioProcessor {
public:
    RackProc() {
        addParameter(slot0_ = new juce::AudioParameterFloat({"slot0.gain", 1},
                                                            "Slot0 Gain", 0.0f, 1.0f, 0.5f));
        addParameter(slot1_ = new juce::AudioParameterFloat({"slot1.gain", 1},
                                                            "Slot1 Gain", 0.0f, 1.0f, 0.25f));
        // A 6-step choice behind what a design may draw with fewer options —
        // the exact shape the step-count hook exists for.
        addParameter(wave_ = new juce::AudioParameterChoice(
            {"lfo_waveform", 1}, "LFO Waveform",
            juce::StringArray{"Sine", "Tri", "Saw", "Square", "S&H", "Noise"}, 0));
        addParameter(onOff_ = new juce::AudioParameterBool({"bypass", 1}, "Bypass", false));
    }
    juce::AudioParameterFloat*  slot0_ = nullptr;
    juce::AudioParameterFloat*  slot1_ = nullptr;
    juce::AudioParameterChoice* wave_ = nullptr;
    juce::AudioParameterBool*   onOff_ = nullptr;

    const juce::String getName() const override { return "RackProc"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}
};

// A minimal paged rack: one knob whose param_key moves between slots, exactly
// like the canonical synthetic-rack fixture's chevron paging.
class PagedView : public pulp::view::DesignFrameView {
public:
    PagedView() : DesignFrameView(makeSvg(), makeElements()) {}
    void pageTo(int slot) {
        set_element_param_key(0, "slot" + std::to_string(slot) + ".gain");
    }

private:
    static std::string makeSvg() {
        return R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
               R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    }
    static std::vector<pulp::view::DesignFrameElement> makeElements() {
        using El = pulp::view::DesignFrameElement;
        auto knob = [](float cx, std::string key) {
            El e;
            e.kind = El::Kind::knob;
            e.cx = cx; e.cy = 40.0f; e.hit_radius = 18.0f;
            e.needle_d = "M0 0L0 -8";
            e.value = 0.5f;
            e.param_key = std::move(key);
            return e;
        };
        return {knob(60.0f, "slot0.gain"), knob(180.0f, "lfo_waveform")};
    }
};

PagedView* g_view = nullptr;  // borrowed; owned by the embed's view tree

std::unique_ptr<pulp::view::View> makeView() {
    auto v = std::make_unique<PagedView>();
    g_view = v.get();
    return v;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    RackProc proc;
    pulp_juce::PulpEmbedComponent comp(pulp::embed::NativeViewFactory{&makeView},
                                       240, 80, proc);
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce live-key-test FAILED\n");
        return 1;
    }
    check(comp.isValid(), "component created from a paged native View");

    // ── the dirty gate holds when nothing moves ─────────────────────────────
    const int afterMount = comp.keyResolveCount();
    for (int i = 0; i < 5; ++i) comp.syncFromHost();
    check(comp.keyResolveCount() == afterMount,
          "an idle pump does NOT re-enumerate the key set (gate holds)");

    // ── a host->UI push reaches the originally-bound element ────────────────
    check(comp.indexOfKey("slot0.gain") == 0, "element 0 starts on slot0.gain");
    proc.slot0_->setValueNotifyingHost(0.7f);
    comp.syncFromHost();
    check(approx(comp.controlValue(0), 0.7), "host push moved the bound element");

    // ── re-key from inside the view: the pump must follow ───────────────────
    g_view->pageTo(1);
    check(comp.indexOfKey("slot1.gain") == 0, "element 0 re-keyed to slot1.gain");

    proc.slot1_->setValueNotifyingHost(0.8f);
    const int beforeRekeyPump = comp.keyResolveCount();
    comp.syncFromHost();
    check(comp.keyResolveCount() == beforeRekeyPump + 1,
          "a view-driven re-key fires the gate exactly once");
    check(approx(comp.controlValue(0), 0.8),
          "the re-keyed element receives its NEW parameter's value");

    // The gate must settle again immediately after firing.
    const int afterRekey = comp.keyResolveCount();
    for (int i = 0; i < 5; ++i) comp.syncFromHost();
    check(comp.keyResolveCount() == afterRekey,
          "the gate settles again after a re-key");

    // The OLD parameter must no longer drive the re-keyed element.
    proc.slot0_->setValueNotifyingHost(0.1f);
    comp.syncFromHost();
    check(approx(comp.controlValue(0), 0.8),
          "the stale parameter no longer moves the re-keyed element");

    // Paging back must push the other slot's CURRENT value, not a stale memo.
    g_view->pageTo(0);
    comp.syncFromHost();
    check(approx(comp.controlValue(0), 0.1),
          "paging back pushes the returning key's current value");

    // ── a host tree change also fires the gate ──────────────────────────────
    const int beforeTreeChange = comp.keyResolveCount();
    proc.updateHostDisplay(
        juce::AudioProcessorListener::ChangeDetails{}.withParameterInfoChanged(true));
    comp.syncFromHost();
    check(comp.keyResolveCount() == beforeTreeChange + 1,
          "a host parameter-tree change fires the gate");

    // ── step count: the PARAMETER's steps, not the design's option count ────
    check(comp.hostParamStepCount("lfo_waveform") == 6,
          "a 6-step choice parameter reports 6");
    check(comp.hostParamStepCount("bypass") == 2, "a bool parameter reports 2");
    check(comp.hostParamStepCount("slot0.gain") == 0,
          "a continuous parameter reports 0 (JUCE's step sentinel maps to 0)");
    check(comp.hostParamStepCount("nonexistent") == 0, "an unknown key reports 0");

    std::printf("pulp-embed-juce live-key-test %s\n", g_fail == 0 ? "PASSED" : "FAILED");
    return g_fail == 0 ? 0 : 1;
}
