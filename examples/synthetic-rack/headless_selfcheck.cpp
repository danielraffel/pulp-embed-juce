// Headless self-check for the canonical fixture — CI-runnable, no window
// shown (same idiom as examples/native-view-test and host-surface-test). Two
// parts:
//
//   Part A (SDK / view-only): construct SyntheticRackView directly and prove its
//     paging logic re-keys the rack knobs (set_element_param_key) and that the
//     host-action hook receives non-paging actions. No embed / ABI involved — this
//     is the view contract on its own.
//
//   Part B (adapter / embed): mount the same view via the NativeViewFactory ctor
//     against a real SyntheticRackProcessor and prove the runtime host-param
//     accessor + action channel (the ABI v8 adapter halves) resolve the static
//     AND paged keys and route the host action.
//
// Exits non-zero if any assertion fails.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PulpEmbedComponent.h"
#include "SyntheticRackProcessor.h"
#include "SyntheticRackView.h"

#include <cstdio>
#include <memory>

namespace {
int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_fail;
}
}  // namespace

// The processor's createEditor() is defined in PluginMain.cpp (returns the real
// editor). This headless test does not link PluginMain.cpp and never opens an
// editor, so provide a stub in this TU to satisfy the vtable key function. No
// ODR conflict — the test binary and the plugin binary are separate targets.
juce::AudioProcessorEditor* SyntheticRackProcessor::createEditor() { return nullptr; }

int main() {
    juce::ScopedJuceInitialiser_GUI juceInit;

    // ── Part A: the view's paging + action logic, in isolation ──────────────
    {
        synth_rack::SyntheticRackView view;
        check(view.element_param_key(synth_rack::kIdxInputGain) == "input.gain",
              "static knob declares param_key input.gain");
        check(view.element_param_key(synth_rack::kIdxRackA) == "slot0.gain",
              "rack knob A starts keyed to slot0.gain");

        std::string routed;
        view.set_host_action_hook([&](const std::string& id) { routed = id; });

        // Page forward: rack knobs re-key to slot 1 (set_element_param_key).
        view.on_action("rack.next");
        check(view.current_slot() == 1, "paging advanced to slot 1");
        check(view.element_param_key(synth_rack::kIdxRackA) == "slot1.gain",
              "rack knob A re-keyed to slot1.gain after paging (set_element_param_key)");
        check(view.element_param_key(synth_rack::kIdxRackB) == "slot1.cutoff",
              "rack knob B re-keyed to slot1.cutoff after paging");

        // Paging chevrons are handled view-internally, NOT forwarded to the host.
        check(routed.empty(), "paging actions are not forwarded to the host hook");

        // A non-paging action falls through to the host hook.
        view.on_action("load_preset");
        check(routed == "load_preset", "non-paging action forwarded to the host hook");
    }

    // ── Part B: the embed adapter surfaces against a real processor ─────────
    SyntheticRackProcessor proc;
    pulp_juce::PulpEmbedComponent comp(
        pulp::embed::NativeViewFactory{&synth_rack::makeSyntheticRackView}, 900, 420, proc);
    check(comp.isValid(), "component created from the SyntheticRackView factory");
    if (!comp.isValid()) {
        std::printf("create failed: %s\n", comp.lastError().toRawUTF8());
        std::printf("pulp-embed-juce synthetic-rack-test FAILED\n");
        return 1;
    }

    // Static controls bind by key==paramID at create.
    check(comp.boundParameterCount() >= 2,
          "at least the two static controls bind (input.gain + mix)");
    check(comp.hostHasParam("input.gain") && comp.hostHasParam("mix"),
          "hostHasParam resolves the static keys");

    // Paged rack keys: every slot's param exists for the plugin's life, so the
    // first and last slot keys both resolve (no remount needed to see a slot).
    check(comp.hostHasParam(synth_rack::slotKey(0, 0).c_str()) &&
              comp.hostHasParam(synth_rack::slotKey(synth_rack::kSlotCount - 1, 1).c_str()),
          "hostHasParam resolves paged rack-slot keys (first + last slot)");
    check(comp.hostParamDisplayText(synth_rack::slotKey(2, 1).c_str(), 0.5).isNotEmpty(),
          "hostParamDisplayText resolves a paged-slot display string");
    check(comp.hostHasParam("slot9.gain") == false,
          "hostHasParam is false for a non-existent slot key");

    // Host-action channel.
    juce::String gotAction;
    int gotIndex = -1;
    comp.onHostAction = [&](const juce::String& action, const juce::var& args) {
        gotAction = action;
        gotIndex = (int) args.getProperty("index", -1);
        return action == "load_preset";
    };
    const bool handled = comp.dispatchHostAction("load_preset", R"({"index":2})");
    check(handled && gotAction == "load_preset" && gotIndex == 2,
          "load_preset host action routed to onHostAction with parsed args");

    std::printf("%s\n", g_fail == 0 ? "pulp-embed-juce synthetic-rack-test OK"
                                    : "pulp-embed-juce synthetic-rack-test FAILED");
    return g_fail == 0 ? 0 : 1;
}
