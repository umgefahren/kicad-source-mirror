# Porting another KiCad editor's UI to gpui + wgpu

Written for whoever does pcbnew next. It is not a description of what we built —
that is `01-plan.md` — it is the set of things we wish we had known on the first
day, including the two designs we got wrong and had to redo.

Read §4 before you write any code. Everything in it cost us time.

---

## 1. The short version

The schematic editor's UI was moved to Rust without porting a single drawing
rule, by adding a `KIGFX::GAL` backend that **records** draw calls into a flat
stream instead of rasterising them. `SCH_PAINTER` and `KIGFX::VIEW` were not
modified at all.

pcbnew works the same way, through the same abstraction, with the same
`KIGFX::VIEW`. `PCB_PAINTER` is a sibling of `SCH_PAINTER`. So the same backend
should serve it **unchanged**, and the interesting work is not the renderer — it
is the host, the tools, and pcbnew's much larger UI surface.

Budget your time accordingly. Rendering is the part that is already done.

---

## 2. What you inherit, already built and tested

| Component | Where | Reuse for pcbnew |
|---|---|---|
| Draw-stream ABI | `include/gal/recording/draw_stream_abi.h` | **Unchanged.** See §2.1 |
| `RECORDING_GAL`, `DRAW_STREAM` | `common/gal/recording/` | **Unchanged** |
| Draw-stream decoder | `rust/crates/kicad-gal` | **Unchanged** |
| Stream → gpui primitives | `rust/crates/kicad-sch-render` | Mostly unchanged; see §3.5 |
| Application shell | `rust/crates/kicad-sch-ui` | Structure reusable, content is editor-specific |
| cargo ↔ CMake integration | `cmake/KiCadRust.cmake` | **Unchanged** |
| Headless GPU + compositor test env | `tools/rust-gpu-testenv/` | **Unchanged** |
| CI workflow | `.github/workflows/rust-sch-ui.yml` | Extend the path filter |

### 2.1 The ABI already covers pcbnew, and this was checked

The draw stream was deliberately designed against the **whole** `KIGFX::GAL`
interface rather than the subset eeschema happens to use. We then verified that
against pcbnew's actual call sites. Every GAL method pcbnew calls is either
recorded or is a non-virtual getter answered from base-class state:

* `DrawHoleWall` — pcbnew-only, has its own opcode (`KGDS_OP_HOLE_WALL`)
* `SetTarget` / `ClearTarget` — pcbnew calls `SetTarget` **105 times** against
  eeschema's handful; recorded
* `StartDiffLayer` / `EndDiffLayer` / `StartNegativesLayer` /
  `EndNegativesLayer` / `SetNegativeDrawMode` — pcbnew's layer-overlay blend
  modes; all recorded
* `DrawEllipse` / `DrawEllipseArc` — recorded
* `EnableDepthTest` — recorded

Two further pieces of evidence that a partial backend is a supported thing to
write, rather than something we got away with: **none of `KIGFX::GAL`'s 79
virtuals is pure**, and `CALLBACK_GAL` (`include/callback_gal.h`) is an existing
headless backend that overrides **exactly one** of them. `OPENGL_GAL` and
`CAIRO_GAL` override 68 each; `RECORDING_GAL` overrides 64.

So you should not need to touch the ABI. If you do, it is versioned
(`KGDS_VERSION`) and both sides assert their layout at compile time, and
`rust/crates/kicad-gal/tests/header_sync.rs` parses the C header as text and
cross-checks every opcode, flag and size against the Rust mirror. That test will
catch you the moment the two drift — it caught us within minutes.

---

## 3. What is actually different about pcbnew

### 3.1 Scale

|  | eeschema | pcbnew |
|---|---|---|
| Source files | 913 | **1,408** |
| Lines | ~405,000 | **~599,000** |
| Files touching wx | 389 | **536** |
| Dialog sources | 124 | **224** |
| Tool sources | 31 | **52** |

Roughly 1.5× on every axis. The UI surface, not the renderer, is where that
lands.

### 3.2 Layers are a first-class problem

A schematic has a modest fixed set of layers. A board has `PCB_LAYER_ID` plus
`GAL_LAYER_ID` — over 150 enumerators — with per-layer visibility, opacity, a
user-controlled active layer, and "high contrast" dimming of inactive layers.
`VIEW::SetTopLayer`, layer ordering and `SetLayerDepth` do far more work here.

The stream records `SET_LAYER_DEPTH` and `SET_TARGET` faithfully, so the
information is all present — but the **renderer** will need real depth handling
and the **UI** needs a layer widget, which has no schematic counterpart. Budget
for both.

### 3.3 Zones mean polygons, and polygons mean tessellation

Filled zones are large `SHAPE_POLY_SET`s. They arrive as `KGDS_OP_POLYGON`
contours with `KGDS_FLAG_HOLE` on holes, and unlike lines and arcs they **cannot**
be drawn analytically — they must be tessellated on the CPU by lyon.

This is the one place where pcbnew is likely to be genuinely harder than
eeschema for performance. Lean on the group cache: the geometry is stable, so
tessellation should happen once per zone per zoom level, never per frame. Verify
that with a benchmark on a real dense board early, not at the end.

### 3.4 Render targets are load-bearing

pcbnew switches render targets constantly and uses difference and negative blend
modes for layer overlays. eeschema barely exercises this, so that part of the
renderer is the **least tested code you will inherit**. Treat it as unproven and
write tests for it first.

### 3.5 What in `kicad-sch-render` is schematic-specific

Less than you would guess. The camera, the f64→f32 origin handling, culling, the
group cache, batching and the opcode→`PathBuilder` translation are all generic.
What is schematic-flavoured is mostly defaults and heuristics — zoom limits, grid
defaults, level-of-detail thresholds. Expect to parameterise rather than rewrite.

### 3.6 `BitmapText` renders differently, on purpose

pcbnew calls `GAL::BitmapText` (for pad numbers and similar). `OPENGL_GAL`
overrides it with a bitmap-font atlas; `RECORDING_GAL` does **not** override it,
so the base implementation resolves it through KIFONT and calls back into
`DrawGlyph` — which we record as geometry.

The consequence: that text renders as stroked outlines rather than from a bitmap
atlas. It is correct, scales cleanly and arguably looks better, but it **is** a
visible difference from the OpenGL canvas and it costs more geometry at low
zoom. Decide deliberately whether to keep it. There is a test asserting the
lowering works (`BitmapTextLowersToGeometryThroughTheBaseClass`).

### 3.7 The 3D viewer is a separate problem

`3d-viewer/` is its own OpenGL renderer with OpenCascade behind it. None of this
applies to it. Leave it alone; it is not part of a pcbnew UI port.

---

## 4. The traps

This is the section that will save you the most time.

### 4.1 `GAL` is concrete, so a missed override fails *silently*

`KIGFX::GAL` is not an abstract interface. Nearly every virtual has an empty
`{}` body, and the state setters have base implementations that update member
state. So if you forget to override one, nothing fails to compile and nothing
throws — the base class quietly updates its state, nothing reaches your stream,
and the renderer draws with stale state. You find out later, as a subtle visual
bug, with no obvious cause.

**This happened to us twice**, with `SetMinLineWidth` and `SetHoverColor`. Both
had opcodes defined and neither was ever emitted. We found them only by
enumerating which GAL methods pcbnew calls and diffing that against our
overrides.

The defence is in the tree: `EveryRenderStateSetterIsRecorded` in
`qa/tests/common/gal/test_recording_gal.cpp` drives every setter and asserts its
opcode appears. **Extend that test before adding any pcbnew-specific recording.**
And when in doubt, run this and diff it against `RECORDING_GAL`'s override list:

```sh
grep -rhoE "m_gal->[A-Za-z_]+" pcbnew/ | sed 's/.*->//' | sort -u
```

### 4.2 `VIEW` interleaves group recording with frame drawing

We initially assumed cached groups are recorded *before* a frame is drawn. They
are not. `VIEW::updateItemGeometry` records an item into a group during its
**update** pass, and `VIEW::draw` replays it during its **draw** pass, so the two
interleave over time.

With a single shared arena that means either relocating every group index after
each frame, or leaking one command block per frame. At 120 Hz, neither is
survivable. The fix — already in the tree — is **two independent arenas**, one
retained and one per-frame, with the rule that recording routes to the group
arena while a group is open and the frame arena otherwise.

Do not "simplify" this back into one arena. There is a test
(`RedrawingAnUnchangedViewCostsNothing`) that will fail if you do, and it fails
for a real reason.

### 4.3 `ChangeGroupColor` must not invalidate the group

`VIEW` recolours a *cached* item rather than re-recording it — that is how
selection and highlighting work — and it keeps using the same group id
afterwards.

Our first implementation dropped the group on `ChangeGroupColor`. The result
would have been that **every item vanished the instant it was selected**. Baking
the new colour into the recorded commands would have been almost as bad, since
it invalidates the renderer's cached GPU buffer for exactly the items the user
is interacting with most.

The colour and depth overrides therefore ride on the `DRAW_GROUP` command
(`KGDS_FLAG_GROUP_COLOR`, `KGDS_FLAG_GROUP_DEPTH`). Selection costs no
re-tessellation and no re-upload. Keep it that way.

### 4.4 Coordinates are nanometres and will not fit in an `f32`

KiCad internal units are nanometres. A large board spans well over 1e9 of them;
an `f32` has a 24-bit mantissa, about 1.7e7. Narrowing world coordinates to
`f32` anywhere before subtracting a camera origin produces visible jitter that
looks like a renderer bug and is not.

The stream stores `f64` for exactly this reason. Subtract the origin in `f64`,
then narrow. This matters *more* for pcbnew than for schematics, because boards
are physically larger in internal units.

### 4.5 gpui has no GPU escape hatch — do not go looking

We spent real effort establishing this, so you do not have to:

* You **cannot** get gpui's `wgpu::Device`, queue or render pass. `gpui` has no
  wgpu dependency at all (the dependency runs the other way), the live GPU
  context is private to the X11 and Wayland client structs, and the renderer's
  only entry point is `draw(&Scene)` with no custom-pass hook.
* You **cannot** usefully hand it a GPU texture. `paint_surface` is macOS-only;
  `paint_image` takes CPU bytes, costing ~25–33 MB of memcpy plus a
  multi-millisecond readback stall per 1080p frame.

The supported path is a custom `Element` emitting gpui's own primitives, and it
is genuinely good: `PathBuilder` is a full lyon frontend (stroked polylines with
caps, joins and miter limits, dash arrays, elliptical arcs, béziers, fill rules)
rasterised into a **4× MSAA** intermediate. Details in `02-gpui-kit-cookbook.md` §3.

### 4.6 gpui cannot rotate text — and it does not matter

`Window::paint_glyph` hard-codes `TransformationMatrix::unit()`, so gpui's text
path cannot rotate. For a PCB editor, where every other reference designator is
rotated, that would be fatal.

It does not apply, because **text never reaches the stream as text**. KIFONT
resolves it to glyphs before the GAL sees it: `STROKE_GLYPH` is a set of
polylines, `OUTLINE_GLYPH` is a `SHAPE_POLY_SET`. `RECORDING_GAL` lowers both to
ordinary geometry, so rotation is free and typography is identical to every
other KiCad canvas.

Do not reach for gpui's text API for board text. You do not need it.

### 4.7 Batching rules you cannot ignore

* **One `paint_layer`.** Primitives at the same `DrawOrder` merge into a single
  batch and therefore a single render pass. Interleaving quads and paths across
  draw orders fragments this and is the biggest performance cliff in gpui.
* **lyon's index buffer is `u16`.** A single `PathBuilder::build()` tops out at
  65,535 vertices, roughly 16,000 stroked segments, and returns
  `TessellationError::TooManyVertices` rather than panicking. Bucket by
  (colour, width, dash) and chunk. pcbnew will hit this far sooner than
  eeschema did.
* **Cull before tessellating.** gpui culls primitives against the content mask,
  but by then you have already paid the CPU tessellation cost.

### 4.8 `GetToolCanvas()` is smaller than it looks

`TOOLS_HOLDER::GetToolCanvas()` is pure virtual and returns a `wxWindow*`, which
makes it look like a hard structural blocker to feeding `TOOL_MANAGER` from a
non-wx host. On closer inspection it mostly is not, and this is worth knowing
before you plan a refactor around it:

* `TOOL_MANAGER` has **zero** wx references in its header. `TOOL_EVENT` is a
  plain value type with wx-free enums.
* Only `TOOL_DISPATCHER` is bound to wx (it derives from `wxEvtHandler`), and it
  is a thin translator. Its drag-threshold and auto-repeat helpers are `static`
  pure functions, reusable as they stand.
* The dispatcher's `GetToolCanvas()` call sites **null-guard it**
  (`common/tool/tool_dispatcher.cpp:590-598`), so returning `nullptr` is viable
  there.
* **eeschema has no `GetToolCanvas()` call sites at all** outside the new host.
  The fourteen in the tree are in `tool_dispatcher.cpp`, `eda_base_frame.cpp`,
  `dialog_shim.cpp`, and the other applications — pcbnew, the 3D viewer,
  bitmap2component, pcb_calculator.

**Audit that last point for pcbnew before relying on it.** `pcb_edit_frame.cpp`
is in the list, so a board editor host may have call sites a schematic host does
not, and whether each is guarded is the thing to check.

What remains is real but bounded: write a dispatcher that builds `TOOL_EVENT`s
from gpui input instead of wx events, and feed `TOOL_MANAGER::ProcessEvent` /
`PostEvent` / `DispatchHotKey` directly. Survey §6 has the exact event
constructions, the `BUT_*`/`MD_*` bit values, and the constraint that hotkeys are
`WXK_*` integers — so gpui key codes must map onto the same numbers.

Input is **not** wired up in the schematic port; see §7.

### 4.9 Toolchain and environment

* **gpui needs nightly Rust** (unstable `std::hint::cold_path`), pinned in
  `rust/rust-toolchain.toml`. Not a preference; a dependency constraint.
* **gpui's screenshot support is macOS-only.** `current_headless_renderer()`
  returns `None` on Linux, so golden-imaging *through gpui* does not work there.
  Use `tools/rust-gpu-testenv/` — headless sway plus lavapipe plus `grim` — which
  screenshots the real application instead.
* **`ydotool` does not work in a container** (no `/dev/uinput`). Use `wtype` on
  Wayland or `xdotool` on X11.
* **Watch the disk.** A full gpui dependency graph is several GB per cargo
  target directory, and a KiCad build tree is several more. Share one
  `CARGO_TARGET_DIR`; cargo will serialise on its lock, which is slower but
  survivable, whereas running out of disk mid-build is not.

---

## 5. Suggested staging

1. **Prove the recorder works for boards.** Extend `sch_dump` into a `pcb_dump`
   (or generalise it) and render real `.kicad_pcb` files to streams. Check
   command, group and coordinate counts against expectations. This is cheap and
   tells you immediately whether §2.1 held.
2. **Extend the setter-coverage test** with anything pcbnew touches that
   eeschema does not, per §4.1.
3. **Golden-stream fixtures** from a few representative boards — something dense,
   something with zones, something multi-layer.
4. **Renderer work**: layers/depth (§3.2), zone tessellation performance (§3.3),
   and render targets and blend modes (§3.4), which are the least-tested
   inherited code.
5. **`PCB_HOST`**, modelled on `SCH_HOST`.
6. **The shell**: reuse the structure, add the layer widget, the appearance
   panel and pcbnew's toolbars.
7. **Input** — only after `GetToolCanvas()` is dealt with (§4.8).

Steps 1–4 are largely independent of the C++ build and can proceed in parallel
with it.

---

## 6. Verification checklist

Before claiming the renderer is right:

- [ ] Every GAL method pcbnew calls is either recorded or a base-class getter
      (§4.1 has the one-liner)
- [ ] Redrawing an unchanged board grows retained group data by zero bytes
- [ ] Selecting an item causes no re-tessellation and no buffer re-upload
- [ ] A board spanning >1e9 internal units pans without jitter (§4.4)
- [ ] Zone-heavy boards hold the frame budget; measured, on a real board
- [ ] Layer visibility, active layer and high-contrast dimming all behave
- [ ] Difference and negative blend modes produce the same result as the
      OpenGL canvas
- [ ] The application actually renders — screenshotted headlessly, looked at

---

## 7. What we did not solve

Stated plainly so you do not assume it exists:

* **Input into `TOOL_MANAGER`** (§4.8). The single biggest remaining piece,
  though smaller than first assessed: the obstacle is writing a non-wx
  dispatcher, not refactoring `GetToolCanvas()`.
* **Dialogs.** All 124 of eeschema's are still wxWidgets; pcbnew has 224.
  `00-architecture-survey.md` §7.4 discusses keeping them, bridging them
  asynchronously through the existing tool coroutines, or rewriting them, and
  recommends the middle option as the migration path.
* **Removing wxBase.** `wxString` and friends remain throughout the C++ core.
  Only the GUI layer left the rendering path.
* **Printing and plotting.** Those go through `PLOTTER`, not the GAL, and were
  not touched.
