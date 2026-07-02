// P1.4 + P2.3 — the runtime host-param accessor and the host-action channel
// (the JUCE adapter halves of ABI v8), exercised through PulpEmbedComponent's
// public seams WITHOUT needing the v8 runtime present. The same backing is what
// the v8 has_param / param_display_text / host_action callbacks trampoline into,
// so proving the seams proves the callbacks.
//
// P1.4 focus: has_param is LIVE — a parameter added after construction resolves
// once the processor fires audioProcessorChanged (cache invalidation), matching
// hosts that swap parameter groups live (paged racks, dynamic slots).
//
// P2.3 focus: dispatchHostAction parses the JSON args to a juce::var and invokes
// onHostAction (insert/remove/reorder rack slots, load a preset, …).

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PulpEmbedComponent.h"

#include <pulp/view/design_frame_view.hpp>

#include <cstdio>
#include <memory>

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}

// A processor whose parameter tree can grow AFTER construction, so we can prove
// the adapter's paramID cache invalidates on audioProcessorChanged.
class MutableProc : public juce::AudioProcessor {
public:
    MutableProc() {
        addParameter(new juce::AudioParameterFloat({"gain", 1}, "Gain", 0.0f, 1.0f, 0.5f));
    }
    // Add a parameter live and notify listeners the parameter info changed —
    // the shape of a host swapping in a new rack slot / parameter group.
    void addLateParam(const juce::String& id) {
        addParameter(new juce::AudioParameterFloat({id, 1}, id, 0.0f, 1.0f, 0.25f));
        updateHostDisplay(juce::AudioProcessorListener::ChangeDetails{}
                              .withParameterInfoChanged(true));
    }
    const juce::String getName() const override { return "MutableProc"; }
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

std::unique_ptr<pulp::view::View> makeEditor() {
    using pulp::view::DesignFrameElement;
    const std::string svg =
        R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    DesignFrameElement k;
    k.kind = DesignFrameElement::Kind::knob;
    k.cx = 40.0f; k.cy = 40.0f; k.hit_radius = 18.0f;
    k.needle_d = "M0 0L0 -8";
    k.param_key = "gain";
    return std::make_unique<pulp::view::DesignFrameView>(
        svg, std::vector<DesignFrameElement>{k});
}
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    MutableProc proc;
    pulp_juce::PulpEmbedComponent comp(
        pulp::embed::NativeViewFactory{&makeEditor}, 240, 80, proc);
    check(comp.isValid(), "component created from a native View factory");
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce host-surface-test FAILED\n");
        return 1;
    }

    // ── P1.4: runtime host-param accessor ────────────────────────────────────
    check(comp.hostHasParam("gain"), "hostHasParam resolves an existing parameter");
    check(!comp.hostHasParam("rack.slot0.mix"),
          "hostHasParam is false for a not-yet-present key");

    // Display text via AudioProcessorParameter::getText (non-empty for a real
    // parameter; empty for an unknown key).
    check(comp.hostParamDisplayText("gain", 0.5).isNotEmpty(),
          "hostParamDisplayText returns text for a known parameter");
    check(comp.hostParamDisplayText("rack.slot0.mix", 0.5).isEmpty(),
          "hostParamDisplayText is empty for an unknown key");
    // Memoized + stable across repeated queries.
    check(comp.hostParamDisplayText("gain", 0.5) == comp.hostParamDisplayText("gain", 0.5),
          "hostParamDisplayText is stable for the same (key, value)");

    // Cache invalidation: add a parameter live; the accessor must resolve it once
    // audioProcessorChanged has fired (no remount).
    proc.addLateParam("rack.slot0.mix");
    check(comp.hostHasParam("rack.slot0.mix"),
          "hostHasParam resolves a live-added parameter (cache invalidated)");
    check(comp.hostParamDisplayText("rack.slot0.mix", 1.0).isNotEmpty(),
          "hostParamDisplayText resolves a live-added parameter");

    // ── P2.3: host-action channel ────────────────────────────────────────────
    juce::String gotAction;
    int gotIndex = -1;
    comp.onHostAction = [&](const juce::String& action, const juce::var& args) {
        gotAction = action;
        gotIndex = (int) args.getProperty("index", -1);
        return action == "load_preset";
    };
    const bool handled = comp.dispatchHostAction("load_preset", R"({"index":3})");
    check(handled, "dispatchHostAction returns the handler's true for a handled action");
    check(gotAction == "load_preset", "onHostAction received the action name");
    check(gotIndex == 3, "onHostAction received the JSON args parsed to a juce::var");

    // An action with no handler match returns false (diagnostic-only, not fatal).
    const bool unhandled = comp.dispatchHostAction("reorder_slots", R"({"from":0,"to":2})");
    check(!unhandled, "an unhandled action returns false without throwing");
    check(gotAction == "reorder_slots", "the handler still saw the unhandled action");

    std::printf("%s\n", g_fail == 0 ? "pulp-embed-juce host-surface-test OK"
                                    : "pulp-embed-juce host-surface-test FAILED");
    return g_fail == 0 ? 0 : 1;
}
