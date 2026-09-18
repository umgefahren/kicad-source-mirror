# What is missing, and what it takes to get an editor

This document exists because the previous ones describe what was built, and a
reader can finish them with the wrong impression of what that adds up to.

**What exists today is a schematic viewer for pre-recorded files, plus a seam
that an editor can be built on. It is not a schematic editor, and wxWidgets has
not been removed from anything.** The wx schematic editor is untouched and is
still the only way to edit a schematic.

## Exactly where it stops

Three facts, each checkable in a minute:

1. **The Rust application cannot open a `.kicad_sch`.** Its only input is
   `--stream <file.kgds>`, a draw stream recorded earlier by the C++
   `kicad-sch-dump` tool. There is no schematic-file handling in the Rust tree.
2. **Input is discarded.** The shell builds a complete event vocabulary —
   `PointerDown`, `PointerMove`, `PointerUp`, `DragBegin`, `DragUpdate`,
   `DragEnd`, `Scroll`, `KeyDown`, `KeyUp`, `ToolCancelled`, with buttons and
   modifiers — and the binary hands it `NullSink`, whose entire implementation
   is `fn handle(&mut self, _event: ShellEvent) {}`.
3. **No Rust code calls the C ABI.** `include/sch_host/sch_host_abi.h` declares
   seventeen `ksch_*` functions, they are implemented, and they are tested —
   *from C++*. Nothing in `rust/` links against them. The bridge was built from
   both ends and never joined in the middle.

So the pipeline that works is:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► file.kgds     (C++, works)
                                                  │
                                            (a file on disk)
                                                  │
               file.kgds ──► kicad-gal ──► gpui window      (Rust, works)
```

and the pipeline an editor needs is:

```
.kicad_sch ──► SCH_HOST ──► RECORDING_GAL ──► stream in memory
                   ▲                               │
                   │                          C ABI │  ◄── not linked
            TOOL_MANAGER                            ▼
                   ▲                          gpui window
                   └──────── input ───────────────┘  ◄── goes to NullSink
```

Two arrows are missing. Neither is speculative work — both ends of each already
exist and are tested.

## What is already done and does not need redoing

Worth being clear about, because it changes the size of what remains:

| | |
|---|---|
| `RECORDING_GAL` + `DRAW_STREAM` | Complete. 30 tests in `qa_common` |
| The draw-stream ABI | Frozen, layout-asserted on both sides, sync-tested |
| `SCH_HOST` | Loads, renders, enumerates sheets, zooms; 16 tests |
| The C ABI | 17 functions, implemented, `bindgen`-verified |
| `kicad-gal` | Validating decoder, 56 tests |
| `kicad-sch-render` | Stream → gpui primitives, 89 tests |
| `kicad-sch-ui` | Shell, 73 interaction tests against real hit testing |
| Action registry | 440 actions enumerable headless with icons and hotkeys |

The rendering half is genuinely finished, on all 466 schematics in the tree.

---

## Stage 1 — Link the C ABI from Rust

**Effort: about a day. Blocks everything else.**

Add a `kicad-sch-sys` crate: `bindgen` over `include/sch_host/sch_host_abi.h`,
a `build.rs` that links the host library, and a thin safe wrapper. The header is
already verified `bindgen`-clean — bindgen parses it, emits 23 `extern "C"`
functions, and the generated crate compiles including its layout assertions.

The awkward part is not the bindings, it is **what to link against**. The host
lives in `eeschema_kiface_objects`, and the eeschema kiface is a 63 MB module
that pulls in all of wxWidgets. Options, in order of preference:

1. Build the host as its own shared library with a narrow export list. Cleanest,
   and the one that makes the Rust binary's dependency footprint honest.
2. Link the kiface objects directly. Fastest to get working, drags in everything.
3. Invert the ownership so C++ `main()` starts and Rust is a library it calls.
   This is probably where it ends up eventually — see Stage 5 — but it is a
   bigger change than Stage 1 should be.

**Done when:** the Rust binary takes a `.kicad_sch` path, calls
`ksch_session_load_file`, and shows the result. No file round-trip.

## Stage 2 — Live re-render

**Effort: about a day. Depends on Stage 1.**

`ksch_session_render` already returns a borrowed `kgds_stream_view` valid until
the next recording pass. Call it per frame instead of reading a file, drive the
viewport through `ksch_session_set_viewport`, and let `SchematicRenderer`'s
`(group id, serial)` cache do its job.

Two things to get right:

- **Lifetimes.** The view borrows C++-owned buffers that the next `Render()`
  invalidates. The safe wrapper must make that unrepresentable, not merely
  documented.
- **Who owns the frame clock.** Today gpui drives it. Once C++ owns the
  document, a change there has to wake the gpui loop.

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
a viewer into something that opens a real schematic and redraws it live.

Stage 4 is where a schematic editor actually lives. Selection alone is a
meaningful milestone; a tool set someone would choose over the wx editor is
substantially more.

Stage 5 is a separate project.

**The honest summary of this branch is that it finishes the rendering third of
the problem and leaves the editing two thirds.** That is a real result — the
rendering third was the part with the most unknowns, and it is now settled and
tested on every schematic in the tree — but it is a third.
