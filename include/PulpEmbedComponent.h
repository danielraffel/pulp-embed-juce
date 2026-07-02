// PulpEmbedComponent — a juce::Component that embeds a Pulp-imported design
// (DesignIR JSON) rendered through the pulp_view_embed C ABI.
//
// Host-parents mode: Pulp hands us its child native view; JUCE's NSViewComponent
// parents/retains/resizes it, and we call pulp_embed_notify_attached() once it
// is in a live window so Pulp fires its view-opened lifecycle. No Pulp C++ type
// appears here — only the flat C ABI header.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_gui_extra/juce_gui_extra.h>  // juce::NSViewComponent (macOS)
#include <pulp_view_embed.h>
#include <pulp_view_embed_native.hpp>  // pulp::embed::NativeViewFactory + create_from_view

#include <functional>
#include <memory>
#include <vector>

namespace juce {
class AudioProcessor;        // fwd — full type only needed in the .cpp
class AudioProcessorEditor;  // fwd — configureResizableEditor takes a reference
}  // namespace juce

namespace pulp_juce {

class PulpEmbedComponent : public juce::Component,
                           private juce::Timer {
public:
    // Builds the embedded view from either an importer JS bundle directory
    // (high-fidelity scripted-UI path; contains ui.js) or a DesignIR JSON file
    // (lightweight native path) — auto-detected. logicalSize is the design's
    // logical size (also used as the design viewport pin).
    PulpEmbedComponent(const juce::File& source,
                       int logicalWidth, int logicalHeight);

    // Same, but bind the design's controls to a juce::AudioProcessor's
    // parameters by string key (the design control key == the JUCE parameter
    // ID, e.g. APVTS ParameterID). Wires the pulp_view_embed host bridge so a
    // dragged knob writes the host parameter with begin/set/end gestures, and
    // host automation / preset recall pushes values back into the matching
    // control (polled on the 30 Hz tick). Controls whose key has no matching
    // JUCE parameter stay visual-only. `processor` must outlive this component.
    //
    // This is the surface real plugins need: without it the embedded UI renders
    // but no knob drives a parameter and no automation moves the UI.
    PulpEmbedComponent(const juce::File& source,
                       int logicalWidth, int logicalHeight,
                       juce::AudioProcessor& processor);

    // Mount a HAND-BUILT native Pulp view (a compiled pulp::view::View, typically
    // a DesignFrameView subclass) instead of an importer-generated design. The
    // factory builds the root tree on the Pulp side. A DesignFrameView whose
    // elements carry a param_key binds to the host parameters exactly like the
    // file-based ctor above (key == JUCE parameter ID); elements without a
    // param_key stay visual-only. `processor` must outlive this component. This
    // is the path for a UI authored in C++/Skia rather than imported.
    PulpEmbedComponent(pulp::embed::NativeViewFactory factory,
                       int logicalWidth, int logicalHeight,
                       juce::AudioProcessor& processor);
    // Same, without a host-parameter bridge (preview / no automation).
    PulpEmbedComponent(pulp::embed::NativeViewFactory factory,
                       int logicalWidth, int logicalHeight);
    ~PulpEmbedComponent() override;

    // Count of design controls that resolved to a host parameter (0 when
    // constructed without a processor, or when no design key matched a
    // parameter ID). Handy for self-checks / "is the bridge live?".
    int boundParameterCount() const noexcept;

    // ── runtime host-param accessor (ABI v8 adapter half) ───────────────────
    // Back the v8 host callbacks (has_param / param_display_text) with a
    // paramID-keyed view of the processor's parameters that is LIVE — it tracks
    // parameters added/removed after construction (rebuilt on the processor's
    // audioProcessorChanged). This is what dynamic/paged UIs (effect racks,
    // tab groups) need: a control whose key resolves only after a rack slot is
    // populated can ask "does this key exist yet?" and "what's the display text
    // for this normalized value?" without a remount. Both are no-ops (false /
    // empty) when constructed without a processor.

    // Live membership test: true iff `key` currently resolves to a host
    // parameter (== the JUCE parameter ID). Reflects late-added parameters.
    // This is the single source of truth the v8 has_param callback trampolines
    // into; the -1.0 get_param sentinel stays only as belt-and-braces
    // for pre-v8 runtime libraries.
    bool hostHasParam(const juce::String& key) const;

    // Formatted display text for `key` at `normalized` [0,1] — e.g. "500 ms",
    // "-6.0 dB" — via juce::AudioProcessorParameter::getText. Memoized per
    // (key, normalized) pair so repeated per-tick queries don't re-run a
    // plugin's getText override. Empty when the key is unknown (logged once,
    // never per frame).
    juce::String hostParamDisplayText(const juce::String& key, double normalized) const;

    // UI->host write seams (the single source of truth the v8 set_param /
    // begin_gesture / end_gesture callbacks trampoline into). They resolve keys
    // against the SAME live parameter map as hostHasParam, so a paged/dynamic
    // control re-keyed after create writes correctly (previously the write
    // path used a stale create-time snapshot and silently dropped paged
    // writes). Return true iff `key` resolved to a host parameter.
    bool hostWriteParam(const juce::String& key, double normalized);
    bool hostBeginGesture(const juce::String& key);
    bool hostEndGesture(const juce::String& key);

    // ── host action/command channel (ABI v8 adapter half) ───────────────────
    // Opaque command + JSON args from the embedded view (a view calls the SDK
    // host-action surface, which the v8 host_action callback bridges here). The
    // adapter parses args_json to a juce::var and invokes this. Return true if
    // the plugin handled it (diagnostic only — never control flow; unhandled
    // actions are logged, not fatal). Example uses: insert / remove / reorder
    // rack slots, load a preset. Set it on the owning editor; unset = every
    // action is an unhandled no-op. Fires only when constructed with a processor.
    std::function<bool(const juce::String& action, const juce::var& args)> onHostAction;

    // Invoke the host-action channel exactly as the v8 callback would: parse
    // `argsJson` to a juce::var and call onHostAction. Returns the handler's
    // result (false when no handler is set). The ABI callback trampolines into
    // this, and tests drive it directly (the channel is exercisable without the
    // v8 runtime present).
    bool dispatchHostAction(const juce::String& action, const juce::String& argsJson);

    // ── resizable editor helper ──────────────────────────────────────────────
    // Configure the OWNING AudioProcessorEditor for host-window resizing that
    // matches the imported design: reads pulp_embed_size_hints (ABI v7), calls
    // setResizable(true, false), installs a juce::ComponentBoundsConstrainer
    // pinned to the design's aspect ratio with min/max bounds from the hints,
    // and sizes the editor to the design's preferred size on open. The embed
    // already letterboxes content to the design viewport internally
    // (compute_design_viewport_transform), so this does NOT re-derive any
    // transform — it only constrains the host window.
    //
    // NOTE: the heavyweight embed NSView covers JUCE's lightweight corner grip,
    // so the grip can't drive resize — resizing is host-window-driven (drag the
    // window edge / the host's plugin-window chrome). Call once after the embed
    // is added to the editor. No-op when the design is non-resizable or hints
    // are unavailable. The installed constrainer lives as long as this component,
    // so it must outlive the editor's resize interactions.
    void configureResizableEditor(juce::AudioProcessorEditor& editor);

    // One design control's parameter description (ABI v5 metadata), for a
    // GREENFIELD plugin that wants to BUILD its APVTS parameters from the design.
    // `key` is the design control key (== the JUCE parameter ID to bind to);
    // `isDiscrete` + `optionCount` choose AudioParameterChoice/Bool vs Float;
    // `defaultNorm` is the imported default [0,1]. `name`/`unit` are populated
    // once the importer carries them (empty until then — fall back to `key`).
    struct DesignParamDesc {
        juce::String key;
        juce::String widgetKind;   // "knob"/"fader"/"toggle"/"dropdown"/"tab_group"/"stepper"
        bool         isDiscrete = false;
        int          optionCount = 0;
        double       defaultNorm = 0.0;
        juce::String name;         // "" until imported
        juce::String unit;         // "" until imported
    };

    // Descriptors for the design's bindable controls (in stable ABI order), read
    // from the live view. A greenfield processor more typically wants them BEFORE
    // the editor exists — use the static readDesignParams() for that.
    std::vector<DesignParamDesc> designParams() const;

    // Current normalized [0,1] value of the design control at ABI `index`
    // (== the designParams() / pulp_embed_param_* ordering). -1.0 if out of range
    // or no view. Reads the LIVE view, so it reflects the value after the
    // host<->UI initial sync — used to verify that an UNBOUND control kept its
    // imported default instead of snapping to 0.
    double controlValue(int index) const;

    // Static greenfield entry point: read a design's parameter descriptors WITHOUT
    // an editor/window (offscreen), so a processor can build its
    // AudioProcessorValueTreeState::ParameterLayout at construction time straight
    // from the design. `source` is a bundle dir (ui.js) or a DesignIR JSON file.
    static std::vector<DesignParamDesc> readDesignParams(const juce::File& source,
                                                         int logicalWidth,
                                                         int logicalHeight);

    // ── text-field string state (ABI v6) ───────────────────────────────────
    // A design's text_field controls carry a UTF-8 string bound to the plugin's
    // OWN state (preset name / label / search text) — saved/restored with the
    // plugin, NOT a DAW-automatable parameter (so it rides the plugin's state,
    // not the APVTS). These read/write the live view, so they work with or
    // without a processor. Use captureStringState() in getStateInformation() and
    // restoreStringState() in setStateInformation().

    // Number of bindable text_field string controls in the design.
    int stringFieldCount() const noexcept;

    // The string control's design key at `index` (empty if out of range).
    juce::String stringFieldKey(int index) const;

    // Current UTF-8 text of the string control identified by `key` (empty if the
    // key is unknown).
    juce::String stringValue(const juce::String& key) const;

    // Host -> view: set the text of the string control identified by `key`
    // (preset recall). Returns true on success; a key matching no text_field is a
    // tolerated no-op (still true), so a blind restore is safe.
    bool setStringValue(const juce::String& key, const juce::String& value);

    // Snapshot every text_field's key/value for getStateInformation(). Reads the
    // live view, so it reflects in-editor edits even without a change handler.
    juce::StringPairArray captureStringState() const;

    // Restore a snapshot in setStateInformation(). Pushes each value host -> view
    // without echoing back through the change handler (no edit loop).
    void restoreStringState(const juce::StringPairArray& state);

    // Optional live-edit notification: invoked (key, new value) whenever the user
    // edits a text_field, so the plugin can mark its state dirty / updateHost
    // DisplayName. Only fires when constructed with a processor (the string
    // callbacks share that bridge's host_ctx).
    void setStringChangeHandler(std::function<void(const juce::String&, const juce::String&)> fn);

    bool isValid() const noexcept { return view_ != nullptr; }
    juce::String lastError() const;
    bool isGpuBacked() const noexcept;
    bool isOpened() const noexcept { return opened_; }

    // Dev hot-reload watcher: poll the bundle's ui.js mtime on the existing
    // timer and call pulp_embed_reload_bundle when it changes (debounced one
    // tick so a mid-write save doesn't reload a half-written file). Editing the
    // bundle then reloads the open editor live — no DAW reload. Bundle path only.
    // Auto-enabled at construction when PULP_EMBED_HOT_RELOAD is set; call this
    // to force it on/off. No-op for the DesignIR (.json) path. Ship it off in
    // release builds (it's a developer loop).
    void enableBundleHotReload(bool enable = true);

    // Verification helpers (used by the self-check demo). writeCapturePng grabs
    // the LIVE GPU back buffer of the running window; writeRenderPng is the
    // deterministic Skia raster. Both return true on success.
    bool writeCapturePng(const juce::File& out);
    bool writeRenderPng(const juce::File& out, int width, int height);

    void resized() override;

    // Hover routing — pulp-view-embed has no platform mouse-tracking code,
    // so without forwarding these events to `pulp_embed_dispatch_mouse_*`,
    // `View::set_hovered` is never called and CSS :hover / onMouseEnter /
    // onMouseLeave never fire in the embedded plugin context, even though
    // `registerHover(id)` correctly arms the lambdas. JUCE delivers
    // `mouseMove` to a Component whenever the cursor is over it (no button
    // required, no opt-in) — this drives hover in the offscreen /
    // host-composited path where THIS component receives the pointer. In the
    // native-view path the child NSView receives platform mouse-moved events
    // directly, so these forwards are simply dormant there.
    void mouseMove(const juce::MouseEvent& e) override;
    void mouseEnter(const juce::MouseEvent& e) override;
    void mouseExit(const juce::MouseEvent& e) override;
    // Press/drag/release forwarding — without these the foreign-host embed never
    // delivers a button gesture to Pulp, so embedded knobs/faders can't be
    // dragged. Routes to pulp_embed_dispatch_mouse_down/drag/up.
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;

private:
    void timerCallback() override;

    // Shared construction body: build the desc (wiring the host bridge when
    // bridge_ is set), create the view, attach it, and start the tick timer.
    void createView(const juce::File& source, int logicalWidth, int logicalHeight);
    void createViewFromFactory(pulp::embed::NativeViewFactory factory,
                               int logicalWidth, int logicalHeight);
    // Shared by both create paths: build the embed descriptor (incl. host bridge
    // wiring) and run the post-create attach + bind + tick.
    PulpEmbedDesc buildDesc(int logicalWidth, int logicalHeight) const;
    void attachAndStart();
    // After the view exists, map design param keys -> host parameters and push
    // initial values UI<-host. No-op when bridge_ is null.
    void resolveParameterBindings();
    // Push any host-side parameter changes (automation / preset recall) into the
    // matching controls. Called from the 30 Hz tick. No-op when bridge_ is null.
    void pumpHostToUi();

    struct HostBridge;  // defined in the .cpp; holds the juce::AudioProcessor map
    std::unique_ptr<HostBridge> bridge_;

   #if JUCE_MAC
    juce::NSViewComponent nsView_;
   #endif
    PulpEmbedView* view_ = nullptr;
    bool opened_ = false;

    // The design's logical size (from the ctor). Retained so configureResizableEditor
    // can fall back to it and so resized() can recognise the design base. The ctor
    // deliberately does NOT force this as the component size (size-on-open):
    // the owning editor drives size, and the first NON-ZERO resized() issues the
    // first pulp_embed_resize — this kills the reopen-while-zoomed double-render.
    int logicalWidth_ = 0;
    int logicalHeight_ = 0;

    // Host-window resize constraint installed by configureResizableEditor;
    // must outlive the editor's resize interactions, so it lives on the component.
    std::unique_ptr<juce::ComponentBoundsConstrainer> constrainer_;

    // Dev hot-reload watcher state (bundle path only).
    bool watch_ = false;
    juce::File watchFile_;       // the bundle's ui.js
    juce::int64 lastWrite_ = 0;  // last applied mtime (ms)
    juce::int64 pendingWrite_ = 0;  // mtime seen but not yet stable (debounce)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PulpEmbedComponent)
};

}  // namespace pulp_juce
