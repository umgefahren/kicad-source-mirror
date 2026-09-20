# What is missing, and what it takes to get an editor

This document exists because the previous ones describe what was built, and a
reader can finish them with the wrong impression of what that adds up to.

**What exists today is a schematic editor with a working canvas and converted
eeschema editing tools.** A user can open a real `.kicad_sch`, select items by clicking
or dragging a box, move them, rotate and mirror and delete them, draw wires,
place junctions, no-connects, labels and sheet pins, cut and paste, undo and redo
any of it, and save a file that KiCad reopens.

**Most dialog workflows still need porting**: placing a symbol needs the library
chooser, editing an item’s properties needs its properties dialog, and ERC needs
`DIALOG_ERC`. Find/Replace
now has GPUI controls backed by the host search-data interface. The remaining
dialogs still use wxWidgets, so **wxWidgets has not been removed** and the wx schematic editor is still the only
complete way to edit a schematic. The dialogs are Stage 5, and Stage 5 is a
project rather than a stage.

> **Stages 1, 2, 3, 4 and 4b are done, and 4b now covers the whole eeschema
> roster.**
> `kicad-eeschema-gpui --schematic FILE.kicad_sch` loads the file through
> eeschema's own reader, keeps the session open for the window's lifetime, asks it
> for a frame whenever the view moves or the document changes, and hands it every
> pointer move, click, drag, scroll and key press as a `TOOL_EVENT`.
>
> Every tool class in `eeschema/tools/` now answers `runsWithoutAFrame()` except
> `SCH_DESIGN_BLOCK_CONTROL`, which is a library pane and a properties dialog end
> to end. What still declines is `common/`'s roster — `COMMON_TOOLS`,
> `COMMON_CONTROL`, `ZOOM_TOOL`, `PICKER_TOOL`, `GROUP_TOOL`, `EMBED_TOOL` — which
> every KiCad program inherits and which is not eeschema's alone to hoist.
>
> **Registering is not the same as working, and this document is careful about the
> difference.** A tool that runs here may still decline every action it owns,
> because the action *is* a dialog. See
> [what each converted tool actually does](#what-each-converted-tool-actually-does).

## Stage 4 completion and Stage 5 progress

**Stage 4 is complete within its defined scope**: host input dispatch (4a), the
eeschema frame hoist (4b, excluding the dialog-only design-block tool), and
M4's action-registry menus/toolbars are implemented. W and native macOS hints
are fixed and verified. This milestone does not claim every shared tool or
model operation is complete: `DeleteJunction`, body-style selection and units
provider seams remain listed below, alongside shared-tool frame dependencies.

**Stage 5 has started, but is not complete.** GPUI Find/Replace supplies search
terms to the host and uses KiCad's search traversal and undo stack. Unsaved
window close and editor Quit prompt Save/Discard/Cancel, with failed saves
keeping the window open. The GPUI API cannot veto external OS quit. Symbol
placement, properties, ERC and functional document panels remain future work.
The known linked golden-render mismatch is recorded in
[`08-action-registry.md`](08-action-registry.md).

## Exactly where it stops

The eeschema tool roster conversion is complete except for
`SCH_DESIGN_BLOCK_CONTROL`. The converted tools still decline work that requires
a dialog; shared tools in `common/` also retain frame dependencies. See the
[per-tool table](#what-each-converted-tool-actually-does) for the distinction
between initialization and usable behavior.

Live menus, toolbars and the command palette now resolve action metadata from
the C++ registry. This does not expose handler availability or dynamic checked
state. The [registry milestone report](08-action-registry.md) records the live
verification and the shortcut, unsaved-close and Find/Replace follow-ups.

So the pipeline now is:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► stream in memory
   ▲               ▲                                  │
   │        TOOL_MANAGER  ◄── HOST_TOOL_DISPATCHER    │
   │               ▲              ▲             C ABI │
 save              │              │                   │
   └───────────────┴── viewport ──┴── input ── gpui window ◄────┘
                                            every view change, and
                                            every edit a tool makes
```

A complete editor still needs the remaining host seams, shared-tool conversions,
functional document panels and dialog workflows (Stage 5).

## What is already done and does not need redoing

Worth being clear about, because it changes the size of what remains:

| | |
|---|---|
| `RECORDING_GAL` + `DRAW_STREAM` | Complete. 30 tests in `qa_common` |
| The draw-stream ABI | Frozen, layout-asserted on both sides, sync-tested |
| `SCH_HOST` | Loads, renders, enumerates sheets, zooms, and is a `TOOLS_HOLDER`; 52 host-suite tests including ABI, registry, search and undo |
| The C ABI | 36 entry points, three of them the runtime; implemented, and bound from Rust |
| `kicad-gal` | Validating decoder, 58 tests |
| `kicad-sch-render` | Stream → gpui primitives, 89 tests |
| `kicad-sch-ui` | Shell, 55 unit and 41 interaction tests, plus 2 toolbar checks and a doctest |
| Action registry | Owned Rust metadata drives live menus, toolbars and the command palette; primary/alternate shortcut names cross the ABI as strings |
| `kicad-sch-sys` | The ABI linked from Rust; 17 live checks pass, with one known golden-render mismatch |
| Live re-render | The canvas asks the session for the frame it is about to paint |
| A non-frame `TOOLS_HOLDER` | Defined rather than undefined: 16 checked casts in eeschema, 7 more entry points in `common/`, 11 tests |
| `HOST_VIEW_CONTROLS` | A `VIEW_CONTROLS` that is told where the pointer is instead of polling the OS |
| `HOST_TOOL_DISPATCHER` | Host input → `TOOL_EVENT`, wx-free; 32 tests in `qa_common` |
| Input end to end | gpui → ABI → `TOOL_MANAGER`, with the key names mapped to `WXK_*` in one place |
| `SCHEMATIC_HOLDER` | Upstream's own bridge class, grown into what a schematic tool needs from whatever is editing the schematic. Implemented by `SCH_BASE_FRAME` and by `SCH_HOST` |
| `UNDO_REDO_HOLDER` | The undo and redo stacks, off `wxFrame`; 4 tests in `qa_common` |
| `SCH_UNDO_REDO` | `SaveCopyInUndoList`, `PutDataInPreviousState`, `Undo`, `Redo`, `Rollback`, off `SCH_EDIT_FRAME` and shared with it; 6 tests, the first eeschema's undo has had |
| Selection | `SCH_SELECTION_TOOL` runs on the host: click, drag box, clear, Escape; 7 tests |
| Move and wire | `SCH_MOVE_TOOL` and `SCH_LINE_WIRE_BUS_TOOL` run; a drag moves, a wire draws, both undoable; 6 tests |
| Save | `ksch_session_save`, and `SCH_HOST::Save()` through `SCH_IO_KICAD_SEXPR`; a saved file reopens and renders |

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

* Input still went to `NullSink`. That was Stage 4a, below, and is done.
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
`eeschema/tools/` is the size of making it not be. Stage 4b has to either give the
host something that *is* a `SCH_BASE_FRAME` or reroute those sites; it cannot
simply install a holder and expect tools.

> Stage 4b rerouted them, for three tools, and the 600 turned out to be the wrong
> unit: most of those sites are `GetScreen()`, `AddToScreen()` and `UpdateItem()`
> repeated, so the interface is about twenty methods and the conversions are
> mechanical. See [Stage 4b](#stage-4b--the-frame-hoist-done-for-the-whole-eeschema-roster).

Stage 4a went on to install exactly such a holder and register exactly that roster,
and this is what happened: all twenty-one classes are dropped by `InitTools()` and
`GetTool<T>()` is null for each, which is defined, testable and not an editor. It
also found that `common/`'s tools — the ones this stage recorded as unaudited —
crash rather than decline, which is the other half of the same lesson.

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

## Stage 4 — A non-wx tool dispatcher (the input seam is done)

**Estimated "the real work". The dispatcher half took about a day; the `m_frame`
half is [Stage 4b](#stage-4b--the-frame-hoist-done-for-the-whole-eeschema-roster), which took
about two days and is what makes the events below reach anything.**

Everything from a gpui event to `TOOL_MANAGER::ProcessEvent` now exists and is
tested end to end. What that took:

| | |
|---|---|
| `include/view/host_view_controls.h`, `common/view/host_view_controls.cpp` | `HOST_VIEW_CONTROLS`: a `VIEW_CONTROLS` that is *told* where the pointer is |
| `include/tool/host_tool_dispatcher.h`, `common/tool/host_tool_dispatcher.cpp` | `HOST_TOOL_DISPATCHER`: host input → `TOOL_EVENT`, and the one place key names become `WXK_*` |
| `eeschema/host/sch_host.{h,cpp}` | `SCH_HOST` is a `TOOLS_HOLDER`, owns a `TOOL_MANAGER`, `SCH_ACTIONS` and both of the above, and registers eeschema's roster |
| `include/sch_host/sch_host_abi.h` (v3) | `ksch_session_dispatch_input`, `_reset_input`, `_run_action`, `_editor_state` |
| `rust/crates/kicad-sch-sys` | `InputEvent`, `InputOutcome`, `EditorState` and four `Session` methods |
| `rust/crates/kicad-eeschema-gpui/src/host_sink.rs` | `HostInputSink`, which replaces `NullSink` when a session is open |
| `rust/crates/kicad-sch-ui/src/input.rs` | `InputSink::take_dirty` and `::selection_count`, so the host can say "re-record" and "this many are selected" |
| 7 entry points in `common/tool/` | See ["the part that was not in the plan"](#the-part-that-was-not-in-the-plan-3) |

`TOOL_MANAGER` needed nothing, as predicted: no wx in its header, and `TOOL_EVENT`
is a plain value type. `TOOL_DISPATCHER` was rewritten rather than subclassed,
also as predicted.

### What the two dispatchers share, and what they deliberately do not

`HOST_TOOL_DISPATCHER` reproduces `TOOL_DISPATCHER`'s *output* contract exactly —
the ten `TOOL_EVENT` shapes of survey §6.3, the `BUT_*`/`MD_*` bit values, click
position taken from the press rather than the release, `TA_MOUSE_UP` versus
`TA_MOUSE_CLICK`, the two-modifier wheel gate, the keyboard event carrying the
cursor — and drops the reconciliation, because the quirks are wx's and not
KiCad's:

| `TOOL_DISPATCHER` does | and this does not, because |
|---|---|
| polls `wxGetMouseState()` to notice a button-up it never received | the host delivers every up, and `POINTER_LEAVE` covers one that happened off-canvas |
| polls `wxGetKeyState()` to drop an auto-repeat that arrived after release | the host says whether a press is a repeat, and a burst cannot outlive the release |
| remaps Ctrl+letter from ASCII 1..26, and Alt+digit from Apple raw key codes | those are wx key-event artefacts; a host reports the key and the modifier separately |
| folds the numpad arrows onto the arrow keys, for `wxEVT_CHAR_HOOK` only | the host reports one or the other and means it |
| flushes a pending click before Escape, because wx may deliver them out of order | host events are ordered, and the invariant that protects still holds |
| asks `wxWindow::FindFocus()` whether a text control has focus | the shell decides what its keyboard focus means and does not send the event |

Two things were kept that look droppable. `IsPastDragThreshold` is *reused*, not
reimplemented — it is `static` and wx-free precisely so it can be. And the macOS
`DragTimeThreshold`, which promotes a slow trackpad drag after 300 ms regardless
of distance, is kept under the same platform guard, because it is a real
behaviour of the wx editor on the same machine rather than a wx artefact.

Three places diverge deliberately, and each is the wx one being wrong rather than
different:

* **A wheel event carries a position.** `TOOL_DISPATCHER` does not set one, and
  because the event is `TC_MOUSE` its `HasPosition()` is true anyway — so
  `Position()` returns the origin instead of tripping the check that exists to
  catch exactly that. Nothing reads it today; filling it in costs a line.
* **Losing the pointer, or the focus, *ends* a drag rather than forgetting it.**
  Clearing the button state alone leaves the tool in its `Wait()` loop with no
  release coming and no state left to match one against, which is a hang rather
  than a lost event. The wx dispatcher gets there by a different road: it polls the
  OS and synthesises the up on the next event it sees. A press that had not become
  a drag still gets nothing — `flushPendingClicks()` emits a click there, but only
  because the OS has confirmed the button is up, and losing the pointer says
  nothing of the kind.
* **A wheel turn emits motion *and* the wheel event when both are true.** wx
  suppresses the wheel event if it made a motion event; a tool handles the two
  independently and suppressing one of two true statements has nothing to
  recommend it. What is *not* optional is that a wheel turn goes through the motion
  path at all: zooming moves the world under a stationary pointer, and a tool
  tracking a drag has to be told.

`ShouldDropAutoRepeat` is the one the plan named as reusable and this does **not**
use. Its whole job is to notice a repeat that arrived after the key was physically
released, which is a thing wx's key model produces and an ordered event stream
cannot. Reusing it would have been cargo cult; the host's own `is_held` flag is
both simpler and more accurate.

### Where the key-code constraint went

The plan's warning was right and its remedy was not. Hotkeys **are** `WXK_*`
integers, and a UI whose keys are named rather than numbered has to be mapped onto
them or every shortcut silently does nothing. But the plan said to transcribe
`wx/defs.h` into a Rust table, and a transcribed table is one wrong entry per
silently broken shortcut, forever.

So the key crosses the ABI as a **name** — `"escape"`, `"pagedown"`, `"f11"`,
`"w"` — and `HOST_TOOL_DISPATCHER::KeyCodeFromName` resolves it in C++, where the
numbers come out of `wx/defs.h` through the compiler. `rust/` contains no `WXK_`
constant at all; `grep -rn "WXK_" rust/crates/` finds nothing but the two comments
that say where the mapping lives. A name the table does not
know is dropped rather than sent as key zero, which would run whatever is bound
to 0.

### The part that was not in the plan

**`common/`'s tools crash a non-frame holder, and it is a live crash rather than a
latent one.** Stage 3 fixed eeschema's tools and said in as many words that
`common/`'s were unchecked and that the test registered only eeschema's own. The
first thing this stage did was register the roster `SCH_EDIT_FRAME` registers —
which includes seven classes from `common/` — and `SCH_HOST`'s constructor
segfaulted, in `TOOL_MANAGER::InitTools()`, before a single `TOOL_EVENT` existed.

The mechanism is `getEditFrame<T>()` again, exactly as Stage 3 found it. Seven
entry points needed the Stage-3 treatment:

| Site | What it did | What it does |
|---|---|---|
| `COMMON_CONTROL` | no `Init()` at all; `Reset()` stored a wild frame | `Init()` declines a non-frame holder |
| `COMMON_TOOLS` | no `Init()`; `Reset()` called `m_frame->GetWindowSettings()` through it | `Init()` declines |
| `ZOOM_TOOL::Init` | `getEditFrame<EDA_DRAW_FRAME>()->AddStandardSubMenus()` | checked, declines |
| `PICKER_TOOL::Init` | same shape | checked, declines |
| `GROUP_TOOL::Init` | stored a wild frame, then `wxCHECK`ed for the selection tool | checked, declines before the `wxCHECK` |
| `PROPERTIES_TOOL::UpdateProperties` | `if( editFrame )` on a `static_cast`, so the guard could never fire | `dynamic_cast`, so it can — the same live bug Stage 3 found in `symbol_editor_control.cpp` |
| `EMBED_TOOL::Init` | `getModel<EDA_ITEM>()->GetEmbeddedFiles()` with no null check | declines when there is no model, which a host has before its first load |

Reverting any one of them reproduces it. Restoring `ZOOM_TOOL::Init`'s unchecked
cast makes `SchHostInput/TheHostIsItsOwnToolHolder` fail on `getEditFrame<T>()`'s
own `wxASSERT` — which is live in this Release QA build, so the tree does warn
about a wrong holder, and then returns the bad pointer anyway.

The lesson is Stage 3's, one layer out. Stage 3 wrote down that `common/`'s tools
were unaudited and moved on, which was a correct statement of scope and a poor
prediction of cost: the next stage could not take a single step without them.
An enumeration of what was *not* checked is not a smaller problem than an
enumeration of what was.

### The second part that was not in the plan: ⌘ is Ctrl

The key-code constraint is the one every document on this branch flags, and it has
a quieter half that none of them mentions: **on macOS, KiCad's `MD_CTRL` is
Command, not Control.**

`wx/defs.h` defines `wxMOD_CMD == wxMOD_CONTROL` there, and physical Control
arrives as `wxMOD_RAW_CONTROL`, which `TOOL_DISPATCHER::decodeModifiers` does not
look at. So in the wx editor on macOS, ⌘Z is undo and ⌃Z is nothing — and every
`.DefaultHotkey( MD_CTRL + 'Z' )` in the tree is written on that understanding.

A shell that reports gpui's modifiers literally — `platform` (⌘) as "meta",
`control` (⌃) as "ctrl" — and a host that maps them literally would produce the
inverse: ⌃Z would undo and ⌘Z would do nothing at all, silently, because an
unmatched hotkey is indistinguishable from no hotkey. The ABI therefore carries the
*physical* keys and `toHostInput()` decides what they mean, mapping `KSCH_MOD_META`
to `MD_CTRL` under `__APPLE__`. Which is the same conclusion as the key names, for
the same reason: the side that already includes `wx/defs.h` is the side that should
be doing the translating.

### What this stage deliberately did not do

* **`TOOLS_HOLDER::GetToolCanvas()` is still pure virtual.** §6.5 step 2 proposed
  giving it a default `nullptr`. It is not worth it: `SCH_HOST` implements it in
  one line, exactly as Stage 3's four-line test double does, and changing a base
  class every KiCad program inherits so that one new class can omit one line buys
  nothing. Survey §7.6's retraction stands — this was never the blocker.
* **Undo/redo is not hoisted.** Nothing this stage does mutates a document, so
  there is nothing to undo yet, and moving `UNDO_REDO_CONTAINER` off
  `EDA_BASE_FRAME` is an edit to a base class every KiCad program inherits. It
  belongs with the first tool that can edit. §6.3 has the shape.
* **`VIEW_CONTROLS` does not drive pan and zoom.** The Rust camera still does, and
  the session adopts it per frame as it has since Stage 2. `HOST_VIEW_CONTROLS`
  answers where the cursor is, which is what the tools ask it; making it the
  *authority* on the view is item 2 of the old list and wants a tool that cares.
* **`WarpMouseCursor` cannot move the pointer.** gpui has no API for it. The
  implementation does the half it can — adopt the position, move the view — and
  records the request, so a host that can honour it may. The survey predicted this
  degrades gracefully, and it does: a tool that warps and then reads the cursor
  back gets what it asked for.

### Tests

* `qa/tests/common/test_host_input.cpp`, **32 cases** in `qa_common`: seven on
  `HOST_VIEW_CONTROLS`, twenty-one on the dispatcher — driving synthetic input
  through a real `TOOL_MANAGER` and asserting on the events a recording tool
  receives — and four on the key-name table.
* `qa/tests/eeschema/test_sch_host.cpp`, **13 new cases** in `qa_eeschema`: seven
  on the host's tool framework, including that the roster is registered and none
  of it survives, and six on the new ABI entry points.
* `rust/crates/kicad-eeschema-gpui/src/host_sink.rs`, **10 unit tests** on the
  translation, against a recorder rather than a linked host.
* `rust/crates/kicad-sch-ui/tests/shell.rs`, **3 new tests**: a host that reports a
  change gets a fresh frame, once; the selection count comes from the host; every
  press gets a release even with two buttons held.
* `rust/crates/kicad-sch-sys/tests/live_session.rs`, **3 new checks** against the
  real host: input moves the cursor the tools read, a whole gesture crosses and
  edits nothing, an unhandled action is reported rather than an error.

`qa_common` is 1,509 cases green (1,477 before), `qa_eeschema` 1,716 (1,703
before), the Rust workspace 248 plus 14 live checks, and `ctest -L rust` 4/4.

### Four things this stage got wrong first, and how

Kept because they are the shape of mistake this seam invites, not because they are
interesting individually. Each was found by review rather than by a test, which is
itself the finding: all four are cases where the code and its own comment agreed
with each other and both were wrong.

| What | Why the tests missed it |
|---|---|
| `TOOL_MANAGER::RunAction( const std::string& )` reports that the *name resolved*, not that anything ran — and `ACTION_MANAGER` registers every action in the process, so a host with no tools would have called all 440 of them "handled" | the test used an action name that does not exist, so it passed either way. `SCH_HOST` now looks the action up and uses the overload whose result is `processEvent`'s |
| the fast-click-drag demotion also tested `pressed`, which the documented event order guarantees is false by then — so the branch was unreachable and a click-then-drag lost its drag entirely | the test fed `DOWN` then `DBLCLICK` with no release between them, an order the ABI says never happens |
| `ksch_editor_state::tool_name` was never `""`: `TOOLS_HOLDER::CurrentToolName()` answers an empty stack with the selection tool's name | the test asserted the pointer was non-null |
| the shell tracks one press at a time, so with two buttons held the release of the first was dropped — and a host with per-button state then believed it held for the rest of the session, turning every later move into a drag from a stale origin | nothing had ever pressed two buttons; this is the failure the dispatcher gave up wx's mouse-state poll on the promise that "the host delivers every up" |

## Stage 4b — The frame hoist (done, for the whole eeschema roster)

**Estimated "the largest single piece of the project". It was the largest, and it
came in smaller than the aggregate estimate for the reason Stage 4a predicted: the
interface a tool needs from its frame is around a dozen methods, not six hundred
call sites.**

A user can now select, move, draw a wire, undo it and save a file KiCad reopens,
which is exactly what this stage said "done" would mean.

### The route taken, and why it needed no new class

Of the two routes, this is the second — reroute `m_frame` onto an interface that
both `SCH_BASE_FRAME` and `SCH_HOST` implement — and the interface turned out to
already exist. `SCHEMATIC_HOLDER` (`eeschema/schematic_holder.h`) was four virtuals
of upstream's own, introduced as

> a bridge class to help the schematic be able to affect SCH_EDIT_FRAME without
> doing anything too wild in terms of passing callbacks constantly in numerous
> files […] The long term goal would be to fix the internal structure and make the
> relationship between frame and schematic less intertwined

which is this stage's goal stated by someone else, earlier. `SCH_BASE_FRAME`
already implemented it. What it grew is the rest of what a schematic tool asks its
editor for:

| | |
|---|---|
| the document | `GetScreen`, `GetSchematic`, `ResolveItem`, `UpdateItem`, `AddToScreen`, `RemoveFromScreen` |
| settings that decide behaviour | `eeconfig`, `GetRenderSettings`, `GetShowAllPins`, `GetOverrideLocks` |
| editing | `SaveCopyInUndoList`, `RecalculateConnections`, `UpdateHopOveredWires`, `OnModify`, the repeat-item list, `AutoRotateItem` |
| notifications a canvas *owner* can act on | `ForceRefreshCanvas`, `SetCurrentCursor`, `HighlightSelectionFilter` |
| which kind of editor this is | `IsSchematicEditor`, `GetSelectionTool` |

Two rules kept it honest. Anything inherently a **window** — a dialog, an info bar,
keyboard focus, hypertext navigation, the hierarchy navigator, the variant selector
— deliberately stayed off it; a tool that wants one downcasts and does nothing when
the answer is null, with a comment at the site saying what is lost. And anything
the **tool framework already answers** stayed off it too: `TOOL_BASE::getView()`
and `getViewControls()` come from `TOOL_MANAGER`, so `m_frame->GetCanvas()->GetView()`
was never a reason to need a frame, and `TOOL_MANAGER::GetToolHolder()` answers
`ToolStackIsEmpty()`, `GetDragAction()`, `IsCurrentTool()` and `PushTool()`.

`SCH_TOOL_BASE<T>` gained the opt-in the rest of the roster will use: an `m_editor`
that is non-null whenever the tool initialised at all, and a `runsWithoutAFrame()`
that is **false by default**, because a tool that has not been converted would hold
a null `m_frame` and crash on its first use rather than decline.

### What each of the five steps cost

| Step | State | What it took |
|---|---|---|
| 1. Selection | done | `SCHEMATIC_HOLDER` grown; `SCH_SELECTION_TOOL::m_frame` becomes `m_editor`; 70 sites, of which ~50 mechanical |
| 2. Pan/zoom through `VIEW_CONTROLS` | **not done** | See below: it needs `COMMON_TOOLS`, which needs an `EDA_DRAW_FRAME` |
| 3. Move | done | 57 sites in `SCH_MOVE_TOOL`, of which 52 were already the interface's |
| 4. Wire | done | And it is a *prerequisite* of step 3, not a successor — see below |
| — undo/redo | done | `UNDO_REDO_HOLDER` off `EDA_BASE_FRAME`, `SCH_UNDO_REDO` off `SCH_EDIT_FRAME`, both shared with the frame |
| 5. Saving | done | `ksch_session_save`, `SCH_HOST::Save()`, ABI version 4 |

**Step 2 is the one that did not happen, and the reason is worth stating.** The
Rust camera still drives the view and the session adopts it per frame, as it has
since Stage 2. Making `VIEW_CONTROLS` the authority instead is not hard in itself —
`HOST_VIEW_CONTROLS` exists and `VIEW_CONTROLS` was always abstract — but it buys
nothing until a *tool* moves the view, and the tools that do are `COMMON_TOOLS` and
`ZOOM_TOOL`, both of which read `EDA_DRAW_FRAME::GetWindowSettings()`,
`OnUpdateSelectGrid()`, `MakeGridHelper()` and `config()`. Converting those means
hoisting onto `TOOLS_HOLDER`, which every KiCad program inherits, so it is pcbnew's
and gerbview's decision as much as eeschema's. It is the natural next piece and it
is not this stage's.

### The part that was not in the plan

Five things, and four of them are the same thing: **the host inherits defaults that
the GUI always overwrites from settings, and a default that is never used is a
default nobody checked.**

**1. `KIGFX::GAL` never initialises its grid size.** Its constructor sets every
other graphics default it has and leaves `m_gridSize` at `VECTOR2D()`'s zero,
because in a GUI `COMMON_TOOLS::Reset()` always fills it in from the window
settings — and that tool declines a non-frame holder. A zero grid is not merely "no
grid": `GRID_HELPER` divides the cursor position by it, so the first tool that
snaps gets an infinity and `KiROUND` asserts on it, several frames from the cause.
Found by running a selection, which is the first thing that snaps.
`SCH_HOST::initGrid()` reads the user's own grid list exactly as `COMMON_TOOLS`
does, so there is no second idea of what eeschema's grid is.

**2. `TOOLS_HOLDER`'s input preferences, the same way.** Its constructor sets
`m_dragAction = MOUSE_DRAG_ACTION::SELECT`, and every frame replaces it from the
user's common settings in `CommonSettingsChanged()`. Until the host called that, a
drag over a *selected* item drew a rubber band instead of moving it — the setting
said otherwise and nothing was reading it. Worse, the rubber-band test written for
step 1 **passed because of the bug**: it always took the branch it was asserting
on. The same call also picks up warp-on-move, immediate actions and the user's
hotkeys.

**3. A session with no document runs nothing.** `SCH_EDIT_FRAME` has a `SCHEMATIC`
and an empty `SCH_SCREEN` from its constructor on, so a tool may use `GetScreen()`
without checking — and does, in hundreds of places. This host has neither until
something is loaded, and `drawWires` on an empty session dereferenced null. Caught
by a test written in Stage 4a for a different reason, which is the argument for
having written it. `SCH_HOST` now refuses input and actions with no document;
giving it an empty document at construction, as the frame has, is the better
long-term answer and changes what the ABI reports for an empty session.

**4. The wire tool is a prerequisite of the move tool.** Moving a wire off a
junction has to add one where it left, and `AddJunctionsIfNeeded` and
`TrimOverLappingWires` are how that is done — so with `SCH_LINE_WIRE_BUS_TOOL`
absent, `SCH_MOVE_TOOL` called them through a null pointer. Step 4 of the plan's
ordering is therefore *inside* step 3, and the plan had them a step apart.

**5. `EDA_DRAW_FRAME` declared a second `m_undoRedoCountMax` that shadowed
`EDA_BASE_FRAME`'s.** `LoadSettings` wrote the user's `max_undo_items` into the
derived one, which nothing read; `PushCommandToUndoList` and `GetMaxUndoItems()`
read the base one, which was only ever the constructor's default. So the preference
was ignored in eeschema, pcbnew, gerbview and the page-layout editor — and, because
`SaveSettings` writes `GetMaxUndoItems()` back, overwritten on every save. Found
only because the hoist moved that member. Fixed; the default is zero, meaning no
limit, so nothing changes for anyone who never set one.

### And a sixth, which is a gap rather than a bug: undo had no tests

Nothing in the QA suite called `SaveCopyInUndoList` or `PutDataInPreviousState` —
they were `SCH_EDIT_FRAME` members and exercising them meant standing up a window —
so **eeschema's undo had no coverage whatsoever.** That is what made moving them
risky, and it is also what made moving them the right call rather than giving the
host a second implementation: the same code now runs in both, and the six cases
that drive it through the host are the first tests it has ever had. The sharpest is
in the live-host suite and is a stronger claim than any assertion about one item's
coordinates: save an untouched copy, drag a wire, save again and confirm the bytes
differ, undo and save once more and confirm the bytes are **identical to the
baseline**.

### One duplication, deliberately

`SCH_HOST_CONTROL` is a four-method tool that handles `ACTIONS::undo`, `redo` and
`save`. `SCH_EDITOR_CONTROL` registers those in the wx editor and still declines a
non-frame holder — 3,885 lines and 61 distinct frame methods, mostly dialogs — so
converting it is not how to get ⌘Z working.

It is a *tool* rather than three C ABI calls because **a hotkey is resolved inside
`TOOL_MANAGER`**: a UI on the far side of the boundary can forward ⌘Z but cannot
intervene in what it means, so special-casing the three action names on the Rust
side would have made the menu work and the key not. The handlers are three lines
each and delegate to the shared `SCH_UNDO_REDO`, so what is duplicated is the
dispatch and not the work. The ABI carries `ksch_session_undo`, `_redo` and `_save`
as well, for a UI that wants a status code rather than a fire-and-forget action.

### Tests

* `qa/tests/common/test_undo_redo_holder.cpp`, **4 cases**: push, pop, the description a
  menu item shows, and the depth limit trimming the *oldest* command. This logic had no
  direct test before, because reaching it meant standing up a frame.
* `qa/tests/eeschema/test_sch_host.cpp`, **20 new cases** across three suites:
  * `SchHostSelection` — a click selects the item under it; a click on nothing clears;
    Escape clears; a drag selects everything inside the box it draws; selecting asks the
    consumer to redraw; a selected item changes what is *recorded*, which is the only way
    a user sees it.
  * `SchHostUndo` — an edit is recorded, undone and redone; undo restores a label's **net
    name**, so the connection graph was rebuilt from the restored document; an empty stack
    is a no-op that says so; the depth limit discards the oldest.
  * `SchHostEditing` — a drag moves the selected item and is undoable; a wire is drawn with
    the pointer and undone; undo, redo and save work as *actions*; the undo **hotkey**
    reaches the host; an edit survives a save and a reload in a second session.
  * plus the roster, which is pinned exhaustively so that converting another tool fails a
    test and says so, and `AnEmptySessionRunsNothing`.
* `rust/crates/kicad-sch-sys/tests/live_session.rs`, **2 new checks** against the linked
  host: a click selects the item under it, computing the screen position from the camera
  contract alone; and the save round trip, whose undo assertion is byte-exact.
* `rust/crates/kicad-eeschema-gpui/src/host_sink.rs`, **1 new unit test**: undo, redo and
  save go through the action registry rather than being short-circuited, which is what
  makes the hotkey and the menu item the same thing.

`qa_eeschema` is 1,735 cases green (1,716 before this stage), `qa_common` 1,513,
`qa_pcbnew` green — the base-class changes touch it — and `ctest -L rust` 4/4.

### The rest of the roster, converted

The table below is what the estimate said each remaining tool would cost, and what
it actually cost. All of them are now done.

**The estimate's method was wrong, and in a way worth recording.** It counted
`grep -c "m_frame->"`. That is not how several of these tools reach their frame:
`SCH_TOOL_BASE::frame()` is `getEditFrame<T>()`, which is an unchecked
`static_cast` of the tool holder — undefined behaviour on a holder that is not a
frame, and invisible to that grep. `EE_GRAPHIC_TOOL` was listed at one site and
has thirty-three. The lesson for the next editor is in
`05-porting-guide.md`: count the *methods* a tool asks its frame for, and count
`frame()` as a site.

| Tool | estimated sites | actual | what it cost |
|---|---:|---:|---|
| `SCH_ALIGN_TOOL` | 4 | 4 | as estimated |
| `SCH_EDIT_TABLE_TOOL` | 1 | 2 | plus `getScreen()` in the header |
| `EE_GRAPHIC_TOOL` | 1 | 33 | 32 through `frame()` |
| `SCH_POINT_EDITOR` | 14 | 21 | plus a helper class holding `EDA_DRAW_FRAME&` |
| `SCH_NAVIGATE_TOOL` | 33 | 35 | needed one interface addition; see below |
| `SCH_FIND_REPLACE_TOOL` | 28 | ~30 | converts, then declines; see below |
| `SCH_INSPECTION_TOOL` | 13 | ~35 | converts, then declines; see below |
| `SCH_DRAWING_TOOLS` | 134 | 149 | 15 through `frame()` |
| `SCH_EDIT_TOOL` | 164 | ~170 | 4 through `frame()` |
| `SCH_EDITOR_CONTROL` | 180 | ~186 | 6 through `getEditFrame` |
| `SCH_DESIGN_BLOCK_CONTROL` | 0 | — | **not converted**, correctly: a library pane and a dialog |
| `COMMON_TOOLS`, `ZOOM_TOOL`, `PICKER_TOOL`, `GROUP_TOOL`, `EMBED_TOOL`, `COMMON_CONTROL` | — | — | **not converted**: in `common/`, needs a `TOOLS_HOLDER`-level hoist, so not eeschema's alone |

`PROPERTIES_TOOL` is in that last row's list in the source but is not in it here,
because it never needed converting: it is a bare `TOOL_INTERACTIVE` with no
`Init()` of its own, so it has run on `SCH_HOST` since before any of this.

One interface method was added, for `SCH_NAVIGATE_TOOL`:
`SCHEMATIC_HOLDER::DisplaySheet( const SCH_SHEET_PATH& )`. Changing sheet is
`SetCurrentSheet` followed by `DisplayCurrentSheet`, and doing only the first
leaves the editor showing the sheet it has left, so it is one call rather than
two. Both implementations already existed; what was missing was the seam.

### What each converted tool actually does

Registering and working are different claims. These tools run on a holder with no
window; what each one can *do* there varies, and the split is always the same —
the model work runs, the dialogs decline.

| Tool | runs headless | declines, because it is a window |
|---|---|---|
| `SCH_SELECTION_TOOL` | all of it | — |
| `SCH_MOVE_TOOL` | all of it | info-bar hint, net-collision preview |
| `SCH_LINE_WIRE_BUS_TOOL` | all of it | — |
| `SCH_ALIGN_TOOL` | all of it | the Align submenu |
| `SCH_EDIT_TABLE_TOOL` | row, column and cell edits | table properties, CSV export |
| `SCH_POINT_EDITOR` | every drag | message panel, cursor shape, menu |
| `EE_GRAPHIC_TOOL` | arcs, Béziers, elliptical arcs | text-box content, graphics import |
| `SCH_NAVIGATE_TOOL` | all eight navigation actions | hypertext links |
| `SCH_DRAWING_TOOLS` | junctions, no-connects, bus entries, labels, hierarchical labels, directive labels, sheet pins, tables, rule areas, images | placing a symbol, importing or drawing a sheet, syncing sheet pins, free text |
| `SCH_EDIT_TOOL` | rotate, mirror, swap, repeat, delete, autoplace, justify, lock, label/text conversion | every properties dialog |
| `SCH_EDITOR_CONTROL` | clipboard, undo, redo, appearance toggles | 44 actions; the File menu, netlists, annotation, cross-probing |
| `SCH_INSPECTION_TOOL` | **nothing** | see below |
| `SCH_FIND_REPLACE_TOOL` | find next/previous, replace/replace-all through host search data | wx dialog entry points; GPUI owns its own panel |

The inspection tool still declines work that lives in dialogs. Find/Replace now
has the search-data seam that was missing in the initial conversion:

* **`SCH_INSPECTION_TOOL` computes nothing.** There is no compute/report seam here
  to hoist. ERC lives inside `DIALOG_ERC`, which owns the `ERC_TESTER` run and the
  marker provider; the symbol check needs a `UNITS_PROVIDER`, which a
  `SCHEMATIC_HOLDER` is not; the marker actions walk the dialog's list rather than
  the document. A headless caller drives `ERC_TESTER` and `CheckLibSymbol()`
  itself, and does not come through this tool.
* **`SCH_FIND_REPLACE_TOOL` accepts host-owned terms.** The GPUI panel sends
  owned search data through the C ABI to `SCHEMATIC_HOLDER::GetHostSearchData()`.
  The existing tool traverses sheets, selects matches and commits replacements;
  Rust receives coordinates, wrap status and replacement counts.

### What the next person should add, in order

Four things were wanted during this work and deliberately not added, because each
is a judgement about the interface rather than a mechanical conversion:

1. **`DeleteJunction( SCH_COMMIT*, SCH_ITEM* )`** — the biggest. About 80 lines in
   `eeschema/bus-wire-junction.cpp` that use only `GetScreen()`, `AddToScreen`,
   `RemoveFromScreen` and the selection tool, all already on the interface. Without
   it a headless delete leaves stale junctions and unmerged wires.
2. **A search-data seam** for `SCH_FIND_REPLACE_TOOL` — now implemented with
   GPUI Find/Replace; see above.
3. **`SelectBodyStyle( SCH_SYMBOL*, int, SCH_COMMIT* )`** —
   `eeschema/picksymbol.cpp:200`, uses only `GetScreen()` and the tool manager.
4. **A units provider** — `GetUserUnits()` or a `UNITS_PROVIDER*`. `CheckLibSymbol()`
   needs one to format pin coordinates, and `EE_GRAPHIC_TOOL` currently falls back
   to `EDA_UNITS::UNSCALED` for its on-canvas dimension labels.

### One behaviour that differs without a window

`SCH_DRAWING_TOOLS::DrawTable` commits the table as dragged out rather than
opening `DIALOG_TABLE_PROPERTIES` to amend it first. That is a different *outcome*
rather than lost feedback, and it is the only one — everywhere else a missing
window costs feedback or refuses the action. It is on a path the GUI does not
have: with a frame the dialog still gates the commit. Discarding the drawn table
was the alternative and is worse for a headless caller.

### A bug this found in the GUI

`SCHEMATIC_HOLDER::GetSchematic()` is documented as the route a converted tool
takes to the document, and Stage 4b's own commits use it that way. **No frame
overrode it.** `SCH_EDIT_FRAME` declares `SCHEMATIC& Schematic()` and nothing
else, so every frame answered the base class's `return nullptr`.

That is a null dereference in the wx editor, not only headless:
`sch_move_tool.cpp` does `m_editor->GetSchematic()->Hierarchy()` unguarded when a
move includes a sheet. One line on `SCH_EDIT_FRAME` fixes it. It went unnoticed
because every existing use sits on a path the QA suite does not drive — which is
the same reason the recording GAL needed `EveryRenderStateSetterIsRecorded`.

Three smaller things this stage deliberately left:

* **`UpdateHopOveredWires` is a no-op on the host.** It recomputes the arcs a wire
  draws where it crosses another — view-only presentation of an unchanged document
  — and a crossing currently draws as two lines.
* **Page-settings undo needs a window**, because `DS_PROXY_UNDO_ITEM` takes an
  `EDA_DRAW_FRAME`. An editor with no page-settings dialog cannot have recorded
  one, and the case says so rather than being silently wrong.
* **The net-collision preview during a drag needs a frame**, for the colour
  settings its markers are drawn with. A drag that would short two nets still
  drags; it is simply not warned about.

**Done when:** a user can select, move, draw a wire, undo it, and save a file that
KiCad reopens unchanged. **All five, verified in `qa_eeschema` and again over the C
ABI from Rust.**

Verified after the roster conversion, on this branch, in this container:
`qa_eeschema` 1,735 cases with the one pre-existing
`ConnectivityExport/AllegroUsesPublishedNetsAndPreservesDeviceFiles` failure --
which fails in isolation and whose exporter this branch does not touch --
`qa_common` 1,521 green, and the Rust workspace green including all 16
`kicad-sch-sys` tests against the linked host, of which two drive the tools
through the real C ABI: a click that selects the item under it, and an edit
undone, redone, saved and reloaded.


## Stage 5 — Dialogs, and who owns `main()`

**Started: GPUI Find/Replace is the first implemented dialog workflow.** It
uses owned host search data instead of a wx dialog. The shell also protects
unsaved window close and editor Quit. These do not complete the other dialogs.

**Effort remaining: a project, not a stage.**

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
day each for 1 and 2, half a day for 3. Stage 4 was estimated as "the real work",
and that turned out to be two separable halves of very different size: the
dispatcher and the ABI came in at about a day, and the frame hoist — Stage 4b, the
one this document used to call "the largest single piece of the project" — came in
at about two. All of 1, 2, 3, 4a and 4b are done.

Between them they turn a viewer of a recorded frame into something a user can edit
a schematic with, for everything that is not a dialog, with no wxFrame anywhere on
the path. The roster conversion that finished 4b cost about a day on top of that,
most of it in two tools; the other eleven were mechanical once the first four had
settled what the interface was.

Every one of them spent most of its time on something that was not in the plan,
and it was the same kind of thing every time: a claim that two layers made
differently and nothing had yet forced them to reconcile.

| Stage | The plan said | The code said |
|---|---|---|
| 1 | a recorded stream is canonical | group ordering is not, across standard libraries |
| 2 | `ksch_viewport::scale` is pixels per internal unit | `SetViewport` fed it to `VIEW::SetScale`, which wants the GAL zoom factor — 2,800× out |
| 3 | eight (later fourteen) unchecked casts are the hazard | sixteen, and the mechanism is `getEditFrame<T>()`, which the lists never mentioned |
| 4a | eeschema's tools were the ones that had to be made safe | `common/`'s crash too, in the host's constructor, before an event exists — and Stage 3 had written down that they were unaudited |
| 4a | transcribe `WXK_*` into a Rust table | don't: send the key's *name* and map it in C++, where a compiler reads the numbers |
| 4b | the interface has to be designed | it existed: `SCHEMATIC_HOLDER`, four virtuals of upstream's own, introduced for this exact purpose |
| 4b | move needs undo; wire comes after move | wire comes *inside* move: moving a wire off a junction has to add one where it left |
| 4b | the frame's defaults are the frame's business | the host inherits mixin defaults the GUI silently overwrites from settings — a zero grid that `GRID_HELPER` divides by, and a drag action that made every drag a rubber band |

None was hard once seen and none was visible until something depended on it.
Stages 3 and 4a are two halves of one lesson — **a list of what you did not check
is not smaller than the list of what you did** — and 4b adds its sibling:

**A default that is never exercised is a default nobody has checked.** Four of 4b's
findings are the same shape. `KIGFX::GAL`'s grid size, `TOOLS_HOLDER`'s drag action,
warp-on-move and immediate actions, and `EDA_DRAW_FRAME`'s shadowed undo limit are
all values a constructor sets and the GUI replaces from settings before anything
reads them. The host is the first thing to read them as they were left, and three of
the four were wrong. The fourth — the undo limit — had been wrong *in the GUI* for
however long the shadowing declaration has been there, and only surfaced because
this stage moved the member it shadowed.

Stage 5 is a separate project, and it is now the thing standing between this and an
editor someone would choose: 124 dialog sources, and most of the 61 frame methods
`SCH_EDITOR_CONTROL` wants are a dialog each.

The branch now provides rendering, host input and the converted eeschema editing
tools, plus registry-backed command presentation. Dialogs, shared-tool frame
dependencies and the remaining model seams still limit it. Rotate and delete
and Find/Replace are available; symbol placement, properties and ERC workflows
are not complete. See [the current follow-up report](08-action-registry.md#follow-up-implementation)
for the next UI work, and the interface additions listed above for host work.
