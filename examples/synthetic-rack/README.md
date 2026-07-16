# synthetic-rack — the canonical end-to-end example

A small JUCE plugin whose editor IS a hand-built native Pulp view, wired to show —
in one place — every runtime surface the JUCE→Pulp UI port needs:

| Surface | Where |
|---|---|
| **Static param binding** (`param_key` == JUCE parameter ID) | `SyntheticRackView` static knobs `input.gain` / `mix` |
| **Runtime-accessor-bound dynamic page** (rack knobs re-keyed with `set_element_param_key`, resolved via the ABI v8 `has_param` / `param_display_text`) | `SyntheticRackView` rack page + `PulpEmbedComponent::hostHasParam` / `hostParamDisplayText` |
| **Host action** (`Kind::action "load_preset"` → `onHostAction`) | `SyntheticRackEditor::onHostAction` |
| **Resizable editor** (aspect-locked host-window resize, one call) | `configureResizableEditor(*this)` |
| **Headless `--screenshot` / self-check** with an **idle-CPU** sample | `SyntheticRackEditor` self-check timer |

## Files

- `SyntheticRackView.h` — the hand-built `pulp::view::DesignFrameView` subclass:
  two static knobs, a paged effect rack (two knobs re-keyed per slot), a slot
  value-label, paging chevrons, and a `load_preset` action button. All from an
  inline SVG — no external assets.
- `SyntheticRackProcessor.h` — a real `juce::AudioProcessor` whose APVTS carries
  the two static params **plus** `kSlotCount` × `{gain, cutoff}` real parameters
  keyed `slot<N>.<id>` (the EffectHost shape). DSP is silent passthrough.
- `PluginMain.cpp` — the editor (mounts the view via the `NativeViewFactory`
  ctor, wires `onHostAction`, calls `configureResizableEditor`, runs the
  headless self-check) + `createPluginFilter`.
- `headless_selfcheck.cpp` — the CI-runnable console self-check (`ctest -R
  synthetic-rack-test`).

## Build & run

```bash
# Sibling checkout of pulp-view-embed is auto-detected (../pulp-view-embed);
# otherwise pass -DPULP_VIEW_EMBED_DIR=/path/to/pulp-view-embed.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install
cmake --build build --target SyntheticRackPlugin_All -j
# artefacts: build/SyntheticRackPlugin_artefacts/Release/{VST3,Standalone}/
```

Headless self-check (proves the editor binds the static + paged controls, routes
the host action, renders, and reports idle CPU — exits non-zero on any failure):

```bash
PULP_EMBED_SELFCHECK=1 \
  build/SyntheticRackPlugin_artefacts/Release/Standalone/SyntheticRack.app/Contents/MacOS/SyntheticRack
# → SELFCHECK opened=1 gpu=1 bound=4 idleCpu=…% live=ok det=ok result=PASS
```

CI-runnable adapter-level self-check (no window, no GPU display needed to build
the assertions — same idiom as the other console tests):

```bash
ctest --test-dir build -R synthetic-rack-test --output-on-failure
```

**Measure in Release.** A Debug build of the Skia/Dawn stack runs ~3× the CPU of
Release; the `idleCpu` number in the self-check is only meaningful in Release.

## The tri-host ratchet

This example is the JUCE corner of the central claim: a view written
against the runtime host-param surface runs **unchanged** in three hosts —
embedded in JUCE (APVTS via the ABI), embedded in iPlug2 (IParams), and native
Pulp (`StateStore`). The SDK-side test **`pulp-test-host-param-surface`** proves a
`DesignFrameView` binds identically against a fake JUCE/iPlug surface and the
native `StateStore` surface; this example is the JUCE-embed end of that ratchet
run as a real plugin. (Verify the SDK test is present on your Pulp checkout —
it lands with the SDK host-param surface work.)

## Dependencies (read before you `cmake`)

A full build needs JUCE + a GPU Pulp SDK + Skia, plus these surfaces across the
three repos:

1. **This repo** — the adapter surfaces (`hostHasParam`,
   `hostParamDisplayText`, `onHostAction` / `dispatchHostAction`,
   `configureResizableEditor`).
2. **`pulp-view-embed`** — ABI **v8** (`has_param` / `param_display_text` /
   `host_action` tail-append callbacks).
3. **Pulp SDK** — `DesignFrameView::set_element_param_key(i, key)`, the
   `HostParamSurface` / `View::host_params()` seam, and the
   `HostActionSurface` / `View::host_actions()` seam.

All three are present, so this example builds against a current SDK. It needs an
SDK new enough to carry the re-key mutator and the host surfaces; an older one
fails at compile time on `set_element_param_key`.
