# Action-registry menus and toolbars — 2026-09-19

> **Current planning:** [10-remaining-stages.md](10-remaining-stages.md) defines
> Stages 6–19 and their execution order. This document records an earlier survey,
> implementation milestone or reuse guidance; historical gaps and proposed
> approaches must be checked against the Stage 5 coverage and
> [completed Stage 6 verification](11-stage6-editing-correctness.md) before use.
> Stage 7 (real document sidebars) is next.


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

The follow-up fixes address both observed presentation defects. Tool action
identity now ignores its invocation payload, so the menu/palette Draw Wires action
matches its W binding. Preferred platform bindings bracket compatibility aliases
because GPUI's menu and tooltip resolvers search in opposite orders.

## Follow-up implementation

Window close and editor Quit now prompt Save, Discard, or Cancel for a modified
host document. Save errors keep the editor open. The current GPUI public API
cannot veto external application termination; OS-level quit remains a framework
limitation.

Find/Replace has a GPUI panel and an owned search-data ABI. The host exposes
search terms through `SCHEMATIC_HOLDER`, runs `SCH_FIND_REPLACE_TOOL`, and returns
match coordinates, wrap status and replacement counts. Replacements use the
existing schematic commit/undo mechanism. The panel supports case/whole-word
matching, sheet/selection scope, fields/pins, next/previous and replace/all.

Follow-up validation: 52 C++ host tests pass, including Unicode replacement,
sheet scope, case/whole-word options, wrap detection and undo. The standalone
Rust run passes 21 binary, 5 sys, 55 UI unit, 41 UI interaction, 2 toolbar and
2 documentation tests; Clippy passes with warnings denied. Linked sys passes
3 unit and 17 live tests, with only the previously recorded group-87 golden
render mismatch failing.

The rebuilt macOS app was checked with `/tmp/gpui-find-replace-check.kicad_sch`:
W appears beside Draw Wires, Edit shows Command hints, Command-Option-F opens
the panel, finding ECC83 centers its value, replacement marks the host modified,
and undo restores the value. Command-W opens Save/Discard/Cancel; Cancel keeps
the window open, and Save closes it after writing the temporary file.

This milestone originally identified `DeleteJunction`, symbol placement and
properties as follow-up work. Stages 5 and 6 subsequently implemented those
paths; see the [Stage 6 verification report](11-stage6-editing-correctness.md).
Registry-backed menus alone still do not establish workflow completeness.
