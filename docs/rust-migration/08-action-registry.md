# Action-registry menus and toolbars — 2026-09-19

M4's command-presentation step is implemented. With `--schematic`, the binary
reads KiCad's registry after opening the host session and supplies an owned
snapshot to the GPUI shell. Menu and toolbar layout and icons stay in Rust;
labels, descriptions and configured primary/alternate shortcuts come from C++.
The command palette and context menu use the same metadata.

`kicad-sch-sys::actions()` copies the existing action records into owned Rust
values. The additional `ksch_action_hotkey_names` ABI function uses KiCad's own
key-name conversion, avoiding a second table of wx numeric key constants.
`shell::init_with_registry` installs the live catalogue. Replay and demo startup
retain their standalone catalogue without requiring a host library.

Missing registry actions are omitted from menus and disabled in toolbars.
Registry presence does **not** mean a handler works without a dialog. The
snapshot does not supply dynamic enablement or checked state and does not
refresh after hotkey or locale changes. C++ captures it on first registry access;
the application reads it after session initialization to capture startup settings.
Rebuild the host library together with Rust because the new accessor is required.

## Verification

The C++ host and release GPUI application build successfully. The affected Rust
UI/binary checks pass: 19 binary tests, 53 UI unit tests, 38 UI interaction tests,
2 toolbar checks and 1 doctest. These include actual registry-to-menu/keymap
conversion and a GPUI test for host labels, palette search and unavailable buttons.
The no-host API tests also pass.

The linked host suite passes 17 of 18 checks, including the new registry check.
Its golden-render comparison fails on retained group 87 (35 commands live versus
63 in the fixture). The original test from HEAD, without registry enumeration,
reproduces the same mismatch. This change does not fix that rendering baseline.

Computer Use checked the rebuilt macOS app with a disposable copy of the ECC83
demo at `/tmp/gpui-registry-check.kicad_sch`. Old running instances were closed
before verifying the new registry labels and the new temporary filename.

| UI operation | Observed result |
|---|---|
| Open Place menu | KiCad labels such as “Draw Wires” and “Place Symbols” appear |
| Choose Draw Wires from menu | Matching toolbar tool becomes active |
| Toolbar Draw Wires, start click, endpoint double-click, Escape | A committed wire appears |
| Toolbar Undo | Wire disappears |
| Command–Shift–Z | Wire returns |
| Edit → Undo | Wire disappears again |
| Command–Shift–P, search “Draw Wires”, Return | Registry label matches and wire tool activates |
| Escape, canvas click, W | Wire tool activates after palette dismissal |
| Toolbar zoom in/out | Zoom changes from 156% to 195% and back |
| Secondary click on canvas | Context menu shows registry commands |

Two presentation defects remain visible: the wire shortcut is absent from the
menu/palette display, and some macOS shortcut hints show Control even though
Command shortcuts work. These observations do not establish the cause; follow-up
should inspect metadata after session initialization and how GPUI matches menu
actions to keybindings. Not every registered command or drawing tool was tested.

## Next work

1. Correct shortcut presentation, with a live-session regression for metadata
   after initialization and a UI check that W and native Command hints appear.
2. Add unsaved-close protection before expanding File workflows. The current
   shell can close a modified document without prompting.
3. Start Stage 5 with a bounded dialog workflow: expose search data through
   `SCHEMATIC_HOLDER` and add a GPUI Find/Replace UI. The document traversal is
   already converted; search terms still belong to the wx frame/dialog.

The host-side interface priorities remain in `06-what-is-missing.md`, notably
`DeleteJunction` for correct wire/junction cleanup. Symbol placement and
properties are larger subsequent dialog milestones; registry-backed menus do
not make those workflows complete.
