# Rewriting eeschema's UI in Rust

This directory documents the first step of moving KiCad's schematic editor off
wxWidgets and OpenGL, onto [gpui-kit](https://crates.io/crates/gpui-kit) and
wgpu — while leaving the C++ document model, file I/O, connectivity engine, ERC
and tools exactly where they are.

**If you are evaluating what this actually delivers, read
[`06-what-is-missing.md`](06-what-is-missing.md) first.** What exists is a schematic
editor with a working canvas and the converted eeschema tools: it opens real
`.kicad_sch` files through the C++ host, redraws from the live document, and a user
can select items by clicking or dragging a box, move, rotate, mirror and delete
them, draw wires, place junctions, no-connects, labels and sheet pins, cut and
paste, undo and redo any of it, and save a file that KiCad reopens — all with no
`wxFrame` anywhere on the path. Every tool class in `eeschema/tools/` now runs in
such a context bar one, and `common/`'s shared tools are the remaining hold-out.

Stage 4's input dispatcher and scoped eeschema frame hoist are complete, as is
M4's registry-backed command presentation. This does not mean complete KiCad
feature parity: shared tools and model seams still have documented gaps.
Stage 5 has started with GPUI Find/Replace backed by host search data and undo.
Window close and editor Quit protect unsaved changes; external OS quit cannot
be vetoed through the current GPUI API. Symbol placement, properties and ERC
still need dialog workflows, and the wx editor remains the complete editor.

For the design, start with **`01-plan.md`**. For the latest registry milestone,
hands-on UI checks and next steps, see [`08-action-registry.md`](08-action-registry.md).
The earlier input and repaint fixes are in [`07-ui-bugfix-pass.md`](07-ui-bugfix-pass.md).

| Document | What it is | Who it is for |
|---|---|---|
| `01-plan.md` | The architecture, where the cut goes, how rendering works, milestones, testing strategy, and the explicit non-goals | Read this first |
| `00-architecture-survey.md` | Reconnaissance of eeschema: layer map and wx coupling, the complete `KIGFX::GAL` interface, the tool dispatcher's wx surface, the host seam, the action registry, rendering rules, test fixtures. Heavy on `file.cpp:line` references | Anyone implementing against the C++ side |
| `02-gpui-kit-cookbook.md` | How to build against gpui-kit 0.6.1, written from its sources with every non-trivial claim compiled: the real trait signatures, what is and is not possible for custom GPU rendering, the widget inventory, input, text, frame pacing, testing, Linux specifics | Anyone writing gpui code |
| `03-build-notes.md` | Configuring and building the C++ tree, with the exact dependency list and timings | Anyone building |
| `04-host-seam.md` | The C++ host that owns a schematic session without a `wxFrame`, the C ABI and the shared library Rust links, and what feeding `TOOL_MANAGER` from Rust would still take | Anyone continuing the migration |
| `05-porting-guide.md` | **How to do this again for pcbnew.** What is reusable unchanged, what is genuinely different about a board editor, and the traps — including the two designs we got wrong and had to redo | Read before starting the next editor |
| `06-what-is-missing.md` | **What this is not, and what an editor still needs.** What the C++ bridge does and does not yet carry, stage by stage, with Stages 1–4b done and the remaining cost measured per tool | Read first if you are judging scope |
| `07-ui-bugfix-pass.md` | Live verification of input, repainting and editing fixes | UI testing |
| `08-action-registry.md` | Registry-backed menus/toolbars, live verification, shortcut fixes, close protection, Find/Replace and remaining limitations | Current milestone and follow-up |

Two more places hold the parts that are code rather than prose:

* `include/gal/recording/draw_stream_abi.h` — the C ABI shared by the recording
  GAL backend and the Rust renderer. It is thoroughly commented and is the
  authoritative description of the draw stream.
* `rust/README.md` — the Rust workspace: crate layout, toolchain requirement,
  and how to build, test and screenshot it.

## What it looks like

There is no screenshot checked in: the tree's `.gitignore` ignores `*.png`
wholesale, so the one this section used to link was never committed and the link
was broken. `docs/rust-migration/images/` is now un-ignored, so a future one will
be taken. Meanwhile, one command produces it:

```sh
eeschema-gpui --light --schematic demos/ecc83/ecc83-pp_v2.kicad_sch
```

The ECC83 valve amplifier from `demos/`, drawn by gpui from the stream
`RECORDING_GAL` records for it. The geometry, colours and typography come from
`SCH_PAINTER` unchanged; only the rasterisation is new.

The side panels show what the shell can actually derive — the stream's retained
group count, command counts and extent — and say plainly that per-item
properties need the C++ document model it is not yet connected to. They are
labelled that way on purpose: a panel showing plausible placeholder data beside
real data is worse than one admitting what it does not have.

Clicking an item selects it, dragging one moves it, `W` draws a wire, `R` rotates,
`Del` deletes, `J` places a junction, `L` places a label, ⌘X/⌘V cut and paste, ⌘Z
undoes and ⌘S saves. What does nothing is every menu item that opens a dialog, and
`06-what-is-missing.md` says which of those is waiting on what.

## The shape of it, in one paragraph

`SCH_PAINTER` already holds every rule about how a schematic looks, and
expresses all of it through the abstract `KIGFX::GAL` interface. So instead of
porting those rules, we added a third GAL backend — alongside OpenGL and Cairo —
that records the calls into a flat, device-independent draw stream rather than
rasterising them. `KIGFX::VIEW` and `SCH_PAINTER` are untouched. Rust decodes
the stream and draws it through gpui, whose renderer is wgpu. Appearance cannot
drift, because there is still exactly one implementation of the drawing rules
and it is the C++ one.

## Honest status

This step delivers the rendering and presentation seam, the input path over it, and
the editing loop for the converted eeschema tool roster. Concretely: the Rust application
opens a `.kicad_sch`, draws it, re-records it from the live document whenever the
view moves *or a tool changes something*, and a user can select, move, rotate,
mirror, delete, draw wires, place junctions and labels, cut and paste, undo, redo
and save.

**Most dialog workflows remain missing.** `SCH_DESIGN_BLOCK_CONTROL` is the one tool
class that still declines a non-frame holder, and `common/`'s shared tools —
`COMMON_TOOLS`, `ZOOM_TOOL`, `PICKER_TOOL`, `GROUP_TOOL`, `EMBED_TOOL`,
`COMMON_CONTROL` — need a `TOOLS_HOLDER`-level hoist that is pcbnew's and
gerbview's decision as much as eeschema's. Beyond that the tools all *run*, but the
ones whose work is a dialog decline the action: no symbol placement, no properties,
and no ERC workflow. Find/Replace now uses GPUI controls and the host search-data
interface; the wx dialog implementations remain in place for the wx editor.
`06-what-is-missing.md` Stage 4b says which tool does what, and which host interface
additions still remain.

Four predictions this branch falsified rather than confirmed, because they are the
useful part:

* `GetToolCanvas()` was never the blocker — it is still pure virtual and `SCH_HOST`
  implements it in one line.
* "Transcribe `WXK_*` into a Rust table" is the wrong way to reconcile the key
  vocabularies. The key crosses the ABI as a *name* and C++ resolves it, so the
  numbers come from `wx/defs.h` through a compiler. `grep -rn "WXK_" rust/crates/`
  finds nothing but comments saying so.
* The interface the tools need did not have to be designed. `SCHEMATIC_HOLDER`
  already existed, four virtuals of upstream's own, introduced with the stated goal
  of making "the relationship between frame and schematic less intertwined".
* "~600 `m_frame->` call sites" is the wrong unit for the work. Most of them are
  `GetScreen()`, `AddToScreen()` and `UpdateItem()` repeated; the interface is about
  twenty methods, and the conversions are mechanical.

What *is* verified, on this branch:

* The C++ tree configures and builds. On four cores in a container: `kicommon`
  15m, `common` 13m, `eeschema_kiface` 63m. It also builds on macOS with the nix
  toolchain of `devenv.nix`, which took four platform fixes — see
  `03-build-notes.md` §6.
* `kicad-sch-dump` renders **all 466 `.kicad_sch` files in the tree** through
  `SCHEMATIC` → `VIEW` → `SCH_PAINTER` → `RECORDING_GAL` into a draw stream —
  zero failures, zero crashes — with retained geometry reused across repeated
  frames rather than regrown.
* The recording backend's 30 tests pass inside KiCad's own `qa_common`.
* `kicad-gal` is 58 tests green, `kicad-sch-render` 89, `kicad-sch-ui` 82,
  `kicad-eeschema-gpui` 16, and the Rust shell renders those streams in a real gpui
  window.
* **The C ABI is linked and driven from Rust.** `kicad-sch-sys` opens a real
  schematic, and the frame it gets back paints the same picture as what
  `kicad-sch-dump` writes for the same file. Sixteen checks, registered with CTest
  as `qa_rust_sch_sys`.
* **The session is held open and re-recorded live.** A pan or a zoom asks the C++
  session for the frame the canvas is about to paint, and the retained geometry
  comes back unchanged, so nothing is re-tessellated. 4.6 ms on the densest sheet
  in the tree, of which 3.7 ms is the C++ recording pass —
  `06-what-is-missing.md` Stage 2 has the table. That work also uncovered a unit
  bug in `SCH_HOST`'s viewport that had made the C++ cull rectangle 52 metres
  wide; fixing it changed no recorded output.
* **A `TOOL_MANAGER` whose holder is not a `wxFrame` is defined behaviour.** All
  sixteen unchecked downcasts of the holder in eeschema are checked, and the tools
  that need a frame decline to initialise rather than trusting a pointer that is
  not one — which `TOOL_MANAGER` already handles by dropping them. Four tests in
  `qa_eeschema` install such a holder; restoring the old cast in `sch_commit.cpp`
  makes one of them segfault. `06-what-is-missing.md` Stage 3 has what the cast
  list missed — and Stage 4a has what *that* list missed, which is that seven tool
  classes in `common/` had the same hazard and crashed rather than declining.
* **Input reaches `TOOL_MANAGER`, end to end.** `HOST_VIEW_CONTROLS` and
  `HOST_TOOL_DISPATCHER` (both in `common/`, both reusable by pcbnew) turn host
  input into the ten `TOOL_EVENT` shapes the wx dispatcher produces, without any of
  its wx-quirk reconciliation; ABI version 3 carries them; and the gpui binary's
  `HostInputSink` replaces the null sink. 32 tests in `qa_common`, 13 more in
  `qa_eeschema`, 10 in the binary, 3 more in the shell, 3 live checks against the
  linked host.
* **Four tools run on a holder that is not a frame, and a user can edit with them.**
  `SCH_SELECTION_TOOL`, `SCH_MOVE_TOOL`, `SCH_LINE_WIRE_BUS_TOOL` and
  `SCH_HOST_CONTROL` initialise and work, because what they ask the holder for is a
  `SCHEMATIC_HOLDER` — the document, the settings, the canvas notifications — rather
  than a window. 13 tests in `qa_eeschema` drive a click, a drag box, a move, a wire
  and the undo hotkey through the real input path.
* **Undo works, and it is the first thing that ever tested eeschema's undo.**
  `SaveCopyInUndoList` and `PutDataInPreviousState` were `SCH_EDIT_FRAME` members
  and nothing in the QA suite called them; they are now `SCH_UNDO_REDO` free
  functions that the frame runs too. The sharpest check is over the ABI: save an
  untouched copy, drag a wire, save and confirm the bytes differ, undo and save and
  confirm the bytes are **identical to the baseline**.
* **A saved file reopens.** `ksch_session_save` writes the `.kicad_sch` files through
  the same writer the editor uses, and a second session loads and renders the result.
* The eeschema and common QA suites both pass in full — 1,735 and 1,513 cases — as
  do `qa_pcbnew` and the other programs' kifaces, which the base-class changes
  (`UNDO_REDO_HOLDER`, `TOOL_INTERACTIVE::HasToolMenu`) also touch.
