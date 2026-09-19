# Computer Use verification — 2026-09-19

For the subsequent registry-backed menus and toolbars check, including current
shortcut-display defects, see [`08-action-registry.md`](08-action-registry.md).

This pass exercised the macOS GPUI application through Computer Use, using a copy
of `demos/ecc83/ecc83-pp_v2.kicad_sch` at `/tmp/gpui-ui-test.kicad_sch`.
The demo in the repository was not edited.

## Fixes

- The canvas takes keyboard focus at startup and when clicked. Closing the command
  palette returns focus to it. The old test harness explicitly assigned focus and
  therefore concealed the startup bug; it now uses the real startup behavior.
- macOS has Command shortcuts, including Command-Shift-Z for redo. Existing
  Control aliases remain available.
- Host edits invalidate the canvas state and its cached dock panel. In the running
  app, undo previously changed the C++ document but left the old picture visible
  until the pointer moved. Keyboard and menu edits now repaint immediately.
- Toolbar activation has no canvas position and waits for a click. Tool hotkeys
  go through the host keyboard dispatcher, retaining immediate placement and
  repeated-hotkey behavior. Previously a toolbar click could start a wire at the
  toolbar's cursor position; repeated hotkeys could also be lost when the shell
  considered the tool already active.
- The host disables pin electrical-type labels, matching the schematic editor
  rather than the symbol-viewer render default. Footprint fields visible in the
  ECC83 demo are intentionally visible in the file and remain so.
- The status bar exposes unsaved state and host failures/unhandled named actions.
- Holding and dragging the secondary mouse button pans the canvas, as does the
  middle button. A secondary click without dragging opens the context menu on
  release; dismissing it returns keyboard focus to the canvas.
- CMake always invokes Cargo's freshness check. Its former output rule depended
  only on the C++ library, so Rust-only edits could silently leave a stale binary.

## Observed in the rebuilt UI

| Gesture | Result |
|---|---|
| Select valve body, press R | Symbol rotates |
| Command-Z, without moving the mouse | Original orientation is painted immediately |
| Command-Shift-Z | Rotation returns immediately |
| Command-S | Status changes from Unsaved changes to Saved |
| Draw Wire toolbar, click start, double-click end, Escape | One wire between canvas points; no toolbar-to-canvas segment |
| Click blank canvas, W, double-click end, Escape | Wire starts at the hotkey cursor |
| Command-Shift-P, Escape, R on a selected symbol | Palette closes and canvas keyboard editing continues |
| Save, quit, reopen the copied schematic | Rotated symbol and both new wires remain |

The focused Rust checks passed: 18 binary tests, 50 shell unit tests, 37 shell
interaction tests, 2 toolbar checks, and 1 doctest. No interaction tests remain
ignored. Secondary-button panning was verified with GPUI mouse events in an
automated regression test, rather than through Computer Use. The seven C++ host
suites passed all 49 cases, including the
regression that activates a wire from chrome with the cursor away from its start.

Two old C++ tests used zoom as an example of an unhandled action, although the
host now supports it. They now use a registered symbol-library action whose tool
is absent from the schematic host. The Rust loading test no longer assumes the
old pin-type labels' exact draw-command count.

## Remaining scope

This is not a completed dialog port. Symbol placement, properties, ERC and other
dialog workflows still need UI implementations. A successful action-dispatch
result does not prove that a dialog ran: some C++ handlers accept an event and
then decline their frame-dependent work. Consequently the new unhandled-action
message does not cover every unavailable workflow.

No-argument startup still displays a demonstration stream, which is not editable.
The sidebar remains a draw-stream summary, not a functional document hierarchy or
property editor. Unsaved-close protection is still absent. This pass did not
validate every drawing tool, clipboard workflow, or multi-sheet operation.
