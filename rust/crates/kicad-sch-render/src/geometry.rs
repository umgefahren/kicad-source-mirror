// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Flattening curved geometry to polylines, and the projection that carries a
//! point from world units into the pixel space a batch is tessellated in.
//!
//! Arcs are flattened here rather than handed to lyon's `arc_to` because the
//! segment count has to be chosen from the arc's size *in pixels*: at the zoom
//! where a pin circle is four pixels across, sixteen segments is fifteen too
//! many, and at the zoom where it fills the window, sixteen is far too few.

use kicad_gal::Affine;

/// Carries a world point into the pixel space a batch is built in.
///
/// Two spaces are used. Frame geometry is projected straight into viewport
/// pixels. Group geometry is projected into *group-local* pixels — relative to
/// the group's own anchor, at the level-of-detail scale — so that the
/// tessellation can be cached and reused while the camera moves; the difference
/// between the two is a translation and a scale applied to the finished
/// vertices.
///
/// Either way the subtraction happens in `f64` before anything is narrowed,
/// which is the rule the whole crate is arranged around.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Projector {
    anchor: [f64; 2],
    scale: f64,
    offset: [f64; 2],
}

impl Projector {
    /// A projection of world units into pixels, `scale` pixels per unit, with
    /// `anchor` landing on `offset`.
    pub fn new(anchor: [f64; 2], scale: f64, offset: [f64; 2]) -> Projector {
        Projector {
            anchor,
            scale,
            offset,
        }
    }

    /// Pixels per world unit.
    pub fn scale(&self) -> f64 {
        self.scale
    }

    /// Project a world point.
    pub fn point(&self, w: [f64; 2]) -> [f32; 2] {
        [
            ((w[0] - self.anchor[0]) * self.scale + self.offset[0]) as f32,
            ((w[1] - self.anchor[1]) * self.scale + self.offset[1]) as f32,
        ]
    }

    /// Project a world length.
    pub fn length(&self, l: f64) -> f32 {
        (l * self.scale) as f32
    }
}

/// Flattening tolerance, in pixels: the greatest distance a chord is allowed to
/// stray from the true curve.
///
/// A fifth of a pixel is below what 4x MSAA can show, and halving it again
/// roughly doubles the segment count for no visible gain.
pub const FLATTEN_TOLERANCE_PX: f64 = 0.2;

/// How many chords to flatten an arc into.
///
/// From the sagitta of a chord: for a radius `r` in pixels and a tolerance `t`,
/// a chord subtending `θ` strays by `r(1 - cos(θ/2))`, so the largest
/// acceptable `θ` is `2·acos(1 - t/r)`. Tiny and huge arcs are both clamped,
/// the first because a sub-pixel arc needs no detail and the second so that a
/// degenerate radius cannot ask for a million segments.
pub fn arc_segment_count(radius_px: f64, sweep: f64, tolerance_px: f64) -> usize {
    let sweep = sweep.abs();
    if !radius_px.is_finite() || !sweep.is_finite() || sweep <= 0.0 {
        return 1;
    }
    if radius_px <= tolerance_px {
        // Smaller than the tolerance: a triangle is indistinguishable from a
        // circle, and a straight line from an arc.
        return 3.min(((sweep / std::f64::consts::FRAC_PI_2).ceil() as usize).max(1));
    }
    let max_angle = 2.0 * (1.0 - tolerance_px / radius_px).clamp(-1.0, 1.0).acos();
    if max_angle <= f64::EPSILON {
        return MAX_ARC_SEGMENTS;
    }
    ((sweep / max_angle).ceil() as usize).clamp(1, MAX_ARC_SEGMENTS)
}

/// Upper bound on the chords a single arc is flattened into.
///
/// At 4096 chords a full circle has a chord every 0.09 degrees, which is under
/// a pixel on a circle the size of a 4K display. The limit is really there so
/// that a corrupt radius costs a bounded amount of work.
pub const MAX_ARC_SEGMENTS: usize = 4096;

/// Flatten a circular arc into world-space points, inclusive of both ends.
///
/// `transform` is the GAL transform in force; it is applied to each point, so
/// an arc under a non-uniform scale comes out as the ellipse it really is.
pub fn flatten_arc(
    center: [f64; 2],
    radius: f64,
    start_angle: f64,
    end_angle: f64,
    transform: Affine,
    scale_px: f64,
    out: &mut Vec<[f64; 2]>,
) {
    let sweep = end_angle - start_angle;
    let radius_px = radius * transform.uniform_scale() * scale_px;
    let n = arc_segment_count(radius_px, sweep, FLATTEN_TOLERANCE_PX);
    out.reserve(n + 1);
    for i in 0..=n {
        let t = start_angle + sweep * (i as f64 / n as f64);
        let (s, c) = t.sin_cos();
        out.push(transform.apply([center[0] + radius * c, center[1] + radius * s]));
    }
}

/// Flatten an elliptical arc into world-space points.
///
/// The ellipse is parameterised before rotation, so the points are not evenly
/// spaced along the arc; the segment count is taken from the major radius,
/// which makes the spacing conservative rather than wrong.
#[allow(clippy::too_many_arguments)]
pub fn flatten_ellipse_arc(
    center: [f64; 2],
    major_radius: f64,
    minor_radius: f64,
    rotation: f64,
    start_angle: f64,
    end_angle: f64,
    transform: Affine,
    scale_px: f64,
    out: &mut Vec<[f64; 2]>,
) {
    let sweep = end_angle - start_angle;
    let r_px = major_radius.abs().max(minor_radius.abs()) * transform.uniform_scale() * scale_px;
    let n = arc_segment_count(r_px, sweep, FLATTEN_TOLERANCE_PX);
    let (sin_r, cos_r) = rotation.sin_cos();
    out.reserve(n + 1);
    for i in 0..=n {
        let t = start_angle + sweep * (i as f64 / n as f64);
        let (s, c) = t.sin_cos();
        let x = major_radius * c;
        let y = minor_radius * s;
        out.push(transform.apply([
            center[0] + x * cos_r - y * sin_r,
            center[1] + x * sin_r + y * cos_r,
        ]));
    }
}

/// Flatten a full circle into a closed world-space contour.
pub fn flatten_circle(
    center: [f64; 2],
    radius: f64,
    transform: Affine,
    scale_px: f64,
    out: &mut Vec<[f64; 2]>,
) {
    let radius_px = radius * transform.uniform_scale() * scale_px;
    let n = arc_segment_count(radius_px, std::f64::consts::TAU, FLATTEN_TOLERANCE_PX).max(3);
    out.reserve(n);
    // The last point is left off: the contour is closed, and repeating the
    // first vertex makes lyon emit a zero-length segment at the seam.
    for i in 0..n {
        let t = std::f64::consts::TAU * (i as f64 / n as f64);
        let (s, c) = t.sin_cos();
        out.push(transform.apply([center[0] + radius * c, center[1] + radius * s]));
    }
}

/// Flatten a cubic Bezier by recursive subdivision on flatness.
///
/// gpui's `PathBuilder` can take a cubic directly, but doing it here keeps
/// every batch a plain polyline, which is what makes chunking for lyon's u16
/// index limit predictable.
pub fn flatten_cubic(
    p0: [f64; 2],
    c0: [f64; 2],
    c1: [f64; 2],
    p1: [f64; 2],
    transform: Affine,
    scale_px: f64,
    out: &mut Vec<[f64; 2]>,
) {
    let tol_world = FLATTEN_TOLERANCE_PX / (transform.uniform_scale() * scale_px).max(1e-12);
    out.push(transform.apply(p0));
    subdivide_cubic(p0, c0, c1, p1, tol_world, 0, transform, out);
    out.push(transform.apply(p1));
}

/// Deepest a cubic is subdivided. 2^16 pieces is far beyond any tolerance that
/// matters, and bounds the work a degenerate control polygon can ask for.
const MAX_CUBIC_DEPTH: u32 = 16;

fn subdivide_cubic(
    p0: [f64; 2],
    c0: [f64; 2],
    c1: [f64; 2],
    p1: [f64; 2],
    tol: f64,
    depth: u32,
    transform: Affine,
    out: &mut Vec<[f64; 2]>,
) {
    if depth >= MAX_CUBIC_DEPTH || cubic_is_flat(p0, c0, c1, p1, tol) {
        return;
    }
    let mid = |a: [f64; 2], b: [f64; 2]| [(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5];
    let p01 = mid(p0, c0);
    let p12 = mid(c0, c1);
    let p23 = mid(c1, p1);
    let p012 = mid(p01, p12);
    let p123 = mid(p12, p23);
    let p0123 = mid(p012, p123);

    subdivide_cubic(p0, p01, p012, p0123, tol, depth + 1, transform, out);
    out.push(transform.apply(p0123));
    subdivide_cubic(p0123, p123, p23, p1, tol, depth + 1, transform, out);
}

/// The usual control-point distance test: the curve is flat when both control
/// points lie within `tol` of the chord.
fn cubic_is_flat(p0: [f64; 2], c0: [f64; 2], c1: [f64; 2], p1: [f64; 2], tol: f64) -> bool {
    let dx = p1[0] - p0[0];
    let dy = p1[1] - p0[1];
    let len2 = dx * dx + dy * dy;
    let tol2 = tol * tol;
    if len2 <= f64::MIN_POSITIVE {
        // A degenerate chord: fall back to comparing against the endpoint.
        let d0 = (c0[0] - p0[0]).powi(2) + (c0[1] - p0[1]).powi(2);
        let d1 = (c1[0] - p0[0]).powi(2) + (c1[1] - p0[1]).powi(2);
        return d0 <= tol2 && d1 <= tol2;
    }
    let cross0 = (c0[0] - p0[0]) * dy - (c0[1] - p0[1]) * dx;
    let cross1 = (c1[0] - p0[0]) * dy - (c1[1] - p0[1]) * dx;
    cross0 * cross0 <= tol2 * len2 && cross1 * cross1 <= tol2 * len2
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn projection_subtracts_before_narrowing() {
        let p = Projector::new([1.2e9, 0.0], 1.0, [0.0, 0.0]);
        let a = p.point([1.2e9, 0.0]);
        let b = p.point([1.2e9 + 1.0, 0.0]);
        assert_eq!(a, [0.0, 0.0]);
        assert_eq!(b, [1.0, 0.0]);
    }

    #[test]
    fn arc_segment_count_tracks_size_on_screen() {
        // A sub-pixel arc needs almost nothing; a huge one needs a lot.
        assert!(arc_segment_count(0.1, std::f64::consts::TAU, 0.2) <= 4);
        let small = arc_segment_count(5.0, std::f64::consts::TAU, 0.2);
        let large = arc_segment_count(500.0, std::f64::consts::TAU, 0.2);
        assert!(large > small * 5, "{small} then {large}");
        assert!(large <= MAX_ARC_SEGMENTS);
    }

    #[test]
    fn arc_segment_count_survives_degenerate_input() {
        for r in [0.0, -1.0, f64::INFINITY, f64::NAN] {
            let n = arc_segment_count(r, 1.0, 0.2);
            assert!((1..=MAX_ARC_SEGMENTS).contains(&n), "radius {r} gave {n}");
        }
        assert_eq!(arc_segment_count(10.0, 0.0, 0.2), 1);
        assert_eq!(arc_segment_count(10.0, f64::NAN, 0.2), 1);
    }

    #[test]
    fn flattened_arc_stays_within_tolerance_of_the_circle() {
        let mut pts = Vec::new();
        let scale = 1.0e-3;
        let r = 1.0e6;
        flatten_arc([0.0, 0.0], r, 0.0, std::f64::consts::PI, Affine::IDENTITY, scale, &mut pts);
        assert!(pts.len() >= 2);

        // Every chord midpoint must be within the tolerance of the true arc.
        for w in pts.windows(2) {
            let m = [(w[0][0] + w[1][0]) * 0.5, (w[0][1] + w[1][1]) * 0.5];
            let err_world = r - (m[0] * m[0] + m[1] * m[1]).sqrt();
            let err_px = err_world * scale;
            assert!(
                err_px <= FLATTEN_TOLERANCE_PX * 1.01,
                "chord strayed {err_px} px"
            );
        }
        // And the endpoints must be exact, not approximate.
        assert!((pts[0][0] - r).abs() < 1e-6);
        assert!((pts[pts.len() - 1][0] + r).abs() < 1e-6);
    }

    #[test]
    fn flattening_respects_the_transform() {
        let mut plain = Vec::new();
        flatten_arc([0.0, 0.0], 100.0, 0.0, 1.0, Affine::IDENTITY, 1.0, &mut plain);
        let mut moved = Vec::new();
        flatten_arc(
            [0.0, 0.0],
            100.0,
            0.0,
            1.0,
            kicad_gal::Affine::translation(1000.0, -500.0),
            1.0,
            &mut moved,
        );
        assert_eq!(plain.len(), moved.len());
        for (a, b) in plain.iter().zip(&moved) {
            assert!((b[0] - a[0] - 1000.0).abs() < 1e-9);
            assert!((b[1] - a[1] + 500.0).abs() < 1e-9);
        }
    }

    #[test]
    fn a_scaled_arc_is_flattened_more_finely() {
        let mut coarse = Vec::new();
        flatten_arc([0.0, 0.0], 100.0, 0.0, 6.0, Affine::IDENTITY, 1.0, &mut coarse);
        let mut fine = Vec::new();
        flatten_arc(
            [0.0, 0.0],
            100.0,
            0.0,
            6.0,
            kicad_gal::Affine::scale(20.0, 20.0),
            1.0,
            &mut fine,
        );
        assert!(fine.len() > coarse.len());
    }

    #[test]
    fn circle_contour_does_not_repeat_its_first_vertex() {
        let mut pts = Vec::new();
        flatten_circle([0.0, 0.0], 1000.0, Affine::IDENTITY, 1.0, &mut pts);
        assert!(pts.len() >= 3);
        let first = pts[0];
        let last = pts[pts.len() - 1];
        assert!((first[0] - last[0]).abs() > 1e-9 || (first[1] - last[1]).abs() > 1e-9);
    }

    #[test]
    fn cubic_flattening_reaches_both_endpoints() {
        let mut pts = Vec::new();
        flatten_cubic(
            [0.0, 0.0],
            [0.0, 1000.0],
            [1000.0, 1000.0],
            [1000.0, 0.0],
            Affine::IDENTITY,
            1.0,
            &mut pts,
        );
        assert_eq!(pts[0], [0.0, 0.0]);
        assert_eq!(pts[pts.len() - 1], [1000.0, 0.0]);
        assert!(pts.len() > 4, "a curved cubic should subdivide");

        // A straight cubic needs no subdivision at all.
        let mut straight = Vec::new();
        flatten_cubic(
            [0.0, 0.0],
            [10.0, 0.0],
            [20.0, 0.0],
            [30.0, 0.0],
            Affine::IDENTITY,
            1.0,
            &mut straight,
        );
        assert_eq!(straight.len(), 2);
    }

    #[test]
    fn cubic_flattening_terminates_on_a_degenerate_control_polygon() {
        let mut pts = Vec::new();
        flatten_cubic(
            [0.0, 0.0],
            [1e12, 1e12],
            [-1e12, -1e12],
            [0.0, 0.0],
            Affine::IDENTITY,
            1.0,
            &mut pts,
        );
        assert!(pts.len() <= (1 << MAX_CUBIC_DEPTH) + 2);
    }

    #[test]
    fn ellipse_arc_honours_its_rotation() {
        let mut pts = Vec::new();
        flatten_ellipse_arc(
            [0.0, 0.0],
            100.0,
            50.0,
            std::f64::consts::FRAC_PI_2,
            0.0,
            0.0,
            Affine::IDENTITY,
            1.0,
            &mut pts,
        );
        // At t = 0 the point is (major, 0) before rotation, so a quarter turn
        // puts it on the +y axis.
        assert!((pts[0][0]).abs() < 1e-9, "{:?}", pts[0]);
        assert!((pts[0][1] - 100.0).abs() < 1e-9, "{:?}", pts[0]);
    }
}
