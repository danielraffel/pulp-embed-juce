# support/ — optional consumer support files

These are **reference/support files, not built by default**. Copy the ones you
need into your own JUCE project. They codify patterns a shipping Pulp-embed
consumer converged on but that don't belong in the adapter's static library.

## `PulpStandaloneApp.cpp` + `ScalableEditorHost.h`

A custom JUCE standalone that presents your plugin's `AudioProcessorEditor`
**chrome-free** — native title bar (so native full-screen and the traffic-light
buttons work), no JUCE tool-bar / options-button strip around the editor. Stock
`juce::StandaloneFilterWindow` forces that chrome strip and blocks native
full-screen, which is wrong for a design-first Pulp GPU editor.

Wire it in CMake on your **Standalone** target:

```cmake
target_compile_definitions(<your-standalone-target> PRIVATE
    JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1)
target_sources(<your-standalone-target> PRIVATE
    support/PulpStandaloneApp.cpp)
target_include_directories(<your-standalone-target> PRIVATE support)
```

With `JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP=1` the JUCE audio-plugin-client
Standalone wrapper does not emit its own `main()`; `PulpStandaloneApp.cpp`
provides one, reusing `juce::StandalonePluginHolder` for the plugin instance +
audio-device management so device selection and state still work.

`ScalableEditorHost` is a chrome-free container with a **fit-to-window scaling
seam**. For a `PulpEmbedComponent`-backed editor keep uniform scaling **off**
(the default): the embed letterboxes to its design viewport internally
(`compute_design_viewport_transform`), and a `juce::AffineTransform` on a
heavyweight `NSViewComponent` has no effect on AppKit's drawing. Use
`PulpEmbedComponent::configureResizableEditor()` for host-window aspect-locked
resize instead. The uniform-scale seam is there for a lightweight (non-embed)
editor.

## Known issue to file upstream — `nextDrawable` standalone crash

A consumer hit a `nextDrawable` (Metal drawable acquisition) crash in the
**standalone** app specifically when the settings/options tab was combined with
an active **audio input** device, and worked around it blind by cargo-culting
another demo's window setup. The signature is a Metal-layer drawable being
requested for a surface that is momentarily zero-sized / off-screen while the
audio-device settings panel is front-most.

This is a `pulp-view-embed` / GPU-host robustness gap (the GPU host should skip
`nextDrawable` for a zero-sized or occluded surface rather than block/crash),
not a JUCE-adapter bug. **Action:** file it against `pulp-view-embed` with a
minimal repro (standalone + audio-input device + settings tab front-most). Until
it's fixed, prefer the chrome-free window here (which keeps the editor surface
sized and front-most) over a stock standalone that surfaces the audio-settings
tab over a zero-sized editor.
