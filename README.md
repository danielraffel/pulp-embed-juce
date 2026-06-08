# pulp-embed-juce

A [JUCE](https://juce.com) adapter for
[`pulp-view-embed`](https://github.com/danielraffel/pulp-view-embed): embed a Pulp-imported design (e.g. a
Figma frame) as a `juce::Component` inside any JUCE app or plugin.

> Status: **experiment**. Thin wrapper over the flat C ABI — no Pulp C++ types
> cross into JUCE translation units (only `pulp_view_embed.h`).

## Status / what works / known limitations / roadmap

**What works (macOS):**

- `PulpEmbedComponent` (a `juce::Component`) embeds a Pulp-imported design at
  full fidelity via the `pulp_view_embed` flat C ABI (host-parents mode through
  `juce::NSViewComponent`).
- Auto-detects its source: an importer `--emit js` bundle dir (high-fidelity
  scripted-UI render, rasterized images/knobs/glass) vs a `.json` DesignIR
  (lightweight native widgets).
- A real `juce_add_plugin` target (`PulpEmbedJucePlugin`) in **VST3 + AU +
  Standalone** whose `AudioProcessorEditor` IS the embedded Pulp design.
- **Interactive parameters:** construct `PulpEmbedComponent` with a
  `juce::AudioProcessor&` and the design's controls bind **bidirectionally** to
  its `AudioProcessorParameter`s — a dragged knob writes the host param
  (begin/set/end gesture); host automation / preset recall pushes values back
  into the control (polled on the 30 Hz tick). The bind key is the **control's
  widget id == the JUCE parameter ID** (e.g. APVTS `ParameterID`); unmatched
  controls stay visual-only. `boundParameterCount()` reports how many resolved.
  (Before this, the C ABI exposed the bridge but the JUCE wrapper never wired
  it, so embedded knobs were visual-only.) The example plugin demonstrates it
  with real APVTS params; point `PULP_EMBED_BUNDLE` at a param-bound bundle to
  see a non-zero bind count (the bundled figma demo is visual-only → binds 0).
- Resize (`resized()` → `pulp_embed_resize`) and a 30 Hz tick timer; teardown
  in correct ownership order (null the `NSViewComponent` before
  `pulp_embed_destroy`).
- Headless self-check (`PULP_EMBED_SELFCHECK=1`) of the Standalone proves the
  editor renders + live-captures without a DAW.

**Known limitations:**

- macOS only today. Windows is the same shape via `juce::HWNDComponent` once
  `pulp-view-embed` registers a Windows `PluginViewHost` factory.
- Requires an installed Pulp SDK on `CMAKE_PREFIX_PATH` (no standalone build).
- Real-DAW load (Logic/REAPER/…) is a remaining manual validation step; CI
  covers build + headless render + pluginval-style editor lifecycle.

**Resolved design questions** (from the foreign-host-embedding plan):

- *Event-loop tick* — borrowed from the host: a `juce::Timer` (and the
  display-link inside Pulp's GPU host) drives `pulp_embed_tick`; the adapter
  does not run its own loop.
- *Parameter model* — string-key based, which maps cleanly onto JUCE's
  `AudioProcessorParameter` (the host owns the param objects and binds each to a
  design key once at editor-create time).

**Roadmap:** Windows `HWNDComponent` host; `pulp add`-style packaged
distribution; zero-copy GPU compositing (currently CPU RGBA readback for the
offscreen path).

## What gets embedded (FAQ)

- **What shows up in your editor?** One rendered child `juce::Component` — the
  imported design, drawn by Pulp — added to your `AudioProcessorEditor` like any
  other component.
- **Which designs?** Anything `pulp import-design` can import: **Figma, Claude
  Design, Stitch, v0, Pencil, React Native** (it consumes the importer's
  `--emit js` bundle or `--emit ir-json`, not the design tool directly). Pulp's
  layout is **flex + grid only**, so CSS block/float/table/multi-column designs
  are out of scope by design.
- **GPU or CPU?** GPU by default (Dawn/Metal + Skia Graphite); CPU raster
  fallback when the GPU stack is absent. The standalone here renders on GPU.
- **JS engine?** Only on the high-fidelity bundle path (Pulp's QuickJS scripted
  UI — that's what makes it pixel-match the importer). The lightweight DesignIR
  path uses native widgets, no JS.
- **Skia/Dawn or just C++?** Your binary statically links the Pulp SDK, which
  brings Skia + Dawn transitively (tens of MB) — but **no Pulp C++ type enters
  your translation units**; you include only `pulp_view_embed.h` (C).
- **Changing the UX later?** Re-run the importer and ship a new bundle (no C++
  edits); bind controls to JUCE `AudioProcessorParameter`s by string key via the
  ABI v3 param bridge to make them interactive.
- **Iterating fast (hot-reload)?** Launch with `PULP_EMBED_HOT_RELOAD=1` and edit
  the bundle's `ui.js` — the open editor live-reloads (values preserved), no
  re-import. Off by default so it never ships in a release. Use absolute asset
  paths (importer default) for the dev loop. See the core
  [Editing & hot-reload](https://github.com/danielraffel/pulp-view-embed#editing--hot-reload-the-dev-loop--no-re-import-per-tweak) guide.

Full architecture + supported-imports table + roadmap:
[`pulp-view-embed` README](https://github.com/danielraffel/pulp-view-embed#what-you-actually-get-plain-english-faq).

## Hot reload (dev loop)

Tweak the design while the plugin/standalone editor is open — no re-import, no
recompile. **Off by default** (so it never ships in a release):

1. Launch with the dev flag: `PULP_EMBED_HOT_RELOAD=1 open "Pulp Embed (JUCE).app"`
   (for a plugin, set it in the environment your DAW inherits).
2. Open the editor so the embedded design is visible.
3. Edit the bundle's `ui.js` (or `theme.json`) and save.
4. The editor live-reloads within a frame or two, **preserving knob/control
   values** — Pulp's `ScriptedUiSession` hot-reloader, pumped by the embed's
   per-tick `poll()`.

`PulpEmbedComponent` does this automatically: when `PULP_EMBED_HOT_RELOAD` is set
it arms a **debounced file-watcher** on the bundle's `ui.js` (polled on its 30 Hz
timer) that calls `pulp_embed_reload_bundle` on change — so you just save, no
manual step. Force it on/off with `component.enableBundleHotReload(true|false)`.
Leave it off in release builds.

Use the importer's default **absolute** asset paths for the dev loop (a
portabilized relative bundle resolves assets through the production wrapper,
which the watcher can't see). Full guide:
[Editing & hot-reload](https://github.com/danielraffel/pulp-view-embed#editing--hot-reload-the-dev-loop--no-re-import-per-tweak).

## Usage

```cpp
#include "PulpEmbedComponent.h"

// Visual-only:
auto* ui = new pulp_juce::PulpEmbedComponent(
    juce::File("design.ir.json"), 1000, 600);
setContentOwned(ui, true);   // add to a window / editor like any Component

// Interactive — bind the design's controls to your processor's parameters.
// Control widget id == JUCE parameter ID; bidirectional (UI gesture -> param,
// automation -> UI). `processor` must outlive the component.
auto* ui = new pulp_juce::PulpEmbedComponent(
    juce::File("design.ir.json"), 1000, 600, processor /* juce::AudioProcessor& */);
// ui->boundParameterCount() -> how many controls resolved to a parameter.
```

`PulpEmbedComponent` uses **host-parents mode**: it takes Pulp's child native
view via `pulp_embed_native_handle`, parents it through `juce::NSViewComponent`,
and calls `pulp_embed_notify_attached` once the view is in a live window so Pulp
fires its view-opened lifecycle. JUCE owns layout (`resized()` →
`pulp_embed_resize`); a 30 Hz timer drives `pulp_embed_tick`. Teardown nulls the
`NSViewComponent` reference before `pulp_embed_destroy` (correct ownership order).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install \
  -DPULP_VIEW_EMBED_DIR=/path/to/pulp-view-embed \
  -DPULP_EMBED_JUCE_BUILD_EXAMPLE=ON
cmake --build build -j
```

JUCE is pulled via FetchContent (pinned tag). The library target
`pulp_embed_juce` is the adapter; `-DPULP_EMBED_JUCE_BUILD_EXAMPLE=ON` also builds
a standalone demo app that embeds the bundled "VST Style" Figma fixture at full
fidelity (it points `PulpEmbedComponent` at the importer JS bundle).

Run the demo:

```bash
open "build/pulp-embed-juce-demo_artefacts/Release/Pulp Embed (JUCE).app"
```

`PulpEmbedComponent(juce::File source, w, h)` auto-detects its argument: a
directory containing `ui.js` (importer `--emit js` bundle) renders through the
high-fidelity scripted-UI path; a `.json` file uses the lightweight DesignIR path.

## Plugin (VST3 + AU)

`examples/plugin/` is a real `juce_add_plugin` target (`PulpEmbedJucePlugin`,
formats **VST3 + AU + Standalone**) whose `AudioProcessorEditor` hosts
`PulpEmbedComponent` — i.e. the plugin editor IS the embedded Pulp design. DSP is
silent passthrough. It builds by default; disable with
`-DPULP_EMBED_JUCE_BUILD_PLUGIN=OFF`.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install \
  -DPULP_VIEW_EMBED_DIR=/path/to/pulp-view-embed
cmake --build build --target PulpEmbedJucePlugin_All -j
# artefacts: build/PulpEmbedJucePlugin_artefacts/Release/{VST3,AU,Standalone}/
```

`COPY_PLUGIN_AFTER_BUILD` is OFF (validate-before-install). Install explicitly by
copying the `.vst3` / `.component` into `~/Library/Audio/Plug-Ins/{VST3,Components}`.

### Validate

```bash
# VST3 — pluginval (brew install --cask pluginval):
/Applications/pluginval.app/Contents/MacOS/pluginval --strictness-level 5 \
  --validate build/PulpEmbedJucePlugin_artefacts/Release/VST3/PulpEmbedJuce.vst3

# AU — auval (copy the .component into ~/Library/Audio/Plug-Ins/Components first):
cp -R build/PulpEmbedJucePlugin_artefacts/Release/AU/PulpEmbedJuce.component \
  ~/Library/Audio/Plug-Ins/Components/
auval -v aufx Pemj Pulp   # opens the AU briefly, silent passthrough DSP
```

### Verify the editor renders the embed (headless)

The Standalone wrapper opens the SAME `AudioProcessorEditor` the VST3/AU host
opens. Its self-check attaches the embed, renders a few live frames, captures,
and quits:

```bash
PULP_EMBED_SELFCHECK=1 \
  build/PulpEmbedJucePlugin_artefacts/Release/Standalone/PulpEmbedJuce.app/Contents/MacOS/PulpEmbedJuce
# → SELFCHECK opened=1 gpu=1 liveCapture=ok deterministic=ok
# writes /tmp/juce-plugin-live-capture.png (live GPU) and /tmp/juce-plugin-render.png
```

pluginval's editor open/close test additionally exercises the editor lifecycle
under a host. **Remaining manual step:** load in a real DAW (Logic, REAPER, …).

## Platform

macOS today (JUCE `NSViewComponent` over Pulp's NSView child). Windows is the
same shape via `juce::HWNDComponent` once `pulp-view-embed` registers a Windows
`PluginViewHost` factory.

## License

MIT.
