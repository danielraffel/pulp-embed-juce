// Regression — an UNBOUND design control (one whose param_key matches no
// host parameter) must KEEP its imported default value, not snap to 0.
//
// The bug: HostBridge::getParam returned 0.0 for an unknown key, and the embed
// shim treats any [0,1] return as authoritative — so it seeded every unmatched
// control to 0 at create. The fix returns the -1.0 sentinel ("no host opinion"),
// so the shim keeps the control's imported default. This mirrors the iPlug2
// adapter's long-standing sentinel.
//
// Native-view path (no external fixture): two knobs with a non-default imported
// value of 0.75. One key ("gain") matches a host parameter; the other ("orphan")
// does not. After the host<->UI initial sync, the bound knob reflects the host
// value (0.5) and the UNBOUND knob must still read 0.75.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PulpEmbedComponent.h"

#include <pulp/view/design_frame_view.hpp>

#include <cmath>
#include <cstdio>
#include <memory>

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

class TinyProc : public juce::AudioProcessor {
public:
    TinyProc() {
        // Host "gain" defaults to 0.5 — deliberately different from the imported
        // control value (0.75) so a bound vs unbound control is distinguishable.
        addParameter(new juce::AudioParameterFloat({"gain", 1}, "Gain", 0.0f, 1.0f, 0.5f));
    }
    const juce::String getName() const override { return "TinyProc"; }
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

// One knob bound to "gain", one with an orphan key that matches no parameter.
// Both carry a non-default imported value of 0.75.
std::unique_ptr<pulp::view::View> makeEditor() {
    using pulp::view::DesignFrameElement;
    auto knob = [](float cx, std::string key) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::knob;
        e.cx = cx; e.cy = 40.0f; e.hit_radius = 18.0f;
        e.needle_d = "M0 0L0 -8";
        e.value = 0.75f;               // imported default != host default != 0
        e.param_key = std::move(key);
        return e;
    };
    const std::string svg =
        R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    std::vector<DesignFrameElement> els{knob(60, "gain"), knob(180, "orphan")};
    return std::make_unique<pulp::view::DesignFrameView>(svg, std::move(els));
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    TinyProc proc;
    pulp_juce::PulpEmbedComponent comp(
        pulp::embed::NativeViewFactory{&makeEditor}, 240, 80, proc);

    check(comp.isValid(), "component created from a native View factory");
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce unbound-default-test FAILED\n");
        return 1;
    }

    // Exactly one control ("gain") resolves to a host parameter.
    check(comp.boundParameterCount() == 1,
          "one control binds to a host parameter (the orphan stays unbound)");

    const int gainIdx = comp.indexOfKey("gain");
    const int orphanIdx = comp.indexOfKey("orphan");
    check(gainIdx >= 0 && orphanIdx >= 0, "both controls enumerate in the view");

    if (orphanIdx >= 0) {
        const double orphanVal = comp.controlValue(orphanIdx);
        // The whole point: NOT snapped to 0 by an authoritative 0.0 seed.
        check(std::abs(orphanVal - 0.75) < 1e-3,
              "UNBOUND control kept its imported default (0.75), did not snap to 0");
    }
    if (gainIdx >= 0) {
        const double gainVal = comp.controlValue(gainIdx);
        // The bound control was seeded from the host default (0.5), as designed.
        check(std::abs(gainVal - 0.5) < 1e-3,
              "BOUND control reflects the host parameter value (0.5)");
    }

    std::printf("%s\n", g_fail == 0 ? "pulp-embed-juce unbound-default-test OK"
                                    : "pulp-embed-juce unbound-default-test FAILED");
    return g_fail == 0 ? 0 : 1;
}
