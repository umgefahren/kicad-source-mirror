# What is missing, and what it takes to get an editor

This document exists because the previous ones describe what was built, and a
reader can finish them with the wrong impression of what that adds up to.

**What exists today is a schematic viewer that opens real `.kicad_sch` files and
redraws them live from the document, plus a seam an editor can be built on. It is
not a schematic editor, and wxWidgets has not been removed from anything.** The
wx schematic editor is untouched and is still the only way to edit a schematic.

> **Stages 1 and 2 below are done.**
> `kicad-eeschema-gpui --schematic FILE.kicad_sch` loads the file through
> eeschema's own reader and keeps the session open for the window's lifetime,
> asking it for a frame whenever the view moves. The two arrows that used to be
> missing or drawn-once are both live. What is left is the one that matters most:
> **nothing the user does reaches the document.**

## Exactly where it stops

One fact, checkable in a minute:

**Input is discarded.** The shell builds a complete event vocabulary —
`PointerDown`, `PointerMove`, `PointerUp`, `DragBegin`, `DragUpdate`, `DragEnd`,
`Scroll`, `KeyDown`, `KeyUp`, `ToolCancelled`, with buttons and modifiers — and
the binary hands it `NullSink`, whose entire implementation is
`fn handle(&mut self, _event: ShellEvent) {}`. Nothing a user does reaches the
document.

So the pipeline that works now is:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► stream in memory
                   ▲                                    │
                   │                              C ABI │
                   └─── viewport ───── gpui window ◄────┘
                        every view change
```

and the pipeline an editor needs adds one arrow, alongside that one:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► stream in memory
                   ▲                                    │
            TOOL_MANAGER                          C ABI │
                   ▲                                    │
                   └───── input ────── gpui window ◄────┘
                          goes to NullSink
```

That arrow is not speculative work either — both its ends exist and are tested.
What stands between here and it is Stages 3 and 4.

## What is already done and does not need redoing

Worth being clear about, because it changes the size of what remains:

| | |
|---|---|
| `RECORDING_GAL` + `DRAW_STREAM` | Complete. 30 tests in `qa_common` |
| The draw-stream ABI | Frozen, layout-asserted on both sides, sync-tested |
| `SCH_HOST` | Loads, renders, enumerates sheets, zooms; 16 tests |
| The C ABI | 26 entry points, three of them the runtime; implemented, and bound from Rust |
| `kicad-gal` | Validating decoder, 58 tests |
| `kicad-sch-render` | Stream → gpui primitives, 89 tests |
| `kicad-sch-ui` | Shell, 79 tests, the interaction ones against real hit testing |
| Action registry | 440 actions enumerable headless with icons and hotkeys |
| `kicad-sch-sys` | The ABI linked from Rust, 11 checks against the live host |
| Live re-render | The canvas asks the session for the frame it is about to paint |

The rendering half is genuinely finished, on all 466 schematics in the tree.

---

## Stage 1 — Link the C ABI from Rust (done)

**Took about a day, as estimated. Everything below it is now unblocked.**

`kicad-eeschema-gpui --schematic FILE.kicad_sch` opens a schematic through
eeschema's reader and shows the frame `SCH_PAINTER` records for it. What that
took:

| | |
|---|---|
| `rust/crates/kicad-sch-sys` | `bindgen` over the header in `build.rs`, plus a safe wrapper |
| `eeschema/host/sch_host_runtime.cpp` | The process singletons, behind two new ABI calls |
| `libkicad_sch_host` | A shared library target, `KICAD_BUILD_RUST_SCH_UI`-gated |
| `host/sch_host_abi.exports` / `.map` | Its export list: 26 symbols, all `ksch_*` |

**What to link against** turned out to be option 1 of the three below, and the
export list is what makes it honest: the library is a full eeschema link, 44 MB,
but the only thing a consumer can reach is the C ABI. The vendored C libraries
that come with the kiface objects would otherwise have exported some 1,900
symbols of their own, because the tree's `-fvisibility=hidden` is applied to C++
only.

1. Build the host as its own shared library with a narrow export list. Cleanest,
   and the one that makes the Rust binary's dependency footprint honest.
2. Link the kiface objects directly. Fastest to get working, drags in everything.
3. Invert the ownership so C++ `main()` starts and Rust is a library it calls.
   This is probably where it ends up eventually — see Stage 5 — but it is a
   bigger change than Stage 1 should be.

### The part that was not in the plan

A C ABI is not callable from a process that has no `PGM_BASE` and no
`KIFACE_BASE`, and there was no way to get either without writing C++ in
`main()` — which a Rust host does not have. `ksch_runtime_init` and
`ksch_runtime_shutdown` (ABI version 2) do that work: wx in console mode, the
settings manager, eeschema's settings registered, the kiface's settings
installed. It lives in the shared library rather than in the host objects, so
`qa_eeschema` and `kicad-sch-dump` keep their own.

Two things the code told us that the plan did not:

* **The host is main-thread-only, not one-session-per-thread.**
  `SCH_CONNECTIVITY::ENGINE::Clear` and `INPUT_STORE::Invalidate` both
  `wxASSERT( wxThread::IsMain() )`, and an ordinary load reaches both. wx takes
  whichever thread called `ksch_runtime_init` to be its main thread, so the rule
  is: one thread, the one that initialised it. The header now says so, the Rust
  wrapper reports a second thread as an error rather than letting it trip the
  assertion, and the integration test runs without libtest's harness because
  libtest would put each check on a worker thread.
* **A recorded stream's ordering is not canonical across standard libraries.**
  A live render here is byte-identical to what `kicad-sch-dump` writes on the
  same machine, and *not* byte-identical to the fixture recorded on Linux: same
  group table, same 2,587 group commands, same coordinates, four of 222 bodies
  under different group ids. `KIGFX::VIEW` visits items in an order that an
  unstable sort over equal keys leaves to the implementation. Nothing renders
  differently — but `generate.sh` on a different platform produces a diff, and a
  test must compare the picture rather than the bytes. See
  `04-host-seam.md` §8.

### What Stage 1 deliberately did not do

* Input still goes to `NullSink`. Stage 4.
* The session was dropped after the first frame, so nothing re-rendered from the
  document. That was Stage 2, below, and is done.
* The hierarchy panel still describes the draw stream rather than the sheet tree,
  and says so, even though `ksch_session_sheet_info` could fill it in today.

## Stage 2 — Live re-render (done)

**Took about a day, as estimated — but not on the part that was predicted. See
"the part that was not in the plan" below.**

The session now stays open for the window's lifetime and the canvas asks it for
the frame it is about to paint. What that took:

| | |
|---|---|
| `kicad-sch-ui/src/document.rs` | `LiveDocument`, the seam: "here is my camera, give me the frame" |
| `kicad-eeschema-gpui/src/main.rs` | The one real implementation, over `kicad_sch_sys::Session` |
| `CanvasState` | Owns the document, decides when to ask, surfaces a failure |
| `Stream::copy_from_view` | The per-frame copy, without the per-frame allocation |
| `SCH_HOST::PixelsPerIUAtUnitZoom` | The unit bug in the next section |

### Where the policy lives, and why it is "when the view moves" rather than "every frame"

The stage description said "call it per frame". That would have been wrong, and
the code does not: `CanvasState::refresh_document` asks only when the answer could
have changed — a pan, a zoom, a resize, or `mark_document_dirty()` for anything
the host did that the canvas cannot see. The window free-runs at the display rate
so that the frame-time readout measures the display rather than how often
something happened; re-recording on each of those redraws would have been pure
waste. `CanvasState::document_renders()` is the counter that makes it checkable,
and the shell test asserts that eight redraws of an untouched view add nothing to
it.

The request goes out in prepaint, *after* the deferred fit and any zoom steps have
settled, because the session culls the frame to the camera it is told about.
Asking before the fit had run would paint one frame culled for the wrong view,
which shows as geometry missing along the edges.

### What it costs, measured

A pan on the heaviest sheets in the tree, release build, cache warm:

| Sheet | Groups | Record (C++) | Copy + revalidate | Prepare (cull, cache) | Total |
|---|---|---|---|---|---|
| `demos/ecc83/ecc83-pp_v2` | 336 | 0.48 ms | 0.004 ms | 0.02 ms | **0.51 ms** |
| `demos/video/video` | 1,328 | 0.61 ms | 0.015 ms | 0.08 ms | **0.71 ms** |
| `demos/tiny_tapeout/tinytapeout-demo` | 5,321 | 2.50 ms | 0.09 ms | 0.72 ms | **3.30 ms** |
| `demos/jetson-agx-thor-baseboard/dcdc` | 5,849 | 3.73 ms | 0.15 ms | 0.68 ms | **4.56 ms** |

The last row is the densest sheet in the tree, and it leaves 3.7 ms of the 8.3 ms
budget. The C++ recording pass dominates, which is the right answer — it is the
part doing real work. Over 60 consecutive panned re-renders the tessellation cache
misses exactly once per group and never again, on every one of those sheets: the
geometry is tessellated when first seen and thereafter only replayed.

The copy is small enough to be uninteresting, which took one change to be true.
`StreamView::to_owned_stream` allocates ten vectors, and doing that per frame put
megabytes of allocator churn inside the budget on a large sheet.
`Stream::copy_from_view` overwrites the buffers instead, so `set_stream_view`
keeps every capacity; `copying_a_view_reuses_the_buffers_it_already_has` asserts
it by section address.

### The part that was not in the plan

**`SCH_HOST` was reporting and accepting the wrong quantity as a scale, and had
been since it was written.** `ksch_viewport::scale` is documented as pixels per
internal unit — which is what the recorded coordinates and a consumer's camera are
in — but `SetViewport` passed it straight to `KIGFX::VIEW::SetScale`, and VIEW's
scale is the *GAL zoom factor*. The two differ by a factor the GAL computes:

```
GAL::computeWorldScale():  worldScale = screenDPI * worldUnitLength * zoomFactor
eeschema's worldUnitLength = 1e-7 / 0.0254 inch per IU     (SCH_WORLD_UNIT)
```

which on this machine is 3.58e-4 pixels per internal unit at a zoom factor of one,
so a correct scale is about **2,800× smaller** than the zoom factor that produces
it. The consequences were invisible right up to the moment something depended on
the camera, which is exactly what this stage does:

* Every requested scale, read as a zoom factor, was ~2,800× too small — a
  zoom-to-fit of an A4 page asks for 4.90e-4, and the zoom factor that means is
  1.37. `VIEW::SetScale` clamped it *up* to eeschema's minimum zoom
  (`ZOOM_MIN_LIMIT_EESCHEMA`, 0.01), which is the most zoomed-out view the editor
  allows, and the session's cull rectangle came out **54 metres wide**. Nothing
  was ever culled. Panning three viewports away from the sheet changed the
  recorded frame body not at all.
* `SCH_HOST::ZoomToFit` computed pixels per internal unit correctly and then fed
  it to the same method, so the C++ "zoom to fit" did not fit either.
* `kicad-sch-dump` printed the clamped zoom factor as `scale 0.01` for every file
  in the corpus, which is why it looked like a constant rather than a measurement.
  That is the single clue that was visible all along, in checked-in output, and it
  was not read as one.

`PixelsPerIUAtUnitZoom()` now converts, and recovers the factor *from the GAL*
rather than recomputing the formula, so that the user's zoom-correction factor —
and anything else that ends up in `computeWorldScale` later — is included without
this code knowing about it. `GetViewScale()` returns `GAL::GetWorldScale()`, which
is the quantity the header always claimed.

Two things worth knowing about the fix:

* **No recorded output changed.** All four checked-in fixtures come back with
  identical group counts, command counts, coordinate counts and byte sizes. A
  zoom-to-fit frames the page, and nothing on those sheets sits outside it, so a
  cull rectangle that is correct and one that is 54 metres wide keep the same
  items. The only line that moved is the `scale` the dump tool prints. The
  live-host suite's order-independent fixture comparison passes unchanged, as do
  `qa_eeschema` (1,699 cases) and `qa_common` (1,477).
* **The clamp is now visible, and is adopted rather than ignored.** eeschema's
  zoom limits bind the wx editor too, so a canvas that honours them is behaving
  correctly rather than being restricted. `SchematicSession::render` reads the
  viewport back and writes the granted scale into the renderer's camera, because
  a canvas showing a wider view than the session believes in would be missing the
  geometry outside the session's idea of the viewport. A zoom that runs into the
  limit simply stops, as it does in the wx editor.

### What Stage 2 deliberately did not do

* **Nothing wakes the gpui loop from C++.** The stage description asked who owns
  the frame clock; the answer is still gpui, and that is sufficient while nothing
  but the shell can change the document. The moment an edit can arrive from
  elsewhere, `mark_document_dirty()` is the entry point it needs — it exists, is
  tested, and is called by nothing yet.
* **Sheet switching is not wired.** `ksch_session_set_sheet` exists and
  invalidates every retained group, so the renderer would have to drop its cache;
  the hierarchy panel that would drive it still describes the draw stream.
* **The docked panels still describe the opening frame**, not the current one.
  With a live document the frame body is re-recorded per view change and
  `KIGFX::VIEW` culls it, so the frame-command count moves with the camera and is
  not a property of the document. The panels now say "the opening frame" where
  they used to say "frame commands"; the live numbers are in the status bar, where
  they are updated every paint. Refreshing the tree per pan was the alternative
  and is worse: `TreeState::set_items` resets selection and expansion, so the
  hierarchy would collapse while the user panned.

## Stage 3 — Make a non-frame `TOOLS_HOLDER` safe

**Effort: half a day. Worth doing regardless of this project.**

eeschema has eight `static_cast<SCH_EDIT_FRAME*>( m_toolMgr->GetToolHolder() )`
with no `dynamic_cast` and no null check:

```
eeschema/sch_commit.cpp:195
eeschema/tools/sch_selection_tool.cpp:191
eeschema/tools/sch_editor_control.cpp:1065, 1220, 1576, 1602, 1654, 1771
```

`sch_commit.cpp` is the one that matters: every edit goes through it. Installing
any `TOOLS_HOLDER` that is not a `SCH_EDIT_FRAME` — which is exactly what a
non-wx host is — is undefined behaviour at each site, and will not announce
itself. It is latent today only because nothing installs a non-frame holder.

For contrast, the same code uses `dynamic_cast<SCH_EDIT_FRAME*>` seventy-two
times, so these read as oversights rather than a deliberate invariant.

**Done when:** the eight are checked casts with a defined behaviour when the
holder is not a frame, and the QA suite still passes. This is a small,
self-contained, independently reviewable change that improves the tree whether
or not the Rust work continues.

## Stage 4 — A non-wx tool dispatcher

**Effort: the real work. Everything before this is plumbing.**

`TOOL_MANAGER` has no wx references in its header and `TOOL_EVENT` is a plain
value type, so the manager itself needs nothing. Only `TOOL_DISPATCHER` is bound
to wx (`class TOOL_DISPATCHER : public wxEvtHandler`), and it is a translator
from `wxEvent` to `TOOL_EVENT`. Write a sibling that translates from the shell's
`ShellEvent` instead and feeds `TOOL_MANAGER::ProcessEvent`, `PostEvent` and
`DispatchHotKey`.

Reusable as they stand — they are `static` and wx-free in substance:

* `TOOL_DISPATCHER::IsPastDragThreshold`
* `TOOL_DISPATCHER::ShouldDropAutoRepeat`

The constraint that will bite: **hotkeys are `WXK_*` integers**. gpui key codes
must map onto exactly those numbers or every keyboard shortcut silently does
nothing. Survey §6 has the event constructions and the `BUT_*` / `MD_*` bit
values.

Then, in rough order of how much each unlocks:

1. **Selection.** `SCH_SELECTION_TOOL` — makes the canvas feel alive and proves
   the round trip.
2. **Pan/zoom through `VIEW_CONTROLS`** rather than the Rust camera, so C++ and
   Rust agree on where the view is. `VIEW_CONTROLS` is already abstract;
   `WX_VIEW_CONTROLS` is just the wx implementation, so this is an
   implementation, not a refactor.
3. **Move** — `SCH_MOVE_TOOL`, the first tool that mutates.
4. **Wire drawing** — `SCH_LINE_WIRE_BUS_TOOL`.
5. **Undo/redo.** Lives on `EDA_BASE_FRAME`, so it needs hoisting to the host.
   This is the genuine debt the survey identified.

**Done when:** a user can select, move, draw a wire, undo it, and save a file
that KiCad reopens unchanged.

## Stage 5 — Dialogs, and who owns `main()`

**Effort: a project, not a stage.**

124 dialog sources in `eeschema/dialogs/`, all wxWidgets, many opened
synchronously from inside a tool with `ShowModal()`. A gpui host is async.
`00-architecture-survey.md` §7.4 lays out the three options; the recommendation
is to keep them for bring-up and bridge them asynchronously through the tool
coroutines, which already support suspension, rather than rewrite 124 dialogs.

Keeping them working means a `wxApp` still exists, which is also the point at
which "wxWidgets has been removed" stops being true in any sense. Removing it
genuinely means Stage 5 completed, and that is a long way past where this is.

---

## Honest sizing

Stages 1–3 are mechanical and bounded: roughly two to three days. Stages 1 and 2
are done and cost about a day each — which turns a viewer of a recorded frame into
a window onto a document that redraws itself. Stage 3 remains.

Both finished stages spent most of their time on something that was not in the
plan, and in both cases it was the same kind of thing: a quantity that two layers
disagreed about and nothing had yet forced them to agree on. Stage 1 found that a
draw stream's group ordering is not canonical across standard libraries; Stage 2
found that the host's viewport scale was the GAL zoom factor where the ABI promised
pixels per internal unit. Neither was a hard problem once seen, and neither was
visible until a consumer depended on it. That is worth expecting for Stages 3 and 4
as well, and is an argument for wiring something end to end early rather than
building each layer to its own satisfaction.

Stage 4 is where a schematic editor actually lives. Selection alone is a
meaningful milestone; a tool set someone would choose over the wx editor is
substantially more.

Stage 5 is a separate project.

**The honest summary of this branch is that it finishes the rendering third of
the problem and leaves the editing two thirds.** That is a real result — the
rendering third was the part with the most unknowns, it is settled and tested on
every schematic in the tree, and it now runs end to end in one process and redraws
from the live document rather than from a copy — but it is a third. Nothing a user
does to the window reaches the schematic.
