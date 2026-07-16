// Driving a control by (key, value) through the REAL pointer path.
//
// simulateParamDragToValue exists so a headless harness can move a NAMED control
// the way a user does — hit-test, capture, the control's own drag law, its emit
// path, the host bridge — rather than reaching past all of that to poke a value
// in. So these tests assert the things only the real path can prove:
//
//   * the HOST parameter arrives at the value, bracketed by begin/end gestures in
//     order, observed through juce::AudioProcessorListener — the same surface a
//     DAW watches for automation and undo;
//   * a control the pointer cannot reach (disabled, so hit-testing skips it) makes
//     the drive FAIL. A helper that bypassed hit-testing would pass here while the
//     control was unusable in the plugin, which is worse than no test at all;
//   * a bogus key fails loudly rather than no-op'ing green;
//   * a discrete parameter lands on the intended INDEX, over its whole range —
//     which pins the normalization to the host's step COUNT minus one.
//
// Note what is asserted and when: a UI write reaches the host PARAMETER
// synchronously, so the parameter is read straight after the drive. The plugin's
// own DSP state is a different thing — it is pulled from the parameter on the
// audio thread per block, and this harness has no audio callback at all, so
// asserting engine state here would test nothing. Assert the parameter.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PulpEmbedComponent.h"

#include <pulp/view/design_frame_view.hpp>

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
bool approx(double a, double b, double tol = 1e-3) { return std::fabs(a - b) < tol; }

// The parameter's NORMALIZED host-facing value, read through the base class —
// the same surface a DAW reads, and the one the concrete types keep private.
double hostNorm(juce::AudioProcessorParameter* p) {
    return static_cast<double>(p->getValue());
}

constexpr int kWaveSteps = 6;  // the choice parameter's step COUNT

class DriveProc : public juce::AudioProcessor {
public:
    DriveProc() {
        addParameter(gain_ = new juce::AudioParameterFloat({"gain", 1}, "Gain",
                                                           0.0f, 1.0f, 0.5f));
        addParameter(tone_ = new juce::AudioParameterFloat({"tone", 1}, "Tone",
                                                           0.0f, 1.0f, 0.5f));
        // Six values, so the normalized step grid is k/5 -- the count is 6 and the
        // divisor is 5. Getting that off by one is the bug this parameter exists
        // to catch, so the test walks every index rather than sampling one.
        addParameter(wave_ = new juce::AudioParameterChoice(
            {"wave", 1}, "Wave",
            juce::StringArray{"Sine", "Tri", "Saw", "Square", "S&H", "Noise"}, 0));
        addParameter(bypass_ = new juce::AudioParameterBool({"bypass", 1}, "Bypass", false));
    }
    juce::AudioParameterFloat*  gain_ = nullptr;
    juce::AudioParameterFloat*  tone_ = nullptr;
    juce::AudioParameterChoice* wave_ = nullptr;
    juce::AudioParameterBool*   bypass_ = nullptr;

    const juce::String getName() const override { return "DriveProc"; }
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

// The host's own view of the gesture: exactly what a DAW listens to in order to
// group an automation pass into one undo step.
struct GestureLog : juce::AudioProcessorListener {
    std::vector<std::string> events;   // "begin:N" / "set:N" / "end:N", in order
    std::vector<float>       values;   // the values carried by the set events

    void audioProcessorParameterChanged(juce::AudioProcessor*, int i, float v) override {
        events.push_back("set:" + std::to_string(i));
        values.push_back(v);
    }
    void audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int i) override {
        events.push_back("begin:" + std::to_string(i));
    }
    void audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int i) override {
        events.push_back("end:" + std::to_string(i));
    }
    void audioProcessorChanged(juce::AudioProcessor*, const ChangeDetails&) override {}
    void clear() { events.clear(); values.clear(); }
};

// Two knobs and a choice knob, laid out so the panel fit is the identity.
class DriveView : public pulp::view::DesignFrameView {
public:
    DriveView() : DesignFrameView(makeSvg(), makeElements()) {}
    using DesignFrameView::set_element_enabled;

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
        // A latching toggle: it commits on the press and ignores the drag, which is
        // a different shape of control from a knob and exercises the "already at
        // the value" path where a stray press would do harm.
        El tog;
        tog.kind = El::Kind::toggle;
        tog.x = 10.0f; tog.y = 60.0f; tog.w = 20.0f; tog.h = 12.0f;
        tog.value = 0.0f;
        tog.param_key = "bypass";
        return {knob(60.0f, "gain"), knob(120.0f, "tone"), knob(180.0f, "wave"), tog};
    }
};

DriveView* g_view = nullptr;  // borrowed; owned by the embed's view tree

std::unique_ptr<pulp::view::View> makeView() {
    auto v = std::make_unique<DriveView>();
    g_view = v.get();
    return v;
}

}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    DriveProc proc;
    GestureLog log;
    proc.addListener(&log);

    pulp_juce::PulpEmbedComponent comp(pulp::embed::NativeViewFactory{&makeView},
                                       240, 80, proc);
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce param-drive-test FAILED\n");
        return 1;
    }
    const int gainIdx = comp.indexOfKey("gain");
    check(gainIdx >= 0, "the design's controls resolved to host parameters");

    // ── a drive by (key, value) lands the HOST parameter on the value ───────
    log.clear();
    check(comp.simulateParamDragToValue("gain", 0.8), "drive gain to 0.8 reports success");
    check(approx(hostNorm(proc.gain_), 0.8), "the HOST parameter arrived at 0.8");
    check(approx(comp.controlValue(gainIdx), 0.8), "the control itself shows 0.8");

    // ── the host saw one properly ordered gesture bracket ───────────────────
    const int gainParam = 0;  // registration order in DriveProc
    check(!log.events.empty() && log.events.front() == "begin:" + std::to_string(gainParam),
          "the gesture opened with begin on the driven parameter");
    check(!log.events.empty() && log.events.back() == "end:" + std::to_string(gainParam),
          "the gesture closed with end on the driven parameter");
    int begins = 0, ends = 0, sets = 0;
    for (const auto& e : log.events) {
        if (e.rfind("begin:", 0) == 0) ++begins;
        else if (e.rfind("end:", 0) == 0) ++ends;
        else ++sets;
    }
    check(begins == 1 && ends == 1, "exactly one begin and one end -- a single undo step");
    check(sets >= 1, "the gesture carried at least one value change");
    check(!log.values.empty() && approx(log.values.back(), 0.8),
          "the LAST value the host received is the target, not an intermediate");

    // Only the driven parameter moved: an aimed click must not spray its
    // neighbours (the knobs are 60px apart, well inside a sloppy hit radius).
    check(approx(hostNorm(proc.tone_), 0.5), "the neighbouring parameter did not move");

    // ── driving DOWN works too (the loop must not assume a direction) ───────
    check(comp.simulateParamDragToValue("gain", 0.2), "drive gain down to 0.2 reports success");
    check(approx(hostNorm(proc.gain_), 0.2), "the host parameter arrived at 0.2");

    // ── the clamped extremes are reachable ─────────────────────────────────
    check(comp.simulateParamDragToValue("gain", 0.0), "drive gain to the bottom");
    check(approx(hostNorm(proc.gain_), 0.0), "the host parameter arrived at 0.0");
    check(comp.simulateParamDragToValue("gain", 1.0), "drive gain to the top");
    check(approx(hostNorm(proc.gain_), 1.0), "the host parameter arrived at 1.0");

    // ── a bogus key fails, loudly, and touches nothing ─────────────────────
    const float gainBefore = hostNorm(proc.gain_);
    log.clear();
    check(!comp.simulateParamDragToValue("no_such_key", 0.5), "an unknown key returns false");
    check(log.events.empty(), "an unknown key emitted no gesture at all");
    check(approx(hostNorm(proc.gain_), gainBefore), "an unknown key moved no parameter");

    // ── a control the POINTER cannot reach must fail ───────────────────────
    // Disabling the element makes hit-testing skip it, so the press lands on
    // nothing. The drive depends on hit-testing, so it must notice and say so —
    // this is the assertion a hit-test-bypassing helper could never make.
    const int toneIdx = comp.indexOfKey("tone");
    const float toneBefore = hostNorm(proc.tone_);
    g_view->set_element_enabled(toneIdx, false);
    check(!comp.simulateParamDragToValue("tone", 0.9),
          "a control hit-testing skips returns false, not a silent pass");
    check(approx(hostNorm(proc.tone_), toneBefore), "the unreachable control's parameter held");
    g_view->set_element_enabled(toneIdx, true);
    check(comp.simulateParamDragToValue("tone", 0.9), "re-enabling makes it drivable again");
    check(approx(hostNorm(proc.tone_), 0.9), "the re-enabled control's parameter arrived");

    // ── discrete: land on the intended INDEX, across the whole range ────────
    // The count is 6, so the last index -- and the divisor -- is 5.
    //
    // The host index alone is a WEAK witness for the divisor: JUCE re-quantizes
    // whatever normalized value it receives onto its own grid, and that rounding
    // happens to absorb an off-by-one divisor for every index here. So the index
    // is checked (it is what a caller ultimately cares about) AND so is the value
    // the CONTROL came to rest on, which is not re-quantized by anything and is
    // therefore what actually pins the convention: driving to k/5 must leave the
    // control ON k/5. A divisor of 6 would park it on k/6 and be caught here.
    const int waveIdx = comp.indexOfKey("wave");
    check(comp.hostParamStepCount("wave") == kWaveSteps,
          "the host reports the choice parameter's step COUNT (6)");
    bool allIndices = true;
    for (int i = 0; i < kWaveSteps; ++i) {
        const double norm = static_cast<double>(i) / (kWaveSteps - 1);  // divisor = count - 1
        const bool ok = comp.simulateParamDragToValue("wave", norm);
        if (!ok || proc.wave_->getIndex() != i || !approx(comp.controlValue(waveIdx), norm)) {
            allIndices = false;
            std::printf("       index %d: asked for %.4f, host landed on %d, control on %.4f\n",
                        i, norm, proc.wave_->getIndex(), comp.controlValue(waveIdx));
        }
    }
    check(allIndices, "every one of the 6 indices is reachable, and rests exactly on k/5");

    // An off-grid request snaps to the NEAREST step rather than landing between
    // two of them (0.55 * 5 = 2.75 -> step 3).
    check(comp.simulateParamDragToValue("wave", 0.55), "an off-grid discrete target succeeds");
    check(proc.wave_->getIndex() == 3, "an off-grid discrete target snapped to the nearest step");

    // ── a latching toggle: driving it to the value it ALREADY holds is a no-op ──
    // A press flips a toggle unconditionally, so a drive that pressed first and
    // checked afterwards would flip a CORRECT control to the wrong value and then
    // report failure. All four transitions must end on the requested value.
    const int bypIdx = comp.indexOfKey("bypass");
    check(bypIdx >= 0, "the toggle resolved to a host parameter");
    check(comp.simulateParamDragToValue("bypass", 1.0), "toggle off -> on");
    check(approx(hostNorm(proc.bypass_), 1.0), "the toggle's parameter turned on");
    check(comp.simulateParamDragToValue("bypass", 1.0), "toggle on -> on (already there)");
    check(approx(hostNorm(proc.bypass_), 1.0),
          "driving a toggle to the value it already holds did NOT flip it");
    check(comp.simulateParamDragToValue("bypass", 0.0), "toggle on -> off");
    check(approx(hostNorm(proc.bypass_), 0.0), "the toggle's parameter turned off");
    check(comp.simulateParamDragToValue("bypass", 0.0), "toggle off -> off (already there)");
    check(approx(hostNorm(proc.bypass_), 0.0), "the toggle stayed off");

    // A discrete drive is still ONE undo step, not one per step crossed on the way.
    log.clear();
    comp.simulateParamDragToValue("wave", 0.0);
    int wBegins = 0, wEnds = 0;
    for (const auto& e : log.events) {
        if (e.rfind("begin:", 0) == 0) ++wBegins;
        else if (e.rfind("end:", 0) == 0) ++wEnds;
    }
    check(wBegins == 1 && wEnds == 1, "a discrete drive is a single bracketed gesture");
    check(proc.wave_->getIndex() == 0, "the discrete drive arrived at index 0");

    proc.removeListener(&log);
    std::printf("pulp-embed-juce param-drive-test %s\n", g_fail == 0 ? "PASSED" : "FAILED");
    return g_fail == 0 ? 0 : 1;
}
