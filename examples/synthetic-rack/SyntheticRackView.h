// SyntheticRackView — a HAND-BUILT pulp::view::DesignFrameView subclass that is
// the canonical fixture: a small paged "effect rack" editor authored in C++
// (not imported). It exercises, in one view, every runtime surface the
// JUCE→Pulp port needs:
//
//   • STATIC top-level controls  — two knobs ("input.gain", "mix") whose
//     DesignFrameElement::param_key is set once at construction. The embed's
//     native-view lane binds these to the host's parameters by key==paramID with
//     zero glue (element_param_key / element_for_param_key).
//
//   • A DYNAMIC "rack page"       — two knobs whose param_key is RE-ASSIGNED with
//     set_element_param_key(i, key) when the user pages to another slot. The
//     re-key re-points the embed's binding + key→index registry at the new key
//     (both directions follow it), and bumps the ABI's key generation so the
//     adapter's host→UI pump re-resolves the new keys against the host's LIVE
//     parameter table (the ABI v8 has_param / param_display_text surface the
//     adapter backs). A value_label reads back the active slot's display text.
//
//   • One Kind::action button     — "load_preset". In the live plugin the view
//     sends this out View::host_actions(), the channel the shim backs with the
//     ABI's host_action callback, landing on PulpEmbedComponent::onHostAction.
//     Paging chevrons ("rack.prev"/"rack.next") are handled view-internally and
//     deliberately never reach the host. With no host surface installed (the
//     headless self-check, previews) the id falls through to host_action_hook_.
//
// Everything is drawn from an inline SVG so the fixture needs no external assets.
#pragma once

#include <pulp/view/design_frame_view.hpp>
#include <pulp/view/host_param_surface.hpp>
#include <pulp/view/view.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace synth_rack {

// Rack shape (the EffectHost model): N slots, each exposing
// a small fixed set of param ids by "slot<N>.<id>" key. The host declares one
// real parameter per (slot, id); the view shows ONE slot at a time and re-keys
// its rack knobs as you page.
inline constexpr int kSlotCount = 3;

// The two per-slot param ids the rack page controls (kept tiny on purpose).
inline const char* kSlotParamIds[] = {"gain", "cutoff"};
inline constexpr int kSlotParamCount = 2;

// "slot0.gain", "slot2.cutoff", … — the single source of truth for a slot key,
// shared by the view (element param_key) and the host (APVTS ParameterID).
inline std::string slotKey(int slot, int param) {
    return "slot" + std::to_string(slot) + "." + kSlotParamIds[param];
}

// Element indices, fixed by construction order in makeElements(). Named so the
// paging logic and any test can address controls without magic numbers.
enum ElementIndex {
    kIdxInputGain = 0,  // static knob → "input.gain"
    kIdxMix       = 1,  // static knob → "mix"
    kIdxRackA     = 2,  // dynamic knob → "slot<cur>.gain"
    kIdxRackB     = 3,  // dynamic knob → "slot<cur>.cutoff"
    kIdxSlotLabel = 4,  // value_label → "SLOT n/N"
    kIdxPrev      = 5,  // action "rack.prev"
    kIdxNext      = 6,  // action "rack.next"
    kIdxLoadPreset = 7, // action "load_preset" (routed to the host)
};

class SyntheticRackView : public pulp::view::DesignFrameView {
public:
    SyntheticRackView()
        : pulp::view::DesignFrameView(makeSvg(), makeElements()) {
        // Paging + host-action routing. Chevrons page the rack locally (re-key +
        // relabel); every other action id is forwarded to the host seam.
        on_action = [this](const std::string& id) {
            if (id == "rack.next") { pageBy(+1); return; }
            if (id == "rack.prev") { pageBy(-1); return; }
            // Non-paging buttons are the HOST's commands, so they go out the
            // view-side host-action channel — which the embed backs with the ABI's
            // host_action callback, landing on PulpEmbedComponent::onHostAction.
            //
            // Sent from here rather than by arming the frame's route_actions_to_host:
            // that routes EVERY Kind::action button, which would push the paging
            // chevrons above at the host as commands it never asked for. This view
            // knows which of its buttons are the host's; the blanket seam does not.
            //
            // Null whenever no host installed a surface (the headless self-check,
            // previews, screenshots) — there the hook below is the only listener.
            if (auto* actions = host_actions()) actions->send_host_action(id, "{}");
            if (host_action_hook_) host_action_hook_(id);
        };
        applySlotLabel();
    }

    // Local fallback for non-paging Kind::action buttons, for the contexts that
    // have no host surface to send to (the headless self-check, previews). In the
    // live plugin the host channel above is what reaches the foreign host; this is
    // left null there and load_preset is simply a no-op locally without it.
    void set_host_action_hook(std::function<void(const std::string&)> fn) {
        host_action_hook_ = std::move(fn);
    }

    int current_slot() const { return current_slot_; }

private:
    // Move the visible slot by delta (wrapping), re-key the two rack knobs to the
    // new slot's parameters, and relabel. set_element_param_key marks the embed's
    // registry dirty so the shim rebinds on the next tick — no remount.
    void pageBy(int delta) {
        current_slot_ = (current_slot_ + delta + kSlotCount) % kSlotCount;
        set_element_param_key(kIdxRackA, slotKey(current_slot_, 0));
        set_element_param_key(kIdxRackB, slotKey(current_slot_, 1));
        applySlotLabel();
    }

    void applySlotLabel() {
        set_element_text(kIdxSlotLabel,
                         "SLOT " + std::to_string(current_slot_ + 1) + "/" +
                             std::to_string(kSlotCount));
    }

    static std::string makeSvg() {
        // 900x420 dark panel with a rack strip. Purely cosmetic — the typed
        // element list below drives all behavior, not the SVG structure.
        return
            R"(<svg width="900" height="420" xmlns="http://www.w3.org/2000/svg">)"
            R"(<rect x="0" y="0" width="900" height="420" fill="#141821"/>)"
            R"(<rect x="0" y="0" width="900" height="72" fill="#1d2330"/>)"
            R"(<rect x="40" y="150" width="820" height="200" rx="10" fill="#10141c"/>)"
            R"(<text x="60" y="44" fill="#c8d0e0" font-size="22">Synthetic Rack</text>)"
            R"(</svg>)";
    }

    static std::vector<pulp::view::DesignFrameElement> makeElements() {
        using E = pulp::view::DesignFrameElement;
        auto knob = [](float cx, float cy, std::string key) {
            E e;
            e.kind = E::Kind::knob;
            e.cx = cx; e.cy = cy; e.hit_radius = 26.0f;
            e.needle_d = "M0 0L0 -22";
            e.value = 0.5f;
            e.param_key = std::move(key);
            return e;
        };
        auto action = [](float x, float y, float w, float h, std::string id) {
            E e;
            e.kind = E::Kind::action;
            e.x = x; e.y = y; e.w = w; e.h = h;
            e.action = std::move(id);
            return e;
        };

        std::vector<E> els(8);
        // Static top row.
        els[kIdxInputGain] = knob(140.0f, 110.0f, "input.gain");
        els[kIdxMix]       = knob(300.0f, 110.0f, "mix");
        // Dynamic rack row — initial slot 0.
        els[kIdxRackA]     = knob(360.0f, 250.0f, slotKey(0, 0));
        els[kIdxRackB]     = knob(540.0f, 250.0f, slotKey(0, 1));
        // Slot readout (live display text).
        E label;
        label.kind = E::Kind::value_label;
        label.x = 620.0f; label.y = 236.0f; label.w = 200.0f; label.h = 28.0f;
        label.text = "SLOT 1/3";
        els[kIdxSlotLabel] = label;
        // Paging chevrons + host action.
        els[kIdxPrev]       = action(60.0f, 236.0f, 40.0f, 40.0f, "rack.prev");
        els[kIdxNext]       = action(108.0f, 236.0f, 40.0f, 40.0f, "rack.next");
        els[kIdxLoadPreset] = action(720.0f, 24.0f, 140.0f, 34.0f, "load_preset");
        return els;
    }

    int current_slot_ = 0;
    std::function<void(const std::string&)> host_action_hook_;
};

// Factory for the embed's NativeViewFactory ctor.
inline std::unique_ptr<pulp::view::View> makeSyntheticRackView() {
    return std::make_unique<SyntheticRackView>();
}

}  // namespace synth_rack
