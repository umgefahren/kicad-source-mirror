// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Translation tests: a stream in, geometry out.
//!
//! gpui supplies no headless renderer on Linux, so golden images are not on
//! offer and this is the layer worth testing instead. It is also the more
//! useful layer: a golden image tells you a frame changed, whereas these say
//! which rule broke.

use kicad_gal::{Affine, Color, GridStyle, Stream, StreamBuilder};
use kicad_sch_render::camera::WorldRect;
use kicad_sch_render::scene::Batch;
use kicad_sch_render::{translate_frame, translate_group, Camera, Projector, SchematicRenderer};

/// One millimetre in KiCad internal units.
const MM: f64 = 1_000_000.0;

fn screen_projector(camera: &Camera) -> Projector {
    Projector::new(
        camera.center(),
        camera.scale(),
        [camera.viewport()[0] * 0.5, camera.viewport()[1] * 0.5],
    )
}

/// A camera at one pixel per 10 000 internal units (100 px per 10 mm), over a
/// sheet placed far from the origin so that precision is under test throughout.
fn camera() -> Camera {
    Camera::new([1.2e9, -9.0e8], 1.0e-4, [800.0, 600.0])
}

fn frame_of(stream: &Stream, camera: &Camera) -> kicad_sch_render::Frame {
    let view = stream.view();
    translate_frame(
        &view,
        screen_projector(camera),
        &camera.visible_world_rect(64.0),
    )
}

fn only_geometry(frame: &kicad_sch_render::Frame) -> &kicad_sch_render::Geometry {
    match frame.items.first() {
        Some(kicad_sch_render::FrameItem::Geometry(g)) => g,
        other => panic!("expected geometry, got {other:?}"),
    }
}

#[test]
fn a_segment_becomes_one_stroke_with_the_recorded_colour_and_width() {
    let c = camera();
    let mut b = StreamBuilder::new();
    b.set_stroke_color(Color::new(0.0, 1.0, 0.0, 1.0));
    b.segment([c.center()[0], c.center()[1]], [c.center()[0] + MM, c.center()[1]], 0.15 * MM);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    let g = only_geometry(&frame);
    assert_eq!(g.batches.len(), 1);
    match &g.batches[0] {
        Batch::Stroke {
            color,
            width_px,
            polylines,
        } => {
            assert_eq!(*color, Color::new(0.0, 1.0, 0.0, 1.0).to_packed());
            // 0.15 mm at 1e-4 px per unit is 15 px.
            assert!((width_px - 15.0).abs() < 0.01, "{width_px}");
            assert_eq!(polylines.len(), 1);
            assert_eq!(polylines[0].points.len(), 2);
            // The segment starts at the camera centre, which is the middle of
            // the viewport, and runs 100 px to the right.
            let p = polylines[0].points[0];
            assert!((p[0] - 400.0).abs() < 0.01 && (p[1] - 300.0).abs() < 0.01, "{p:?}");
            let q = polylines[0].points[1];
            assert!((q[0] - 500.0).abs() < 0.01, "{q:?}");
        }
        other => panic!("expected a stroke, got {other:?}"),
    }
}

#[test]
fn a_hairline_is_floored_at_the_minimum_line_width() {
    let c = camera();
    let mut b = StreamBuilder::new();
    b.set_min_line_width(2.0);
    // One internal unit wide: 1e-4 px before the floor.
    b.set_line_width(1.0);
    b.line([c.center()[0], c.center()[1]], [c.center()[0] + MM, c.center()[1]]);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    match &only_geometry(&frame).batches[0] {
        Batch::Stroke { width_px, .. } => assert_eq!(*width_px, 2.0),
        other => panic!("expected a stroke, got {other:?}"),
    }
}

#[test]
fn a_filled_and_stroked_shape_emits_both_in_that_order() {
    let c = camera();
    let mut b = StreamBuilder::new();
    b.set_is_fill(true);
    b.set_is_stroke(true);
    b.set_fill_color(Color::new(1.0, 0.0, 0.0, 1.0));
    b.set_stroke_color(Color::new(0.0, 0.0, 1.0, 1.0));
    b.rectangle(c.center(), [c.center()[0] + MM, c.center()[1] + MM]);
    let s = b.finish().expect("valid");

    let g = frame_of(&s, &c);
    let g = only_geometry(&g);
    assert_eq!(g.batches.len(), 2);
    // Fill first, then the outline over it: painter's order, and the order the
    // GAL backends use.
    assert!(matches!(g.batches[0], Batch::Fill { .. }));
    assert!(matches!(g.batches[1], Batch::Stroke { .. }));
}

#[test]
fn a_polygon_hole_is_wound_against_its_outline() {
    // Non-zero fill only subtracts a hole when it winds the other way, and
    // SHAPE_POLY_SET makes no promise about that.
    let c = camera();
    let o = c.center();
    let outline = [
        [o[0], o[1]],
        [o[0] + 4.0 * MM, o[1]],
        [o[0] + 4.0 * MM, o[1] + 4.0 * MM],
        [o[0], o[1] + 4.0 * MM],
    ];
    // Deliberately the same winding as the outline.
    let hole = [
        [o[0] + MM, o[1] + MM],
        [o[0] + 3.0 * MM, o[1] + MM],
        [o[0] + 3.0 * MM, o[1] + 3.0 * MM],
        [o[0] + MM, o[1] + 3.0 * MM],
    ];

    let mut b = StreamBuilder::new();
    b.set_is_fill(true);
    b.set_is_stroke(false);
    b.polygon(&outline);
    b.polygon_hole(&hole);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    match &only_geometry(&frame).batches[0] {
        Batch::Fill { contours, .. } => {
            assert_eq!(contours.len(), 2);
            let a = kicad_sch_render::scene::signed_area2(&contours[0].points);
            let b = kicad_sch_render::scene::signed_area2(&contours[1].points);
            assert!(a * b < 0.0, "hole was not reversed: {a} and {b}");
        }
        other => panic!("expected a fill, got {other:?}"),
    }
}

#[test]
fn a_hole_with_no_outline_before_it_is_dropped_not_filled() {
    let c = camera();
    let o = c.center();
    let mut b = StreamBuilder::new();
    b.set_is_fill(true);
    b.set_is_stroke(false);
    b.polygon_hole(&[[o[0], o[1]], [o[0] + MM, o[1]], [o[0] + MM, o[1] + MM]]);
    let s = b.finish().expect("valid");
    assert!(frame_of(&s, &c).items.is_empty());
}

#[test]
fn transforms_compose_and_the_stack_restores_them() {
    let c = camera();
    let mut b = StreamBuilder::new();
    b.save();
    b.translate(c.center()[0], c.center()[1]);
    b.scale(2.0, 2.0);
    b.segment([0.0, 0.0], [MM, 0.0], 0.1 * MM);
    b.restore();
    // After the restore the transform is back to the identity, so this segment
    // lands near the world origin, far off screen.
    b.segment([0.0, 0.0], [MM, 0.0], 0.1 * MM);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    let g = only_geometry(&frame);
    let Batch::Stroke {
        polylines,
        width_px,
        ..
    } = &g.batches[0]
    else {
        panic!("expected strokes");
    };
    // The transformed segment starts at the viewport centre and is twice as
    // long and twice as wide as the untransformed one.
    let p = polylines[0].points[0];
    assert!((p[0] - 400.0).abs() < 0.01 && (p[1] - 300.0).abs() < 0.01, "{p:?}");
    let q = polylines[0].points[1];
    assert!((q[0] - 600.0).abs() < 0.01, "expected 200 px, got {q:?}");
    assert!((width_px - 20.0).abs() < 0.01, "{width_px}");

    // The second segment is back at the world origin: a long way off screen.
    let far = polylines
        .iter()
        .chain(g.batches.iter().filter_map(|b| match b {
            Batch::Stroke { polylines, .. } => polylines.first(),
            _ => None,
        }))
        .any(|p| p.points[0][0] < -1000.0);
    assert!(far, "the restore did not put the transform back");
}

#[test]
fn rotation_matches_the_recorded_angle() {
    let c = camera();
    let mut b = StreamBuilder::new();
    b.translate(c.center()[0], c.center()[1]);
    b.rotate(std::f64::consts::FRAC_PI_2);
    b.segment([0.0, 0.0], [MM, 0.0], 0.1 * MM);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    let Batch::Stroke { polylines, .. } = &only_geometry(&frame).batches[0] else {
        panic!("expected a stroke");
    };
    let q = polylines[0].points[1];
    // A quarter turn takes +x to +y.
    assert!((q[0] - 400.0).abs() < 0.05, "{q:?}");
    assert!((q[1] - 400.0).abs() < 0.05, "{q:?}");
}

#[test]
fn an_arc_is_flattened_more_finely_as_it_is_zoomed_in() {
    let mut b = StreamBuilder::new();
    b.set_stroke_color(Color::WHITE);
    b.arc([1.2e9, -9.0e8], 10.0 * MM, 0.0, std::f64::consts::PI);
    let s = b.finish().expect("valid");

    let count_at = |scale: f64| {
        let c = Camera::new([1.2e9, -9.0e8], scale, [800.0, 600.0]);
        let frame = frame_of(&s, &c);
        match &only_geometry(&frame).batches[0] {
            Batch::Stroke { polylines, .. } => polylines[0].points.len(),
            other => panic!("expected a stroke, got {other:?}"),
        }
    };
    let coarse = count_at(1.0e-5);
    let fine = count_at(1.0e-3);
    assert!(fine > coarse * 3, "{coarse} then {fine}");
}

#[test]
fn a_circle_becomes_a_closed_shape_rather_than_an_open_one() {
    let c = camera();
    let mut b = StreamBuilder::new();
    b.circle(c.center(), MM);
    let s = b.finish().expect("valid");
    let frame = frame_of(&s, &c);
    match &only_geometry(&frame).batches[0] {
        Batch::Stroke { polylines, .. } => assert!(polylines[0].closed),
        other => panic!("expected a stroke, got {other:?}"),
    }
}

#[test]
fn consecutive_matching_primitives_share_a_batch_and_a_change_splits_it() {
    let c = camera();
    let o = c.center();
    let mut b = StreamBuilder::new();
    b.set_stroke_color(Color::new(1.0, 0.0, 0.0, 1.0));
    for i in 0..50 {
        let y = o[1] + i as f64 * 0.1 * MM;
        b.segment([o[0], y], [o[0] + MM, y], 0.05 * MM);
    }
    b.set_stroke_color(Color::new(0.0, 0.0, 1.0, 1.0));
    b.segment([o[0], o[1]], [o[0] + MM, o[1]], 0.05 * MM);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    let g = only_geometry(&frame);
    assert_eq!(g.batches.len(), 2, "colour change should start a new batch");
    assert_eq!(g.batches[0].shape_count(), 50);
    assert_eq!(g.batches[1].shape_count(), 1);
}

#[test]
fn a_group_body_is_reordered_by_depth_but_the_frame_body_is_not() {
    let c = camera();
    let o = c.center();

    // Inside a group: two depths, interleaved. The near one must end up last.
    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.set_stroke_color(Color::new(1.0, 0.0, 0.0, 1.0));
        g.set_layer_depth(-10.0); // near
        g.segment([o[0], o[1]], [o[0] + MM, o[1]], 0.05 * MM);
        g.set_stroke_color(Color::new(0.0, 0.0, 1.0, 1.0));
        g.set_layer_depth(10.0); // far
        g.segment([o[0], o[1]], [o[0] + MM, o[1]], 0.05 * MM);
        g.set_stroke_color(Color::new(1.0, 0.0, 0.0, 1.0));
        g.set_layer_depth(-10.0);
        g.segment([o[0], o[1] + MM], [o[0] + MM, o[1] + MM], 0.05 * MM);
    });
    let s = b.finish().expect("valid");
    let view = s.view();
    let g = translate_group(&view, 0, 1.0e-4).expect("group 0 exists");
    assert_eq!(
        g.geometry.batches.len(),
        2,
        "the two near strokes should have batched together"
    );
    let Batch::Stroke { color, .. } = &g.geometry.batches[0] else {
        panic!("expected a stroke");
    };
    assert_eq!(
        *color,
        Color::new(0.0, 0.0, 1.0, 1.0).to_packed(),
        "the far stroke must be painted first"
    );
    assert_eq!(g.min_depth, -10.0);

    // The same sequence in the frame body keeps stream order: VIEW chose it.
    let mut b = StreamBuilder::new();
    b.set_stroke_color(Color::new(1.0, 0.0, 0.0, 1.0));
    b.set_layer_depth(-10.0);
    b.segment([o[0], o[1]], [o[0] + MM, o[1]], 0.05 * MM);
    b.set_stroke_color(Color::new(0.0, 0.0, 1.0, 1.0));
    b.set_layer_depth(10.0);
    b.segment([o[0], o[1]], [o[0] + MM, o[1]], 0.05 * MM);
    b.set_stroke_color(Color::new(1.0, 0.0, 0.0, 1.0));
    b.set_layer_depth(-10.0);
    b.segment([o[0], o[1] + MM], [o[0] + MM, o[1] + MM], 0.05 * MM);
    let s = b.finish().expect("valid");
    let frame = frame_of(&s, &c);
    assert_eq!(only_geometry(&frame).batches.len(), 3);
}

#[test]
fn frame_items_keep_the_order_groups_and_geometry_were_recorded_in() {
    use kicad_sch_render::FrameItem;
    let c = camera();
    let o = c.center();
    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.circle([o[0], o[1]], MM);
    });
    b.group(1, 1, |g| {
        g.circle([o[0], o[1]], MM);
    });
    b.segment([o[0], o[1]], [o[0] + MM, o[1]], MM * 0.1);
    b.draw_group(0);
    b.segment([o[0], o[1]], [o[0] + MM, o[1]], MM * 0.1);
    b.draw_group(1);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    assert!(matches!(frame.items[0], FrameItem::Geometry(_)));
    assert!(matches!(frame.items[1], FrameItem::Group { id: 0, .. }));
    assert!(matches!(frame.items[2], FrameItem::Geometry(_)));
    assert!(matches!(frame.items[3], FrameItem::Group { id: 1, .. }));
    assert_eq!(frame.items.len(), 4);
}

#[test]
fn the_group_colour_override_is_carried_rather_than_baked_in() {
    let c = camera();
    let o = c.center();
    let red = Color::new(1.0, 0.0, 0.0, 1.0);
    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.set_stroke_color(Color::new(1.0, 1.0, 1.0, 1.0));
        g.circle([o[0], o[1]], MM);
    });
    b.draw_group_with(0, Some(red), Some(-100.0));
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    match &frame.items[0] {
        kicad_sch_render::FrameItem::Group {
            id,
            color_override,
            depth_override,
        } => {
            assert_eq!(*id, 0);
            assert_eq!(*color_override, Some(red));
            assert_eq!(*depth_override, Some(-100.0));
        }
        other => panic!("expected a group, got {other:?}"),
    }

    // The group's own geometry is untouched, which is what keeps the cached
    // tessellation valid across a selection change.
    let view = s.view();
    let g = translate_group(&view, 0, 1.0e-4).expect("group 0 exists");
    let Batch::Stroke { color, .. } = &g.geometry.batches[0] else {
        panic!("expected a stroke");
    };
    assert_eq!(*color, Color::new(1.0, 1.0, 1.0, 1.0).to_packed());
}

// ------------------------------------------------------------------- the grid

fn grid_frame(style: GridStyle, pitch: f64, scale: f64) -> kicad_sch_render::Frame {
    let c = Camera::new([0.0, 0.0], scale, [800.0, 600.0]);
    let mut b = StreamBuilder::new();
    b.grid(
        [0.0, 0.0],
        [pitch, pitch],
        1.0,
        style,
        Color::new(0.4, 0.4, 0.4, 1.0),
    );
    let s = b.finish().expect("valid");
    frame_of(&s, &c)
}

#[test]
fn a_dotted_grid_becomes_quads() {
    // 1 mm pitch at 1e-4 px per unit is 100 px between marks: 8 x 6 of them.
    let frame = grid_frame(GridStyle::Dots, MM, 1.0e-4);
    let g = only_geometry(&frame);
    assert_eq!(g.batches.len(), 1);
    match &g.batches[0] {
        Batch::Quads { rects, .. } => {
            assert!(rects.len() >= 40 && rects.len() <= 100, "{}", rects.len())
        }
        other => panic!("expected quads, got {other:?}"),
    }
}

#[test]
fn a_lined_grid_becomes_one_stroke_batch() {
    let frame = grid_frame(GridStyle::Lines, MM, 1.0e-4);
    let g = only_geometry(&frame);
    assert_eq!(g.batches.len(), 1, "the grid should be a single batch");
    match &g.batches[0] {
        Batch::Stroke { polylines, .. } => {
            // Roughly nine verticals and seven horizontals.
            assert!(polylines.len() >= 14 && polylines.len() <= 22, "{}", polylines.len());
        }
        other => panic!("expected strokes, got {other:?}"),
    }
}

#[test]
fn a_cross_grid_becomes_two_strokes_per_intersection() {
    let dots = grid_frame(GridStyle::Dots, MM, 1.0e-4);
    let crosses = grid_frame(GridStyle::SmallCross, MM, 1.0e-4);
    let n_dots = only_geometry(&dots).shape_count();
    let n_cross = only_geometry(&crosses).shape_count();
    assert_eq!(n_cross, n_dots * 2);
}

#[test]
fn a_grid_denser_than_the_minimum_spacing_is_not_drawn() {
    // 1 mm pitch at 1e-6 px per unit is one pixel between marks.
    let frame = grid_frame(GridStyle::Dots, MM, 1.0e-6);
    assert!(frame.items.is_empty(), "a grey wash was drawn");
}

#[test]
fn a_degenerate_grid_pitch_is_ignored() {
    let c = Camera::new([0.0, 0.0], 1.0e-4, [800.0, 600.0]);
    let mut b = StreamBuilder::new();
    b.grid([0.0, 0.0], [0.0, 0.0], 1.0, GridStyle::Lines, Color::WHITE);
    let s = b.finish().expect("valid");
    assert!(frame_of(&s, &c).items.is_empty());
}

// ------------------------------------------------------------------- renderer

/// A stream with `n` symbol-like groups in a grid, each drawn once.
fn sheet(n: u32) -> Stream {
    let mut b = StreamBuilder::new();
    let cols = (n as f64).sqrt().ceil() as u32;
    for i in 0..n {
        let x = (i % cols) as f64 * 20.0 * MM;
        let y = (i / cols) as f64 * 20.0 * MM;
        b.group(i, 1, |g| {
            g.set_stroke_color(Color::new(0.5, 0.0, 0.0, 1.0));
            g.set_line_width(0.15 * MM);
            g.rectangle([x, y], [x + 10.0 * MM, y + 6.0 * MM]);
            g.segment([x - 2.5 * MM, y + 2.0 * MM], [x, y + 2.0 * MM], 0.15 * MM);
            g.segment([x - 2.5 * MM, y + 4.0 * MM], [x, y + 4.0 * MM], 0.15 * MM);
            g.circle([x - 2.5 * MM, y + 2.0 * MM], 0.3 * MM);
        });
    }
    for i in 0..n {
        b.draw_group(i);
    }
    b.finish().expect("valid")
}

#[test]
fn a_pan_re_tessellates_nothing() {
    // The property the whole cache design exists for.
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(64));
    r.set_viewport([1200.0, 900.0]);
    r.zoom_to_fit(20.0);

    let first = r.prepare([0.0, 0.0]);
    assert!(first.stats.cache_misses > 0, "nothing was tessellated at all");
    assert!(first.stats.groups_drawn > 0);

    for _ in 0..30 {
        r.pan(3.0, 2.0);
        let f = r.prepare([0.0, 0.0]);
        assert_eq!(f.stats.cache_misses, 0, "a pan re-tessellated");
    }
}

#[test]
fn a_small_zoom_re_tessellates_nothing_but_a_large_one_does() {
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(16));
    r.set_viewport([1200.0, 900.0]);
    r.zoom_to_fit(20.0);
    r.prepare([0.0, 0.0]);

    // Under one level-of-detail step: the cached triangles are reused and only
    // stretched by the residual.
    r.zoom_to_point(1.01, [600.0, 450.0]);
    assert_eq!(r.prepare([0.0, 0.0]).stats.cache_misses, 0);

    // Two octaves is far past a step, so new triangles are needed.
    r.zoom_to_point(4.0, [600.0, 450.0]);
    assert!(r.prepare([0.0, 0.0]).stats.cache_misses > 0);
}

#[test]
fn a_selection_re_tessellates_nothing() {
    let mut a = StreamBuilder::new();
    a.group(0, 1, |g| {
        g.set_stroke_color(Color::WHITE);
        g.rectangle([0.0, 0.0], [10.0 * MM, 6.0 * MM]);
    });
    a.draw_group(0);
    let plain = a.finish().expect("valid");

    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.set_stroke_color(Color::WHITE);
        g.rectangle([0.0, 0.0], [10.0 * MM, 6.0 * MM]);
    });
    b.draw_group_with(0, Some(Color::new(1.0, 1.0, 0.0, 1.0)), None);
    let selected = b.finish().expect("valid");

    let mut r = SchematicRenderer::new();
    r.set_stream(plain);
    r.set_viewport([800.0, 600.0]);
    r.zoom_to_fit(20.0);
    r.prepare([0.0, 0.0]);

    // The same group at the same serial, now drawn with an override.
    r.set_stream(selected);
    let f = r.prepare([0.0, 0.0]);
    assert_eq!(f.stats.cache_misses, 0, "selecting an item rebuilt its geometry");
    assert_eq!(f.stats.cache_hits, 1);
}

#[test]
fn re_recording_a_group_does_re_tessellate_it() {
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(4));
    r.set_viewport([800.0, 600.0]);
    r.zoom_to_fit(20.0);
    r.prepare([0.0, 0.0]);

    // Same ids, new serials: the producer re-recorded every body.
    let mut b = StreamBuilder::new();
    for i in 0..4u32 {
        b.group(i, 2, |g| {
            g.set_stroke_color(Color::WHITE);
            g.circle([i as f64 * MM, 0.0], MM);
        });
    }
    for i in 0..4u32 {
        b.draw_group(i);
    }
    r.set_stream(b.finish().expect("valid"));
    let f = r.prepare([0.0, 0.0]);
    assert_eq!(f.stats.cache_misses, 4);
}

#[test]
fn groups_outside_the_viewport_are_culled_before_they_are_tessellated() {
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(100));
    r.set_viewport([400.0, 300.0]);
    // Zoom right in on the first symbol, so most of the sheet is off screen.
    r.camera_mut().set_center([0.0, 0.0]);
    r.camera_mut().set_scale(2.0e-3);

    let f = r.prepare([0.0, 0.0]);
    assert_eq!(f.stats.groups_referenced, 100);
    assert!(f.stats.groups_culled > 80, "culled {}", f.stats.groups_culled);
    assert!(f.stats.groups_drawn >= 1);
    assert_eq!(
        f.stats.groups_drawn + f.stats.groups_culled,
        f.stats.groups_referenced
    );
}

#[test]
fn zoom_to_fit_frames_the_whole_sheet() {
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(25));
    r.set_viewport([1000.0, 800.0]);
    r.zoom_to_fit(20.0);
    let f = r.prepare([0.0, 0.0]);
    assert_eq!(f.stats.groups_culled, 0, "content was culled after a fit");
    assert_eq!(f.stats.groups_drawn, 25);

    let doc = r.document_bounds();
    assert!(!doc.is_empty());
    let visible = r.camera().visible_world_rect(0.0);
    assert!(visible.min[0] <= doc.min[0] && visible.max[0] >= doc.max[0]);
}

#[test]
fn the_spatial_index_agrees_with_a_direct_bounds_test() {
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(200));
    r.set_viewport([600.0, 400.0]);
    r.zoom_to_fit(10.0);
    r.zoom_to_point(6.0, [300.0, 200.0]);

    let query = r.camera().visible_world_rect(64.0);
    let mut from_index = r.visible_groups();
    from_index.sort_unstable();

    let stream = r.stream().expect("a stream is set");
    let view = stream.view();
    let mut brute: Vec<u32> = view
        .groups()
        .iter()
        .filter(|g| {
            kicad_sch_render::translate::measure_group(&view, g.id).intersects(&query)
        })
        .map(|g| g.id)
        .collect();
    brute.sort_unstable();
    assert_eq!(from_index, brute);
}

#[test]
fn replacing_a_stream_whose_groups_are_unchanged_keeps_the_cache() {
    let mut r = SchematicRenderer::new();
    r.set_stream(sheet(9));
    r.set_viewport([800.0, 600.0]);
    r.zoom_to_fit(20.0);
    r.prepare([0.0, 0.0]);
    let after_first = r.cache_stats();

    // A new frame body over the same document: exactly what a pan produces on
    // the C++ side.
    r.set_stream(sheet(9));
    let f = r.prepare([0.0, 0.0]);
    assert_eq!(f.stats.cache_misses, 0);
    assert_eq!(r.cache_stats().entries, after_first.entries);
}

#[test]
fn an_empty_renderer_prepares_an_empty_frame() {
    let mut r = SchematicRenderer::new();
    r.set_viewport([800.0, 600.0]);
    let f = r.prepare([0.0, 0.0]);
    assert_eq!(f.stats.groups_drawn, 0);
    assert_eq!(f.stats.vertices, 0);
    assert!(r.document_bounds().is_empty());
    // Fitting an empty document must not produce an infinite scale.
    r.zoom_to_fit(20.0);
    assert!(r.camera().scale().is_finite());
}

#[test]
fn nanometre_coordinates_keep_their_resolution_through_translation() {
    // Two segments one internal unit apart, a billion units from the origin,
    // at a zoom where one unit is one pixel. If the origin were subtracted
    // after narrowing, these would land on the same pixel.
    let c = Camera::new([1.2e9, 1.2e9], 1.0, [400.0, 400.0]);
    let mut b = StreamBuilder::new();
    b.segment([1.2e9, 1.2e9], [1.2e9, 1.2e9 + 10.0], 1.0);
    b.segment([1.2e9 + 1.0, 1.2e9], [1.2e9 + 1.0, 1.2e9 + 10.0], 1.0);
    let s = b.finish().expect("valid");

    let frame = frame_of(&s, &c);
    let Batch::Stroke { polylines, .. } = &only_geometry(&frame).batches[0] else {
        panic!("expected strokes");
    };
    assert_eq!(polylines.len(), 2);
    let dx = polylines[1].points[0][0] - polylines[0].points[0][0];
    assert!((dx - 1.0).abs() < 1e-3, "one unit came out as {dx} px");
}

#[test]
fn a_bitmap_is_counted_as_unsupported_rather_than_drawn_wrong() {
    let c = camera();
    let mut b = StreamBuilder::new();
    let img = b.add_image(2, 2, kicad_gal::ImageFormat::Rgba8, &[0u8; 16]);
    b.bitmap(img, Affine::translation(c.center()[0], c.center()[1]), 1.0);
    let s = b.finish().expect("valid");
    let frame = frame_of(&s, &c);
    assert_eq!(frame.stats.unsupported, 1);
    assert!(frame.items.is_empty());
}

#[test]
fn group_bounds_include_half_the_stroke_width() {
    let mut b = StreamBuilder::new();
    b.group(0, 1, |g| {
        g.segment([0.0, 0.0], [10.0 * MM, 0.0], 2.0 * MM);
    });
    let s = b.finish().expect("valid");
    let view = s.view();
    let bounds = kicad_sch_render::translate::measure_group(&view, 0);
    // The centreline is 10 mm long; a 2 mm stroke adds a millimetre at each
    // end and a millimetre above and below.
    assert!((bounds.min[0] + MM).abs() < 1.0, "{bounds:?}");
    assert!((bounds.max[0] - 11.0 * MM).abs() < 1.0, "{bounds:?}");
    assert!((bounds.size()[1] - 2.0 * MM).abs() < 1.0, "{bounds:?}");
}

#[test]
fn an_unknown_group_reference_is_survivable() {
    // The decoder rejects a DRAW_GROUP of an unknown id, so this can only
    // arise if a group is measured that is not there. It must not panic.
    let s = sheet(2);
    let view = s.view();
    assert!(translate_group(&view, 99, 1.0e-4).is_none());
    assert!(kicad_sch_render::translate::measure_group(&view, 99).is_empty());
    assert_eq!(WorldRect::EMPTY.size(), [0.0, 0.0]);
}
