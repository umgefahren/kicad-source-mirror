# Rewriting eeschema's UI in Rust

This directory documents the first step of moving KiCad's schematic editor off
wxWidgets and OpenGL, onto [gpui-kit](https://crates.io/crates/gpui-kit) and
wgpu — while leaving the C++ document model, file I/O, connectivity engine, ERC
and tools exactly where they are.

**If you are evaluating what this actually delivers, read
[`06-what-is-missing.md`](06-what-is-missing.md) first.** What exists is a
schematic *viewer*: it opens real `.kicad_sch` files through the C++ host and
draws what `SCH_PAINTER` records for them, and everything a user does to the
window is collected and discarded. It is not a schematic editor, and wxWidgets
has not been removed from anything. That document says exactly where it stops and
what the remaining stages are.

For the design, start with **`01-plan.md`**.

| Document | What it is | Who it is for |
|---|---|---|
| `01-plan.md` | The architecture, where the cut goes, how rendering works, milestones, testing strategy, and the explicit non-goals | Read this first |
| `00-architecture-survey.md` | Reconnaissance of eeschema: layer map and wx coupling, the complete `KIGFX::GAL` interface, the tool dispatcher's wx surface, the host seam, the action registry, rendering rules, test fixtures. Heavy on `file.cpp:line` references | Anyone implementing against the C++ side |
| `02-gpui-kit-cookbook.md` | How to build against gpui-kit 0.6.1, written from its sources with every non-trivial claim compiled: the real trait signatures, what is and is not possible for custom GPU rendering, the widget inventory, input, text, frame pacing, testing, Linux specifics | Anyone writing gpui code |
| `03-build-notes.md` | Configuring and building the C++ tree, with the exact dependency list and timings | Anyone building |
| `04-host-seam.md` | The C++ host that owns a schematic session without a `wxFrame`, the C ABI and the shared library Rust links, and what feeding `TOOL_MANAGER` from Rust would still take | Anyone continuing the migration |
| `05-porting-guide.md` | **How to do this again for pcbnew.** What is reusable unchanged, what is genuinely different about a board editor, and the traps — including the two designs we got wrong and had to redo | Read before starting the next editor |
| `06-what-is-missing.md` | **What this is not, and what an editor still needs.** What the C++ bridge does and does not yet carry, stage by stage, with Stage 1 done | Read first if you are judging scope |

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

This step delivers the rendering and presentation seam, not a finished editor.
Concretely: the Rust application opens a `.kicad_sch` and draws it, and then
every pointer event, key press and tool activation goes to a null sink.
`06-what-is-missing.md` covers this properly, stage by stage.

**Input is not yet wired into `TOOL_MANAGER`** — that is the next milestone of
substance, and `04-host-seam.md` records what it would take. Its prerequisite is
not the one it appears to be: `GetToolCanvas()` is largely a red herring, while
eight unchecked `static_cast<SCH_EDIT_FRAME*>` of the tool holder are undefined
behaviour for any non-frame host. See `05-porting-guide.md` §4.8. What is not done
is stated in each document rather than left for a reader to discover.

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
* `kicad-gal` is 56 tests green, `kicad-sch-render` 89, `kicad-sch-ui` 70, and the
  Rust shell renders those streams in a real gpui window.
* **The C ABI is linked and driven from Rust.** `kicad-sch-sys` opens a real
  schematic, and the frame it gets back is byte-identical to what
  `kicad-sch-dump` writes for the same file on the same machine. Ten checks,
  registered with CTest as `qa_rust_sch_sys`.
* The eeschema and common QA suites both pass in full. The single pre-existing
  `ConnectivityExport/AllegroUsesPublishedNetsAndPreservesDeviceFiles` failure
  recorded earlier on this branch no longer reproduces; a flaky hang in
  `HttpLibPlugin` did, and is fixed (`03-build-notes.md` §6).
