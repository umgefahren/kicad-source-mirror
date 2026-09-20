# Remaining migration stages

This is the authoritative execution order after Stage 5, updated 2026-09-20.
**Stage 6 — Editing correctness is complete**; see its
[verification report](11-stage6-editing-correctness.md). Continue with
**Stage 7 — Real document sidebars**. Document lifecycle comes later, as requested. A request such as
“implement Stage 7 in docs/rust-migration” refers to the scope and completion
criteria below. The older M1–M6 milestones in `01-plan.md` are architectural
milestones, not these implementation stage numbers.

## Scope and baseline

The target is a complete, usable GPUI schematic editor, its associated symbol
editing workflows, and integration with the existing KiCad suite. Rust owns
presentation and input; C++ remains authoritative for schematic/library data,
connectivity, validation, undo, file formats, ERC and simulation. New UI must use
GPUI/gpui-kit or a suitable native OS service, never a fallback wx dialog.
Native macOS menus and independent GPUI workflow windows are already implemented.

Stages 1–4b established the renderer/host/input seam and converted the scoped
schematic tool roster. Stage 5 implemented the dialog families described in
[09-dialog-workflows.md](09-dialog-workflows.md); that is a delivered baseline,
not a claim of full feature parity. Do not rebuild those workflows from scratch.
Reuse their typed services and close the specific gaps assigned here.

This plan covers the known remaining work and explicitly requires an inventory
of unverified actions. Historical TODOs must be checked against current code:
a missing test is not proof that a feature is missing, and registration of an
action is not proof that it works. Newly discovered gaps get an owning stage and
an acceptance case before that stage can be declared complete.

Porting pcbnew, GerbView, the project manager or the 3D renderer, replacing wxBase
utility types, and rewriting the C++ models in Rust are separate projects. They
are not hidden prerequisites of finishing this schematic presentation port.
The continuation boundary is described at the end of this document.

## Execution order and status

The status column is the completion ledger; all new stages start as **Planned**.
Use **In progress** while implementing and **Complete** only after the stage
criteria pass, with a linked verification report. Dependencies list technical
prerequisites; the numbered order is the default implementation order even where
independent investigation is possible.

| Stage | Deliverable | Depends on | Status |
| --- | --- | --- | --- |
| 6 | Editing correctness and transactional model operations | 5 | Complete ([verification](11-stage6-editing-correctness.md)) |
| 7 | Real hierarchy, selection properties and document sidebars | 6 | Planned |
| 8 | Remaining shared tools and accurate command availability | 6, 7 | Planned |
| 9 | Rendering and interactive visual parity | 6, 8 | Planned |
| 10 | Document/project lifecycle, recovery and safe shutdown | 6–9 | Planned |
| 11 | Complete schematic tables, import, export and printing | 7, 10 | Planned |
| 12 | Complete symbol editing and design-block workflows | 7, 8, 10, 11 | Planned |
| 13 | Complete ERC and simulation workflows | 7, 9, 12 | Planned |
| 14 | PCB and KiCad suite integration | 8, 10, 12, 13 | Planned |
| 15 | Preferences, libraries and package/data-source management | 10, 12, 14 | Planned |
| 16 | Platform behavior, accessibility and UI consistency | 7–15 | Planned |
| 17 | Responsiveness, performance and long-session reliability | 6–16 | Planned |
| 18 | Separate the host build from wx GUI and OpenGL presentation | 8–17 | Planned |
| 19 | Release packaging, CI and final parity qualification | 6–18 | Planned |

Testing accompanies every stage; Stage 19 is the aggregate release gate, not the
first time tests or CI are used. Existing CMake/Rust integration and CI are
extended, not replaced merely because the original M6 mentioned them.

## Rules for completing a stage

1. Check the current code and the relevant wx editor behavior; record a small
   acceptance matrix of user actions and expected results. Distinguish missing,
   partially implemented, verified and deliberately out-of-scope behavior.
2. Implement through the existing host/ABI/UI split. Preserve project isolation,
   native validation and normal undo/redo semantics. Version incompatible ABI
   changes and update exports, safe wrappers and no-host stubs together.
3. Add meaningful model/ABI regressions for correctness changes and GPUI
   interaction tests for focus, dispatch and window behavior. Test both valid and
   rejected operations, cancellation, stale data and undo where applicable.
4. Build the affected targets and use Computer Use on the actual rebuilt app with
   disposable representative schematics. Include save/reopen where the stage
   changes persistent state. Shared C++ changes also need relevant legacy-editor
   and other-consumer regressions.
5. Update this status table and the coverage report with results and remaining
   limits. A visible but inert control is not completion. A framework blocker
   stays open, with the required upstream work recorded; do not silently replace
   an acceptance requirement with a reduced feature.

## Stage 6 — Editing correctness

**Outcome:** the already exposed editing operations produce the same schematic
and connectivity as the wx editor and can be undone reliably.

Work:

- Audit the model seams listed in the Stage 4b history, starting with junction
  deletion and wire merging (`DeleteJunction`), body-style changes
  (`SelectBodyStyle`) and units-provider needs. Implement missing behavior through
  shared model services rather than a second Rust algorithm.
- Exercise delete, move, drag, rotate, mirror, repeat, duplicate and clipboard
  operations on connected wires, buses, junctions, labels, symbols and sheets.
  Check dangling endpoints, net names, bus membership and connection rebuilding.
- Validate unit/body changes, multi-unit symbols, variants, sheet instance paths,
  nested hierarchy and sheet-pin edits. Preserve native restrictions and identities.
- Make placement prompts, cancel/Escape, failed Apply, stale property snapshots
  and switching tools transactional. No orphan previews, extra undo entries or
  partially applied model edits; dirty state must match actual document changes.
- Capture the enabled schematic action inventory and its missing/partial behavior
  in a checked-in parity checklist. Assign residual presentation/tool gaps to the
  later stages below; resolve model-corruption or incorrect-connectivity cases here.

**Done when:** a regression matrix compares connectivity, item state and saved
output before/after edits against the native model, including undo/redo and
reload. Computer Use reproduces representative connected edits without a wx
frame. Historical method names alone do not substitute for behavior tests.

## Stage 7 — Real document sidebars

**Outcome:** the sidebars describe and edit the live schematic instead of its
recorded drawing commands.

Work:

- Expose owned document, hierarchy, selection and property snapshots through the
  host seam, using stable IDs and revision/invalidation notifications. Reuse the
  Stage 5 typed property schema and native validation.
- Replace the diagnostic hierarchy with real sheet instances and page/name
  information. Support expand/collapse, current-sheet indication and navigation;
  repeated sheet instances must be distinguishable.
- Connect canvas selection and the property sidebar in both directions. Handle
  no selection, a single item and mixed/multiple selection, with units, read-only
  values and useful validation feedback. Batch edits must form coherent undo steps.
- Populate selection filters, appearance controls and document/net navigation
  from host state. Keep context menus and inspectors synchronized with the canvas,
  floating property windows, sheet changes, undo/redo and deletion.
- Move group/command counts and stream extents into an explicit developer
  inspector. Recorded-stream/demo mode must clearly state that editing is unavailable.

**Done when:** selecting and editing actual items updates the sidebar and canvas
immediately; undo, deleting the selection and changing sheets cannot leave stale
editable values. A multi-sheet fixture is navigable entirely through the sidebar,
and diagnostic placeholders are absent from the normal document UI.

## Stage 8 — Shared tools and command availability

**Outcome:** tools no longer silently decline because their holder is not a wx
frame, and menus accurately represent what the current context can do.

Work:

- Audit and convert the schematic-relevant behavior of `COMMON_TOOLS`,
  `COMMON_CONTROL`, `ZOOM_TOOL`, `PICKER_TOOL`, `GROUP_TOOL` and `EMBED_TOOL`.
  Add small `TOOLS_HOLDER`-level capabilities that existing wx consumers can also
  implement. Keep PCB/Gerber regressions in scope for shared changes.
- Complete grouping/ungrouping and entered-group editing, point/object picking,
  tool-driven zoom/pan, auto-pan, snapping, grid changes and embedded-file actions.
  Wire embedded-content management to GPUI controls where required.
- Reconcile the host/view-controls camera contract: tool-driven camera changes
  and the Rust viewport must share one authority, with no oscillation or duplicate
  transforms. Preserve the verified pan/zoom behavior.
- Expose dynamic enabled/checked state and context-sensitive menus, not only
  static action metadata. Propagate native selection/tool context to menus,
  toolbar controls and the command palette; explain unavailable operations.
- Replace remaining tool-menu/message/cursor assumptions with host notifications
  and GPUI presentation. Include specialized context actions and hypertext links.

**Done when:** every schematic-relevant shared action has an acceptance test or
an explicit later-stage owner. Menus and shortcuts agree on availability, a tool
can change the view, and no enabled action silently falls into a frame-only path.

## Stage 9 — Rendering and interactive visual parity

**Outcome:** all content used by supported schematics and editing tools is drawn
correctly at every practical zoom level.

Work:

- Implement `KGDS_OP_BITMAP`, including placement transforms, rotation/mirroring,
  alpha, clipping and cache lifetime. If GPUI needs an image-transform capability,
  add or upstream it rather than ignoring the opcode or drawing at the wrong angle.
- Restore zoom-dependent item/selection refresh, wire-hop arcs, net-collision
  preview, snapping/anchor markers, tool dimensions and correct unit formatting.
- Audit layer/depth order, fills, text geometry, overlays, selection shadows,
  DNP/operating-point display, HiDPI and theme colors against native rendering.
- Resolve the known group-87 live/golden mismatch by identifying its cause.
  Retain legitimate cross-platform ordering normalization; never simply bless a
  new fixture to hide omitted geometry.
- Add reproducible rendered comparisons alongside geometry tests. Document which
  renderer opcodes are schematic requirements and which, such as PCB-specific
  negative/difference layers, belong to a future board port.

**Done when:** image-bearing, dense, hierarchical and overlay-heavy fixtures have
no unexplained missing schematic content, the linked rendering regression is
green, and pan/zoom/theme checks pass through Computer Use.

## Stage 10 — Document lifecycle and recovery

**Outcome:** users can manage real projects from the application without launching
a new process with a command-line filename.

Work:

- Implement New, Open, recent files/projects, Save As, Save All, Revert and normal
  file-drop/OS-open entry points. Use GPUI or native file pickers with correct
  filters, extension handling, overwrite confirmation and cancellation.
- Support multiple document windows and project/session ownership; define duplicate
  opens and locking. Keep `${KIPRJMOD}`, library tables, settings and dialog actions
  attached to the correct project when focus or documents change.
- Save full hierarchies safely, including renamed/copied projects and linked source
  sheets. Make copy-versus-reference behavior and external file changes explicit.
- Implement autosave, backups, crash recovery and recovery-file cleanup using
  KiCad's existing conventions. Handle read-only paths, missing files and I/O errors.
- Extend dirty-document protection to all close/quit paths, including OS termination
  requests. Close the current GPUI veto limitation with a platform/framework hook;
  forced process termination is handled by recovery, not a promise of a prompt.
- Audit repeated load/unload ownership and historical loader leak reports, including
  `EESCHEMA_HELPERS::LoadSchematic`; fix any still present. Dispose dependent windows,
  background work and session-scoped resources safely.

**Done when:** create → edit → Save As → close → reopen works from the UI; two
projects cannot affect each other's paths or data; failed saves and canceled
close/quit preserve edits; simulated crash/restart offers usable recovery.

## Stage 11 — Schematic tables, import, export and printing

**Outcome:** existing Stage 5 document forms cover the full schematic workflows
rather than only their first set of options.

Work:

- Complete the symbol-fields/BOM table experience: field/column CRUD, batch editing,
  selection, filtering, sorting, grouping, presets and clipboard import/export.
- Complete table/cell properties, table CSV export, global text/graphics filters
  and styles, page/drawing-sheet options, annotation scopes and exporter options.
- Extend schematic import beyond the current flat append/project-setting limits:
  nested hierarchy, group preservation, library-table conflicts, net-chain settings
  and portable copy-versus-link behavior. Reuse Stage 10 path ownership rules.
- Verify all netlist/BOM/plot formats and failure reporting against native exporters.
  Add native printer selection, page setup and printing alongside PDF export.
- Finish document operations identified by the Stage 6 action inventory, including
  preview/report/confirmation behavior where native operations provide it.

**Done when:** representative table/import/export workflows round-trip through
native KiCad, canceled operations leave files/models intact, and output/printing
options match the parity checklist rather than merely producing one valid file.

## Stage 12 — Symbol editing and design blocks

**Outcome:** schematic users can perform their symbol/library and design-block
workflows without returning to a wx editor.

Work:

- Complete chooser search/filtering, previews, units/body styles and footprint
  associations using the existing library manager and preview renderer.
- Build the real library tree, symbol editing canvas/docks and editing tool context.
  Existing metadata, pin-table, inheritance and pin-map services remain the basis;
  add missing pin/graphic drawing, selection and undo workflows around them.
- Complete new/copy/import/rename/delete/save, derived-symbol restrictions,
  alternate units/body styles, symbol checking and library save-conflict handling.
- Port `SCH_DESIGN_BLOCK_CONTROL`: library browsing, previews, metadata, creating
  blocks from selections, insertion and placement, with proper hierarchy and undo.
- Integrate footprint browsing/assignment with existing suite providers; distinguish
  editing the footprint field from the complete assignment workflow.

**Done when:** a user creates/edits a library symbol, saves it, places it in a
schematic and updates it from the library; a reusable design block can be created
and inserted with one correct undo sequence. Files reopen in native KiCad.

## Stage 13 — ERC and simulation completeness

**Outcome:** the GPUI analysis windows cover the native analysis workflows and
stay attached to the correct live document.

Work:

- Supply missing ERC footprint and drawing-sheet providers; audit every ERC rule,
  severity/filter, exclusion persistence, marker navigation and report export.
- Audit simulation analyses/options, model libraries, IBIS, parameter/pin editors,
  parser diagnostics, result vectors, plot traces/cursors/axes and signal formatting
  against the native simulator. Extend the implemented subset where it differs.
- Complete simulation-session/workbook persistence, reopening and document-change
  invalidation, including user signals and plot configuration.
- Define run/stop/close/failure ownership and multiple project behavior for the
  process-global engine. Stale results must never be presented as current results
  for another schematic. Implement the required progress/cancel UI here; Stage 17
  handles the broader scheduling/performance work.

**Done when:** native ERC reference cases and representative simulation analyses
agree in results; exclusions and simulation workbooks survive reload; stop,
engine failure, document changes and window closure leave a usable application.

## Stage 14 — PCB and KiCad suite integration

**Outcome:** the GPUI schematic editor interoperates with the existing suite,
including a still-wx PCB editor.

Work:

- Connect project-manager launch/open, process/session identity and inter-editor
  messaging without constructing an `SCH_EDIT_FRAME` as an adapter.
- Implement live schematic↔PCB cross-probing, highlighting/selection and navigation.
  Preserve the current saved-board backannotation option.
- Complete Update PCB from Schematic and Update Schematic from PCB against open,
  possibly unsaved documents, including preview, conflicts and link maintenance.
- Complete footprint-assignment integration and external datasheet/help/document
  launching with clear unavailable-provider and disconnected-editor behavior.

**Done when:** a schematic edit can update a running PCB editor, cross-probing works
both ways, and board changes can be previewed/applied back to the correct schematic.
No PCB UI rewrite is required, and saved-file fallback remains usable.

## Stage 15 — Settings, library sources and packages

**Outcome:** users can configure the editor and its content sources through GPUI
with the same persistence and compatibility guarantees as native KiCad.

Work:

- Close the preferences/setup inventory: hotkey editing/import/export and conflict
  feedback, grids, units, colors/themes, fonts, simulator options and project/global
  settings scope. Apply changes consistently to all affected windows.
- Complete project/global library-table management, path substitution, database/HTTP
  connections and remote-provider configuration, refresh, authentication and errors.
- Add repository/catalog browsing, update discovery and installation to the existing
  local PCM ZIP/install/replace/uninstall services. Preserve validation and rollback;
  distinguish schematic undo from user-level package/configuration changes.
- Audit package resource discovery, migration of existing settings and absent or
  incompatible data sources on clean installations.

**Done when:** settings survive restart, project overrides remain isolated, normal
library/provider/package workflows are possible without hand-editing files, and
failed updates cannot silently destroy a working package or configuration.

## Stage 16 — Platform behavior and accessibility

**Outcome:** the completed workflows behave as native desktop applications on
supported platforms, not just in one development setup.

Work:

- Preserve native macOS menus and independent workflow windows; complete standard
  shortcuts, window switching, modal/modeless behavior, focus restoration, display
  changes, restored window geometry and native file/print/help integration.
- Verify Linux X11/Wayland and Windows behavior where GPUI supports the target.
  Identify required GPUI platform work explicitly; unsupported targets cannot be
  counted as passing. Keep normal platform conventions rather than forcing macOS UI.
- Audit accessibility roles/names, keyboard-only navigation, screen-reader access,
  tab order, IME/composition, Unicode entry, clipboard and high-contrast/HiDPI modes.
- Route user-facing strings through localization; audit units, number formatting,
  long translations, small windows, themes, error reporting and shortcut customization.

**Done when:** the platform acceptance matrix has actual results for each supported
target, every core workflow is keyboard-accessible, and no supported target has an
unowned framework/platform blocker. Explicitly document any unreleased target.

## Stage 17 — Responsiveness and reliability

**Outcome:** large designs and long sessions remain responsive and do not leak
resources or corrupt state when work is canceled.

Work:

- Profile cold open, hierarchy/selection updates, editing, save/export, ERC, library
  search and simulation. Establish fixture-specific latency/memory baselines and
  the existing 8.3 ms/120 Hz rendering target on named hardware.
- Move safe expensive work off the UI thread or divide it into bounded steps, with
  progress and cancellation. Respect the native runtime/connectivity thread-affinity
  contract; do not send arbitrary session/model calls to worker threads.
- Avoid repeated whole-document snapshots, unnecessary recording/tessellation and
  unbounded caches. Preserve retained geometry and viewport culling.
- Stress repeated open/close, undo/redo, project switching, library refresh, simulator
  runs and cancellation. Test stale-result rejection and shutdown with work in flight.

**Done when:** recorded performance budgets pass on representative large fixtures,
long-running jobs have useful progress/cancel behavior, and repeated-operation tests
show stable resource use without lost changes or cross-session results.

## Stage 18 — Remove wx GUI from the schematic host build

**Outcome:** the GPUI schematic executable no longer pulls in the legacy schematic
presentation layer simply to use its model services.

Work:

- Inventory link/runtime dependencies and split reusable schematic, library,
  export, analysis and settings services from `eeschema_kiface_objects` GUI code.
  Extract remaining presentation-owned services rather than copying their logic.
- Replace wx GUI dependencies still reached for dialogs, events, menus, clipboard,
  printing or UI-only providers. Move GPUI-specific adapters to explicit targets.
- Remove the GPUI executable's dependency on OpenGL/wx presentation while retaining
  permitted wxBase/model utilities. Verify actual loaded libraries, not only imports.
- Keep the legacy wx schematic editor and other suite applications buildable;
  do not remove their presentation code or require their simultaneous migration.
- Document remaining utility dependencies and exported ABI/thread/lifetime contracts.

**Done when:** a clean GPUI build and runtime dependency audit demonstrate that the
schematic presentation is independent of wx GUI/OpenGL, while both frontends use
the same tested model services and the legacy build still passes its checks.

## Stage 19 — Packaging, CI and release qualification

**Outcome:** the port is installable, reproducible and qualified as a schematic
editor, rather than only runnable from the development tree.

Work:

- Extend existing CMake/Rust integration and CI for clean linked/unlinked builds,
  ABI/export checks, native host tests, UI interactions and rendering regressions.
  Pin/document toolchains and required GPUI changes; avoid accidental local dependencies.
- Package the app, shared libraries, icons, fonts, translations, schemas, simulation
  libraries and other resources. Validate resource lookup outside `devenv`, build
  paths and temporary `.app` bundles. Cover platform signing/installer requirements.
- Run the complete action/workflow matrix and representative native KiCad projects
  on each release target, including failure/recovery cases and repeated save/reload.
- Document installation, limitations, migration of settings and switching back to
  the wx editor. Resolve the historical notes against current behavior.
- Make an explicit release decision: every in-scope parity gap is fixed or openly
  accepted as a release limitation; no silent omissions, skipped failures or hidden
  dependency on a developer's system configuration.

**Done when:** installed release artifacts pass the complete matrix on clean target
environments, CI is reproducible, all earlier stages meet their acceptance criteria,
and the current documentation matches what the shipped application does.

## Known-gap ownership

This table maps the existing reports to the stages that close their gaps. It is
also the starting checklist for the Stage 6 inventory.

| Known gap or historical item to revalidate | Owning stage |
| --- | --- |
| Junction deletion/merging, body-style model changes, transactional edits | 6 |
| Diagnostic sidebars, real hierarchy and mixed-selection property editing | 7 |
| Shared frame-dependent tools, grouping, picking, embedded files, camera authority | 8 |
| Dynamic action enablement/checks and tool-specific context menus | 8 |
| Bitmap rendering, wire hops, collision preview, zoom-dependent shadows/text | 9 |
| Units-provider model contract / visual dimension formatting | 6 / 9 |
| Group-87 golden mismatch and unexplained render differences | 9 |
| New/Open/Save As, recovery, project isolation, loader lifetime, OS-quit veto | 10 |
| Linked source-sheet save ownership and copy-versus-reference behavior | 10 |
| Full fields/BOM/table UI, CSV export, global styles, import/table merge limits | 11 |
| Native printing, complete plot/netlist options and drawing-sheet editing options | 11 |
| Symbol-editor canvas/docks, chooser parity, design blocks | 12 |
| Footprint assignment UI / running-suite integration | 12 / 14 |
| Optional ERC providers and remaining analysis/model/plot/workbook features | 13 |
| Live PCB backannotation, forward update, cross-probing and inter-editor links | 14 |
| PCM repository browser, settings/hotkeys and remote-library/provider parity | 15 |
| Platform conventions, accessibility, localization, window layout persistence | 16 |
| Blocking ERC/export/library work, memory use, thread-safe scheduling | 17 |
| Legacy GUI linkage and remaining wx presentation dependencies | 18 |
| Clean installation, resource lookup, CI matrix and final full-workflow audit | 19 |

Historical reports can contain already-fixed items (for example page-settings
undo, search-data ownership or symbol placement). Record the evidence and mark
those resolved during the inventory; do not implement them twice. PCB-specific
negative/difference rendering and depth requirements remain on the future PCB
roadmap unless a schematic acceptance case proves they are needed here.

## After the schematic port

After Stage 19, use [05-porting-guide.md](05-porting-guide.md) to create a separate
numbered pcbnew roadmap: first audit board rendering/layers and tool dependencies,
then establish the board host/input/editing loop, then board workflows and parity,
and finally packaging and dependency removal. Revalidate the guide's reuse claims
against the renderer and shared-tool work completed above.

The project manager, GerbView, page-layout editor, footprint editor and 3D viewer
need their own inventories and acceptance plans; the 3D renderer is not covered
by the 2D draw-stream approach. A later replacement of wxBase or C++ model code
requires a separate proposal with compatibility and performance gates. Finishing
Stage 19 does not claim those projects are complete.
