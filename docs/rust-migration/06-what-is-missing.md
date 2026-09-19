# What is missing, and what it takes to get an editor

This document exists because the previous ones describe what was built, and a
reader can finish them with the wrong impression of what that adds up to.

**What exists today is a schematic viewer that opens real `.kicad_sch` files and
redraws them live from the document, plus a seam an editor can be built on. It is
not a schematic editor, and wxWidgets has not been removed from anything.** The
wx schematic editor is untouched and is still the only way to edit a schematic.

> **Stages 1, 2 and 3 below are done.**
> `kicad-eeschema-gpui --schematic FILE.kicad_sch` loads the file through
> eeschema's own reader and keeps the session open for the window's lifetime,
> asking it for a frame whenever the view moves. The two arrows that used to be
> missing or drawn-once are both live, and pointing a `TOOL_MANAGER` at something
> that is not a `wxFrame` is no longer undefined behaviour. What is left is the one
> that matters most: **nothing the user does reaches the document.**
>
> Stage 3 also changed the shape of Stage 4, and not for the better — see
> [What this does and does not buy Stage 4](#what-this-does-and-does-not-buy-stage-4).

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
| A non-frame `TOOLS_HOLDER` | Defined rather than undefined: 16 checked casts, 5 tool entry points that decline, 4 tests |

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

## Stage 3 — Make a non-frame `TOOLS_HOLDER` safe (done)

**Estimated half a day, took about that. Everything the stage description asked
for is done — and the description was describing a symptom rather than the
mechanism. See "the part that was not in the plan".**

eeschema had unchecked `static_cast`s of `TOOL_MANAGER::GetToolHolder()` with no
`dynamic_cast` and no null check. This document counted eight and
`04-host-seam.md` §6.2 counted fourteen; the real number is **sixteen**, because
neither count included `tools/sch_selection_tool.cpp:191` and `:271`, and both
counted only `SCH_EDIT_FRAME` while `sch_commit.cpp` also casts to
`SCH_BASE_FRAME` and `SYMBOL_EDIT_FRAME` and `symbol_editor_control.cpp` casts to
`SYMBOL_VIEWER_FRAME`. All sixteen are now `dynamic_cast` with a defined path
when the answer is null:

| File | Sites | What happens with no frame |
|---|---|---|
| `sch_commit.cpp` | 6 | The edit goes through; the undo stack and the canvas refresh are skipped, which is what the `frame &&` guards already said and could not deliver |
| `tools/sch_editor_control.cpp` | 6 | Net highlighting and the net-chain actions return without doing anything |
| `tools/sch_selection_tool.cpp` | 2 | The two context sub-menus stay empty |
| `tools/symbol_editor_control.cpp` | 2 | See below — this pair was a live bug, not a latent one |

`sch_commit.cpp` is the one that mattered, because every edit goes through it,
and it is the one the new test pins down: restore the `static_cast` on line 195
and `NonFrameToolsHolder/ACommitEditsTheDocumentWithoutAFrame` segfaults.

### The part that was not in the plan

**The sixteen casts were the symptom. The mechanism is `getEditFrame<T>()`**
(`include/tool/tool_base.h:182`), which is how every tool's `m_frame` is set:

```cpp
template <typename T>
T* getEditFrame() const
{
#if !defined( QA_TEST )
    wxASSERT( dynamic_cast<T*>( getToolHolderInternal() ) );
#endif
    return static_cast<T*>( getToolHolderInternal() );   // ← the same cast, tree-wide
}
```

So converting the enumerated list would have left `SCH_TOOL_BASE<T>::Init()` —
the first thing that runs for seventeen of eeschema's tool classes — doing
`m_frame = getEditFrame<T>()` and then `m_frame->IsType(...)` through a pointer
that is not a frame. The enumerated list was never a sufficient condition for
safety, and a reviewer working only from it would have produced a commit that
looked complete and changed nothing about the actual hazard.

What makes a non-frame holder defined is checking at the **one** place each tool
learns what its frame is, and declining:

```cpp
m_frame = dynamic_cast<T*>( m_toolMgr->GetToolHolder() );

if( !m_frame )
    return false;
```

`TOOL_MANAGER::InitTools()` already unregisters and deletes a tool whose `Init()`
returns false, so this is the framework's own answer to "this tool cannot run in
this holder" rather than a new mechanism. Four entry points needed it —
`SCH_TOOL_BASE<T>::Init()`, `SCH_SELECTION_TOOL::Init()`,
`SYMBOL_EDITOR_CONTROL::Init()`, `SCH_DESIGN_BLOCK_CONTROL::Init()` — plus
`SIMULATOR_CONTROL::Reset()`, whose existing null test could never fire.

One consequence worth stating, because it moves work into Stage 4 rather than
out of it: the thirteen derived `Init()`s all called `SCH_TOOL_BASE::Init()` and
**discarded its result**, so the base's return value meant nothing. They now
propagate it. That is what makes the refusal real.

The second finding is smaller and is a **live** bug rather than a latent one.
`symbol_editor_control.cpp:974,983` read:

```cpp
if( SYMBOL_VIEWER_FRAME* viewerFrame = static_cast<SYMBOL_VIEWER_FRAME*>( ...GetToolHolder() ) )
    viewerFrame->SelectPreviousSymbol();
```

`SYMBOL_EDITOR_CONTROL` is registered by `SYMBOL_EDIT_FRAME` as well as by
`SYMBOL_VIEWER_FRAME` (`symbol_edit_frame.cpp:492`, `symbol_viewer_frame.cpp:283`),
and a `static_cast` of a non-null holder is never null, so in the symbol *editor*
that `if` always succeeded and called a viewer method on something that is not a
viewer. The `dynamic_cast` makes the test mean what it plainly says.

### What this does and does not buy Stage 4

It buys the absence of undefined behaviour, which was the whole of the stage. It
does **not** buy a working tool on a non-frame holder, and the staging in
`04-host-seam.md` §6.5 step 6 — "make `SCH_HOST` a `TOOLS_HOLDER` and register the
eeschema tools" — now has a visible, testable answer: every one of them declines.
`m_frame` is the route to the screen, the selection, the undo stack and every
dialog, and the survey's count of roughly 600 `m_frame->` sites in
`eeschema/tools/` is the size of making it not be. Stage 4 has to either give the
host something that *is* a `SCH_BASE_FRAME` or reroute those sites; it cannot
simply install a holder and expect tools.

That is a worse answer than the one this stage was expected to produce, and it is
the true one. It was not visible from the cast list.

### Tests

`qa/tests/eeschema/test_non_frame_tools_holder.cpp`, four cases in `qa_eeschema`:

* a commit against a non-frame holder edits the document, marks the screen
  modified and rebuilds connectivity — pushed *without* `SKIP_UNDO`, because
  deciding to skip undo is the commit's job and not a caller's;
* every one of eeschema's nineteen tool classes, both editors', declines such a
  holder, one `TOOL_MANAGER` each so a failure names itself;
* `InitTools()` leaves `GetTool<T>()` null for the ones that declined;
* the same commit with a null holder, which is what `EESCHEMA_HELPERS` installs
  for the CLI, so the two cases read as one behaviour.

The test double is 4 lines: `GetToolCanvas()` is `TOOLS_HOLDER`'s only pure
virtual and returning `nullptr` from it is already a production state. It does
have to supply a `KIGFX::VIEW`, because `SCH_SELECTION_TOOL`'s destructor unlinks
itself from `getView()` without a null check — a real constraint on any host, and
one `SCH_HOST` already meets.

`qa_eeschema` is 1,703 cases green (1,699 before, plus these four) and
`qa_common` 1,477; `ctest -L rust` is 4/4.

### What Stage 3 deliberately did not do

* **`getEditFrame<T>()` itself is unchanged.** Making it a checked cast is the
  right fix and it is one line, but `tool_base.h` is included tree-wide and the
  change would alter what pcbnew, gerbview and the 3D viewer get back from every
  call site. That is a separate commit with a separate review, and it is not
  eeschema's to make. Note that the assertion above **is** live in this Release
  QA build — the reverted-fix experiment above tripped it — so the tree does warn
  about a wrong holder today. It warns and then returns the bad pointer anyway.
* **pcbnew was not audited.** It has its own equivalents, including
  `tools/drawing_tool.cpp:232,269` (C-style casts to `PCB_EDIT_FRAME`) and
  `tools/pcb_selection_tool.cpp:145`, plus `dialogs/dialog_position_relative.cpp:279`.
  `05-porting-guide.md` §4.8 already says to audit them before starting there.
* **`common/` tools were not touched.** `COMMON_TOOLS`, `ZOOM_TOOL`,
  `PICKER_TOOL`, `PROPERTIES_TOOL`, `EMBED_TOOL` and `GROUP_TOOL` are registered
  by eeschema's frames but live in `common/`, and whether each survives a
  non-frame holder is unchecked. The test registers only eeschema's own.
* **Downcasts within the frame hierarchy are left alone.**
  `sch_selection_tool.cpp:1221,1242,1251` and `sch_tool_base.cpp:330,332`
  `static_cast` an already-valid `SCH_BASE_FRAME*` to a more derived frame. Those
  are guarded by `m_isSymbolEditor` or by a context-menu id that only one editor
  produces; they are a different question from "is the holder a frame at all".

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

The step this stage now has to start with, which Stage 3 promoted from an
assumption to a measured fact: **decide what `m_frame` is.** Every eeschema tool
declines a holder that is not a `SCH_BASE_FRAME`, deliberately and testably, so
there is no version of "send a `TOOL_EVENT` and see what happens" that reaches a
tool. Two routes, and they want costing before either is started:

* **Give the host a `SCH_BASE_FRAME`.** Cheap to write and it makes every tool
  work at once, but it means a `wxFrame` — so wxWidgets' GUI layer stays in the
  process and the project's stated goal is deferred rather than approached.
* **Reroute `m_frame`.** `00-architecture-survey.md` §7 measured that roughly 470
  of the ~600 `m_frame->` sites in `eeschema/tools/` are plain model or settings
  access with no wx involvement. Hoisting those onto an interface both
  `SCH_BASE_FRAME` and `SCH_HOST` implement is the honest version and is a large
  mechanical change — which is exactly why `SCH_HOST`'s header says it is not a
  refactor of `SCH_EDIT_FRAME`.

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

Stages 1–3 were estimated at roughly two to three days and came in there: about a
day each for 1 and 2, half a day for 3. All three are done. Between them they turn
a viewer of a recorded frame into a window onto a document that redraws itself, on
a tool framework that no longer corrupts memory when its holder is not a `wxFrame`.

All three spent most of their time on something that was not in the plan, and it
was the same kind of thing every time: a claim that two layers made differently
and nothing had yet forced them to reconcile.

| Stage | The plan said | The code said |
|---|---|---|
| 1 | a recorded stream is canonical | group ordering is not, across standard libraries |
| 2 | `ksch_viewport::scale` is pixels per internal unit | `SetViewport` fed it to `VIEW::SetScale`, which wants the GAL zoom factor — 2,800× out |
| 3 | eight (later fourteen) unchecked casts are the hazard | sixteen, and the mechanism is `getEditFrame<T>()`, which the lists never mentioned |

None was hard once seen and none was visible until something depended on it. Stage
3's version is the most useful one to carry into Stage 4, because it is about
*enumerations* rather than about numbers: a list of call sites, arrived at by
grepping for a cast, looked like a specification and was not one. The thing that
made it safe was found by asking where the value comes from, not by fixing the
places it is used.

Stage 4 is where a schematic editor actually lives, and Stage 3 sharpened the first
question it has to answer: what `m_frame` is for a host that is not a frame. That
is a decision about how much of `SCH_EDIT_FRAME` gets hoisted, and it is a larger
question than "write a dispatcher". Selection alone is still a meaningful
milestone; a tool set someone would choose over the wx editor is substantially
more.

Stage 5 is a separate project.

**The honest summary of this branch is that it finishes the rendering third of
the problem and leaves the editing two thirds.** That is a real result — the
rendering third was the part with the most unknowns, it is settled and tested on
every schematic in the tree, and it now runs end to end in one process and redraws
from the live document rather than from a copy — but it is a third. Nothing a user
does to the window reaches the schematic.
