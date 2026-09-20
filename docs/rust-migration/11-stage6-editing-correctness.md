# Stage 6 — Editing correctness verification

Verification date: 2026-09-20, macOS arm64, native host and GPUI frontend.

## Delivered changes

- Extracted junction deletion and collinear wire/bus merging into
  `SCHEMATIC_HOLDER`. Both the wx frame and GPUI tools call this service.
  Coincident wires and buses are never deduplicated against each other.
- Extracted body-style changes into the same shared service. Cycling joins the
  caller's commit, rejects invalid low values, and creates no undo entry for a
  no-op, including a requested style clamped to the current style.
- Duplicate, paste and repeated-symbol placement suspend a schematic tool
  coroutine while awaiting input. They no longer block GPUI inside a nested
  `wxYield` loop. The transaction remains alive until placement completes or
  cancels. Canceling a later item in a repeated selection reverts the whole edit.
- Repeated symbols use the native reference allocator across their sheet
  instances. Repeated sheets are checked for recursion before any model mutation.
- Property Apply compares the current field/property values, current instance
  path, variant and sheet page number with the session's last snapshot. A stale
  form is rejected before mutation. Successful Apply refreshes its baseline;
  repeated Apply of unchanged values remains a no-op. No ABI layout changed.
- Corrected four library regression fixtures to bind their library manager to
  their own project. The fixture owns a different settings manager from `Pgm()`;
  using the implicit process project resolved `${KIPRJMOD}` incorrectly.

## Acceptance matrix

Tests are in `qa/tests/eeschema/test_sch_host.cpp` unless otherwise specified.
These are representative behavioral cases, not a claim that every possible
combination of geometry and command has been exhaustively tested.

| Behavior | Expected result and evidence |
| --- | --- |
| Delete a four-way junction on wires and buses | `JunctionDeletionMergesWiresAndBusesAndRoundTrips`: four segments become two crossing lines; horizontal/vertical nets separate; Undo restores the junction and connectivity; Redo and save/reload preserve endpoints and layer. Bus member counts are checked. |
| Coincident wire and bus | `JunctionCleanupNeverDeduplicatesAWireAgainstABus`: both layers survive cleanup. |
| Body style | `BodyStyleUsesOneTransactionAndRejectsNoOps`: shared service changes style in one undo entry, Undo/Redo restore style, invalid/no-op requests add none. |
| Rotate and mirror | `RotatingAndMirroringSymbolsMatchNativeGeometry`: tool results match native symbol connection-point transformations; UUID and Undo/Redo are preserved. |
| Move and drag | Existing `ADragMovesTheSelectedItemAndIsUndoable`; canceled move is covered with duplicate/repeat cancellation. Native connectivity move/remove/restore regression suites also run. |
| Duplicate and repeat | `DuplicateAndRepeatCommitOnceWithFreshIdentity`: dispatch returns while placement is pending, a click commits once, new UUID/reference are assigned, Undo/Redo remove/restore the same identity. |
| Cancel mixed repeat / duplicate | `CancelMoveAndDuplicatePreservesIdentityAndUndo`: no extra items, altered original identity, dirty flag or undo entry; a label preceding the canceled repeated symbol also rolls back. |
| Property validation and stale Apply | `PropertiesValidateReferencesAndNoOpDoesNotCreateUndo`: invalid text, non-finite/out-of-range distances and invalid choices reject before mutation; stale Apply following Undo is rejected. |
| Failed Apply focus and draft | GPUI `failed_property_apply_preserves_draft_and_keeps_shortcuts_in_dialog`: failed Apply retains the draft, retry submits the same values, editing shortcuts stay in the form, Close restores canvas input. |
| Placement prompts and Escape | Existing text, bus-entry, raster-image, sheet and symbol chooser tests cover validation, cancellation and one-commit placement. |
| Sheet pins and hierarchy | Existing synchronization, recursive-relink rejection and replacement-screen tests; native hierarchy, repeated-instance, sheet-pin and connectivity suites. |
| Units, body selection, multi-unit and variants | Host already implements `UNITS_PROVIDER`; ABI distances remain explicitly millimetres. Native property setters retain per-instance unit selection; native multi-unit, variant-field, variant-symbol and pin-map tests remain the authority. Existing chooser ABI rejects out-of-range units/styles. |
| Clipboard | Computer Use copied a symbol, started paste, canceled with Escape, then one Undo removed the preceding committed duplicate. Native duplicate uses the same paste/placement path. |
| Save/reopen | Native round-trip tests plus Computer Use of the actual rebuilt executable; see below. |

## Action inventory and later-stage ownership

[stage6-action-inventory.csv](stage6-action-inventory.csv) captures all 440 entries
from the linked `ksch_action_count` / `ksch_action_at` registry, sorted by stable
name. 110 names occur in the static menu/tool layout. This column describes
presentation registration, **not dynamic enablement or proof of implementation**.
The UI currently uses static registration to resolve commands; dynamic availability
is Stage 8. Rows without individual verification explicitly say so.

Stage 8 owns remaining shared-tool/context-menu and command-availability audits;
Stage 11 owns complete document/table forms; Stage 12 owns symbol editing and
block workflows; Stage 13 owns analysis; Stage 14 owns suite integration. The four
PCB registry entries are other-consumer scope. Stage 9 owns visual overlays and
units formatting. A recursive repeated sheet is safely rejected; a guided
copy-versus-link repair workflow belongs to Stages 10–12.

Properties currently use a session-owned baseline for the selected item. This
supports the existing single property workflow per item. Stage 7's owned revision
snapshots must extend this contract before supporting independent simultaneous
editors of the same item. No known corruption case is deferred as presentation.

## Computer Use

Built `kicad_sch_host`, `qa_eeschema` and `eeschema_gpui`, then launched the linked
binary as `/tmp/KiCad-Stage6.app` with a disposable schematic containing R1,
a four-way junction, and HORIZONTAL/VERTICAL labels.

1. Deleted the junction through the canvas. Saved output contained two wires and
   no junction; Undo restored the dot and Redo removed it again.
2. Selected R1 and used the native Edit → Duplicate menu. GPUI remained responsive
   during placement; clicking placed R2 with its own reference.
3. Copied R2 and pasted. The R3 preview appeared; Escape removed it. One Undo
   removed R2, demonstrating the canceled paste added no undo entry.
4. Redid R2, saved, quit and reopened the same file. The reopened canvas showed
   R1/R2 and the crossing without a junction. No wx schematic frame was created.

The document sidebars still show diagnostic geometry counts (Stage 7); they are
not used as correctness evidence.

## Automated verification

- Native host/ABI: 89 cases, passing.
- GPUI/input baseline: 136 tests plus one doctest, passing; the added failed-Apply
  interaction test also passes.
- Full native suite: **1,776 cases, no errors**, including the host cases, after
  correcting project binding in the four library fixtures. Optional external
  importer corpora were absent; those corpus-dependent checks reported skips.
- Remote integration: rebuilt `qa_eeschema` and passed both updated action-handling
  cases, including the added host/ABI zoom-to-fit assertions.
- `kicad_sch_host`, `qa_eeschema`, `eeschema_gpui` builds and
  `git diff --check`: passing.

Useful commands:

```sh
devenv shell -- ninja -C build -j10 kicad_sch_host qa_eeschema eeschema_gpui
devenv shell -- build/qa/tests/eeschema/qa_eeschema '--run_test=SchHost*' --log_level=message
devenv shell -- env KICAD9_SYMBOL_DIR="$PWD/qa/data/libraries" build/qa/tests/eeschema/qa_eeschema --log_level=message
devenv shell -- env KICAD_SCH_HOST_DIR= cargo test --manifest-path rust/Cargo.toml -p kicad-sch-ui -p kicad-eeschema-gpui
```
