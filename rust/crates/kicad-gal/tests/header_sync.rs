// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Proves that `src/abi.rs` has not drifted from the C header it mirrors.
//!
//! The mirrors are hand written so that this crate needs no C toolchain, which
//! trades a build-time guarantee for a test-time one. This is that test: it
//! reads `include/gal/recording/draw_stream_abi.h` as text and checks every
//! opcode number, flag bit, target, size assertion, serialised field order and
//! coordinate-reference rule against the Rust side.
//!
//! It is deliberately a *parser*, not a copy of the header's values: a copy
//! would drift in exactly the same way the mirrors can.

use std::collections::BTreeMap;
use std::path::PathBuf;

use kicad_gal::abi::{
    coord_refs, kgds_cmd, kgds_file_header, kgds_group, kgds_image, kgds_stream_view, flags,
    GridStyle, Op, Target, MAX_COORD_REFS,
};
use kicad_gal::{KGDS_MAGIC, KGDS_VERSION};

fn header_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../include/gal/recording/draw_stream_abi.h")
}

fn header_text() -> String {
    let p = header_path();
    std::fs::read_to_string(&p)
        .unwrap_or_else(|e| panic!("cannot read the ABI header at {}: {e}", p.display()))
}

/// Strip C and C++ comments, so that documentation cannot be mistaken for code.
fn strip_comments(src: &str) -> String {
    let mut out = String::with_capacity(src.len());
    let chars: Vec<char> = src.chars().collect();
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

/// The body of `enum <name>` or `typedef enum <name> { ... }`.
fn enum_body<'a>(src: &'a str, name: &str) -> &'a str {
    let at = src
        .find(&format!("enum {name}"))
        .unwrap_or_else(|| panic!("the header no longer declares `enum {name}`"));
    let open = src[at..]
        .find('{')
        .unwrap_or_else(|| panic!("`enum {name}` has no body"))
        + at;
    let close = src[open..]
        .find('}')
        .unwrap_or_else(|| panic!("`enum {name}` body is unterminated"))
        + open;
    &src[open + 1..close]
}

/// Parse `NAME = <expr>,` pairs from an enum body, evaluating the small subset
/// of C expressions the header actually uses.
fn enum_values(body: &str) -> BTreeMap<String, u64> {
    let mut out = BTreeMap::new();
    let mut implicit: u64 = 0;
    for item in body.split(',') {
        let item = item.trim();
        if item.is_empty() {
            continue;
        }
        match item.split_once('=') {
            Some((name, value)) => {
                let v = eval_c_int(value.trim());
                out.insert(name.trim().to_string(), v);
                implicit = v + 1;
            }
            None => {
                out.insert(item.to_string(), implicit);
                implicit += 1;
            }
        }
    }
    out
}

/// Evaluate a C integer literal or `a << b`, with optional `u`/`U` suffixes.
fn eval_c_int(expr: &str) -> u64 {
    let expr = expr.trim();
    if let Some((lhs, rhs)) = expr.split_once("<<") {
        return eval_c_int(lhs) << eval_c_int(rhs);
    }
    let t = expr.trim().trim_end_matches(['u', 'U']);
    if let Some(hex) = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")) {
        u64::from_str_radix(hex, 16).unwrap_or_else(|e| panic!("bad hex literal {expr:?}: {e}"))
    } else {
        t.parse()
            .unwrap_or_else(|e| panic!("bad integer literal {expr:?}: {e}"))
    }
}

/// The value of an object-like `#define`.
fn define(src: &str, name: &str) -> u64 {
    for line in src.lines() {
        let line = line.trim();
        if let Some(rest) = line.strip_prefix(&format!("#define {name} ")) {
            return eval_c_int(rest.trim());
        }
    }
    panic!("the header no longer defines `{name}`");
}

/// Field names of `typedef struct <name> { ... }`, in declaration order.
fn struct_fields(src: &str, name: &str) -> Vec<String> {
    let at = src
        .find(&format!("struct {name}"))
        .unwrap_or_else(|| panic!("the header no longer declares `struct {name}`"));
    let open = src[at..].find('{').expect("struct has no body") + at;
    let close = src[open..].find('}').expect("struct body unterminated") + open;
    src[open + 1..close]
        .split(';')
        .filter_map(|decl| {
            let decl = decl.trim();
            if decl.is_empty() {
                return None;
            }
            // The declarator is the last token; pointers bind to the type in
            // this header's style (`const kgds_cmd* cmds`).
            decl.rsplit(|c: char| c.is_whitespace() || c == '*')
                .find(|t| !t.is_empty())
                .map(str::to_string)
        })
        .collect()
}

#[test]
fn magic_and_version_match_the_header() {
    let src = strip_comments(&header_text());
    assert_eq!(define(&src, "KGDS_MAGIC") as u32, KGDS_MAGIC);
    assert_eq!(define(&src, "KGDS_VERSION") as u32, KGDS_VERSION);
    assert_eq!(
        define(&src, "KGDS_MAX_COORD_REFS") as usize,
        MAX_COORD_REFS
    );
}

#[test]
fn every_opcode_matches_the_header() {
    let src = strip_comments(&header_text());
    let mut c_ops = enum_values(enum_body(&src, "kgds_op"));

    // KGDS_OP_MAX is a bound, not an opcode.
    let c_max = c_ops
        .remove("KGDS_OP_MAX")
        .expect("the header no longer terminates kgds_op with KGDS_OP_MAX");

    let rust_ops: BTreeMap<String, u64> = Op::ALL
        .iter()
        .map(|op| (op.c_name().to_string(), *op as u64))
        .collect();

    // Compare as whole maps so that an added or removed opcode is reported as
    // such rather than as a mismatched value.
    let only_c: Vec<_> = c_ops.keys().filter(|k| !rust_ops.contains_key(*k)).collect();
    let only_rust: Vec<_> = rust_ops.keys().filter(|k| !c_ops.contains_key(*k)).collect();
    assert!(
        only_c.is_empty(),
        "opcodes in the header that kicad-gal does not know: {only_c:?}"
    );
    assert!(
        only_rust.is_empty(),
        "opcodes kicad-gal knows that the header does not declare: {only_rust:?}"
    );

    for (name, c_value) in &c_ops {
        assert_eq!(
            rust_ops[name], *c_value,
            "opcode {name} is {c_value:#x} in the header but {:#x} in kicad-gal",
            rust_ops[name]
        );
    }

    // Nothing may be numbered at or past the enum's own terminator.
    for (name, v) in &rust_ops {
        assert!(*v < c_max, "opcode {name} is not below KGDS_OP_MAX");
    }
}

/// The Rust constant a `KGDS_FLAG_*` name stands for.
fn rust_flag(c_name: &str) -> Option<u16> {
    Some(match c_name {
        "KGDS_FLAG_HOLE" => flags::HOLE,
        "KGDS_FLAG_CLOSED" => flags::CLOSED,
        "KGDS_FLAG_GLYPH" => flags::GLYPH,
        "KGDS_FLAG_GROUP_COLOR" => flags::GROUP_COLOR,
        "KGDS_FLAG_GROUP_DEPTH" => flags::GROUP_DEPTH,
        _ => return None,
    })
}

#[test]
fn flags_and_targets_match_the_header() {
    let src = strip_comments(&header_text());

    let c_flags = enum_values(enum_body(&src, "kgds_flag"));
    for (name, value) in &c_flags {
        let rust = rust_flag(name)
            .unwrap_or_else(|| panic!("the header defines flag {name}, which kicad-gal does not"));
        assert_eq!(rust as u64, *value, "{name} drifted");
    }
    // KNOWN must cover exactly the declared bits, or validation would either
    // reject a legal stream or accept an unknown bit.
    let union = c_flags.values().fold(0u64, |a, b| a | b);
    assert_eq!(union, flags::KNOWN as u64);

    let c_targets = enum_values(enum_body(&src, "kgds_target"));
    for (name, value) in &c_targets {
        let rust = match name.as_str() {
            "KGDS_TARGET_CACHED" => Target::Cached,
            "KGDS_TARGET_NONCACHED" => Target::NonCached,
            "KGDS_TARGET_OVERLAY" => Target::Overlay,
            "KGDS_TARGET_TEMP" => Target::Temp,
            other => panic!("the header declares an unknown render target {other}"),
        };
        assert_eq!(rust as u64, *value, "{name} drifted");
    }
    assert_eq!(c_targets.len(), 4);

    let c_formats = enum_values(enum_body(&src, "kgds_image_format"));
    assert_eq!(c_formats.len(), 1);
    assert_eq!(c_formats["KGDS_IMAGE_RGBA8"], 0);
}

#[test]
fn record_sizes_match_the_headers_static_asserts() {
    let src = strip_comments(&header_text());

    let mut found = 0;
    for line in src.lines() {
        let line = line.trim();
        let Some(rest) = line
            .strip_prefix("static_assert(")
            .or_else(|| line.strip_prefix("_Static_assert("))
        else {
            continue;
        };
        let Some((lhs, rhs)) = rest.split_once("==") else {
            continue;
        };
        let type_name = lhs
            .trim()
            .trim_start_matches("sizeof(")
            .trim()
            .trim_start_matches('(')
            .trim()
            .trim_end_matches(')')
            .trim()
            .trim_end_matches(')')
            .trim();
        let expected = eval_c_int(rhs.split(',').next().unwrap_or("").trim()) as usize;
        let actual = match type_name {
            "kgds_cmd" => size_of::<kgds_cmd>(),
            "kgds_group" => size_of::<kgds_group>(),
            "kgds_image" => size_of::<kgds_image>(),
            "kgds_file_header" => size_of::<kgds_file_header>(),
            other => panic!("the header asserts a size for {other}, which kicad-gal does not mirror"),
        };
        assert_eq!(actual, expected, "sizeof({type_name}) drifted");
        found += 1;
    }
    // Two preprocessor arms, four assertions each.
    assert_eq!(found, 8, "the header's size assertions changed shape");
}

#[test]
fn serialised_field_order_matches_the_header() {
    let src = strip_comments(&header_text());

    // The file format writes its sections in the order the header declares the
    // counts, so this order is what `Stream::to_bytes` depends on.
    assert_eq!(
        struct_fields(&src, "kgds_file_header"),
        vec![
            "magic",
            "version",
            "flags",
            "reserved",
            "group_cmd_count",
            "group_coord_count",
            "group_count",
            "frame_cmd_count",
            "frame_coord_count",
            "string_bytes",
            "image_count",
            "image_bytes",
        ]
    );

    assert_eq!(
        struct_fields(&src, "kgds_stream_view"),
        vec![
            "version",
            "flags",
            "group_cmds",
            "group_cmd_count",
            "group_coords",
            "group_coord_count",
            "groups",
            "group_count",
            "frame_cmds",
            "frame_cmd_count",
            "frame_coords",
            "frame_coord_count",
            "strings",
            "string_bytes",
            "images",
            "image_count",
            "image_data",
            "image_bytes",
        ]
    );

    assert_eq!(
        struct_fields(&src, "kgds_cmd"),
        vec!["op", "flags", "arg0", "arg1", "arg2", "arg3", "arg4"]
    );
    assert_eq!(
        struct_fields(&src, "kgds_group"),
        vec!["id", "serial", "first_cmd", "cmd_count"]
    );
    assert_eq!(
        struct_fields(&src, "kgds_image"),
        vec![
            "width",
            "height",
            "format",
            "reserved",
            "data_offset",
            "data_length"
        ]
    );
}

#[test]
fn stream_view_is_the_size_the_producer_sees() {
    // Not asserted in the header, because its size is pointer-width dependent.
    // Asserting it here catches a field being added on one side only.
    assert_eq!(size_of::<kgds_stream_view>(), 136);
}

/// Parse `kgds_coord_refs` into `opcode -> [(arg slot, scalar count)]`.
///
/// The header calls this function the single source of truth for which parts of
/// the coordinate arena a command touches; both the producer's compaction and
/// this crate's bounds checking hang off it, so it is worth parsing rather than
/// trusting.
fn parse_coord_ref_table(src: &str) -> BTreeMap<String, Vec<CoordRefRule>> {
    let at = src
        .find("int kgds_coord_refs(")
        .or_else(|| src.find("int kgds_coord_refs ("))
        .expect("the header no longer defines kgds_coord_refs");
    let switch = src[at..]
        .find("switch")
        .expect("kgds_coord_refs has no switch")
        + at;
    let open = src[switch..].find('{').expect("switch has no body") + switch;

    // Walk to the matching close brace.
    let bytes = src.as_bytes();
    let mut depth = 0usize;
    let mut close = open;
    for (i, c) in bytes.iter().enumerate().skip(open) {
        match c {
            b'{' => depth += 1,
            b'}' => {
                depth -= 1;
                if depth == 0 {
                    close = i;
                    break;
                }
            }
            _ => {}
        }
    }
    let body = &src[open + 1..close];

    let mut table = BTreeMap::new();
    let mut pending: Vec<String> = Vec::new();
    let mut refs: Vec<CoordRefRule> = Vec::new();
    // A `KGDS_REF` guarded by `if( aCmd->flags & KGDS_FLAG_X )` only applies
    // when that bit is set; `DRAW_GROUP`'s optional depth override is the one
    // case that uses it.
    let mut guard: Option<String> = None;

    for raw in body.lines() {
        let line = raw.trim();
        if let Some(rest) = line.strip_prefix("case ") {
            if !refs.is_empty() {
                // A new case group started without an intervening `break`;
                // the header does not do that, so treat it as drift.
                panic!("kgds_coord_refs fell through a case group unexpectedly");
            }
            pending.push(rest.trim_end_matches(':').trim().to_string());
        } else if let Some(rest) = line.strip_prefix("if(") {
            let cond = rest.rsplit_once(')').map(|(c, _)| c).unwrap_or(rest);
            let flag = cond
                .split('&')
                .next_back()
                .expect("an if with no operand")
                .trim()
                .to_string();
            assert!(
                cond.contains("aCmd->flags"),
                "kgds_coord_refs grew a condition this test cannot evaluate: {line}"
            );
            guard = Some(flag);
        } else if let Some(rest) = line.strip_prefix("KGDS_REF(") {
            let inner = rest.rsplit_once(')').expect("malformed KGDS_REF").0;
            let (start, count) = inner.split_once(',').expect("KGDS_REF takes two arguments");
            let slot = start
                .trim()
                .trim_start_matches("aCmd->arg")
                .trim()
                .parse::<u32>()
                .expect("KGDS_REF start is not an arg slot");
            refs.push(CoordRefRule {
                slot,
                count: count.trim().to_string(),
                guard: guard.take(),
            });
        } else if line.starts_with("break;") {
            for name in pending.drain(..) {
                table.insert(name, refs.clone());
            }
            refs.clear();
            guard = None;
        } else if line.starts_with("default:") {
            pending.clear();
            guard = None;
        }
    }

    table
}

/// One `KGDS_REF` line from the header, with the flag that guards it if any.
#[derive(Clone, Debug)]
struct CoordRefRule {
    slot: u32,
    count: String,
    guard: Option<String>,
}

/// Evaluate a `KGDS_REF` count expression against a command.
fn eval_count(expr: &str, cmd: &kgds_cmd) -> u64 {
    let expr = expr.trim();
    if let Some((lhs, rhs)) = expr.split_once('*') {
        let l = eval_count(lhs, cmd);
        let r = eval_count(rhs, cmd);
        // Deliberately computed in u64, where the C function uses uint32_t; see
        // the note on `CoordRef`.
        return l * r;
    }
    if let Some(slot) = expr.trim().strip_prefix("aCmd->arg") {
        return match slot.trim() {
            "0" => cmd.arg0 as u64,
            "1" => cmd.arg1 as u64,
            "2" => cmd.arg2 as u64,
            "3" => cmd.arg3 as u64,
            "4" => cmd.arg4 as u64,
            other => panic!("unknown arg slot {other}"),
        };
    }
    eval_c_int(expr)
}

#[test]
fn coordinate_reference_table_matches_the_header() {
    let src = strip_comments(&header_text());
    let table = parse_coord_ref_table(&src);

    // Distinct, non-trivial arguments so that a swapped slot shows up as a
    // different `start` rather than coincidentally matching.
    let probe = |op: Op, flags: u16| kgds_cmd {
        op: op as u16,
        flags,
        arg0: 101,
        arg1: 7,
        arg2: 303,
        arg3: 404,
        arg4: 505,
    };

    // Flag combinations worth probing: none, and each bit the header defines.
    // A guarded reference only shows up in one of them, which is exactly the
    // behaviour that would otherwise drift unnoticed.
    let mut flag_sets = vec![0u16];
    let c_flags = enum_values(enum_body(&src, "kgds_flag"));
    for value in c_flags.values() {
        flag_sets.push(*value as u16);
    }
    flag_sets.push(flags::KNOWN);

    for op in Op::ALL {
        for set in &flag_sets {
            let cmd = probe(*op, *set);
            let rust = coord_refs(&cmd);
            let expected: Vec<&CoordRefRule> = table
                .get(op.c_name())
                .map(|rules| {
                    rules
                        .iter()
                        .filter(|r| match &r.guard {
                            None => true,
                            Some(flag) => (*set as u64 & c_flags[flag.as_str()]) != 0,
                        })
                        .collect()
                })
                .unwrap_or_default();

            assert_eq!(
                rust.len(),
                expected.len(),
                "{} with flags {set:#x}: header lists {} coordinate runs, kicad-gal produces {}",
                op.c_name(),
                expected.len(),
                rust.len()
            );

            for (i, rule) in expected.iter().enumerate() {
                let want_start = match rule.slot {
                    0 => cmd.arg0,
                    1 => cmd.arg1,
                    2 => cmd.arg2,
                    3 => cmd.arg3,
                    _ => cmd.arg4,
                };
                assert_eq!(
                    rust[i].start,
                    want_start,
                    "{} with flags {set:#x}: run {i} should start at arg{}",
                    op.c_name(),
                    rule.slot
                );
                assert_eq!(
                    rust[i].count,
                    eval_count(&rule.count, &cmd),
                    "{} with flags {set:#x}: run {i} length drifted",
                    op.c_name()
                );
            }
            assert!(
                rust.len() <= MAX_COORD_REFS,
                "{} exceeds KGDS_MAX_COORD_REFS",
                op.c_name()
            );
        }
    }
}

#[test]
fn grid_styles_match_the_gal_display_options_header() {
    // GRID_STYLE is not part of the draw-stream ABI: the stream passes it as a
    // plain double. That makes it exactly the kind of thing that drifts
    // silently, so pin it to its actual definition.
    let path = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("../../../include/gal/gal_display_options.h");
    let src = strip_comments(
        &std::fs::read_to_string(&path)
            .unwrap_or_else(|e| panic!("cannot read {}: {e}", path.display())),
    );
    let body = enum_body(&src, "class GRID_STYLE");
    let values = enum_values(body);
    assert_eq!(values["DOTS"], GridStyle::Dots as u64);
    assert_eq!(values["LINES"], GridStyle::Lines as u64);
    assert_eq!(values["SMALL_CROSS"], GridStyle::SmallCross as u64);
    assert_eq!(values.len(), 3, "GRID_STYLE gained a variant: {values:?}");
}
