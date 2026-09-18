# What is missing, and what it takes to get an editor

This document exists because the previous ones describe what was built, and a
reader can finish them with the wrong impression of what that adds up to.

**What exists today is a schematic viewer that opens real `.kicad_sch` files,
plus a seam an editor can be built on. It is not a schematic editor, and
wxWidgets has not been removed from anything.** The wx schematic editor is
untouched and is still the only way to edit a schematic.

> **Stage 1 below is done** (see [Stage 1](#stage-1--link-the-c-abi-from-rust-done)).
> `kicad-eeschema-gpui --schematic FILE.kicad_sch` loads the file through
> eeschema's own reader and draws the frame the C++ painter records, with no file
> in between. That closes the first of the two missing arrows; everything else in
> this document still stands, including the one that matters most.

## Exactly where it stops

Two facts, each checkable in a minute:

1. **Input is discarded.** The shell builds a complete event vocabulary —
   `PointerDown`, `PointerMove`, `PointerUp`, `DragBegin`, `DragUpdate`,
   `DragEnd`, `Scroll`, `KeyDown`, `KeyUp`, `ToolCancelled`, with buttons and
   modifiers — and the binary hands it `NullSink`, whose entire implementation
   is `fn handle(&mut self, _event: ShellEvent) {}`. Nothing a user does reaches
   the document.
2. **Nothing re-renders.** The session records one frame at startup, the Rust
   side copies it, and the session is dropped. Panning and zooming move a camera
   over that copy — which is right for a viewer and is not an editor: a document
   that changed would still be showing its old geometry. That is Stage 2.

So the pipeline that works now is:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► stream in memory
                                                   │
                                             C ABI │  (kicad-sch-sys)
                                                   ▼
                                             gpui window            (works, once)
```

and the pipeline an editor needs is:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► stream in memory
                   ▲                               │
                   │                          C ABI │  ◄── every frame, not once
            TOOL_MANAGER                            ▼
                   ▲                          gpui window
                   └──────── input ───────────────┘  ◄── goes to NullSink
```

One arrow is missing and one is drawn once instead of continuously. Neither is
speculative work — both ends of each already exist and are tested.

## What is already done and does not need redoing

Worth being clear about, because it changes the size of what remains:

| | |
|---|---|
| `RECORDING_GAL` + `DRAW_STREAM` | Complete. 30 tests in `qa_common` |
| The draw-stream ABI | Frozen, layout-asserted on both sides, sync-tested |
| `SCH_HOST` | Loads, renders, enumerates sheets, zooms; 16 tests |
| The C ABI | 26 entry points, three of them the runtime; implemented, and bound from Rust |
| `kicad-gal` | Validating decoder, 56 tests |
| `kicad-sch-render` | Stream → gpui primitives, 89 tests |
| `kicad-sch-ui` | Shell, 70 tests, the interaction ones against real hit testing |
| Action registry | 440 actions enumerable headless with icons and hotkeys |
| `kicad-sch-sys` | The ABI linked from Rust, 10 checks against the live host |

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
* The session is dropped after the first frame and the Rust side keeps a copy, so
  nothing re-renders from the document. Stage 2, which is where the borrowed
  `StreamView` the wrapper already returns starts being used per frame.
* The hierarchy panel still describes the draw stream rather than the sheet tree,
  and says so, even though `ksch_session_sheet_info` could fill it in today.

## Stage 2 — Live re-render

**Effort: about a day. Depends on Stage 1.**

`ksch_session_render` already returns a borrowed `kgds_stream_view` valid until
the next recording pass. Call it per frame instead of reading a file, drive the
viewport through `ksch_session_set_viewport`, and let `SchematicRenderer`'s
`(group id, serial)` cache do its job.

Two things to get right:

- **Lifetimes.** The view borrows C++-owned buffers that the next `Render()`
  invalidates. The safe wrapper must make that unrepresentable, not merely
  documented. *Done in Stage 1:* `Session::render` returns a `StreamView`
  borrowed from `&mut self`, so holding one and rendering again does not compile.
  `SchematicRenderer::set_stream_view` already takes exactly that, so the copy
  Stage 1 makes is one call away from being gone.
- **Who owns the frame clock.** Today gpui drives it. Once C++ owns the
  document, a change there has to wake the gpui loop.
- **Who holds the session.** Stage 1 drops it after the first frame; keeping it
  for the window's lifetime means the shell owns it, and the shell is
  `!Send`-friendly but the session is main-thread-only (above), which the gpui
  main thread satisfies.

**Done when:** panning and zooming re-render from the live document, and the
group cache still reports no re-upload on an unchanged view.

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

Stages 1–3 are mechanical and bounded: roughly two to three days, and they turn
a viewer into something that opens a real schematic and redraws it live. Stage 1
is done and cost about a day of that, most of it on the two surprises in its own
section rather than on the bindings.

Stage 4 is where a schematic editor actually lives. Selection alone is a
meaningful milestone; a tool set someone would choose over the wx editor is
substantially more.

Stage 5 is a separate project.

**The honest summary of this branch is that it finishes the rendering third of
the problem and leaves the editing two thirds.** That is a real result — the
rendering third was the part with the most unknowns, and it is now settled and
tested on every schematic in the tree, and as of Stage 1 it is settled
*end to end in one process* rather than through a file — but it is a third.
