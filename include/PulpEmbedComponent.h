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

#include <memory>

namespace juce { class AudioProcessor; }  // fwd — full type only needed in the .cpp

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
    ~PulpEmbedComponent() override;

    // Count of design controls that resolved to a host parameter (0 when
    // constructed without a processor, or when no design key matched a
    // parameter ID). Handy for self-checks / "is the bridge live?".
    int boundParameterCount() const noexcept;

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

private:
    void timerCallback() override;

    // Shared construction body: build the desc (wiring the host bridge when
    // bridge_ is set), create the view, attach it, and start the tick timer.
    void createView(const juce::File& source, int logicalWidth, int logicalHeight);
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

    // Dev hot-reload watcher state (bundle path only).
    bool watch_ = false;
    juce::File watchFile_;       // the bundle's ui.js
    juce::int64 lastWrite_ = 0;  // last applied mtime (ms)
    juce::int64 pendingWrite_ = 0;  // mtime seen but not yet stable (debounce)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PulpEmbedComponent)
};

}  // namespace pulp_juce
