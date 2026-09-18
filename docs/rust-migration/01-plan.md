# Replacing eeschema's UI layer with gpui-kit + wgpu

## Goal

Remove wxWidgets and OpenGL from the schematic editor's **presentation layer**, and
replace them with a Rust UI built on [`gpui-kit`](https://crates.io/crates/gpui-kit)
rendering through [`wgpu`](https://crates.io/crates/wgpu), while keeping KiCad's
C++ document model, file I/O, connectivity engine, ERC, netlist exporters and
interactive tools exactly where they are.

This is deliberately the *first* step of a longer migration. Nothing here
reimplements schematic semantics in Rust. The value of the step is that it
establishes a durable seam: once pixels and input belong to Rust, further
subsystems can cross the boundary one at a time without another UI rewrite.

## Where the cut goes

```
      ┌──────────────────────────────────────────────────────────┐
      │ Rust                                                     │
      │                                                          │
      │  kicad-eeschema-gpui ....... binary, owns main()         │
      │  kicad-sch-ui .............. gpui-kit shell: window,     │
      │                              menu bar, toolbars, docks,  │
      │                              status bar, canvas element  │
      │  kicad-render-wgpu ......... wgpu renderer for the       │
      │                              recorded draw stream        │
      │  kicad-gal ................. decoder + safe bindings     │
      │  kicad-sch-sys ............. raw FFI declarations        │
      └───────────────────────────┬──────────────────────────────┘
                                  │  C ABI, no C++ types crossing
      ┌───────────────────────────┴──────────────────────────────┐
      │ C++ (new, thin — this is the only C++ we add)            │
      │                                                          │
      │  SCH_HOST .................. owns SCHEMATIC, VIEW,       │
      │                              TOOL_MANAGER, settings,     │
      │                              undo stack; no wxFrame      │
      │  RECORDING_GAL ............. KIGFX::GAL subclass that    │
      │                              records instead of rasters  │
      │  HOST_TOOL_DISPATCHER ...... builds TOOL_EVENTs from     │
      │                              host input, no wx events    │
      │  ACTION_REGISTRY ........... enumerates TOOL_ACTIONs so  │
      │                              Rust can build menus        │
      └───────────────────────────┬──────────────────────────────┘
                                  │  ordinary C++ calls
      ┌───────────────────────────┴──────────────────────────────┐
      │ C++ (unchanged — not touched by this work)               │
      │                                                          │
      │  SCH_PAINTER, KIGFX::VIEW, SCH_ITEM and the whole        │
      │  document model, sch_io (.kicad_sch/.kicad_sym),         │
      │  CONNECTION_GRAPH, ERC, netlist export, geometry,        │
      │  SCH_SELECTION_TOOL / SCH_MOVE_TOOL / SCH_EDIT_TOOL ...  │
      └──────────────────────────────────────────────────────────┘
```

### The load-bearing idea: record, don't rasterise

`SCH_PAINTER` already contains every rule about how a schematic looks — pin
graphic styles, field placement, junction dot sizing, fill conventions,
selection highlighting. It expresses all of it in calls to the abstract
`KIGFX::GAL` interface, which today is implemented by `OPENGL_GAL` and
`CAIRO_GAL`.

So we do not port any drawing logic. We add a third backend, `RECORDING_GAL`,
whose implementation of every `GAL` virtual appends a command into a flat
buffer. `KIGFX::VIEW` walks the item tree exactly as it does now, `SCH_PAINTER`
draws exactly as it does now, and what comes out the far end is a
device-independent draw stream that Rust rasterises with wgpu.

Two consequences worth stating plainly:

* Every visual rule stays in one place, in C++, still covered by its existing
  tests. We cannot drift from upstream's appearance by accident.
* The draw stream is a clean, inspectable, serialisable artifact. It makes
  golden-file testing of rendering possible without a GPU, and it is the thing
  the Rust renderer is tested against.

A detail that turned out to matter more than expected: **text never reaches the
renderer as text**. KIFONT resolves it to glyphs before the GAL sees it, and
those glyphs are already geometry — a `STROKE_GLYPH` is a set of polylines and
an `OUTLINE_GLYPH` is a `SHAPE_POLY_SET`. The recorder lowers both to ordinary
polyline and polygon commands. So the Rust side needs no font handling at all,
typography stays identical to every other KiCad canvas, and gpui's inability to
rotate text — which would otherwise have been fatal, since schematics rotate
text constantly — simply does not apply.

The `GAL` methods that *return* values were the anticipated hard part, and
proved smaller than feared: thirteen virtuals return data, all answerable from
GAL-local state with no round trip. Nothing reads pixels back, and text metrics
live on `KIFONT::FONT` rather than on the GAL, so no extents query exists at
all. `00-architecture-survey.md` enumerates them.

## How the pixels get drawn

gpui renders through wgpu, so replacing the OpenGL GAL is a consequence of
adopting gpui rather than a separate project. What took some investigation was
*how* to get schematic geometry into that renderer.

Three mechanisms were considered and two ruled out, by reading gpui's sources
rather than guessing:

* **Injecting our own draw calls into gpui's frame** is not possible. `gpui`
  has no wgpu dependency at all — the dependency runs the other way — and the
  live GPU context is private to the X11 and Wayland client structs. The
  renderer's only entry point is `draw(&Scene)`; there is no custom-pass or
  external-texture hook.
* **Rendering to our own texture and compositing it** is possible but not
  viable. Off macOS the only route is `paint_image`, which takes CPU bytes:
  roughly 25–33 MB of memcpy plus a GPU→CPU readback stall per 1080p frame.
  That does not fit in a 120 Hz budget.
* **A custom gpui `Element` emitting gpui's own primitives** is the answer, and
  it turns out to be a better one than a bespoke pipeline. `PathBuilder` is a
  full lyon frontend: stroked polylines with caps, joins and miter limits, dash
  arrays, elliptical arcs, quadratic and cubic béziers, filled polygons with
  fill rules. Paths are rasterised into a **4× MSAA** intermediate. For
  schematic line work that is better antialiasing than a naive single-sampled
  custom pipeline, for none of the work.

So the renderer writes no shaders. Where its effort goes instead is the CPU
side, which is what actually decides whether 8.3 ms is met on a large sheet:

* **Retained groups.** `KIGFX::VIEW` already caches per-item geometry through
  `BeginGroup`/`EndGroup`, and the draw stream preserves that structure. Each
  group carries a serial, and tessellated paths are cached against it, so a pan
  or a zoom re-tessellates nothing.
* **Batching.** Primitives are bucketed by colour, width and dash and emitted as
  one path per bucket, chunked below lyon's 65 535-vertex index limit. Keeping
  everything inside a single `paint_layer` means gpui merges it into one
  primitive batch and therefore one render pass.
* **Culling before tessellation.** gpui culls primitives against the content
  mask, but the CPU tessellation cost is already paid by then, so the renderer
  culls against the viewport first.

The 120 Hz target is a frame budget of 8.3 ms, and it is measured rather than
asserted: the renderer benchmarks tessellation and primitive emission, and the
application carries a frame-time readout.

One property worth stating, because it is what makes the budget plausible: a
redraw of an unchanged document touches no retained geometry at all. This is
verified — 200 consecutive frames over a static schematic leave the recorded
group data byte-identical, with a frame body of four commands.

## Milestones

| # | Milestone | Depends on |
|---|-----------|------------|
| M1 | Rust workspace; wgpu renderer; gpui-kit shell rendering a recorded draw stream from a golden file | nothing |
| M2 | `RECORDING_GAL` + C ABI + a `sch-dump` tool that emits a draw stream from a real `.kicad_sch` | C++ build |
| M3 | Live linkage: Rust loads the host library, renders live, forwards input into `TOOL_MANAGER` | M1, M2 |
| M4 | Menus and toolbars generated from the action registry; select / move / wire tools driving the C++ tools | M3 |
| M5 | Tests: C++ unit tests for `RECORDING_GAL`; Rust decoder and golden-image tests; headless UI interaction tests | M1–M4 |
| M6 | CMake/Corrosion integration, CI, documentation | M5 |

M1 is deliberately independent of the C++ build, so the renderer and UI can be
developed and tested even while the C++ tree is still being brought up.

## Testing strategy

Three layers, because the seam makes each of them cheap:

1. **C++ side.** `RECORDING_GAL` is exercised by the existing QA harness: render
   known schematics through `VIEW`/`SCH_PAINTER` and assert on the recorded
   command stream. This catches changes in drawing rules with no GPU involved.
2. **Draw-stream golden files.** Streams recorded from repository fixtures are
   checked in. The Rust decoder is tested against them, and they let the
   renderer be developed without linking C++ at all.
3. **Rendered-image golden tests.** The wgpu renderer draws those streams
   headlessly on Mesa's `lavapipe` software Vulkan device and the output is
   compared against reference PNGs with a perceptual tolerance. Plus headless
   interaction tests that drive the gpui-kit shell and assert on resulting
   `TOOL_EVENT`s.

See `tools/rust-gpu-testenv/ENVIRONMENT.md` for how the software GPU and
headless compositor are set up.

## Where this actually got to

The milestones above describe the intended shape. What landed is M1, M2 and the
rendering half of M3: rendering works end to end on all 466 schematics in the
tree, and the Rust binary loads the host library, opens a real `.kicad_sch`
through it — `--schematic FILE.kicad_sch` — and re-records the frame from that
live session whenever the view moves. What has not started is the *input* half of
M3: nothing a user does reaches `TOOL_MANAGER`. **See `06-what-is-missing.md`**
for precisely where it stops and what the remaining stages are.

One prediction in this document is worth revisiting in the light of that. The
plan said to call the host "per frame"; the code asks only when the answer could
have changed — a pan, a zoom, a resize, or an explicit invalidation — because the
window free-runs at the display rate and re-recording on each of those redraws
would be waste. The distinction is the same one the retained-group design rests
on, applied one level up.

## Explicit non-goals for this step

* Reimplementing the `.kicad_sch` / `.kicad_sym` file format in Rust. File I/O
  stays in C++.
* Porting the ~200 wx dialogs. The initial shell covers the canvas, menus,
  toolbars, docks and status bar; dialogs are bridged or deferred, and the
  survey records which tool actions depend on one.
* Touching pcbnew, gerbview, the 3D viewer or the project manager.
* Removing wxBase. wxWidgets' *GUI* layer leaves the schematic editor's
  presentation path; `wxString` and friends remain as utility types throughout
  the C++ core and are a separate, later migration.
* Feeding input into `TOOL_MANAGER`. This is the next milestone of substance
  rather than a structural obstacle: `TOOL_MANAGER` has no wx dependency,
  `TOOL_EVENT` is a plain value type, and only `TOOL_DISPATCHER` is bound to wx.
  What is needed is a dispatcher that builds `TOOL_EVENT`s from gpui input.
  `docs/rust-migration/04-host-seam.md` records what it would take.

## Related documents

* `00-architecture-survey.md` — layer map, GAL interface, wx coupling, fixtures
* `02-gpui-kit-cookbook.md` — how to build against gpui-kit, verified
* `03-build-notes.md` — how to configure and build the C++ tree here
