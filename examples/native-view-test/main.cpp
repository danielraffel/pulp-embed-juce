// Headless e2e for the native-view embed path: mount a HAND-BUILT
// pulp::view::DesignFrameView (not an importer-generated design) via the
// NativeViewFactory ctor, and prove its param_key'd elements resolve to the
// host's juce::AudioProcessor parameters by key == paramID. This is the path a
// JUCE plugin uses to drive its DSP from a compiled Pulp UI.
//
// Runs under ScopedJuceInitialiser_GUI so the juce::Component / Timer and the
// windowed embed (PluginViewHost) can construct; no window is shown.

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

// Smallest possible host: two float params whose paramIDs are the binding keys a
// native control declares (gain, cutoff). No DSP runs — we only need the
// parameter table the embed bridge matches against.
class TinyProc : public juce::AudioProcessor {
public:
    TinyProc() {
        addParameter(new juce::AudioParameterFloat({"gain", 1}, "Gain", 0.0f, 1.0f, 0.5f));
        addParameter(new juce::AudioParameterFloat({"cutoff", 1}, "Cutoff", 0.0f, 1.0f, 0.5f));
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

// A hand-built native editor: one panel, one knob bound to "gain", one bound to
// "cutoff", and one unbound knob (no param_key) that must stay visual-only.
std::unique_ptr<pulp::view::View> makeNativeEditor() {
    using pulp::view::DesignFrameElement;
    auto knob = [](float cx, std::string key) {
        DesignFrameElement e;
        e.kind = DesignFrameElement::Kind::knob;
        e.cx = cx; e.cy = 40.0f; e.hit_radius = 18.0f;
        e.needle_d = "M0 0L0 -8";
        e.param_key = std::move(key);
        return e;
    };
    const std::string svg =
        R"(<svg width="240" height="80" xmlns="http://www.w3.org/2000/svg">)"
        R"(<rect x="0" y="0" width="240" height="80" fill="#222"/></svg>)";
    std::vector<DesignFrameElement> els{knob(40, "gain"), knob(120, "cutoff"),
                                        knob(200, "")};  // last: visual-only
    return std::make_unique<pulp::view::DesignFrameView>(svg, std::move(els));
}
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    TinyProc proc;
    const int W = 240, H = 80;

    pulp_juce::PulpEmbedComponent comp(
        pulp::embed::NativeViewFactory{&makeNativeEditor}, W, H, proc);

    check(comp.isValid(), "component created from a native View factory");
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce native-view-test FAILED\n");
        return 1;
    }

    // The two param_key'd knobs resolve to "gain"/"cutoff"; the keyless knob does
    // not bind. So exactly two design controls map to host parameters.
    check(comp.boundParameterCount() == 2,
          "two param_key'd native controls bind to host params (keyless one stays visual)");

    std::printf("%s\n", g_fail == 0 ? "pulp-embed-juce native-view-test OK"
                                    : "pulp-embed-juce native-view-test FAILED");
    return g_fail == 0 ? 0 : 1;
}
