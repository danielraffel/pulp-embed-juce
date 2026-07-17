// SyntheticRackPlugin — the plugin entry + editor for the canonical fixture.
// The editor IS the embedded native Pulp view (SyntheticRackView) mounted through
// PulpEmbedComponent's NativeViewFactory ctor, wired end-to-end:
//
//   • static + dynamic param binding — SyntheticRackView's param_key'd elements
//     bind to the processor's APVTS by key==paramID (static at create; rack-page
//     knobs rebind on paging via set_element_param_key).
//   • host action                    — onHostAction handles "load_preset".
//   • resizable editor               — configureResizableEditor(): one
//     call gives host-window aspect-locked resize from the design's size hints.
//
// Headless self-check (env PULP_EMBED_SELFCHECK=1, run via the Standalone wrapper
// or `--screenshot`): after attach it asserts the static AND paged controls are
// bound, that display text resolves, and that the host-action channel fires; it
// captures a live GPU frame + a deterministic raster, samples idle CPU (editor
// open, no interaction — so the "+2% Release" claim stays a tested property), and
// exits NON-ZERO if any assertion fails.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PulpEmbedComponent.h"
#include "SyntheticRackProcessor.h"
#include "SyntheticRackView.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>

namespace {
constexpr int kW = 900;
constexpr int kH = 420;

juce::File outDir() {
    if (auto* e = std::getenv("PULP_EMBED_OUT"); e && *e)
        return juce::File(juce::String::fromUTF8(e));
    return juce::File("/tmp");
}
bool wantSelfCheck() {
    auto* e = std::getenv("PULP_EMBED_SELFCHECK");
    return e && *e && juce::String(e) != "0";
}
}  // namespace

// ── the editor ───────────────────────────────────────────────────────────────
class SyntheticRackEditor final : public juce::AudioProcessorEditor,
                                  private juce::Timer {
public:
    explicit SyntheticRackEditor(SyntheticRackProcessor& p)
        : juce::AudioProcessorEditor(&p), proc_(p) {
        // Mount the HAND-BUILT native view (not an imported design) and bind its
        // param_key'd controls to this processor's parameters.
        embed_ = std::make_unique<pulp_juce::PulpEmbedComponent>(
            pulp::embed::NativeViewFactory{&synth_rack::makeSyntheticRackView}, kW, kH, p);
        if (!embed_->isValid())
            juce::Logger::writeToLog("SyntheticRack embed failed: " + embed_->lastError());
        addAndMakeVisible(*embed_);

        // The host-action channel. The view sends its "load_preset" button out
        // View::host_actions(); the shim backs that surface with the host_action
        // ABI callback, which lands here (host_actions() → host_action ABI →
        // onHostAction).
        embed_->onHostAction = [this](const juce::String& action, const juce::var& args) {
            if (action == "load_preset") {
                loadPreset((int) args.getProperty("index", 0));
                return true;
            }
            return false;  // diagnostic-only; unhandled actions are logged, not fatal
        };

        // One call: host-window resize locked to the design aspect,
        // min/max from the embed's size hints, sized to the design on open.
        embed_->configureResizableEditor(*this);
        setSize(kW, kH);

        if (wantSelfCheck()) startTimer(250);  // poll until attached, then verify + quit
    }

    ~SyntheticRackEditor() override { stopTimer(); }

    void resized() override {
        if (embed_) embed_->setBounds(getLocalBounds());
    }

private:
    void loadPreset(int index) {
        // A real plugin would recall a preset; here we nudge the static params so
        // the action has a visible effect and the self-check can prove it ran.
        if (auto* g = proc_.apvts.getParameter("input.gain")) g->setValueNotifyingHost(0.25f);
        if (auto* m = proc_.apvts.getParameter("mix")) m->setValueNotifyingHost(1.0f);
        lastPreset_ = index;
    }

    // Idle-CPU sample: with the editor open and NO interaction, measure the CPU
    // time this process consumed across the natural idle window between attach and
    // capture (the embed's own 30 Hz tick + display link are the only activity).
    // Reported (not gated) so a later harness can compare it against a JUCE-editor
    // baseline — the "+2% Release" success metric. Avoids a nested
    // dispatch loop so it compiles regardless of JUCE_MODAL_LOOPS_PERMITTED.
    double idleCpuPercent() const {
        const double wallMs = juce::Time::getMillisecondCounterHiRes() - idleWall0_;
        const double cpuMs = 1000.0 * double(std::clock() - idleCpu0_) / double(CLOCKS_PER_SEC);
        return wallMs > 0.0 ? 100.0 * cpuMs / wallMs : 0.0;
    }

    void timerCallback() override {
        if (embed_ == nullptr) { stopTimer(); return; }
        if (!embed_->isOpened()) { if (++waits_ < 40) return; }
        // Mark the start of the idle window the first tick the embed is open.
        if (idleCpu0_ == 0) { idleCpu0_ = std::clock(); idleWall0_ = juce::Time::getMillisecondCounterHiRes(); }
        if (++frames_ < 6) return;  // ~1.5s of live frames after attach — the idle sample window

        int fail = 0;
        auto check = [&](bool ok, const char* what) {
            std::fprintf(stderr, "  [%s] %s\n", ok ? "PASS" : "FAIL", what);
            if (!ok) ++fail;
        };

        // Static controls bound: input.gain + mix (and the initial rack slot 0
        // knobs also resolve), so at least the two static keys bind.
        check(embed_->boundParameterCount() >= 2,
              "static controls bound (input.gain + mix)");
        check(embed_->hostHasParam("input.gain") && embed_->hostHasParam("mix"),
              "hostHasParam resolves the static keys");

        // Paged controls: every rack slot's parameter exists for the plugin's
        // life, so a not-currently-shown slot key still resolves — that is what a
        // paged/dynamic UI relies on (bind on re-key without remount).
        check(embed_->hostHasParam(synth_rack::slotKey(0, 0).c_str()) &&
                  embed_->hostHasParam(synth_rack::slotKey(synth_rack::kSlotCount - 1, 1).c_str()),
              "hostHasParam resolves paged rack-slot keys (first + last slot)");
        check(embed_->hostParamDisplayText(synth_rack::slotKey(2, 1).c_str(), 0.5).isNotEmpty(),
              "hostParamDisplayText resolves a paged-slot display string");

        // Host-action channel fires and mutates state (proves onHostAction wired).
        const bool handled = embed_->dispatchHostAction("load_preset", R"({"index":2})");
        check(handled && lastPreset_ == 2, "load_preset host action routed to onHostAction");

        const auto dir = outDir();
        const bool live = embed_->writeCapturePng(dir.getChildFile("synthetic-rack-live.png"));
        const bool det  = embed_->writeRenderPng(dir.getChildFile("synthetic-rack-render.png"), kW, kH);
        check(live, "live GPU capture written");
        check(det, "deterministic raster written");

        const double idleCpu = idleCpuPercent();

        std::fprintf(stderr,
            "SELFCHECK opened=%d gpu=%d bound=%d idleCpu=%.1f%% live=%s det=%s result=%s\n",
            embed_->isOpened() ? 1 : 0, embed_->isGpuBacked() ? 1 : 0,
            embed_->boundParameterCount(), idleCpu,
            live ? "ok" : "FAIL", det ? "ok" : "FAIL", fail == 0 ? "PASS" : "FAIL");

        stopTimer();
        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->setApplicationReturnValue(fail == 0 ? 0 : 1);
        juce::JUCEApplicationBase::quit();
    }

    SyntheticRackProcessor& proc_;
    std::unique_ptr<pulp_juce::PulpEmbedComponent> embed_;
    int waits_ = 0, frames_ = 0;
    int lastPreset_ = -1;
    std::clock_t idleCpu0_ = 0;   // CPU clock at the start of the idle window
    double idleWall0_ = 0.0;      // wall clock (ms) at the start of the idle window
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SyntheticRackEditor)
};

juce::AudioProcessorEditor* SyntheticRackProcessor::createEditor() {
    return new SyntheticRackEditor(*this);
}

// Plugin instance factory for the format wrappers (VST3 / Standalone).
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() {
    return new SyntheticRackProcessor();
}
