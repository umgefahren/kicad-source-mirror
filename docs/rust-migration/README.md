# Rewriting eeschema's UI in Rust

This directory documents the first step of moving KiCad's schematic editor off
wxWidgets and OpenGL, onto [gpui-kit](https://crates.io/crates/gpui-kit) and
wgpu — while leaving the C++ document model, file I/O, connectivity engine, ERC
and tools exactly where they are.

Start with **`01-plan.md`**. Everything else is supporting detail.

| Document | What it is | Who it is for |
|---|---|---|
| `01-plan.md` | The architecture, where the cut goes, how rendering works, milestones, testing strategy, and the explicit non-goals | Read this first |
| `00-architecture-survey.md` | Reconnaissance of eeschema: layer map and wx coupling, the complete `KIGFX::GAL` interface, the tool dispatcher's wx surface, the host seam, the action registry, rendering rules, test fixtures. Heavy on `file.cpp:line` references | Anyone implementing against the C++ side |
| `02-gpui-kit-cookbook.md` | How to build against gpui-kit 0.6.1, written from its sources with every non-trivial claim compiled: the real trait signatures, what is and is not possible for custom GPU rendering, the widget inventory, input, text, frame pacing, testing, Linux specifics | Anyone writing gpui code |
| `03-build-notes.md` | Configuring and building the C++ tree, with the exact dependency list and timings | Anyone building |
| `04-host-seam.md` | The C++ host that owns a schematic session without a `wxFrame`, the C ABI, and what feeding `TOOL_MANAGER` from Rust would still take | Anyone continuing the migration |

Two more places hold the parts that are code rather than prose:

* `include/gal/recording/draw_stream_abi.h` — the C ABI shared by the recording
  GAL backend and the Rust renderer. It is thoroughly commented and is the
  authoritative description of the draw stream.
* `rust/README.md` — the Rust workspace: crate layout, toolchain requirement,
  and how to build, test and screenshot it.

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
In particular, **input is not yet wired into `TOOL_MANAGER`**:
`TOOLS_HOLDER::GetToolCanvas()` returns a `wxWindow*` and is a genuine blocker
that needs real refactoring rather than a shim. `04-host-seam.md` records what
that would take. What is not done is stated in each document rather than left
for a reader to discover.
