# Porting another KiCad editor's UI to gpui + wgpu

> **Current planning:** [10-remaining-stages.md](10-remaining-stages.md) defines
> Stages 6–19 and their execution order. This document records an earlier survey,
> implementation milestone or reuse guidance; historical gaps and proposed
> approaches must be checked against the Stage 5 coverage and
> [completed Stage 6 verification](11-stage6-editing-correctness.md) before use.
> Stage 7 (real document sidebars) is next.


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
| `VIEW_CONTROLS` for a host that owns its pointer | `include/view/host_view_controls.h`, `common/view/host_view_controls.cpp` | **Unchanged** — in `common/` for this reason; it knows nothing about schematics |
| Host input → `TOOL_EVENT` | `include/tool/host_tool_dispatcher.h`, `common/tool/host_tool_dispatcher.cpp` | **Unchanged**, including the key-name → `WXK_*` table |
| Checked tool-holder casts in `common/tool/` | `COMMON_CONTROL`, `COMMON_TOOLS`, `ZOOM_TOOL`, `PICKER_TOOL`, `GROUP_TOOL`, `PROPERTIES_TOOL`, `EMBED_TOOL` | **Already done** — pcbnew registers most of the same tools, so this hazard is behind you (§4.8) |
| Process singletons for a headless host | `eeschema/host/sch_host_runtime.cpp` | **Unchanged** — `ksch_runtime_init` stands up wx, the settings manager and the kiface settings, and a board host needs exactly the same |
| Undo and redo stacks off `wxFrame` | `include/undo_redo_holder.h`, `common/undo_redo_holder.cpp` | **Unchanged** — `UNDO_REDO_HOLDER` is in `common/` for this reason; `PCB_BASE_EDIT_FRAME` already inherits it through `EDA_BASE_FRAME`, so a board host inherits it too and implements `ClearUndoORRedoList` the way `PCB_EDIT_FRAME` does |
| The editing-context pattern | `eeschema/schematic_holder.h`, `eeschema/schematic_undo_redo.h` | **The pattern, not the code.** pcbnew needs its own `BOARD_HOLDER`-shaped interface and its own `PCB_UNDO_REDO`; §4.11 is what to copy and what to avoid |
| `TOOL_INTERACTIVE::HasToolMenu()`, `SCH_TOOL_BASE::runsWithoutAFrame()` | `include/tool/tool_interactive.h`, `eeschema/tools/sch_tool_base.h` | The first is **unchanged** and already in `common/`; the second is the shape to copy onto `PCB_TOOL_BASE` |
| Host shared library + export list | `eeschema/CMakeLists.txt`, `host/sch_host_abi.exports` / `.map` | Copy the pattern: one `SHARED` target over the kiface objects, exporting only the ABI |
| The ABI, bound and wrapped in Rust | `rust/crates/kicad-sch-sys` | Copy the pattern: `bindgen` in `build.rs`, auto-detected library, and a stub build so the workspace still compiles with no C++ |
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

### 4.4 The internal unit is not the same in both editors

**A schematic IU is 100 nm. A board IU is 1 nm.** `SCH_IU_PER_MM` is 1e4 and
`PCB_IU_PER_MM` is 1e6 (`include/base_units.h:68-70`). Everything that converts
between internal units and millimetres — status-bar readouts, grid sizes,
property panels — is wrong by a factor of 100 if you carry the schematic
constant into pcbnew. We got this wrong once already, in the opposite
direction, and it is invisible until someone reads a coordinate.

The consequence for precision is that pcbnew is *harder*, not easier: a 500 mm
board is 5e8 IU against an `f32`'s 24-bit mantissa of about 1.7e7. Narrowing a
world coordinate to `f32` anywhere before subtracting a camera origin produces
visible jitter that looks like a renderer bug and is not. The stream stores
`f64` for exactly this reason: subtract the origin in `f64`, then narrow.

For reference, an A0 sheet is 1.19e7 schematic IU — already close enough to the
`f32` limit for the low bits to matter, which is why the rule holds in both
editors even though the margin differs.

### 4.4.1 `KIGFX::VIEW`'s "scale" is not pixels per internal unit

The same trap one level up, and the one we actually fell into. A host that hands
its consumer's camera to `VIEW::SetScale` is passing the wrong quantity:

```
GAL::computeWorldScale():  worldScale = screenDPI * worldUnitLength * zoomFactor
```

`VIEW::SetScale` sets the **zoom factor**. Pixels per internal unit is
`worldScale`. The two differ by `screenDPI * worldUnitLength`, and both halves of
that vary: `worldUnitLength` is per editor (`SCH_WORLD_UNIT` is `1e-7/0.0254` inch
per IU; pcbnew has its own) and `screenDPI` is a runtime value. Measured here the
factor is about 2,800, and there is a user zoom-correction factor folded in on top.
So do not hard-code it: `SCH_HOST::PixelsPerIUAtUnitZoom()` recovers it from the
GAL by dividing `GetWorldScale()` by the current zoom, which picks up everything
`computeWorldScale` puts in without knowing what that is.

What makes this worth a section of its own is **how long it can hide**.
`VIEW::SetScale` clamps to the editor's zoom limits (`ZOOM_MIN_LIMIT_EESCHEMA` is
0.01), so a value that is 2,800× off does not blow up — it silently pins the camera
at the most zoomed-out setting the editor allows. `VIEW::Redraw` then culls to a rectangle tens of metres wide,
which means *nothing is ever culled*, which means every frame looks right. The
recorded streams were correct, all 466 of them, and the whole corpus run reported
`scale 0.01` for every file as though it were a constant. It only became visible
when a consumer started driving the camera per frame and expected the cull to
follow — the first thing that actually depended on the number.

Two habits this argues for, both cheap:

* **Give units to the field name or the doc comment, once, in the header the two
  sides share**, and then make the conversion a named function rather than an
  inline multiply.
* **Assert on a derived quantity, not on the number.** The test that would have
  caught this is "after zoom-to-fit, the page spans the viewport" —
  `page_extent * scale ≈ viewport_px`. It needs no fixture and no reference
  image, and it fails loudly for a scale in the wrong space. It is now in
  `rust/crates/kicad-sch-sys/tests/live_session.rs`.

Also: the clamp does not go away once the units are right, and a consumer with a
wider zoom range than the editor's must adopt what it was granted rather than
ignore it. A canvas showing a wider view than the host believes in is missing the
geometry outside the host's viewport.

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

### 4.8 The input blocker is not the one it looks like

The obvious candidate is `TOOLS_HOLDER::GetToolCanvas()`: pure virtual,
returns a `wxWindow*`, sitting right on the path a non-wx host needs. It turns
out to be the lesser problem.

Why it is *not* the blocker:

* `TOOL_MANAGER` has **zero** wx references in its header, and `TOOL_EVENT` is a
  plain value type. Only `TOOL_DISPATCHER` derives from `wxEvtHandler`, and it is
  a thin translator whose drag-threshold and auto-repeat helpers are `static`
  pure functions, reusable as they stand.
* The dispatcher's `GetToolCanvas()` uses are **null-guarded**
  (`common/tool/tool_dispatcher.cpp:590-598`), and several production
  implementations already return `nullptr`.
* eeschema has no `GetToolCanvas()` call sites of its own.

**The real obstacle is unchecked downcasting of the tool holder.** eeschema
contained sixteen `static_cast<SOME_FRAME*>( m_toolMgr->GetToolHolder() )` — no
`dynamic_cast`, no null check — in `sch_commit.cpp`, `tools/sch_editor_control.cpp`,
`tools/sch_selection_tool.cpp` and `tools/symbol_editor_control.cpp`.
`sch_commit.cpp` is the one that matters: every edit goes through it.

So installing *any* `TOOLS_HOLDER` that is not the expected frame — which is
exactly what a non-wx host is — was undefined behaviour at each of those sites,
and it would not announce itself.

Worth noting for contrast: the same file set uses `dynamic_cast<SCH_EDIT_FRAME*>`
seventy-two times. The unchecked ones read as an oversight rather than a
deliberate invariant, which is what made them cheap to fix.

**Do this before any input work, and do not stop at the cast list.** This is the
part we got wrong twice while writing it down. Both earlier drafts of this section
enumerated the casts by grepping for `static_cast<.*GetToolHolder`, published a
count (eight, then fourteen), and treated the list as the specification. It was
neither complete — the true number is sixteen — nor sufficient:

```cpp
// include/tool/tool_base.h:182 — the mechanism, in no grep for GetToolHolder
template <typename T> T* getEditFrame() const
{
    wxASSERT( dynamic_cast<T*>( getToolHolderInternal() ) );   // compiled out under QA_TEST
    return static_cast<T*>( getToolHolderInternal() );
}
```

That is how every tool's `m_frame` is set, tree-wide, in eeschema and pcbnew
alike. Convert only the enumerated sites and every tool still holds a pointer that
is not a frame, from its first line of `Init()` onward, and the commit looks
finished.

What actually works is checking at the one place per tool where the frame is
learned, and returning `false`:

```cpp
m_frame = dynamic_cast<T*>( m_toolMgr->GetToolHolder() );
if( !m_frame )
    return false;
```

`TOOL_MANAGER::InitTools()` already unregisters and deletes a tool whose `Init()`
returns false, so this is the framework's own answer rather than a new mechanism,
and it is where the tool roster's real requirement becomes visible instead of
latent. In eeschema that was five entry points (`SCH_TOOL_BASE<T>::Init()`,
`SCH_SELECTION_TOOL::Init()`, `SYMBOL_EDITOR_CONTROL::Init()`,
`SCH_DESIGN_BLOCK_CONTROL::Init()`, `SIMULATOR_CONTROL::Reset()`) — plus thirteen
derived `Init()`s that called `SCH_TOOL_BASE::Init()` and discarded its result, so
the base's return value meant nothing until they were changed to propagate it.
Expect pcbnew's `PCB_TOOL_BASE` to want the same shape.

**And do not stop at your editor's own directory.** This is the third time the
same list came up short, and the shape of the mistake is worth more than the
count. Eeschema's sixteen sites were found by grepping `eeschema/`, the write-up
recorded that `common/`'s tools were "unchecked" and out of scope — and the very
next step, registering the roster the frame registers, segfaulted in the host's
constructor on `common/`'s tools before a single `TOOL_EVENT` existed. Seven more
entry points needed the same treatment:

| Site | Why it was worse than eeschema's | |
|---|---|---|
| `COMMON_CONTROL`, `COMMON_TOOLS` | no `Init()` at all, so nothing could decline; `Reset()` learned the frame and dereferenced it | added one |
| `ZOOM_TOOL::Init`, `PICKER_TOOL::Init` | `getEditFrame<EDA_DRAW_FRAME>()->AddStandardSubMenus()` — a virtual call on line two | checked |
| `GROUP_TOOL::Init` | stored a wild frame, then `wxCHECK`ed for something else | checked first |
| `PROPERTIES_TOOL::UpdateProperties` | `if( editFrame )` on a `static_cast`, so the guard could never fire | `dynamic_cast` |
| `EMBED_TOOL::Init` | `getModel<EDA_ITEM>()` with no null check — not a frame problem, a model one | null-checked |

pcbnew registers most of the same `common/` tools, so that work is already done for
it. The lesson is not about `common/`: **the list of what you did not check is the
same size as the list of what you did.**

**Then find out what your `m_frame` is going to be, early.** The consequence of
the above is that a non-frame holder gets *no tools at all*, which is defined and
testable and still not an editor. Survey §7 measured roughly 470 of ~600
`m_frame->` sites in `eeschema/tools/` as plain model or settings access with no wx
in them; deciding whether to hoist those onto an interface or to give the host a
real frame is the actual size of "wire up input", and it is worth costing on day
one rather than discovering after the dispatcher works.

Audit pcbnew for its own equivalents before starting. There are at least
`tools/drawing_tool.cpp:232,269` (C-style casts to `PCB_EDIT_FRAME`),
`tools/pcb_selection_tool.cpp:145` and `dialogs/dialog_position_relative.cpp:279`,
and `pcb_edit_frame.cpp` also appears in the `GetToolCanvas()` list, so the
distribution differs.

The rest is bounded, and it is **already written and reusable**: `HOST_VIEW_CONTROLS`
(`include/view/host_view_controls.h`) and `HOST_TOOL_DISPATCHER`
(`include/tool/host_tool_dispatcher.h`) live in `common/` rather than in eeschema
for exactly this reason. They know nothing about schematics. A pcbnew host needs
neither rewritten: make `PCB_HOST` a `TOOLS_HOLDER`, give it one of each, and the
ten `TOOL_EVENT` shapes of survey §6.3 arrive.

`qa/tests/common/test_host_input.cpp` is the pattern to copy for testing it — a
`RECORDING_GAL`, a `VIEW`, a four-line `TOOLS_HOLDER` double and a tool that
records every event offered to it, so the whole contract is asserted with no GUI
and no display. `qa/tests/eeschema/test_non_frame_tools_holder.cpp` is the pattern
for the cast audit.

**One design decision to inherit rather than re-make.** Survey §6 states the
constraint correctly — hotkeys are `WXK_*` integers, so the UI's keys must map onto
the same numbers — and then recommends transcribing `wx/defs.h` into a Rust table.
Do not. A transcribed table is one wrong entry per silently broken shortcut, and
nothing in the build will ever tell you. The key crosses the ABI as a **name**
(`"escape"`, `"f11"`, `"w"`) and `HOST_TOOL_DISPATCHER::KeyCodeFromName` resolves
it in C++, where a compiler reads the numbers out of the real header. That function
is shared, so pcbnew gets it for free.

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
* **`bindgen` needs libclang, and under a wrapped toolchain it needs telling
  where the platform headers are.** libclang does not read the compiler wrapper's
  flags. `devenv.nix` uses nixpkgs' `rustPlatform.bindgenHook`, which exports
  `BINDGEN_EXTRA_CLANG_ARGS`; without something equivalent bindgen fails on
  `#include <stdint.h>`.

### 4.10 The host is main-thread-only, and says so by asserting

Not "one session per thread" — one thread, the one that initialised the runtime.
`SCH_CONNECTIVITY::ENGINE::Clear` (`eeschema/connectivity/conn_engine.cpp:117`)
and `SCH_CONNECTIVITY::INPUT_STORE::Invalidate`
(`eeschema/connectivity/conn_inputs.cpp:421`) both `wxASSERT( wxThread::IsMain() )`,
and `SCHEMATIC`'s constructor reaches both, so an ordinary load trips them off the
main thread. wx's idea of "main" is whichever thread called `wxInitialize`.

Two consequences worth inheriting rather than rediscovering:

* Make the wrapper enforce it. `kicad-sch-sys` records the initialising thread
  and returns an error for a session asked for from another, which is a value
  instead of an assertion on someone else's stack.
* **A libtest harness will violate it**, because it runs each `#[test]` on a
  worker thread. `rust/crates/kicad-sch-sys/tests/live_session.rs` uses
  `harness = false` and a plain `main()` for that reason.

Whether pcbnew's connectivity has the same constraint is unchecked, but
`CONNECTIVITY_DATA` is threaded, so assume it does until measured.

---

### 4.11 The tools need an editing context, and pcbnew's is not the same object

§4.8 above is about the tool holder being *safe*. Making a tool actually *run* is a
separate problem, and eeschema solved it by growing `SCHEMATIC_HOLDER` — upstream's
own four-virtual bridge between the schematic and the frame — into what a schematic
tool asks its editor for: the document, the settings that decide behaviour, and a
couple of notifications a canvas owner can act on. `SCH_BASE_FRAME` and `SCH_HOST`
both implement it. `04-host-seam.md` §9 is the full account.

Four things transfer directly.

**Two rules about what belongs on the interface.** Anything inherently a window —
dialogs, info bars, focus, docked panes — stays off it, and a tool that wants one
downcasts to the frame and does nothing when the answer is null, with a comment
saying what is lost. And anything the tool framework already answers stays off it:
`TOOL_BASE::getView()`/`getViewControls()` come from `TOOL_MANAGER`, and
`TOOL_MANAGER::GetToolHolder()` answers `ToolStackIsEmpty()`, `IsCurrentTool()`,
`PushTool()` and `GetDragAction()`. In eeschema that second rule removed most of what
looked like frame access.

**`~600 m_frame-> call sites` is the wrong unit.** The survey's count for
`eeschema/tools/` made the work look enormous; in practice most sites are
`GetScreen()`, `AddToScreen()` and `UpdateItem()` repeated, so the interface is about
twenty methods and each tool's conversion is mechanical. Count *distinct methods per
tool*, not sites: `SCH_MOVE_TOOL` had 57 sites and 13 distinct methods, of which 4
were new.

**Opt in, never out.** `runsWithoutAFrame()` defaults to false, so an unconverted
tool declines rather than initialising with a null frame and crashing on the first
click. A converted tool must tolerate a null `m_frame` *and* a null `m_menu` —
`TOOL_INTERACTIVE` only builds a `TOOL_MENU` when `Pgm().IsGUI()`.

**Expect the defaults nobody has checked.** This is the part most likely to catch
pcbnew too. A host is the first thing to read a mixin's constructor defaults as they
were left, because every frame overwrites them from settings before anything reads
them. In eeschema that was `KIGFX::GAL`'s grid size (zero, and `GRID_HELPER` divides
by it), `TOOLS_HOLDER`'s drag action (`SELECT`, so every drag rubber-banded) and
`EDA_DRAW_FRAME`'s shadowed undo limit. **`SCH_HOST` calls
`TOOLS_HOLDER::CommonSettingsChanged()` in its constructor; a board host must too**,
and must give its GAL a grid. The grid one is not eeschema-specific at all.

One eeschema-specific finding that has a pcbnew analogue worth looking for: the wire
tool turned out to be a *prerequisite* of the move tool, because moving a wire off a
junction has to add one where it left. Look for the equivalent coupling between
`PCB_POINT_EDITOR`, the router and `PCB_MOVE_TOOL` before assuming they can be
converted in any order.

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
5. **`PCB_HOST`**, modelled on `SCH_HOST`, and a `kicad-pcb-sys` modelled on
   `kicad-sch-sys` — the shared-library target, the export list, the runtime call
   and the bindgen build script are all patterns to copy rather than decisions to
   retake (§2). Mind §4.10 and §4.4.1 while doing it, and add the
   "zoom-to-fit spans the viewport" assertion from §4.4.1 on day one; it costs
   nothing and catches the scale bug we shipped.
6. **Live re-render early, not last.** Hold the session open and ask it for the
   frame the canvas is about to paint, rather than recording once and panning over
   a copy. It is about a day's work (`06-what-is-missing.md`, Stage 2) and it is
   what turns latent unit and camera disagreements into test failures. Ours stayed
   hidden through an entire 466-file corpus run precisely because nothing yet
   depended on the camera.
7. **The shell**: reuse the structure, add the layer widget, the appearance
   panel and pcbnew's toolbars.
8. **The editing context, and then one tool at a time** (§4.11). `BOARD_HOLDER`-shaped
   interface, undo through `UNDO_REDO_HOLDER`, then selection, then move. Do selection
   first for the same reason eeschema did: it proves the whole round trip through a
   real KiCad tool and it is the tool with no mutation to get wrong. Expect the
   defaults check in §4.11 to catch something on day one.
8. **Input** — only after the tool-holder downcasts are checked, in `pcbnew/` *and*
   in whatever `common/` tools your frame registers (§4.8). `GetToolCanvas()` is
   not the gate it looks like. The dispatcher and the view controls are shared code
   you do not have to write; wiring them to `PCB_HOST` is an afternoon, and it will
   deliver events to a `TOOL_MANAGER` with no tools in it until step 9.
9. **Decide what `m_frame` is, and convert one tool.** This is the step that turns
   an input path into an editor, and it is the largest one. Cost it before step 8
   rather than after: §4.8's two routes, and the measurement that a single tool's
   frame surface is around fourteen methods rather than the aggregate's hundreds of
   call sites.

Steps 1–4 are largely independent of the C++ build and can proceed in parallel
with it.

---

## 6. Verification checklist

Before claiming the renderer is right:

- [ ] Every GAL method pcbnew calls is either recorded or a base-class getter
      (§4.1 has the one-liner)
- [ ] Redrawing an unchanged board grows retained group data by zero bytes
- [ ] **Panning the host's camera changes the frame body and nothing else** —
      the group table and every group body byte-identical, the `DRAW_GROUP` list
      different. If the frame body does *not* change, the camera is not reaching
      the cull and you have the §4.4.1 bug
- [ ] **After zoom-to-fit, the page or board extent spans the viewport**
      (`extent * scale ≈ viewport_px`). The cheapest possible check that the
      scale is in the unit the ABI claims (§4.4.1)
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

* **Most of the tools.** Four of eeschema's twenty-one classes run on a holder that
  is not a frame — selection, move, wire, and a small undo/redo/save control — and
  seventeen still decline one. The mechanism is settled (§4.11), so what is left is
  the same conversion applied again, costed per tool in
  `06-what-is-missing.md` Stage 4b. For pcbnew the equivalent work has not been
  started at all, and its tool roster is larger.
* **Dialogs.** All 124 of eeschema's are still wxWidgets; pcbnew has 224.
  `00-architecture-survey.md` §7.4 discusses keeping them, bridging them
  asynchronously through the existing tool coroutines, or rewriting them, and
  recommends the middle option as the migration path.
* **Removing wxBase.** `wxString` and friends remain throughout the C++ core.
  Only the GUI layer left the rendering path.
* **Printing and plotting.** Those go through `PLOTTER`, not the GAL, and were
  not touched.
