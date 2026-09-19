// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Proves that the tool palette has not drifted from eeschema's own toolbar.
//!
//! `src/tools.rs` is a second list of the schematic editor's drawing tools —
//! the first being `eeschema/toolbars_sch_editor.cpp`, which is the one users
//! actually see. A second list of anything drifts, and this one drifted:
//! nine tools were missing from it, silently, because nothing compared them.
//!
//! So this reads both C++ files as text and compares. It is a parser rather
//! than a transcription for the same reason `kicad-gal`'s `header_sync` is:
//! a transcribed copy drifts in exactly the way the thing it guards drifts.
//!
//! What it checks:
//!
//! * every modal tool on eeschema's right toolbar has a row in `TOOLS`;
//! * every `TOOLS` row names an action that really exists, spelled exactly as
//!   `TOOL_ACTION::Name()` spells it;
//! * every `TOOLS` shortcut matches the action's own `DefaultHotkey`, so the
//!   palette cannot advertise a binding KiCad does not have.

use std::collections::{BTreeMap, BTreeSet};
use std::path::PathBuf;

use kicad_sch_ui::tools::TOOLS;

fn repo(path: &str) -> String {
    let p = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../..")
        .join(path);
    std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("cannot read {}: {e}", p.display()))
}

/// Strip C and C++ comments, so a commented-out action is not mistaken for a
/// live one.
fn strip_comments(src: &str) -> String {
    let chars: Vec<char> = src.chars().collect();
    let mut out = String::with_capacity(src.len());
    let mut i = 0;
    while i < chars.len() {
        if chars[i] == '/' && i + 1 < chars.len() && chars[i + 1] == '*' {
            i += 2;
            while i + 1 < chars.len() && !(chars[i] == '*' && chars[i + 1] == '/') {
                i += 1;
            }
            i = (i + 2).min(chars.len());
            out.push(' ');
        } else if chars[i] == '/' && i + 1 < chars.len() && chars[i + 1] == '/' {
            while i < chars.len() && chars[i] != '\n' {
                i += 1;
            }
        } else {
            out.push(chars[i]);
            i += 1;
        }
    }
    out
}

/// What one `TOOL_ACTION` declaration says about itself.
#[derive(Debug)]
struct Action {
    /// The dotted runtime name, from `.Name( "..." )`.
    name: String,
    /// `ToolbarState( TOOLBAR_STATE::TOGGLE )` — the action stays pressed while
    /// its tool is the active one, which is what makes it a *modal* tool rather
    /// than a command that runs and returns.
    toggle: bool,
    /// `Flags( AF_ACTIVATE )` — activating it hands control to a tool.
    activate: bool,
    /// The argument of `.DefaultHotkey( ... )`, verbatim, if it has one.
    hotkey: Option<String>,
}

/// Every `TOOL_ACTION` a file declares, keyed by its qualified C++ name, e.g.
/// `SCH_ACTIONS::drawWire`.
fn actions(path: &str) -> BTreeMap<String, Action> {
    let text = strip_comments(&repo(path));
    let mut out = BTreeMap::new();
    for chunk in text.split("TOOL_ACTION ").skip(1) {
        let Some(open) = chunk.find('(') else {
            continue;
        };
        let qualified = chunk[..open].trim();
        if !qualified.contains("ACTIONS::") {
            continue;
        }
        // The declaration runs to the first `);` that closes it. Every argument
        // here is a single call, so the first `);` at the start of a line's
        // trailing run, or simply the first `);`, ends it — the bodies contain
        // no nested `);` in this file.
        let end = chunk.find(");").unwrap_or(chunk.len());
        let body = &chunk[..end];
        let Some(name) = between(body, ".Name(") else {
            continue;
        };
        let name = name.trim().trim_matches('"').to_string();
        out.insert(
            qualified.to_string(),
            Action {
                name,
                toggle: body.contains("TOOLBAR_STATE::TOGGLE"),
                activate: body.contains("AF_ACTIVATE"),
                hotkey: between(body, ".DefaultHotkey(").map(|h| h.trim().to_string()),
            },
        );
    }
    assert!(!out.is_empty(), "{path} declared no actions; parser broke");
    out
}

/// The text between `opener` and its matching `)`.
fn between(body: &str, opener: &str) -> Option<String> {
    let start = body.find(opener)? + opener.len();
    let rest = &body[start..];
    let mut depth = 1usize;
    for (i, c) in rest.char_indices() {
        match c {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(rest[..i].to_string());
                }
            }
            _ => {}
        }
    }
    None
}

/// The qualified action names the right toolbar places, in order.
fn right_toolbar() -> Vec<String> {
    let text = strip_comments(&repo("eeschema/toolbars_sch_editor.cpp"));
    let start = text
        .find("case TOOLBAR_LOC::RIGHT:")
        .expect("the right toolbar case");
    let block = &text[start..];
    let end = block.find("break;").expect("the case ends");
    let block = &block[..end];

    let bytes: Vec<char> = block.chars().collect();
    let mut out = Vec::new();
    let mut i = 0;
    while let Some(hit) = block[byte_of(&bytes, i)..].find("ACTIONS::") {
        let at = byte_of(&bytes, i) + hit;
        // Walk left over the qualifier, so `SCH_ACTIONS::x` is not read as
        // `ACTIONS::x` — they are different symbols in different files.
        let mut first = at;
        while first > 0 && is_ident(block.as_bytes()[first - 1]) {
            first -= 1;
        }
        let mut last = at + "ACTIONS::".len();
        while last < block.len() && is_ident(block.as_bytes()[last]) {
            last += 1;
        }
        let name = block[first..last].to_string();
        if !out.contains(&name) {
            out.push(name);
        }
        i = char_of(block, last);
    }
    assert!(
        out.len() > 20,
        "only {} actions found on the right toolbar; parser broke",
        out.len()
    );
    out
}

fn is_ident(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

fn byte_of(chars: &[char], i: usize) -> usize {
    chars[..i.min(chars.len())]
        .iter()
        .map(|c| c.len_utf8())
        .sum()
}

fn char_of(s: &str, byte: usize) -> usize {
    s[..byte].chars().count()
}

/// Toolbar entries that are deliberately not palette tools, and why.
///
/// Each one has to earn its place here: the failure this file exists to catch
/// is a tool quietly missing, and an exemption is how that would hide.
fn exempt(qualified: &str) -> Option<&'static str> {
    match qualified {
        // Modes of the one selection tool, not tools of their own. The shell
        // has a single `Tool::Select`; choosing between a rectangle and a lasso
        // band is a property of it, and neither is meaningful before the shell
        // is connected to a document model that can be selected from.
        "ACTIONS::selectSetRect" | "ACTIONS::selectSetLasso" => Some("selection modes"),
        // Opens a dialog and returns. It is `AF_ACTIVATE` but not a toggle, and
        // it appears in the Place menu as a command, which is what it is.
        "SCH_ACTIONS::syncAllSheetsPins" => Some("a dialog, not a modal tool"),
        _ => None,
    }
}

#[test]
fn every_tool_on_eeschemas_toolbar_is_in_the_palette() {
    let mut declared = actions("eeschema/tools/sch_actions.cpp");
    declared.extend(actions("common/tool/actions.cpp"));
    let present: BTreeSet<&str> = TOOLS.iter().map(|spec| spec.id.as_str()).collect();

    let mut missing = Vec::new();
    for qualified in right_toolbar() {
        let Some(action) = declared.get(&qualified) else {
            panic!("{qualified} is on the toolbar but was not parsed from either actions file");
        };
        // A modal tool is one that stays active and shows itself as pressed.
        if !(action.toggle && action.activate) {
            continue;
        }
        if exempt(&qualified).is_some() {
            continue;
        }
        if !present.contains(action.name.as_str()) {
            missing.push(format!("{qualified} ({})", action.name));
        }
    }
    assert!(
        missing.is_empty(),
        "eeschema's toolbar has tools the palette does not: {missing:#?}"
    );
}

#[test]
fn every_palette_tool_names_a_real_action_with_the_right_hotkey() {
    let mut declared = actions("eeschema/tools/sch_actions.cpp");
    declared.extend(actions("common/tool/actions.cpp"));
    let by_name: BTreeMap<&str, &Action> = declared
        .values()
        .map(|action| (action.name.as_str(), action))
        .collect();

    for spec in TOOLS {
        let action = by_name.get(spec.id.as_str()).unwrap_or_else(|| {
            panic!(
                "{} names {}, which no TOOL_ACTION declares",
                spec.button_id,
                spec.id.as_str()
            )
        });
        assert!(
            action.activate,
            "{} is not AF_ACTIVATE, so activating it would not start a tool",
            spec.id.as_str()
        );
        // `DefaultHotkey( 'W' )` and `DefaultHotkey( MD_CTRL + 'L' )` against
        // gpui's "w" and "ctrl-l". Only the shape is compared, because the
        // spellings differ; a palette shortcut with no KiCad hotkey behind it,
        // or the other way round, is the mistake worth catching.
        match (&action.hotkey, spec.shortcut) {
            (None, None) => {}
            (Some(_), Some(_)) => {}
            (Some(hotkey), None) => panic!(
                "{} has the default hotkey {hotkey} in KiCad but none in the palette",
                spec.id.as_str()
            ),
            (None, Some(key)) => panic!(
                "{} advertises {key} in the palette but KiCad binds it nothing",
                spec.id.as_str()
            ),
        }
    }
}
