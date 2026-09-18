// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Tessellating [`Geometry`] into gpui primitives, and painting them.
//!
//! Two constraints shape everything here.
//!
//! * lyon's index buffer is `u16`, so a single `PathBuilder::build()` tops out
//!   at 65 535 vertices. Batches are therefore chunked, and a chunk that still
//!   overflows is split rather than dropped — `build()` reports
//!   `TooManyVertices` instead of panicking, which makes the fallback easy but
//!   only if it is actually written.
//! * Tessellation is the expensive half of a frame, so its result is kept.
//!   Everything in [`Tessellated`] is in *group-local* pixel space and is
//!   carried to its final position by [`paint_tessellated`], which applies a
//!   scale and a translation to the finished vertices. That is a single pass
//!   over the vertex array — the same order of work gpui's own `Path::scale`
//!   already does on the way into the scene — and it is what lets a pan cost
//!   no tessellation at all.

use gpui::{
    px, Background, Bounds, FillOptions, FillRule, Path, PathBuilder, PathStyle, Pixels, Point,
    Rgba, Size, StrokeOptions, Window,
};
use lyon_tessellation::{LineCap, LineJoin};

use crate::scene::{Batch, Geometry, PackedColor, PixelRect, Polyline};

/// Points allowed into one stroke tessellation before it is split.
///
/// lyon's round joins and caps emit several vertices per input point; eight is
/// a conservative estimate that keeps the usual case under the `u16` ceiling in
/// one go. Underestimating only costs a retry, so the budget errs low.
const STROKE_CHUNK_POINTS: usize = 6_000;

/// Points allowed into one fill tessellation before it is split.
///
/// A fill emits roughly one vertex per input point plus one per self
/// intersection, so the budget can be far higher than for strokes.
const FILL_CHUNK_POINTS: usize = 20_000;

/// How many times a chunk that still overflows is halved before it is given up
/// on. Sixteen halvings takes any realistic batch down to a single shape.
const MAX_SPLIT_DEPTH: u32 = 16;

/// One gpui primitive, tessellated and ready to paint.
#[derive(Clone, Debug)]
pub enum DrawItem {
    /// A tessellated path, in group-local pixel space.
    Path {
        /// Packed RGBA8, overridable at paint time.
        color: PackedColor,
        /// The tessellated geometry.
        path: Path<Pixels>,
    },
    /// Axis-aligned rectangles, in group-local pixel space. gpui draws quads
    /// as one instanced call, so these stay quads rather than becoming paths.
    Quads {
        /// Packed RGBA8, overridable at paint time.
        color: PackedColor,
        /// The rectangles.
        rects: Vec<PixelRect>,
    },
}

/// A tessellated [`Geometry`], in paint order.
#[derive(Clone, Debug, Default)]
pub struct Tessellated {
    /// The primitives, in the order they must be painted.
    pub items: Vec<DrawItem>,
    /// Total vertices, which is what the cache budgets against.
    pub vertices: usize,
    /// Shapes that could not be tessellated even after splitting. Always zero
    /// in practice; counted rather than ignored so that a regression shows up
    /// as a number instead of as missing geometry.
    pub dropped: usize,
}

/// Unpack a stream colour. Red is in the low byte and alpha in the high one,
/// matching `kgds_pack_color`.
pub fn to_rgba(packed: PackedColor) -> Rgba {
    Rgba {
        r: (packed & 0xFF) as f32 / 255.0,
        g: ((packed >> 8) & 0xFF) as f32 / 255.0,
        b: ((packed >> 16) & 0xFF) as f32 / 255.0,
        a: ((packed >> 24) & 0xFF) as f32 / 255.0,
    }
}

/// Convert a packed RGBA8 colour to something gpui will paint with.
pub fn to_background(packed: PackedColor) -> Background {
    to_rgba(packed).into()
}

/// Tessellate geometry into gpui primitives.
pub fn tessellate(geometry: &Geometry) -> Tessellated {
    let mut out = Tessellated::default();
    for batch in &geometry.batches {
        match batch {
            Batch::Stroke {
                color,
                width_px,
                polylines,
            } => emit_strokes(&mut out, *color, *width_px, polylines),
            Batch::Fill { color, contours } => {
                // Contours are already wound so that a non-zero rule subtracts
                // holes whatever orientation the producer used.
                let polys: Vec<Polyline> = contours
                    .iter()
                    .map(|c| Polyline {
                        points: c.points.clone(),
                        closed: true,
                    })
                    .collect();
                emit_fills(&mut out, *color, &polys);
            }
            Batch::Quads { color, rects } => out.items.push(DrawItem::Quads {
                color: *color,
                rects: rects.clone(),
            }),
        }
    }
    out
}

fn emit_strokes(out: &mut Tessellated, color: PackedColor, width_px: f32, polylines: &[Polyline]) {
    for chunk in chunk_by_points(polylines, STROKE_CHUNK_POINTS) {
        build_stroke_chunk(out, color, width_px, chunk, 0);
    }
}

fn emit_fills(out: &mut Tessellated, color: PackedColor, polys: &[Polyline]) {
    // A fill's contours cannot be split arbitrarily: an outline and its holes
    // have to be resolved together or the holes fill in. Splitting therefore
    // happens between shapes only, and a single shape that overflows is
    // dropped rather than drawn wrong.
    for chunk in chunk_by_points(polys, FILL_CHUNK_POINTS) {
        build_fill_chunk(out, color, chunk, 0);
    }
}

/// Split a slice of shapes into runs whose total point count stays under a
/// budget. A single shape longer than the budget becomes a run of its own.
fn chunk_by_points(shapes: &[Polyline], budget: usize) -> Vec<&[Polyline]> {
    let mut out = Vec::new();
    let mut start = 0;
    let mut acc = 0;
    for (i, s) in shapes.iter().enumerate() {
        let n = s.points.len();
        if acc > 0 && acc + n > budget {
            out.push(&shapes[start..i]);
            start = i;
            acc = 0;
        }
        acc += n;
    }
    if start < shapes.len() {
        out.push(&shapes[start..]);
    }
    out
}

fn build_stroke_chunk(
    out: &mut Tessellated,
    color: PackedColor,
    width_px: f32,
    shapes: &[Polyline],
    depth: u32,
) {
    if shapes.is_empty() {
        return;
    }
    // Round caps and joins, which is what the GAL backends draw and what a
    // schematic's wires and pin lines look like.
    let options = StrokeOptions::default()
        .with_line_width(width_px.max(f32::MIN_POSITIVE))
        .with_line_cap(LineCap::Round)
        .with_line_join(LineJoin::Round);
    let mut builder = PathBuilder::default().with_style(PathStyle::Stroke(options));
    for s in shapes {
        add_polyline(&mut builder, s);
    }

    match builder.build() {
        Ok(path) => push_path(out, color, path),
        Err(_) => split_and_retry(out, shapes, depth, &mut |o, part, d| {
            build_stroke_chunk(o, color, width_px, part, d)
        }),
    }
}

fn build_fill_chunk(out: &mut Tessellated, color: PackedColor, shapes: &[Polyline], depth: u32) {
    if shapes.is_empty() {
        return;
    }
    let options = FillOptions::default().with_fill_rule(FillRule::NonZero);
    let mut builder = PathBuilder::default().with_style(PathStyle::Fill(options));
    for s in shapes {
        add_polyline(&mut builder, s);
    }

    match builder.build() {
        Ok(path) => push_path(out, color, path),
        Err(_) => split_and_retry(out, shapes, depth, &mut |o, part, d| {
            build_fill_chunk(o, color, part, d)
        }),
    }
}

/// Halve a chunk that overflowed and try each half.
fn split_and_retry(
    out: &mut Tessellated,
    shapes: &[Polyline],
    depth: u32,
    again: &mut dyn FnMut(&mut Tessellated, &[Polyline], u32),
) {
    if depth >= MAX_SPLIT_DEPTH || shapes.len() <= 1 {
        // One shape alone was too big for a u16 index buffer. That needs over
        // 8 000 points in a single contour, which no schematic produces; it is
        // counted so that a regression is visible rather than silent.
        out.dropped += shapes.len().max(1);
        return;
    }
    let mid = shapes.len() / 2;
    again(out, &shapes[..mid], depth + 1);
    again(out, &shapes[mid..], depth + 1);
}

fn add_polyline(builder: &mut PathBuilder, s: &Polyline) {
    if s.points.len() < 2 {
        return;
    }
    if s.closed {
        let pts: Vec<Point<Pixels>> = s.points.iter().map(|p| pt(*p)).collect();
        builder.add_polygon(&pts, true);
        return;
    }
    let mut iter = s.points.iter();
    if let Some(first) = iter.next() {
        builder.move_to(pt(*first));
    }
    for p in iter {
        builder.line_to(pt(*p));
    }
}

fn pt(p: [f32; 2]) -> Point<Pixels> {
    gpui::point(px(p[0]), px(p[1]))
}

fn push_path(out: &mut Tessellated, color: PackedColor, path: Path<Pixels>) {
    if path.vertices.is_empty() {
        return;
    }
    out.vertices += path.vertices.len();
    out.items.push(DrawItem::Path { color, path });
}

/// How to carry group-local pixel space to where it belongs on screen.
///
/// `scale` is the residual between the level of detail the geometry was
/// tessellated at and the camera's real scale — always within one LOD step of
/// one — and `offset` is where the group's anchor lands in the viewport.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Placement {
    /// Residual scale.
    pub scale: f32,
    /// Where the anchor lands, in logical pixels.
    pub offset: [f32; 2],
}

impl Placement {
    /// The identity placement, for geometry already in viewport pixels.
    pub const IDENTITY: Placement = Placement {
        scale: 1.0,
        offset: [0.0, 0.0],
    };

    /// True when this placement would not move anything.
    pub fn is_identity(&self) -> bool {
        self.scale == 1.0 && self.offset == [0.0, 0.0]
    }

    /// Apply to a point in the tessellated space.
    pub fn apply(&self, p: [f32; 2]) -> [f32; 2] {
        [
            p[0] * self.scale + self.offset[0],
            p[1] * self.scale + self.offset[1],
        ]
    }
}

/// Paint tessellated geometry through a window.
///
/// `override_color` replaces every colour, which is how a selected or
/// highlighted group is drawn without discarding its cached tessellation —
/// `KIGFX::VIEW` recolours a cached item in place, and the stream carries that
/// as a flag on the replay rather than as a change to the group body.
///
/// The caller is responsible for being inside a single `paint_layer`: every
/// primitive painted at one draw order merges into one gpui path batch and
/// therefore one render pass, and splitting that up is the largest performance
/// cliff in the whole pipeline.
pub fn paint_tessellated(
    window: &mut Window,
    tessellated: &Tessellated,
    placement: Placement,
    override_color: Option<PackedColor>,
) {
    for item in &tessellated.items {
        match item {
            DrawItem::Path { color, path } => {
                let color = override_color.unwrap_or(*color);
                let placed = place_path(path, placement);
                window.paint_path(placed, to_background(color));
            }
            DrawItem::Quads { color, rects } => {
                let color = override_color.unwrap_or(*color);
                let background = to_background(color);
                for r in rects {
                    let min = placement.apply(r.min);
                    let max = placement.apply(r.max);
                    let bounds = Bounds {
                        origin: gpui::point(px(min[0]), px(min[1])),
                        size: Size {
                            width: px(max[0] - min[0]),
                            height: px(max[1] - min[1]),
                        },
                    };
                    window.paint_quad(gpui::fill(bounds, background));
                }
            }
        }
    }
}

/// Copy a tessellated path into its place on screen.
///
/// `Path`'s vertices and bounds are public, so a cached path can be moved
/// without re-tessellating. The `st_position` of each vertex is untouched
/// because it is a curve parameter, not a coordinate — `PathBuilder` sets it to
/// a constant for the straight triangles lyon produces.
fn place_path(path: &Path<Pixels>, placement: Placement) -> Path<Pixels> {
    let mut out = path.clone();
    if placement.is_identity() {
        return out;
    }
    for v in &mut out.vertices {
        let p = placement.apply([v.xy_position.x.to_f64() as f32, v.xy_position.y.to_f64() as f32]);
        v.xy_position = gpui::point(px(p[0]), px(p[1]));
    }
    let min = placement.apply([
        out.bounds.origin.x.to_f64() as f32,
        out.bounds.origin.y.to_f64() as f32,
    ]);
    out.bounds.origin = gpui::point(px(min[0]), px(min[1]));
    out.bounds.size = Size {
        width: px(out.bounds.size.width.to_f64() as f32 * placement.scale),
        height: px(out.bounds.size.height.to_f64() as f32 * placement.scale),
    };
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scene::{BatchBuilder, Contour};

    fn poly(n: usize) -> Polyline {
        Polyline {
            points: (0..n).map(|i| [i as f32, (i % 7) as f32]).collect(),
            closed: false,
        }
    }

    #[test]
    fn colour_unpacking_matches_the_abi_packing() {
        // Red in the low byte, alpha in the high one, as `kgds_pack_color`
        // writes it.
        let rgba = to_rgba(kicad_gal::pack_color(1.0, 0.0, 0.0, 1.0));
        assert!(rgba.r > 0.99 && rgba.g < 0.01 && rgba.b < 0.01 && rgba.a > 0.99);

        let half = to_rgba(kicad_gal::pack_color(0.0, 0.5, 0.0, 0.25));
        assert!(half.r < 0.01 && (half.g - 0.5).abs() < 0.01 && (half.a - 0.25).abs() < 0.01);
    }

    #[test]
    fn chunking_respects_the_budget_and_keeps_every_shape() {
        let shapes: Vec<Polyline> = (0..10).map(|_| poly(100)).collect();
        let chunks = chunk_by_points(&shapes, 250);
        assert!(chunks.len() > 1);
        assert_eq!(chunks.iter().map(|c| c.len()).sum::<usize>(), shapes.len());
        for c in &chunks {
            let pts: usize = c.iter().map(|s| s.points.len()).sum();
            // Either the chunk is under budget, or it is a single shape that
            // could not be split further.
            assert!(pts <= 250 || c.len() == 1);
        }
    }

    #[test]
    fn a_single_oversized_shape_becomes_its_own_chunk() {
        let shapes = vec![poly(10), poly(5_000), poly(10)];
        let chunks = chunk_by_points(&shapes, 100);
        assert!(chunks.iter().any(|c| c.len() == 1 && c[0].points.len() == 5_000));
        assert_eq!(chunks.iter().map(|c| c.len()).sum::<usize>(), 3);
    }

    #[test]
    fn tessellating_strokes_produces_vertices() {
        let mut b = BatchBuilder::new();
        b.stroke(0xFFFF_FFFF, 2.0, poly(10));
        let t = tessellate(&b.finish());
        assert_eq!(t.dropped, 0);
        assert!(t.vertices > 0);
        assert!(matches!(t.items[0], DrawItem::Path { .. }));
    }

    #[test]
    fn tessellating_a_fill_produces_vertices() {
        let mut b = BatchBuilder::new();
        b.fill(
            0xFF00_FF00,
            [Contour {
                points: vec![[0.0, 0.0], [100.0, 0.0], [100.0, 100.0], [0.0, 100.0]],
            }],
        );
        let t = tessellate(&b.finish());
        assert_eq!(t.dropped, 0);
        assert!(t.vertices >= 4);
    }

    #[test]
    fn a_batch_far_past_the_u16_limit_is_split_rather_than_lost() {
        // 40 000 stroked segments is several times what one lyon build can
        // index. Every one of them has to come out the other side.
        let mut b = BatchBuilder::new();
        for i in 0..40_000 {
            b.stroke(
                0xFFFF_FFFF,
                1.0,
                Polyline {
                    points: vec![[i as f32, 0.0], [i as f32, 10.0]],
                    closed: false,
                },
            );
        }
        let t = tessellate(&b.finish());
        assert_eq!(t.dropped, 0, "geometry was dropped rather than split");
        assert!(t.items.len() > 1, "the batch was not split at all");
        for item in &t.items {
            if let DrawItem::Path { path, .. } = item {
                assert!(
                    path.vertices.len() <= u16::MAX as usize + 1,
                    "a path exceeded the u16 index space"
                );
            }
        }
    }

    #[test]
    fn quads_stay_quads() {
        let mut b = BatchBuilder::new();
        b.quad(0xFF00_00FF, PixelRect::from_corners([0.0, 0.0], [1.0, 1.0]));
        b.quad(0xFF00_00FF, PixelRect::from_corners([2.0, 2.0], [3.0, 3.0]));
        let t = tessellate(&b.finish());
        assert_eq!(t.items.len(), 1);
        match &t.items[0] {
            DrawItem::Quads { rects, .. } => assert_eq!(rects.len(), 2),
            other => panic!("expected quads, got {other:?}"),
        }
    }

    #[test]
    fn placing_a_path_moves_its_vertices_and_its_bounds() {
        let mut b = BatchBuilder::new();
        b.stroke(0xFFFF_FFFF, 2.0, poly(4));
        let t = tessellate(&b.finish());
        let DrawItem::Path { path, .. } = &t.items[0] else {
            panic!("expected a path");
        };

        let placement = Placement {
            scale: 2.0,
            offset: [100.0, -50.0],
        };
        let placed = place_path(path, placement);
        assert_eq!(placed.vertices.len(), path.vertices.len());
        for (a, b) in path.vertices.iter().zip(&placed.vertices) {
            let want = placement.apply([
                a.xy_position.x.to_f64() as f32,
                a.xy_position.y.to_f64() as f32,
            ]);
            assert!((b.xy_position.x.to_f64() as f32 - want[0]).abs() < 1e-3);
            assert!((b.xy_position.y.to_f64() as f32 - want[1]).abs() < 1e-3);
            // The curve parameter is not a coordinate and must not move.
            assert_eq!(a.st_position, b.st_position);
        }
        assert!(
            (placed.bounds.size.width.to_f64() - path.bounds.size.width.to_f64() * 2.0).abs() < 1e-3
        );
    }

    #[test]
    fn the_identity_placement_copies_nothing_into_the_wrong_place() {
        let mut b = BatchBuilder::new();
        b.stroke(0xFFFF_FFFF, 2.0, poly(4));
        let t = tessellate(&b.finish());
        let DrawItem::Path { path, .. } = &t.items[0] else {
            panic!("expected a path");
        };
        let placed = place_path(path, Placement::IDENTITY);
        for (a, b) in path.vertices.iter().zip(&placed.vertices) {
            assert_eq!(a.xy_position, b.xy_position);
        }
    }
}
