# Rewriting eeschema's UI in Rust

The schematic editor is being moved from wxWidgets/OpenGL presentation to
GPUI/gpui-kit, while retaining KiCad's C++ model, file I/O, connectivity, undo,
ERC and simulation services.

**Next: [Stage 7 — Real document sidebars](10-remaining-stages.md#stage-7--real-document-sidebars).**
Stage 6 editing correctness is [implemented and verified](11-stage6-editing-correctness.md).
The authoritative plan for all remaining schematic-port work is
[10-remaining-stages.md](10-remaining-stages.md). Each stage has a scope,
dependencies and completion criteria, so it can be requested and implemented
individually. Document lifecycle intentionally follows correctness and sidebars.

## Current status

Stages 1–4b established live rendering, the C ABI, input dispatch, the scoped
schematic tool conversion and registry-backed command presentation. Stage 5
implemented GPUI property, document, library, simulation, setup and ERC workflows,
plus native macOS menus and independent resizable workflow windows. The port can
load real schematics, edit through native tools, undo/redo and save native files.

This is not yet full KiCad parity. Shared tools and model seams remain, normal
sidebars still show draw-stream diagnostics, bitmap rendering is missing, document
lifecycle is incomplete, and several advanced workflows cover only part of the
native editor's options. The executable still links wx runtime/GUI dependencies.
See [09-dialog-workflows.md](09-dialog-workflows.md) for delivered behavior and
[the gap ownership table](10-remaining-stages.md#known-gap-ownership) for follow-up.

Stage 6 verification includes 1,776 native tests (89 host/ABI cases), 136 GPUI/input
baseline tests plus the new failed-Apply interaction test, and Computer Use. Linked live-session tests have one known draw-stream
golden mismatch; this is owned by Stage 9. Counts are a dated verification record,
not a substitute for running checks on subsequent changes.

## Implementation sequence

Stage 6 is complete; Stages 7–19 are planned. Their full acceptance criteria live in the roadmap.

| Stage | Work |
| --- | --- |
| 6 | Editing correctness and transactional model operations |
| 7 | Real hierarchy, properties and document sidebars |
| 8 | Shared tools and accurate command availability |
| 9 | Rendering and interactive visual parity |
| 10 | Document/project lifecycle, recovery and shutdown |
| 11 | Complete schematic tables, import, export and printing |
| 12 | Symbol editing and design blocks |
| 13 | ERC and simulation completeness |
| 14 | PCB and KiCad suite integration |
| 15 | Preferences, library sources and packages |
| 16 | Platform behavior, accessibility and UI consistency |
| 17 | Responsiveness, performance and reliability |
| 18 | Remove wx GUI/OpenGL presentation from the schematic host build |
| 19 | Packaging, CI and release qualification |

Testing and live UI verification accompany every stage. The original **M1–M6**
architecture milestones in `01-plan.md` are distinct from these **Stage** numbers.
Other KiCad editors and rewriting the C++ model are separate projects, not implied
by completion of this schematic port.

## Documentation map

| Document | Purpose |
| --- | --- |
| [10-remaining-stages.md](10-remaining-stages.md) | **Current ordered implementation plan and stage completion criteria** |
| [11-stage6-editing-correctness.md](11-stage6-editing-correctness.md) | Stage 6 changes, acceptance matrix, action inventory and verification |
| [09-dialog-workflows.md](09-dialog-workflows.md) | Delivered Stage 5 workflows, limits and verification |
| [01-plan.md](01-plan.md) | Architecture, original milestones and presentation/model boundary |
| [00-architecture-survey.md](00-architecture-survey.md) | Source survey, GAL/tool coupling, rendering rules and fixtures; historical findings need revalidation |
| [02-gpui-kit-cookbook.md](02-gpui-kit-cookbook.md) | GPUI APIs, widgets, rendering, input and testing |
| [03-build-notes.md](03-build-notes.md) | C++ configuration, dependencies and platform build notes |
| [04-host-seam.md](04-host-seam.md) | Host/ABI design and implementation history |
| [05-porting-guide.md](05-porting-guide.md) | Reuse and lessons for a future, separately planned PCB editor port |
| [06-what-is-missing.md](06-what-is-missing.md) | Current scope summary and historical Stages 1–5 implementation record |
| [07-ui-bugfix-pass.md](07-ui-bugfix-pass.md) | Earlier live input/repaint verification |
| [08-action-registry.md](08-action-registry.md) | Registry milestone, Find/Replace and historical golden mismatch evidence |

For workspace layout and commands, see [../../rust/README.md](../../rust/README.md).
The authoritative recording format is
[../../include/gal/recording/draw_stream_abi.h](../../include/gal/recording/draw_stream_abi.h).

## Running the current editor

After building the linked application:

```sh
eeschema-gpui --light --schematic demos/ecc83/ecc83-pp_v2.kicad_sch
```

The host records `SCH_PAINTER` geometry; Rust renders it and forwards editing input
through `TOOL_MANAGER`. The C++ painter remains the authority for drawing rules,
but the Rust renderer must still implement every required operation correctly;
sharing drawing rules does not itself prove visual parity.
