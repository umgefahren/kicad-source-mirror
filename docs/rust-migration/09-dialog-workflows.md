# Stage 5 dialog workflows

The schematic host uses GPUI and gpui-kit panels for the workflows below.
Rust owns the inputs, choices, checkboxes, scrolling, previews and reports. The
C++ schematic, library and settings models remain authoritative: Rust receives
copied values through the C ABI, never pointers to document items. Apply calls
validate input and use native commits, exporters or settings persistence.
These entry points do not open their former wx dialogs or use a generic dialog
bridge. wx utility/runtime code is still linked into the process.

On macOS, application menus use GPUI's native menu-bar API. The editor no longer
draws File/Edit/View/Place menus inside its content area. The application menu
includes preferences, Services, Hide and Quit; native editing commands target the
focused GPUI input. Other platforms retain the in-window menu bar.

ERC, simulation, properties, choosers, Find/Replace and document/settings workflows
open in independent, movable and resizable GPUI windows. ERC and simulation can
remain open together while the schematic stays visible. Closing a dialog or
pressing Command-W closes that window; closing the owning schematic also closes
its workflow windows. Dialog bodies scroll within the available window height.

## Document and setup workflows

The shared document panel has typed controls for the following native services.
Numbers and choices have meaningful labels; hidden identities detect stale
symbol, field, bus and sheet snapshots where the operation depends on them.

| Workflow | Implemented behavior |
| --- | --- |
| Annotate / increment annotations | Sheet, selection or whole-document scope; ordering, numbering algorithm, reset and increment; native reference validation and undo. |
| Page settings / page number | Paper, orientation, title block, comments and sheet number; current/all-sheet scope; native page snapshot undo and redo. |
| Netlist | KiCad, XML, SPICE, CADSTAR, OrCAD, PADS and Allegro native exporters. |
| Plot / print | PDF, SVG, DXF, PostScript, HPGL and PNG plot exports; sheet scope, monochrome and drawing-sheet options. Print exports PDF. |
| BOM | Standard/manufacturing/custom/saved presets, columns, grouping, quantity, DNP/exclusion filters and named preset persistence. |
| Symbol fields table | Edit existing fields across the schematic with a stable symbol/field snapshot and a single undo step. |
| Change / update / remap / rescue symbols | Library resolution before mutation, flattened derived definitions, unit compatibility and undo; rescue displays cached/library conflicts. |
| Library IDs | Edit symbol library identifiers through native replacement validation. |
| Bus aliases | Add/update/delete project aliases, validate references and cycles, persist and recalculate connectivity. |
| Netclasses | Add/update/delete classes, dimensions and net-pattern assignments, project persistence and validation. |
| Net chains | Rename/delete/configure chains and create manual chains with member nets and resolved terminal references/pins. |
| Global text and graphics | Scope and target filters; text size/bold/italic and line width with undo. |
| Import settings | Import schematic formatting/parity settings, ERC severities/pin matrix, netclasses and bus aliases from a project; validate before applying and restore values on save failure. |
| Bus migration | Inspect conflicting vector labels on physical bus groups, choose replacement ranges and commit one undo step. |
| Update from PCB | Import a saved `.kicad_pcb` or native PCB netlist, preview the native backannotation report, then apply selected reference/value/footprint/net/attribute/field updates with undo. |
| Field name case conflicts | Inspect conflicting names/values, keep first or last, or join values; validate snapshot and commit one undo step. |
| Import schematic | Import an external `.kicad_sch` as a hierarchical sheet, or append a flat sheet with offsets, annotation retention and optional grouping; detached loading, validation and one undo step. |
| Data sources | List installed PCM data sources and install/replace local ZIP archives or uninstall a selected package. Replacement preserves old package files on extraction failure. |

Schematic Setup exposes native formatting/defaults, parity checks, ERC severity
choices and the pin compatibility matrix. The importer uses the same typed
schema. Separate GPUI editors cover bus aliases, netclasses and net chains.

## Properties, libraries and simulation

The symbol/power chooser browses configured libraries or cached definitions,
filters power symbols, renders native symbol previews, supports unit/body choices,
and resumes native placement.
Each host session owns its library manager; `${KIPRJMOD}` follows that session's
project rather than another GUI window's project.

Item Properties covers symbol fields, custom-field creation/removal, references,
geometry/style/flags, labels and their fields, text/text boxes, wires, buses,
shapes, junctions, no-connects, sheets, sheet pins, library pins and rule areas.
The exact fields follow the selected native item type. Sheet relinking validates
hierarchy recursion; new-sheet placement and sheet-pin synchronization use native
hierarchy edits. Vector and raster import options use GPUI forms with native
decoding and transactional placement.
Properties validate the complete request before mutation, check the item UUID,
and use native undo. Variant and multi-unit rules remain in the model.

GPUI library workflows edit project/global symbol tables, database/HTTP
connections, library symbol metadata and fields, new/copied/imported symbols,
pin tables, named pin maps and footprint associations, whole-library and
related-symbol fields, parent-field updates and schematic pin-map synchronization.
Remote-provider settings and graphics import also use GPUI panels. Preferences
exposes typed editor, display, formatting, simulator and shortcut settings.

Simulation panels configure model fields, typed model parameters and pin mappings,
model-library/IBIS selections and parser reports, analyses, result vectors, numeric
formatting, user-defined signals and simulator preferences. Results include a
GPUI plot with axis, range and cursor controls. The process-global simulator is
associated with the owning host session so destroying an older session cannot
stop a newer session's simulation or expose its results.

The ERC panel runs the native engine, owns its result strings, supports marker
exclusion and navigates to the violation's sheet and coordinates. Find/Replace,
selection filters, inspection lists and hierarchy navigation remain GPUI-owned.

Host shortcuts are suppressed while a GPUI text input has focus, including
Command/Control-A, clipboard actions and undo/redo. Frameless Save is registered
only by the host control so it cannot be swallowed by a frame-only handler.

## Scope and remaining differences

These limits have follow-up owners in
[Stages 6–19](10-remaining-stages.md#known-gap-ownership). The next implementation
work is Stage 7 (real document sidebars);
[Stage 6 editing correctness is complete](11-stage6-editing-correctness.md).
The entries below describe the current baseline, not permanent exclusions.

This is coverage of the reachable schematic workflows, not a claim of exact
widget-for-widget parity with every source in the wx dialog directory.

* Schematic import keeps hierarchical source files linked; later Save can update those
  files. Appending supports flat schematics, flattens existing groups into an optional
  new group, and does not merge source project library tables or net-chain settings.
  Nested sources use hierarchical-sheet mode.
* Printing produces a PDF; there is no native operating-system printer picker.
* Raster import and placement use the native model, but the existing GPUI draw
  stream renderer still lacks bitmap rendering. This is not full image-display parity.
* PCB backannotation reads a saved board/netlist; it does not use inter-editor
  mail to fetch unsaved PCB changes or push links back to a running PCB editor.
* PCM data-source management handles local archives and installed packages. It
  does not include the wx repository catalogue/browser. It changes user package
  storage, independently of schematic undo. A failed filesystem rollback keeps
  its backup and reports the recovery path.
* The document symbol-fields table edits existing fields. Per-item Properties
  provides custom-field creation/removal; it is not a spreadsheet clone of all
  wx table interactions.
* Global text/graphics editing currently offers scope/target, text size,
  bold/italic and stroke width, rather than every wx filter and style option.
* ERC follows the headless engine path. cvpcb-dependent footprint checks and
  drawing-sheet checks require optional providers not supplied by this host.
  ERC, export and some library operations run synchronously and can pause the UI
  on large designs.
* The drawing stream sidebars are not a replacement for every library-editor
  dock or designer tool. No removal of the wx dependency is implied.

## Verification

Native regressions cover request validation, stale snapshots, undo/redo,
export output, typed setup persistence/reload, settings import, PCB preview and
apply, vector-bus migration, field-case resolution and package rollback.
GPUI interaction tests exercise dialog routing, text editing, choice controls,
Apply/Close and suppression of schematic shortcuts while inputs have focus.

Final integration verification (2026-09-20):

* 83 native `SchHost*` regression tests pass.
* 143 standalone Rust tests, including GPUI interactions and doctests, pass.
* Clippy passes with `-D warnings`; the final linked GPUI application builds.
* Linked live-session integration: 18 pass, one known draw-stream golden group-87
  mismatch remains, as documented in `08-action-registry.md`.
* Computer Use exercised annotation and undo, native symbol preview, setup
  persistence, multiline text placement and undo, netlist export, library metadata
  save, ERC exclude/restore, real ngspice waveforms and user-signal evaluation on
  disposable projects. The final executable respects an entered `.tran 10u 1m`
  despite an embedded `.tran 1u 10m`, and keyboard commands remain available after
  switching from a focused analysis input to the plot.
* The macOS window correction was verified with Computer Use: File/Edit/Inspect
  appear in the system menu bar; ERC and Simulator have their own native windows;
  Inspect → Simulator works while ERC is active; Edit → Select All targets the
  simulation command; a real transient plot renders in the separate simulator;
  Command-W closes the simulator while keeping its schematic open. Automated
  tests also cover sibling-window survival, retained drafts on reopening, native
  close callbacks and owner-window teardown.

Computer Use also exposed and verified fixes for text-input Command-A reaching
the schematic, duplicate frameless Save handlers and off-screen ERC exclusion
buttons. These representative checks do not assert manual coverage of every form.
