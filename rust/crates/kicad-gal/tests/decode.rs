// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Decoder tests: round trips, and the rejection cases that matter because the
//! producer lives in another process.
//!
//! The corruption tests are the point of this file. A draw stream arrives
//! across an FFI boundary, so "malformed input produces a `DecodeError`" is a
//! safety property, not a nicety — and the only convincing way to test it is to
//! damage a valid stream in every way the harness can think of and require that
//! every outcome is either a clean parse or a clean error.

use kicad_gal::abi::{kgds_cmd, kgds_group, kgds_image, kgds_stream_view, Op};
use kicad_gal::{
    Color, Command, DecodeError, GridStyle, ImageFormat, Stream, StreamBuilder, StreamParts,
    StreamView, Target, KGDS_VERSION,
};

/// A stream that touches every opcode family, so that a round trip exercises
/// every argument shape rather than just the common ones.
fn rich_stream() -> Stream {
    let mut b = StreamBuilder::new();

    let checker: Vec<u8> = (0..(4 * 3 * 4)).map(|i| (i * 7 % 251) as u8).collect();
    let img = b.add_image(4, 3, ImageFormat::Rgba8, &checker);

    b.group(0, 1, |g| {
        g.set_is_stroke(true);
        g.set_is_fill(false);
        g.set_stroke_color(Color::new(0.1, 0.2, 0.3, 1.0));
        g.set_line_width(152_400.0);
        g.set_layer_depth(-12.0);
        g.line([0.0, 0.0], [2_540_000.0, 0.0]);
        g.segment([0.0, 0.0], [0.0, 2_540_000.0], 101_600.0);
        g.segment_chain(&[[0.0, 0.0], [1e6, 0.0], [1e6, 1e6]], 50_800.0);
        g.polyline(&[[0.0, 0.0], [1e6, 1e6], [2e6, 0.0]]);
        g.polyline_closed(&[[0.0, 0.0], [1e6, 0.0], [1e6, 1e6]]);
    });

    b.group(1, 4, |g| {
        g.set_is_fill(true);
        g.set_fill_color(Color::new(1.0, 0.5, 0.0, 0.8));
        g.polygon(&[[0.0, 0.0], [4e6, 0.0], [4e6, 4e6], [0.0, 4e6]]);
        g.polygon_hole(&[[1e6, 1e6], [3e6, 1e6], [3e6, 3e6], [1e6, 3e6]]);
        g.circle([5e6, 5e6], 1e6);
        g.arc([5e6, 5e6], 2e6, 0.0, std::f64::consts::PI);
        g.arc_segment([5e6, 5e6], 3e6, 0.25, 1.75, 76_200.0);
        g.rectangle([-1e6, -1e6], [1e6, 1e6]);
        g.curve([0.0, 0.0], [1e6, 2e6], [3e6, 2e6], [4e6, 0.0]);
        g.ellipse([0.0, 0.0], 3e6, 1e6, 0.4);
        g.ellipse_arc([0.0, 0.0], 3e6, 1e6, 0.4, 0.0, 2.0, 50_000.0);
        g.hole_wall([2e6, 2e6], 500_000.0, 25_400.0);
        g.set_glyph(true);
        g.polygon(&[[0.0, 0.0], [1e5, 0.0], [1e5, 1e5]]);
        g.set_glyph(false);
    });

    // Group 9 replays group 0, which exercises nested DRAW_GROUP resolution.
    b.group(9, 2, |g| {
        g.save();
        g.translate(1e7, 0.0);
        g.rotate(0.3);
        g.scale(2.0, 2.0);
        g.transform(kicad_gal::Affine::translation(5.0, 6.0));
        g.draw_group(0);
        g.restore();
    });

    b.begin_frame(1920, 1080);
    b.clear_screen(Color::new(0.05, 0.05, 0.07, 1.0));
    b.set_target(Target::NonCached);
    b.grid(
        [0.0, 0.0],
        [1_270_000.0, 1_270_000.0],
        1.0,
        GridStyle::Dots,
        Color::new(0.3, 0.3, 0.3, 1.0),
    );
    b.set_target(Target::Cached);
    b.set_min_line_width(1.0);
    b.enable_depth_test(true);
    b.set_negative_draw_mode(false);
    b.draw_group(0);
    b.draw_group(1);
    b.draw_group(9);
    b.start_diff_layer();
    b.end_diff_layer();
    b.start_negatives_layer();
    b.end_negatives_layer();
    b.set_target(Target::Overlay);
    b.clear_target(Target::Overlay);
    b.set_hover_color(Color::WHITE);
    b.bitmap(img, kicad_gal::Affine::translation(1e6, 1e6), 0.75);
    b.cursor([1e6, 2e6], Color::new(1.0, 1.0, 1.0, 1.0));
    b.nop();
    b.end_frame();

    b.finish().expect("the rich stream is valid")
}

#[test]
fn builder_output_round_trips_through_the_file_format() {
    let stream = rich_stream();
    let bytes = stream.to_bytes();

    // Sections are 8-byte aligned, so the whole file is.
    assert_eq!(bytes.len() % 8, 0);

    let back = Stream::from_bytes(&bytes).expect("round trip decodes");
    assert_eq!(stream, back);
    assert_eq!(back.to_bytes(), bytes, "re-serialising is byte-identical");
}

#[test]
fn round_trip_through_read_and_write() {
    let stream = rich_stream();
    let mut buf = Vec::new();
    stream.write(&mut buf).expect("write succeeds");
    let back = Stream::read(std::io::Cursor::new(&buf)).expect("read succeeds");
    assert_eq!(stream, back);
}

#[test]
fn geometry_decodes_to_the_values_that_were_recorded() {
    let mut b = StreamBuilder::new();
    b.segment([1.0, 2.0], [3.0, 4.0], 5.0);
    b.circle([6.0, 7.0], 8.0);
    b.arc([9.0, 10.0], 11.0, 12.0, 13.0);
    b.polyline_closed(&[[1.0, 1.0], [2.0, 2.0], [3.0, 3.0]]);
    b.polygon_hole(&[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]]);
    let s = b.finish().expect("valid");
    let v = s.view();
    let cmds: Vec<_> = v.frame().map(|i| i.command).collect();

    assert_eq!(
        cmds[0],
        Command::Segment {
            p0: [1.0, 2.0],
            p1: [3.0, 4.0],
            width: 5.0
        }
    );
    assert_eq!(
        cmds[1],
        Command::Circle {
            center: [6.0, 7.0],
            radius: 8.0
        }
    );
    assert_eq!(
        cmds[2],
        Command::Arc {
            center: [9.0, 10.0],
            radius: 11.0,
            start_angle: 12.0,
            end_angle: 13.0
        }
    );
    match cmds[3] {
        Command::Polyline { points, closed } => {
            assert!(closed);
            assert_eq!(points.len(), 3);
            assert_eq!(points.get(2), Some([3.0, 3.0]));
        }
        other => panic!("expected a polyline, got {other:?}"),
    }
    match cmds[4] {
        Command::Polygon { hole, .. } => assert!(hole),
        other => panic!("expected a polygon, got {other:?}"),
    }
}

#[test]
fn glyph_flag_survives_decoding() {
    let mut b = StreamBuilder::new();
    b.set_glyph(true);
    b.polygon(&[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]]);
    b.set_glyph(false);
    b.polygon(&[[0.0, 0.0], [1.0, 0.0], [1.0, 1.0]]);
    let s = b.finish().expect("valid");
    let v = s.view();
    let insts: Vec<_> = v.frame().collect();
    assert!(insts[0].is_glyph());
    assert!(!insts[1].is_glyph());
}

#[test]
fn clearing_the_frame_leaves_group_data_untouched() {
    // This is the property the whole retained-group design rests on: a pan
    // rebuilds the frame body and must not disturb a single group index, so the
    // renderer's (id, serial) cache can be trusted with no invalidation logic.
    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.segment([0.0, 0.0], [1e6, 0.0], 1e5);
        g.circle([1e6, 1e6], 5e5);
    });
    b.begin_frame(100, 100);
    b.draw_group(0);
    b.end_frame();
    let first = b.finish().expect("valid");

    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.segment([0.0, 0.0], [1e6, 0.0], 1e5);
        g.circle([1e6, 1e6], 5e5);
    });
    b.begin_frame(100, 100);
    b.draw_group(0);
    b.end_frame();
    b.clear_frame();
    // A second, different frame over the same document.
    b.begin_frame(100, 100);
    b.cursor([5e5, 5e5], Color::WHITE);
    b.draw_group(0);
    b.end_frame();
    let second = b.finish().expect("valid");

    assert_eq!(first.group_cmds(), second.group_cmds());
    assert_eq!(first.group_coords(), second.group_coords());
    assert_eq!(first.groups(), second.groups());
    assert_ne!(first.frame_cmds(), second.frame_cmds());
}

#[test]
fn group_ids_need_not_be_dense_or_ordered() {
    let mut b = StreamBuilder::new();
    b.group(4_000_000_000, 1, |g| {
        g.circle([0.0, 0.0], 1.0);
    });
    b.group(7, 1, |g| {
        g.circle([1.0, 1.0], 2.0);
    });
    b.draw_group(7);
    b.draw_group(4_000_000_000);
    let s = b.finish().expect("valid");
    let v = s.view();
    assert_eq!(v.group(7).map(|g| g.serial), Some(1));
    assert!(v.group_body(4_000_000_000).is_some());
    assert!(v.group(8).is_none());
}

// ---------------------------------------------------------------- rejections

/// Sections that decode cleanly on their own, used as a starting point for
/// deliberately breaking one thing at a time.
fn minimal_parts<'a>(
    group_cmds: &'a [kgds_cmd],
    group_coords: &'a [f64],
    groups: &'a [kgds_group],
    frame_cmds: &'a [kgds_cmd],
    frame_coords: &'a [f64],
) -> StreamParts<'a> {
    StreamParts {
        group_cmds,
        group_coords,
        groups,
        frame_cmds,
        frame_coords,
        ..StreamParts::default()
    }
}

fn cmd(op: Op, arg0: u32, arg1: u32) -> kgds_cmd {
    kgds_cmd {
        op: op as u16,
        flags: 0,
        arg0,
        arg1,
        arg2: 0,
        arg3: 0,
        arg4: 0,
    }
}

#[test]
fn rejects_an_unknown_abi_version() {
    let parts = StreamParts {
        version: KGDS_VERSION + 1,
        ..StreamParts::default()
    };
    assert!(matches!(
        StreamView::new(parts),
        Err(DecodeError::UnsupportedVersion { .. })
    ));
}

#[test]
fn rejects_an_unknown_opcode() {
    let cmds = [cmd(Op::Nop, 0, 0); 1].map(|mut c| {
        c.op = 0x7F;
        c
    });
    let err = StreamView::new(minimal_parts(&[], &[], &[], &cmds, &[])).unwrap_err();
    assert!(matches!(err, DecodeError::UnknownOpcode { raw: 0x7F, .. }));
}

#[test]
fn rejects_an_unknown_flag_bit() {
    let mut c = cmd(Op::Circle, 0, 0);
    c.flags = 1 << 5;
    let coords = [0.0, 0.0, 1.0];
    let err = StreamView::new(minimal_parts(&[], &[], &[], &[c], &coords)).unwrap_err();
    assert!(matches!(err, DecodeError::UnknownFlags { .. }));
}

#[test]
fn rejects_a_coordinate_index_past_the_end_of_its_arena() {
    // Three coordinates are needed for a circle; only two are present.
    let err = StreamView::new(minimal_parts(
        &[],
        &[],
        &[],
        &[cmd(Op::Circle, 0, 0)],
        &[0.0, 0.0],
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::CoordOutOfRange { .. }));
}

#[test]
fn rejects_a_point_run_that_overflows_its_arena() {
    // A polyline claiming four billion points cannot be satisfied, and the
    // length computation must not wrap into something that can.
    let err = StreamView::new(minimal_parts(
        &[],
        &[],
        &[],
        &[cmd(Op::Polyline, 0, u32::MAX)],
        &[0.0, 0.0, 1.0, 1.0],
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::CoordOutOfRange { .. }));

    // 0x80000000 points is 2^32 scalars, which wraps to zero in uint32_t
    // arithmetic. It must still be rejected rather than read as an empty run.
    let err = StreamView::new(minimal_parts(
        &[],
        &[],
        &[],
        &[cmd(Op::Polyline, 0, 0x8000_0000)],
        &[0.0, 0.0],
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::CoordOutOfRange { .. }));
}

#[test]
fn a_frame_command_may_not_reach_into_the_group_arena() {
    // The group arena has plenty of coordinates; the frame arena has none. A
    // frame command must be checked against its own arena only.
    let group_coords = [0.0, 0.0, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0];
    let err = StreamView::new(minimal_parts(
        &[],
        &group_coords,
        &[],
        &[cmd(Op::Circle, 0, 0)],
        &[],
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::CoordOutOfRange { .. }));
}

#[test]
fn rejects_a_group_body_outside_the_group_command_array() {
    let groups = [kgds_group {
        id: 0,
        serial: 1,
        first_cmd: 0,
        cmd_count: 5,
    }];
    let err =
        StreamView::new(minimal_parts(&[cmd(Op::Nop, 0, 0)], &[], &groups, &[], &[])).unwrap_err();
    assert!(matches!(err, DecodeError::GroupOutOfRange { .. }));
}

#[test]
fn rejects_a_draw_group_of_an_unknown_id() {
    let err = StreamView::new(minimal_parts(
        &[],
        &[],
        &[],
        &[cmd(Op::DrawGroup, 3, 0)],
        &[],
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::UnknownGroupId { id: 3, .. }));
}

#[test]
fn rejects_duplicate_group_ids() {
    let groups = [
        kgds_group {
            id: 5,
            serial: 0,
            first_cmd: 0,
            cmd_count: 0,
        },
        kgds_group {
            id: 5,
            serial: 0,
            first_cmd: 0,
            cmd_count: 0,
        },
    ];
    let err = StreamView::new(minimal_parts(&[], &[], &groups, &[], &[])).unwrap_err();
    assert!(matches!(err, DecodeError::DuplicateGroupId { id: 5 }));
}

#[test]
fn rejects_a_group_that_draws_itself() {
    // Without this check, replay would recurse until the stack ran out, on
    // input that came from another process.
    let group_cmds = [cmd(Op::DrawGroup, 0, 0)];
    let groups = [kgds_group {
        id: 0,
        serial: 1,
        first_cmd: 0,
        cmd_count: 1,
    }];
    let err = StreamView::new(minimal_parts(&group_cmds, &[], &groups, &[], &[])).unwrap_err();
    assert!(matches!(err, DecodeError::GroupCycle { id: 0 }));
}

#[test]
fn rejects_a_longer_group_cycle() {
    let group_cmds = [cmd(Op::DrawGroup, 1, 0), cmd(Op::DrawGroup, 0, 0)];
    let groups = [
        kgds_group {
            id: 0,
            serial: 1,
            first_cmd: 0,
            cmd_count: 1,
        },
        kgds_group {
            id: 1,
            serial: 1,
            first_cmd: 1,
            cmd_count: 1,
        },
    ];
    let err = StreamView::new(minimal_parts(&group_cmds, &[], &groups, &[], &[])).unwrap_err();
    assert!(matches!(err, DecodeError::GroupCycle { .. }));
}

#[test]
fn accepts_a_diamond_of_group_references() {
    // Two groups referring to a third is not a cycle, and rejecting it would be
    // a false positive on perfectly ordinary reuse.
    let group_cmds = [
        cmd(Op::Nop, 0, 0),
        cmd(Op::DrawGroup, 0, 0),
        cmd(Op::DrawGroup, 0, 0),
    ];
    let groups = [
        kgds_group {
            id: 0,
            serial: 1,
            first_cmd: 0,
            cmd_count: 1,
        },
        kgds_group {
            id: 1,
            serial: 1,
            first_cmd: 1,
            cmd_count: 1,
        },
        kgds_group {
            id: 2,
            serial: 1,
            first_cmd: 2,
            cmd_count: 1,
        },
    ];
    assert!(StreamView::new(minimal_parts(&group_cmds, &[], &groups, &[], &[])).is_ok());
}

#[test]
fn rejects_non_finite_coordinates() {
    for bad in [f64::NAN, f64::INFINITY, f64::NEG_INFINITY] {
        let coords = [0.0, 0.0, bad];
        let err = StreamView::new(minimal_parts(
            &[],
            &[],
            &[],
            &[cmd(Op::Circle, 0, 0)],
            &coords,
        ))
        .unwrap_err();
        assert!(
            matches!(err, DecodeError::NonFiniteCoord { .. }),
            "{bad} was not rejected: {err}"
        );
    }
}

#[test]
fn rejects_an_unknown_render_target() {
    let err = StreamView::new(minimal_parts(
        &[],
        &[],
        &[],
        &[cmd(Op::SetTarget, 9, 0)],
        &[],
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::UnknownTarget { raw: 9, .. }));
}

#[test]
fn rejects_an_unknown_grid_style() {
    let coords = [0.0, 0.0, 1.0, 1.0, 1.0, 17.0];
    let err = StreamView::new(minimal_parts(
        &[],
        &[],
        &[],
        &[cmd(Op::Grid, 0, 0)],
        &coords,
    ))
    .unwrap_err();
    assert!(matches!(err, DecodeError::UnknownGridStyle { .. }));

    // A fractional style is not a style either.
    let coords = [0.0, 0.0, 1.0, 1.0, 1.0, 0.5];
    assert!(matches!(
        StreamView::new(minimal_parts(
            &[],
            &[],
            &[],
            &[cmd(Op::Grid, 0, 0)],
            &coords
        )),
        Err(DecodeError::UnknownGridStyle { .. })
    ));
}

#[test]
fn rejects_a_bitmap_of_a_missing_image() {
    let coords = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 1.0];
    let mut c = cmd(Op::Bitmap, 0, 0);
    c.arg2 = 6;
    let err = StreamView::new(minimal_parts(&[], &[], &[], &[c], &coords)).unwrap_err();
    assert!(matches!(err, DecodeError::ImageOutOfRange { .. }));
}

#[test]
fn rejects_image_pixels_outside_the_image_arena() {
    let images = [kgds_image {
        width: 2,
        height: 2,
        format: 0,
        reserved: 0,
        data_offset: 8,
        data_length: 16,
    }];
    let parts = StreamParts {
        images: &images,
        image_data: &[0u8; 16],
        ..StreamParts::default()
    };
    assert!(matches!(
        StreamView::new(parts),
        Err(DecodeError::ImageDataOutOfRange { .. })
    ));
}

#[test]
fn rejects_an_image_whose_size_disagrees_with_its_dimensions() {
    let images = [kgds_image {
        width: 4,
        height: 4,
        format: 0,
        reserved: 0,
        data_offset: 0,
        data_length: 16, // should be 4 * 4 * 4
    }];
    let parts = StreamParts {
        images: &images,
        image_data: &[0u8; 16],
        ..StreamParts::default()
    };
    assert!(matches!(
        StreamView::new(parts),
        Err(DecodeError::ImageSizeMismatch { .. })
    ));
}

#[test]
fn rejects_an_unknown_image_format() {
    let images = [kgds_image {
        width: 1,
        height: 1,
        format: 42,
        reserved: 0,
        data_offset: 0,
        data_length: 4,
    }];
    let parts = StreamParts {
        images: &images,
        image_data: &[0u8; 4],
        ..StreamParts::default()
    };
    assert!(matches!(
        StreamView::new(parts),
        Err(DecodeError::UnknownImageFormat { raw: 42, .. })
    ));
}

// ------------------------------------------------------------ file rejection

#[test]
fn rejects_a_file_that_is_not_a_stream() {
    assert!(matches!(
        Stream::from_bytes(&[0u8; 80]),
        Err(DecodeError::BadMagic { .. })
    ));
    assert!(matches!(
        Stream::from_bytes(b"short"),
        Err(DecodeError::Truncated { .. })
    ));
}

#[test]
fn rejects_a_file_from_a_future_abi_version() {
    let mut bytes = rich_stream().to_bytes();
    bytes[4..8].copy_from_slice(&(KGDS_VERSION + 1).to_le_bytes());
    assert!(matches!(
        Stream::from_bytes(&bytes),
        Err(DecodeError::UnsupportedVersion { .. })
    ));
}

#[test]
fn every_truncation_of_a_valid_file_is_a_clean_error() {
    let bytes = rich_stream().to_bytes();
    for cut in 0..bytes.len() {
        // Any error is fine; not panicking, and not accepting a truncated
        // stream as valid, is the point.
        if Stream::from_bytes(&bytes[..cut]).is_ok() {
            panic!(
                "a stream truncated to {cut} of {} bytes parsed",
                bytes.len()
            );
        }
    }
    assert!(Stream::from_bytes(&bytes).is_ok());
}

#[test]
fn a_header_claiming_an_absurd_section_size_fails_rather_than_allocating() {
    let mut bytes = rich_stream().to_bytes();
    // group_cmd_count at offset 16.
    bytes[16..24].copy_from_slice(&u64::MAX.to_le_bytes());
    assert!(Stream::from_bytes(&bytes).is_err());

    let mut bytes = rich_stream().to_bytes();
    // image_bytes at offset 72.
    bytes[72..80].copy_from_slice(&(1u64 << 60).to_le_bytes());
    assert!(Stream::from_bytes(&bytes).is_err());
}

/// xorshift64*, so that the corruption sweep is reproducible without a
/// dependency.
struct Rng(u64);

impl Rng {
    fn next_u64(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_F491_4F6C_DD1D)
    }
}

#[test]
fn corrupting_any_byte_of_a_valid_file_never_panics() {
    let original = rich_stream().to_bytes();
    let mut rng = Rng(0x1234_5678_9ABC_DEF0);
    let mut accepted = 0usize;

    // Every byte, with several replacement values each: enough to reach the
    // interesting shapes (huge counts, bogus opcodes, NaN bit patterns) without
    // taking long enough to be skipped in CI.
    for i in 0..original.len() {
        for _ in 0..4 {
            let mut bytes = original.clone();
            bytes[i] = (rng.next_u64() & 0xFF) as u8;
            if Stream::from_bytes(&bytes).is_ok() {
                accepted += 1;
            }
        }
    }

    // Some single-byte edits land in padding or in a value that is still legal
    // (a colour, a coordinate), so acceptance is expected; the assertion worth
    // making is that most edits are caught, and that none of them panicked.
    assert!(
        accepted < original.len() * 4,
        "corruption was never detected, which means nothing is being checked"
    );
}

#[test]
fn truncating_the_command_array_in_a_view_is_a_clean_error() {
    // The serialised path is not the only way in: the FFI path hands over
    // already-split sections, and a producer that mis-set one count must be
    // caught there too.
    let stream = rich_stream();
    for drop in 1..=stream.group_cmds().len().min(8) {
        let short = &stream.group_cmds()[..stream.group_cmds().len() - drop];
        let parts = StreamParts {
            group_cmds: short,
            group_coords: stream.group_coords(),
            groups: stream.groups(),
            frame_cmds: stream.frame_cmds(),
            frame_coords: stream.frame_coords(),
            images: stream.images(),
            ..StreamParts::default()
        };
        assert!(
            StreamView::new(parts).is_err(),
            "a group table pointing past a shortened command array was accepted"
        );
    }
}

// ------------------------------------------------------------------- from_raw

#[test]
fn from_raw_accepts_a_well_formed_view() {
    let stream = rich_stream();
    let raw = kgds_stream_view {
        version: stream.version(),
        flags: stream.flags(),
        group_cmds: stream.group_cmds().as_ptr(),
        group_cmd_count: stream.group_cmds().len(),
        group_coords: stream.group_coords().as_ptr(),
        group_coord_count: stream.group_coords().len(),
        groups: stream.groups().as_ptr(),
        group_count: stream.groups().len(),
        frame_cmds: stream.frame_cmds().as_ptr(),
        frame_cmd_count: stream.frame_cmds().len(),
        frame_coords: stream.frame_coords().as_ptr(),
        frame_coord_count: stream.frame_coords().len(),
        strings: std::ptr::null(),
        string_bytes: 0,
        images: stream.images().as_ptr(),
        image_count: stream.images().len(),
        image_data: std::ptr::null(),
        image_bytes: 0,
    };

    // The image arena is reachable through the owned stream, so point at it
    // properly rather than leaving images unbacked.
    let pixels = stream.view().image_pixels(0).expect("image 0 has pixels");
    let raw = kgds_stream_view {
        image_data: pixels.as_ptr(),
        image_bytes: pixels.len(),
        ..raw
    };

    // SAFETY: every pointer above comes from a live slice of `stream`, which
    // outlives the view; nothing mutates it while the view exists.
    let view = unsafe { StreamView::from_raw(&raw) }.expect("a faithful view decodes");
    assert_eq!(view.groups().len(), stream.groups().len());
    assert_eq!(view.frame().count(), stream.frame_cmds().len());
}

#[test]
fn from_raw_rejects_a_null_pointer_with_a_non_zero_count() {
    let raw = kgds_stream_view {
        version: KGDS_VERSION,
        flags: 0,
        group_cmds: std::ptr::null(),
        group_cmd_count: 4,
        group_coords: std::ptr::null(),
        group_coord_count: 0,
        groups: std::ptr::null(),
        group_count: 0,
        frame_cmds: std::ptr::null(),
        frame_cmd_count: 0,
        frame_coords: std::ptr::null(),
        frame_coord_count: 0,
        strings: std::ptr::null(),
        string_bytes: 0,
        images: std::ptr::null(),
        image_count: 0,
        image_data: std::ptr::null(),
        image_bytes: 0,
    };
    // SAFETY: the only non-zero count has a null pointer, which is exactly what
    // this call must detect before forming a slice.
    let err = unsafe { StreamView::from_raw(&raw) }.unwrap_err();
    assert!(matches!(err, DecodeError::NullSection { .. }));
}

#[test]
fn from_raw_rejects_a_misaligned_section() {
    let backing = [0u8; 64];
    // One byte in, so the pointer cannot be an aligned `double`.
    let misaligned = unsafe { backing.as_ptr().add(1) } as *const f64;
    let raw = kgds_stream_view {
        version: KGDS_VERSION,
        flags: 0,
        group_cmds: std::ptr::null(),
        group_cmd_count: 0,
        group_coords: misaligned,
        group_coord_count: 4,
        groups: std::ptr::null(),
        group_count: 0,
        frame_cmds: std::ptr::null(),
        frame_cmd_count: 0,
        frame_coords: std::ptr::null(),
        frame_coord_count: 0,
        strings: std::ptr::null(),
        string_bytes: 0,
        images: std::ptr::null(),
        image_count: 0,
        image_data: std::ptr::null(),
        image_bytes: 0,
    };
    // SAFETY: the misaligned section is rejected before any slice is formed, so
    // the pointer is never dereferenced.
    let err = unsafe { StreamView::from_raw(&raw) }.unwrap_err();
    assert!(matches!(err, DecodeError::MisalignedSection { .. }));
}

#[test]
fn an_empty_stream_is_valid() {
    let s = StreamBuilder::new().finish().expect("empty is valid");
    assert!(s.is_empty());
    assert_eq!(s.view().frame().count(), 0);
    let back = Stream::from_bytes(&s.to_bytes()).expect("an empty stream round trips");
    assert_eq!(s, back);
}

// -------------------------------------------------------------- builder misuse

#[test]
fn builder_rejects_nested_groups() {
    let mut b = StreamBuilder::new();
    b.begin_group(0, 1);
    b.begin_group(1, 1);
    b.end_group();
    assert!(b.finish().is_err());
}

#[test]
fn builder_rejects_an_unterminated_group() {
    let mut b = StreamBuilder::new();
    b.begin_group(0, 1);
    b.circle([0.0, 0.0], 1.0);
    assert!(b.finish().is_err());
}

#[test]
fn builder_rejects_a_duplicate_group_id() {
    let mut b = StreamBuilder::new();
    b.group(3, 1, |g| {
        g.circle([0.0, 0.0], 1.0);
    });
    b.group(3, 2, |g| {
        g.circle([0.0, 0.0], 1.0);
    });
    assert!(b.finish().is_err());
}

#[test]
fn builder_rejects_an_image_of_the_wrong_length() {
    let mut b = StreamBuilder::new();
    b.add_image(2, 2, ImageFormat::Rgba8, &[0u8; 8]);
    assert!(b.finish().is_err());
}

#[test]
fn builder_allows_a_group_to_be_recorded_after_a_frame() {
    // VIEW records an item into a group during its update pass, which can
    // happen after a frame has already been drawn. The two arenas exist so that
    // this is not a special case.
    let mut b = StreamBuilder::new();
    b.begin_frame(10, 10);
    b.end_frame();
    b.group(0, 1, |g| {
        g.circle([0.0, 0.0], 1.0);
    });
    b.clear_frame();
    b.begin_frame(10, 10);
    b.draw_group(0);
    b.end_frame();
    let s = b.finish().expect("valid");
    assert_eq!(s.groups().len(), 1);
}
