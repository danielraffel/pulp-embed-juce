# Porting a JUCE plugin UI to Pulp

This is the cross-repo walkthrough for replacing a JUCE plugin's editor with a
Pulp GPU UI while keeping the JUCE/DSP engine unchanged. Your plugin stays a
`juce::AudioProcessor`; only `createEditor()` changes. Nothing about your JUCE
licensing obligations changes — you build in your own project, under your own
JUCE license.

The canonical, buildable reference for everything below is
[`examples/synthetic-rack/`](../examples/synthetic-rack/README.md).

---

## 1. Decision tree — which UI path?

```
Do you already have a design in a metadata-carrying tool
(Figma / Stitch / v0 / Pencil / Claude Design / React Native)?
│
├─ YES ──▶ IMPORTED DESIGN
│          `pulp import-design` → an --emit js bundle (or ir-json).
│          Mount it with PulpEmbedComponent(juce::File source, w, h [, processor]).
│          Tag controls by giving each interactive node an id == your JUCE
│          parameter ID; static binding is automatic. Re-run the importer to
│          restyle — no C++ edits. Best when a designer owns the look.
│
├─ NO, I'll build the UI in C++ ──▶ HAND-BUILT NATIVE VIEW   ◀── this walkthrough
│          Author a pulp::view::DesignFrameView (subclass), set each element's
│          param_key, mount it with the NativeViewFactory ctor. Full control of
│          custom paint / paged / dynamic controls. This is what synthetic-rack
│          and Dream Date FX use.
│
└─ I want HTML/CSS/JS chrome, not native widgets ──▶ WEBVIEW
           Out of scope for pulp-embed-juce. Use a juce::WebBrowserComponent or
           Pulp's webview-ui lane directly; you lose the GPU/Skia render path and
           the param_key bridge described here.
```

Flex + grid only: Pulp's layout engine is Yoga, so CSS block/float/table/
multi-column designs won't import. Knob/fader/panel plugin UIs are the sweet
spot.

---

## 2. The 5-step recipe (hand-built native view)

### Step 1 — get the SDK + embed library

Primary path: download the signed, notarized
`libpulp_view_embed.dylib` + SDK-headers tarball from a GitHub Release and point
`CMAKE_PREFIX_PATH` at it. Interim path: build the Pulp SDK from source, then
point `-DPULP_VIEW_EMBED_DIR` at a `pulp-view-embed` checkout (a sibling
`../pulp-view-embed` is auto-detected — see the root `CMakeLists.txt`).

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk-install \
  -DPULP_VIEW_EMBED_DIR=/path/to/pulp-view-embed   # or rely on the sibling probe
```

Your binary statically links the Pulp SDK (Skia + Dawn, tens of MB), but **no
Pulp C++ type enters your JUCE translation units** on the imported-design path —
you include only `pulp_view_embed.h`. On the hand-built path you additionally
include `pulp/view/design_frame_view.hpp` to author the view.

### Step 2 — add `PulpEmbedComponent` to your editor

Your editor owns one `PulpEmbedComponent` and forwards `resized()`:

```cpp
embed_ = std::make_unique<pulp_juce::PulpEmbedComponent>(
    pulp::embed::NativeViewFactory{&makeMyView}, kW, kH, processor);
addAndMakeVisible(*embed_);
// in resized(): embed_->setBounds(getLocalBounds());
```

`processor` must outlive the component. Construct without it for a preview
(no automation, no binding).

### Step 3 — build the `DesignFrameView` and tag controls with `param_key`

Author your UI as a `DesignFrameView` (or subclass). Give each interactive
element a `param_key` **equal to the JUCE parameter ID** it drives:

```cpp
DesignFrameElement gain;
gain.kind = DesignFrameElement::Kind::knob;
gain.cx = 140; gain.cy = 110; gain.hit_radius = 26; gain.needle_d = "M0 0L0 -22";
gain.param_key = "input.gain";   // == your APVTS ParameterID
```

Static controls now bind bidirectionally with zero glue: a drag writes the host
parameter (begin/set/end gesture), automation/preset recall pushes the value
back into the control on the 30 Hz tick. `boundParameterCount()` reports how
many resolved. Unmatched keys stay visual-only (and keep their imported default —
the `-1.0` unknown-key sentinel, never snapped to 0).

### Step 4 — wire the runtime accessor + actions for DYNAMIC controls

Static binding covers controls that exist for the plugin's whole life. For
**paged/dynamic** controls (effect racks, tabs, slots that appear late), re-key
the element when the page changes and let the runtime accessor resolve it:

```cpp
// in the view, on paging:
set_element_param_key(rackKnobA, "slot" + std::to_string(slot) + ".gain");
// the embed marks its key→index registry dirty and rebinds on the next tick.

// in the editor, query live membership / display text (ABI v8):
if (embed_->hostHasParam("slot2.cutoff"))
    label = embed_->hostParamDisplayText("slot2.cutoff", 0.5);  // "50 %"
```

For view→host **commands** (load preset, insert/reorder rack slots), declare a
`Kind::action` button and handle it in the editor:

```cpp
embed_->onHostAction = [this](const juce::String& action, const juce::var& args) {
    if (action == "load_preset") { loadPreset((int) args["index"]); return true; }
    return false;   // diagnostic only — unhandled is logged, never fatal
};
```

Calls into the host are legal only from tick/update, never from `paint()` — the
embed snapshots values + display text once per tick and paints from the snapshot.

### Step 5 — `configureResizableEditor`

One call gives you host-window resize locked to the design's aspect ratio, with
min/max from the design's size hints, sized to the design on open:

```cpp
// after adding the embed to the editor:
embed_->configureResizableEditor(*this);
```

Resizing is host-window-driven (drag the window edge / the host's plugin-window
chrome): the heavyweight embed NSView covers JUCE's lightweight corner grip, so
`useBottomRightCornerResizer` is off. The embed letterboxes content internally —
no transform is re-derived adapter-side.

For a chrome-free standalone (native title bar / full-screen), adopt the
[`support/`](../support/README.md) files
(`PulpStandaloneApp.cpp` + `ScalableEditorHost.h`).

---

## 3. The Debug/Release trap (read this before judging performance)

**A Debug build of the Skia/Dawn render stack runs ~3× the CPU of Release** — no
`-O3`/`NDEBUG`, live asserts, no inlining of Skia/Dawn/Yoga. "The embedded editor
feels slow" in a Debug build is almost always the build type, not the code. A
Debug-built adapter logs one line to that effect on first attach:
`[pulp-embed] built Debug — expect ~3x CPU; measure Release`.

**Always measure in Release:**

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
```

The synthetic-rack self-check prints an `idleCpu=…%` sample (editor open, no
interaction) — only meaningful in Release; it exists so the "≤ +3% vs the JUCE
editor" success metric stays a tested property.

---

## 4. Troubleshooting (keyed to real output)

**`create failed: result=N err=…`** — from `pulp_embed_last_create_error`. The
result codes (`PulpEmbedResult` in `pulp_view_embed.h`):

| result | code | meaning / fix |
|---|---|---|
| 1 | `INVALID_ARG` | null/zero arg, or `struct_size` / `abi_version` mismatch. Usually **version skew**: your headers are newer than the runtime `libpulp_view_embed`. Set `desc.abi_version = min(PULP_VIEW_EMBED_ABI_VERSION, pulp_embed_abi_version())` and rebuild; dynamic-UI (v8) features degrade gracefully on an older library. |
| 2 | `PARSE` | DesignIR JSON failed to parse (imported-design path). Re-run `pulp import-design`. |
| 3 | `MATERIALIZE` | native view-tree build failed — your `NativeViewFactory` returned `nullptr`, or the design produced an empty tree. |
| 4 | `VIEW_OPEN` | `ViewBridge::open()` failed. |
| 5 | `HOST_CREATE` | **no `PluginViewHost` factory** for the platform — the GPU host isn't registered (e.g. a CPU-only SDK build, or Windows before the Skia archive lands). |
| 6 | `ATTACH` | attach to the parent view didn't take (parent not in a live window yet). Make sure you `addAndMakeVisible` before the window is shown. |
| 7 | `UNSUPPORTED` | capability absent (e.g. CPU capture / no Skia readback). |
| 10 | `WRONG_THREAD` | called off the view's creator thread — call the ABI from the JUCE message thread. |

**`boundParameterCount() == 0`** — your control keys don't match any JUCE
parameter ID. The bind key is `param_key` (hand-built) / the control's widget id
(imported) and it must equal the `juce::ParameterID`. The bundled figma demo is
visual-only, so it binds 0 by design — point at a param-bound design.

**Unbound control snaps to 0** — you're on an older adapter that returns 0 for
an unknown key. Adopt the `-1.0` unknown-key `get_param` sentinel (shipped) so
unbound controls keep their imported default.

**Editor double-renders / wrong size when reopened while zoomed** — you're
forcing the design size in the ctor. Let the owning editor drive size
(size-on-open); the first non-zero `resized()` issues the first
`pulp_embed_resize`.

**Empty boxes / filenames instead of images in a headless screenshot** — the
CoreGraphics screenshot backend doesn't composite file images; re-render with
the Skia backend.

### `pulp-embed-validate` preflight

Before wiring anything into a DAW, run the preflight from `pulp-view-embed` — it
parses + materializes the design, prints the active backend (`GPU`/`CPU`), and
reports the parameter bridge, ABI-v5 per-control metadata, the ABI-v8 host-param
snapshot, and (ABI v7) missing-asset queries:

```bash
pulp-embed-validate <bundle-dir | design.ir.json> [--design-w N --design-h N] \
  [--host-keys gain,mix] [--out preview.png]
# → "design parses + materializes through the embed ABI" [PASS]
# → "active backend: GPU"
```

A red line here localizes the failure to the design/ABI before JUCE is in the
picture.

---

## 5. Next

- `examples/synthetic-rack/` — the buildable end-to-end reference for all of the
  above (static + paged binding, host action, resize, self-check).
- `../support/README.md` — chrome-free standalone wiring.
- Windows support is in progress (a prebuilt Windows Skia archive is the blocker;
  the `HWNDComponent` host is the same shape as the macOS `NSViewComponent` one).
