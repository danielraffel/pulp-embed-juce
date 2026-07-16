# pulp-embed-juce

The **adapter library** (`PulpEmbedComponent`) that embeds a Pulp-imported design
(e.g. a Figma frame) as a `juce::Component` inside any JUCE app or plugin. It's a
[JUCE](https://juce.com) wrapper over
[`pulp-view-embed`](https://github.com/danielraffel/pulp-view-embed). Both the
importer and the new-plugin template below depend on this adapter — **this is the
bridge you extend or study**, not a starting point for your own plugin.

> Status: **experiment**. Thin wrapper over the flat C ABI — no Pulp C++ types
> cross into JUCE translation units (only `pulp_view_embed.h`).

## Which repo do I want?

Three JUCE pieces, and they are **not interchangeable**:

| I want to… | Repo |
|---|---|
| **Import an existing JUCE plugin's UI** — automated: a Pulp UI over your *unchanged* JUCE DSP (`--emit hybrid-ui`) | [`pulp-import-juce`](https://github.com/danielraffel/pulp-import-juce) |
| **Start a NEW plugin from scratch** with a hand-built Pulp UI — clone the GitHub template → one bound knob (it does **not** import anything) | [`pulp-embed-juce-template`](https://github.com/danielraffel/pulp-embed-juce-template) |
| **Extend / understand the bridge** both of the above depend on | **`pulp-embed-juce` — the adapter library (this repo)** |

**Most common mix-up:** the *template* does not import anything. To bring an
existing plugin's UI across, use
[`pulp-import-juce`](https://github.com/danielraffel/pulp-import-juce), not the
template. Canonical map: the Pulp SDK guide
[**Putting a Pulp UI in a JUCE plugin**](https://github.com/danielraffel/pulp/blob/main/docs/guides/juce-embed.md).

> **Note on this repo's own examples.** `examples/` here (including
> `examples/synthetic-rack/`) `add_subdirectory` the adapter — they exist to
> **develop and test the adapter itself**, not as a consumer starter. If you want
> a clean project to build *your* plugin on, start from
> [`pulp-embed-juce-template`](https://github.com/danielraffel/pulp-embed-juce-template).

**Porting a JUCE plugin UI by hand?** Start with the walkthrough:
[`docs/porting-a-juce-plugin-ui.md`](docs/porting-a-juce-plugin-ui.md) (decision
tree + 5-step recipe + Debug/Release trap + troubleshooting), and the canonical
end-to-end example [`examples/synthetic-rack/`](examples/synthetic-rack/README.md)
(static + paged param binding, a host action, `configureResizableEditor`, and a
headless self-check, in one small plugin).

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
  `param_key` == the JUCE parameter ID** (e.g. APVTS `ParameterID`); unmatched
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

**Resolved design questions** for foreign-host embedding:

- *Event-loop tick* — borrowed from the host: a `juce::Timer` (and the
  display-link inside Pulp's GPU host) drives `pulp_embed_tick`; the adapter
  does not run its own loop.
- *Parameter model* — string-key based, which maps cleanly onto JUCE's
  `AudioProcessorParameter` (the host owns the param objects and binds each to a
  design key once at editor-create time).

**Roadmap:** Windows `HWNDComponent` host; `pulp add`-style packaged
distribution; zero-copy GPU compositing (currently CPU RGBA readback for the
offscreen path).

## Performance — measure in Release

**A Debug build of the Skia/Dawn render stack runs roughly ~3x the CPU of
Release** (no `-O3`/`NDEBUG`, live asserts, no inlining of Skia/Dawn/Yoga). A
"the embedded editor feels slow" observation in a Debug build is almost always
the build type, not the code — **always measure in Release before judging embed
performance.** A Debug-built adapter emits one runtime log line to that effect on
first attach (`[pulp-embed] built Debug — expect ~3x CPU; measure Release`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # ← the CPU number that matters
```

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
// Control param_key == JUCE parameter ID; bidirectional (UI gesture -> param,
// automation -> UI). `processor` must outlive the component.
auto* ui = new pulp_juce::PulpEmbedComponent(
    juce::File("design.ir.json"), 1000, 600, processor /* juce::AudioProcessor& */);
// ui->boundParameterCount() -> how many controls resolved to a parameter.
```

**Greenfield — build APVTS params from the design.** A processor declares its
parameters at construction (before any editor), so use the static
`readDesignParams()` to read the design's controls offscreen and generate the
layout (ABI v5 metadata: kind, discreteness, option count, default; name/unit
arrive with the importer metadata slice). The control key doubles as the
`ParameterID`, so binding "just works" afterward:

```cpp
juce::AudioProcessorValueTreeState::ParameterLayout layout;
for (auto& p : pulp_juce::PulpEmbedComponent::readDesignParams(
         juce::File("design.ir.json"), 1000, 600)) {
    const auto name = p.name.isEmpty() ? p.key : p.name;
    if (p.isDiscrete)
        layout.add(std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID{p.key, 1}, name, makeChoices(p.optionCount), 0));
    else
        layout.add(std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID{p.key, 1}, name,
            juce::NormalisableRange<float>(0.0f, 1.0f), (float) p.defaultNorm));
}
```

The host still owns the params (authoritative); an existing plugin keeps declaring
them by hand and uses `designParams()` (instance, post-create) as a cross-check.

### Dynamic controls — runtime host-param accessor & host actions

Static binding (control `param_key` == parameter ID, resolved once at create) covers
knobs/faders that exist for the plugin's whole life. **Dynamic / paged UIs**
(effect racks, tab groups, controls whose parameter appears only after a slot is
populated) need two more surfaces, both backed by the adapter and bridged over
the ABI (v8) when the runtime library supports it:

```cpp
// Runtime param accessor — LIVE membership + display text. Tracks parameters
// added/removed after construction (rebuilt on the processor's
// audioProcessorChanged), so a late-bound rack-slot key resolves without a
// remount. Display text comes from AudioProcessorParameter::getText, memoized
// per (key, value); unresolved keys are logged once, never per frame.
if (embed->hostHasParam("rack.slot0.mix"))
    label = embed->hostParamDisplayText("rack.slot0.mix", 0.5);  // e.g. "50 %"

// Host action/command channel — opaque command + JSON args from the view.
// Return true if handled (diagnostic only; unhandled actions are logged, not
// fatal). Example: insert/remove/reorder rack slots, load a preset.
embed->onHostAction = [&](const juce::String& action, const juce::var& args) {
    if (action == "load_preset") { loadPreset((int) args["index"]); return true; }
    return false;
};
```

When constructed against a pre-v8 runtime library the adapter negotiates the ABI
version down (`min(header, pulp_embed_abi_version())`) and these dynamic
features stay dormant; the `-1.0` unknown-key `get_param` sentinel keeps unbound
controls at their imported defaults regardless.

A paged control also **re-keys itself from inside the view** — no host call, no
reload. The adapter follows it: the host→UI pump re-resolves its bindings
whenever the view's key set moves (`pulp_embed_param_key_generation`, ABI v10) or
the processor's parameter tree changes, so a re-keyed control keeps tracking
automation. Both are gated, so an idle editor costs a single integer compare per
tick rather than a full key re-enumeration — `keyResolveCount()` exposes how
often the gate actually fired, and holds constant on an idle UI. Call
`syncFromHost()` to pump immediately instead of waiting for the next tick.

### Discrete controls: the divisor comes from the parameter (ABI v10)

A design cannot know a host parameter's discreteness. A radio drawn with **3**
visible options may be bound to a **6**-step parameter, and a control that
derives its value from the number of options it draws addresses the wrong steps.
Ask the host instead:

```cpp
const int steps = embed->hostParamStepCount("lfo_waveform");  // 6
// 0 means CONTINUOUS or UNKNOWN — the two are deliberately indistinguishable,
// so treat 0 as "do not use a step divisor" rather than a step count of zero.
```

Backed by `juce::AudioProcessorParameter::getNumSteps()`, so an
`AudioParameterChoice` reports its choice count and an `AudioParameterBool`
reports 2. JUCE has no "is continuous" predicate — it returns a large sentinel
(`AudioProcessor::getDefaultNumParameterSteps()`) for a parameter that declared
no step interval — and the adapter maps that sentinel to the contract's 0. The
value is a step **count**, not a pre-computed divisor; derive the divisor from it.

### Resizable editor (one call)

`configureResizableEditor()` reads the design's `pulp_embed_size_hints`, calls
`setResizable(true, false)`, installs a `juce::ComponentBoundsConstrainer` locked
to the design's aspect ratio with min/max from the hints, and sizes the editor
to the design's preferred size on open:

```cpp
// in your AudioProcessorEditor ctor, after adding the embed:
embed_->configureResizableEditor(*this);
```

Resizing is **host-window-driven** — the heavyweight embed NSView covers JUCE's
lightweight corner grip, so `useBottomRightCornerResizer` is false and you drag
the window edge / the host's plugin-window chrome. The embed letterboxes content
to the design viewport itself, so this only constrains the host window (no
transform is re-derived adapter-side).

### Standalone (chrome-free) support files

For a design-first standalone (native title bar, native full-screen, no JUCE
tool-bar chrome), see [`support/`](support/README.md):
`PulpStandaloneApp.cpp` + `ScalableEditorHost.h`, wired via
`JUCE_USE_CUSTOM_PLUGIN_STANDALONE_APP`. That doc also records a `nextDrawable`
standalone crash (settings-tab + audio-input) to file against `pulp-view-embed`.

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

### Drive a control by (key, value) — through the real gesture path

`simulateParamDragToValue` moves a named control to a normalized value by
synthesizing a **real pointer gesture** on it: it resolves the key to its
control, asks the ABI where that control is (`pulp_embed_param_hit_point`), then
presses / drags / releases at those coordinates.

```cpp
if (!embed->simulateParamDragToValue("cutoff", 0.8))
    return fail("cutoff is not drivable");
// The host parameter has already moved — assert it now (see the note below).
```

It runs the same code a user's mouse runs — hit-test, capture, the control's own
drag law, its emit path, the host bridge — so a regression anywhere along it
makes this **fail** rather than pass. That is the whole point: a helper that
reached past hit-testing and poked the value in would stay green while the UI was
unclickable, and a test that cannot fail the way production fails is worth little.

Two consequences worth knowing:

- **It returns `false`, never a silent no-op**, for an unknown key, a control the
  ABI cannot locate, or a control that does not respond to the drag — each logs
  which. In a QA harness a silent no-op is the worst outcome: it makes a broken
  control look tested. Treat `false` as a failed check.
- **It does not model the drag law, it measures it.** The loop probes the control,
  reads back what it did, and converges — so knob vs fader sensitivity, direction,
  and window scaling are all handled without this code knowing any of them, and a
  law that regressed fails to converge rather than being faithfully mirrored by a
  matching inverse here.

Discrete parameters are snapped onto the host's step grid first, so you can pass a
rounded value and still land on the intended step. The divisor is the step
**count minus one** — see [the divisor section](#discrete-controls-the-divisor-comes-from-the-parameter-abi-v10).

### Gotcha: assert the host *parameter*, not the engine — but do it synchronously

A UI write reaches the **host parameter** synchronously: by the time
`simulateParamDragToValue` (or a real mouse-up) returns, `getValue()` on the
parameter is already the new value. The plugin's own DSP/engine state is a
different thing — it is pulled from the parameter on the **audio thread, once per
block** — and a headless harness usually has **no audio callback at all**, so that
state may never update no matter how long you wait.

```cpp
embed->simulateParamDragToValue("cutoff", 0.8);

// RIGHT — the parameter is the synchronous, host-facing truth.
auto* p = processor.getParameters()[cutoffIndex];
check(std::abs(p->getValue() - 0.8f) < 1e-3f);

// WRONG — engine state is filled in by processBlock, which never ran here.
check(dsp.currentCutoff() == ...);   // hangs on a "flaky" failure forever
```

So: assert the parameter, and assert it *immediately* — no waiting, no yielding.
Waiting for engine state to catch up in a headless harness is a wait that never
ends. (Do not confuse this with the capture rule below, which is the opposite
shape: pixels *do* need a frame to pass first.)

### Gotcha: capture on the *next* message-loop callback, not the same tick

A screen capture (`writeCapturePng` and friends) only records what has actually
been rendered — the pixels from the **last completed frame**. Capturing
synchronously in the same call stack that just changed state writes the *old*
frame (or, before the first paint, nothing at all):

```cpp
// WRONG — no frame has rendered since the change; the PNG is stale/empty.
processor.setValueNotifyingHost(paramIndex, 0.8f);
writeCapturePng("after.png");          // same tick, no paint pass yet

// RIGHT — let one frame render, then capture from the next callback.
processor.setValueNotifyingHost(paramIndex, 0.8f);
juce::Timer::callAfterDelay(32, [&] {  // one+ frame later, top of the loop
    writeCapturePng("after.png");
});
```

This is why the self-check renders a few live frames before it captures, and why
a QA harness that drives real commit paths should apply state, yield to the
message loop (a frame-gated `startTimer` / `callAfterDelay`), and capture on the
following callback. Capturing on the same tick is the classic "my PNG is blank /
one edit behind" trap.

### QA harness (`PulpEmbedQaHarness`)

That drive → settle → capture → compare dance is packaged in
`include/PulpEmbedQaHarness.h` so you don't re-roll the frame gating (or the
timing trap) per plugin. Give it your `PulpEmbedComponent`, a list of named
steps, an output path, and an optional reference PNG:

```cpp
// Hold the harness as a MEMBER — run() is async (it arms a Timer and returns),
// so a local would be destroyed before onDone fires. `qa_` outlives the run.
pulp_juce::PulpEmbedQaHarness qa_;

qa_.run(*embed,
        { { "load_preset routed", [&] { return embed->dispatchHostAction("load_preset", R"({"index":2})"); } } },
        outDir.getChildFile("render.png"),
        referenceDir.getChildFile("golden.png"),   // omit (empty File) to skip compare
        [](const auto& r) {
            juce::JUCEApplicationBase::getInstance()->setApplicationReturnValue(r.ok() ? 0 : 1);
            juce::JUCEApplicationBase::quit();
        });
```

It waits for `isOpened()`, lets a few frames render, runs each step, writes the
**deterministic** raster (`writeRenderPng` — CPU-stable, so a reference diff is
meaningful on a GPU-less CI runner), and — the part the hand-rolled self-checks
never had — diffs it against your reference via `PulpEmbedImageCompare.h`
(per-channel tolerance + an allowed diff-pixel fraction for incidental
antialiasing). Compare the deterministic raster, not the live GPU
`writeCapturePng`, against a committed reference; reserve live-capture compares
for a GPU lane. The pixel math is pure and unit-tested
(`test/qa_image_compare_test.cpp`, `ctest -R qa-compare-test`); the JUCE glue is
covered by `qa-harness-test`.

## Platform

macOS today (JUCE `NSViewComponent` over Pulp's NSView child). Windows is the
same shape via `juce::HWNDComponent` once `pulp-view-embed` registers a Windows
`PluginViewHost` factory.

## License

MIT for this adapter's own source.

This project **depends on** [JUCE](https://juce.com) but ships no JUCE source
and vendors no JUCE modules — it resolves JUCE from your own checkout, under
**your own JUCE license** (AGPLv3 or a commercial JUCE licence). Building and
distributing a binary from this repo is subject to JUCE's licensing terms, which
are your responsibility as the builder.

Not affiliated with or endorsed by JUCE or Raw Material Software.
