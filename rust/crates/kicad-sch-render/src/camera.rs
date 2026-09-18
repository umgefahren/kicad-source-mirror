// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The world↔screen transform, and the precision rule that goes with it.
//!
//! KiCad's internal unit is the nanometre, so an A0 sheet spans more than
//! 1.2e9 of them. An `f32` has a 24-bit mantissa: at 1.2e9 its spacing is about
//! 64 units, and at a zoom where one pixel is one micrometre that is 64 pixels
//! of error. Coordinates therefore stay `f64` until the camera origin has been
//! subtracted, and only the small relative number is narrowed. Everything in
//! this module exists to make that ordering impossible to get wrong: the only
//! way to reach screen space is through a method that subtracts first.

/// An axis-aligned rectangle in world (nanometre) units.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WorldRect {
    /// Minimum corner.
    pub min: [f64; 2],
    /// Maximum corner.
    pub max: [f64; 2],
}

impl WorldRect {
    /// An empty rectangle that absorbs any point it is extended by.
    pub const EMPTY: WorldRect = WorldRect {
        min: [f64::INFINITY, f64::INFINITY],
        max: [f64::NEG_INFINITY, f64::NEG_INFINITY],
    };

    /// A rectangle from two opposite corners, in any order.
    pub fn from_corners(a: [f64; 2], b: [f64; 2]) -> WorldRect {
        WorldRect {
            min: [a[0].min(b[0]), a[1].min(b[1])],
            max: [a[0].max(b[0]), a[1].max(b[1])],
        }
    }

    /// True when the rectangle has never been extended.
    pub fn is_empty(&self) -> bool {
        self.min[0] > self.max[0] || self.min[1] > self.max[1]
    }

    /// Extend to contain a point.
    pub fn extend(&mut self, p: [f64; 2]) {
        self.min[0] = self.min[0].min(p[0]);
        self.min[1] = self.min[1].min(p[1]);
        self.max[0] = self.max[0].max(p[0]);
        self.max[1] = self.max[1].max(p[1]);
    }

    /// Extend to contain another rectangle.
    pub fn union(&mut self, other: &WorldRect) {
        if other.is_empty() {
            return;
        }
        self.extend(other.min);
        self.extend(other.max);
    }

    /// Grow by `by` world units on every side.
    pub fn inflated(&self, by: f64) -> WorldRect {
        if self.is_empty() {
            return *self;
        }
        WorldRect {
            min: [self.min[0] - by, self.min[1] - by],
            max: [self.max[0] + by, self.max[1] + by],
        }
    }

    /// True when the two rectangles share any area or edge.
    pub fn intersects(&self, other: &WorldRect) -> bool {
        !(self.is_empty()
            || other.is_empty()
            || self.max[0] < other.min[0]
            || other.max[0] < self.min[0]
            || self.max[1] < other.min[1]
            || other.max[1] < self.min[1])
    }

    /// Width and height.
    pub fn size(&self) -> [f64; 2] {
        if self.is_empty() {
            [0.0, 0.0]
        } else {
            [self.max[0] - self.min[0], self.max[1] - self.min[1]]
        }
    }

    /// The centre point.
    pub fn center(&self) -> [f64; 2] {
        if self.is_empty() {
            [0.0, 0.0]
        } else {
            [
                self.min[0] + (self.max[0] - self.min[0]) * 0.5,
                self.min[1] + (self.max[1] - self.min[1]) * 0.5,
            ]
        }
    }
}

/// Smallest scale the camera will hold, in pixels per world unit.
///
/// At 1e-9 one pixel is one metre, which is already far outside anything a
/// schematic needs; the limit exists so that a runaway zoom cannot produce a
/// denormal or a division by zero.
pub const MIN_SCALE: f64 = 1e-9;

/// Largest scale the camera will hold. At 10 px per internal unit a pixel is a
/// tenth of a nanometre.
pub const MAX_SCALE: f64 = 10.0;

/// Number of level-of-detail steps per octave of zoom.
///
/// Cached tessellation is keyed by LOD rather than by exact scale, so that a
/// continuous zoom re-tessellates a few times per octave instead of every
/// frame. The residual between the LOD's scale and the real one is applied to
/// the cached vertices, so geometry stays exact; only stroke widths and
/// flattening tolerance carry the quantisation, and at 16 steps per octave that
/// is at most 2.2% either way — well under a pixel on any stroke a schematic
/// draws.
pub const LOD_STEPS_PER_OCTAVE: f64 = 16.0;

/// The world↔screen transform for the canvas.
///
/// The camera is expressed as a world point at the centre of the viewport plus
/// a scale, rather than as a matrix, because that is the form in which pan and
/// zoom stay exact in `f64`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Camera {
    center: [f64; 2],
    scale: f64,
    viewport: [f64; 2],
}

impl Default for Camera {
    fn default() -> Self {
        Camera {
            center: [0.0, 0.0],
            // One pixel per 10 000 internal units: a 200 mm sheet fits a
            // 20 000 px span, which is a sane starting point before the first
            // `zoom_to_fit`.
            scale: 1.0e-4,
            viewport: [1.0, 1.0],
        }
    }
}

impl Camera {
    /// A camera centred on `center` at `scale` pixels per world unit.
    pub fn new(center: [f64; 2], scale: f64, viewport: [f64; 2]) -> Camera {
        let mut c = Camera::default();
        c.center = center;
        c.set_scale(scale);
        c.set_viewport(viewport);
        c
    }

    /// The world point at the centre of the viewport.
    pub fn center(&self) -> [f64; 2] {
        self.center
    }

    /// Move the centre to a world point.
    pub fn set_center(&mut self, center: [f64; 2]) {
        if center[0].is_finite() && center[1].is_finite() {
            self.center = center;
        }
    }

    /// Screen pixels per world unit.
    pub fn scale(&self) -> f64 {
        self.scale
    }

    /// Set the scale, clamped to the supported range. A non-finite scale is
    /// ignored rather than propagated into every subsequent transform.
    pub fn set_scale(&mut self, scale: f64) {
        if scale.is_finite() {
            self.scale = scale.clamp(MIN_SCALE, MAX_SCALE);
        }
    }

    /// Viewport size in logical pixels.
    pub fn viewport(&self) -> [f64; 2] {
        self.viewport
    }

    /// Resize the viewport, keeping the centred world point centred.
    pub fn set_viewport(&mut self, viewport: [f64; 2]) {
        // A zero-sized viewport happens for one frame during layout; keeping a
        // positive size avoids dividing by it everywhere downstream.
        self.viewport = [viewport[0].max(1.0), viewport[1].max(1.0)];
    }

    /// The quantised zoom level cached tessellation is keyed by.
    ///
    /// Two scales with the same exponent differ by less than one LOD step, and
    /// a cached tessellation built for one is reused for the other.
    pub fn lod(&self) -> i32 {
        (self.scale.log2() * LOD_STEPS_PER_OCTAVE).round() as i32
    }

    /// The exact scale that [`Camera::lod`] stands for.
    pub fn lod_scale(&self) -> f64 {
        (self.lod() as f64 / LOD_STEPS_PER_OCTAVE).exp2()
    }

    /// The factor to apply to geometry tessellated at [`Camera::lod_scale`] to
    /// bring it to the real scale. Always within one LOD step of 1.
    pub fn lod_residual(&self) -> f64 {
        self.scale / self.lod_scale()
    }

    /// Map a world point to logical pixels.
    ///
    /// The subtraction happens in `f64` and only the result is narrowed, which
    /// is the whole reason this method exists rather than a matrix.
    pub fn world_to_screen(&self, w: [f64; 2]) -> [f32; 2] {
        let x = (w[0] - self.center[0]) * self.scale + self.viewport[0] * 0.5;
        let y = (w[1] - self.center[1]) * self.scale + self.viewport[1] * 0.5;
        [x as f32, y as f32]
    }

    /// Map a world point to logical pixels without narrowing, for callers that
    /// need to keep accumulating in `f64`.
    pub fn world_to_screen_f64(&self, w: [f64; 2]) -> [f64; 2] {
        [
            (w[0] - self.center[0]) * self.scale + self.viewport[0] * 0.5,
            (w[1] - self.center[1]) * self.scale + self.viewport[1] * 0.5,
        ]
    }

    /// Map logical pixels back to world units.
    pub fn screen_to_world(&self, s: [f64; 2]) -> [f64; 2] {
        [
            (s[0] - self.viewport[0] * 0.5) / self.scale + self.center[0],
            (s[1] - self.viewport[1] * 0.5) / self.scale + self.center[1],
        ]
    }

    /// Convert a world length to pixels.
    pub fn world_to_px(&self, len: f64) -> f64 {
        len * self.scale
    }

    /// Convert a pixel length to world units.
    pub fn px_to_world(&self, len: f64) -> f64 {
        len / self.scale
    }

    /// Pan by a screen-space delta, as a drag does.
    pub fn pan_by_pixels(&mut self, dx: f64, dy: f64) {
        if !dx.is_finite() || !dy.is_finite() {
            return;
        }
        self.center[0] -= dx / self.scale;
        self.center[1] -= dy / self.scale;
    }

    /// Pan by a world-space delta.
    pub fn pan_by_world(&mut self, dx: f64, dy: f64) {
        if dx.is_finite() && dy.is_finite() {
            self.center[0] += dx;
            self.center[1] += dy;
        }
    }

    /// Zoom by `factor` about a point on screen, keeping the world point under
    /// that pixel exactly where it is.
    ///
    /// The anchoring is done by recomputing the centre from the fixed world
    /// point rather than by accumulating a delta, so repeated wheel events do
    /// not drift.
    pub fn zoom_to_point(&mut self, factor: f64, screen: [f64; 2]) {
        if !factor.is_finite() || factor <= 0.0 {
            return;
        }
        let anchor_world = self.screen_to_world(screen);
        self.set_scale(self.scale * factor);
        // Solve world_to_screen(anchor_world) == screen for the new centre.
        self.center = [
            anchor_world[0] - (screen[0] - self.viewport[0] * 0.5) / self.scale,
            anchor_world[1] - (screen[1] - self.viewport[1] * 0.5) / self.scale,
        ];
    }

    /// Frame a world rectangle, leaving `padding_px` pixels of margin.
    ///
    /// An empty or degenerate rectangle only moves the camera; it does not
    /// produce an infinite scale.
    pub fn zoom_to_fit(&mut self, rect: &WorldRect, padding_px: f64) {
        if rect.is_empty() {
            return;
        }
        self.set_center(rect.center());
        let [w, h] = rect.size();
        let avail_x = (self.viewport[0] - 2.0 * padding_px).max(1.0);
        let avail_y = (self.viewport[1] - 2.0 * padding_px).max(1.0);
        let sx = if w > 0.0 { avail_x / w } else { f64::INFINITY };
        let sy = if h > 0.0 { avail_y / h } else { f64::INFINITY };
        let s = sx.min(sy);
        if s.is_finite() {
            self.set_scale(s);
        }
    }

    /// The world rectangle currently visible, optionally grown by a margin in
    /// pixels so that geometry whose stroke spills into view is not culled.
    pub fn visible_world_rect(&self, margin_px: f64) -> WorldRect {
        let m = margin_px / self.scale;
        let half_w = self.viewport[0] * 0.5 / self.scale + m;
        let half_h = self.viewport[1] * 0.5 / self.scale + m;
        WorldRect {
            min: [self.center[0] - half_w, self.center[1] - half_h],
            max: [self.center[0] + half_w, self.center[1] + half_h],
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A camera over a sheet placed a long way from the origin, which is the
    /// case f32 coordinates would ruin.
    fn far_camera() -> Camera {
        Camera::new([1.234_567_891e9, -9.876_543_21e8], 1.0e-4, [1920.0, 1080.0])
    }

    #[test]
    fn centre_maps_to_the_middle_of_the_viewport() {
        let c = far_camera();
        let s = c.world_to_screen(c.center());
        assert!((s[0] - 960.0).abs() < 1e-3, "{s:?}");
        assert!((s[1] - 540.0).abs() < 1e-3, "{s:?}");
    }

    #[test]
    fn nanometre_coordinates_survive_the_round_trip() {
        // The property that matters: subtracting the origin in f64 first keeps
        // sub-pixel accuracy even at 1.2e9 internal units, where an f32 world
        // coordinate would already be quantised to ~64 units.
        let c = far_camera();
        for dx in [-1e6, -1.0, 0.0, 1.0, 1e6] {
            for dy in [-1e6, 0.0, 1e6] {
                let w = [c.center()[0] + dx, c.center()[1] + dy];
                let s = c.world_to_screen_f64(w);
                let back = c.screen_to_world(s);
                assert!(
                    (back[0] - w[0]).abs() < 1e-3 && (back[1] - w[1]).abs() < 1e-3,
                    "round trip lost precision: {w:?} -> {s:?} -> {back:?}"
                );
            }
        }
    }

    #[test]
    fn narrowing_happens_after_the_subtraction() {
        // Two world points one internal unit apart, a billion units from the
        // origin. Narrowed naively they would collide; narrowed after the
        // subtraction they stay distinct.
        let c = Camera::new([1.2e9, 0.0], 1.0, [100.0, 100.0]);
        let a = c.world_to_screen([1.2e9, 0.0]);
        let b = c.world_to_screen([1.2e9 + 1.0, 0.0]);
        assert_ne!(a[0], b[0]);
        assert!((b[0] - a[0] - 1.0).abs() < 1e-4);

        // The naive alternative, for contrast: as f32 the two are the same
        // number, which is the bug this ordering exists to prevent.
        assert_eq!(1.2e9f32, 1.2e9f32 + 1.0);
    }

    #[test]
    fn zoom_to_point_keeps_that_pixel_over_the_same_world_point() {
        let mut c = far_camera();
        let anchor = [1500.0, 200.0];
        let before = c.screen_to_world(anchor);
        for _ in 0..40 {
            c.zoom_to_point(1.1, anchor);
        }
        let after = c.screen_to_world(anchor);
        // Forty wheel clicks must not drift: the anchor is recomputed from the
        // fixed world point each time rather than accumulated.
        let tol = c.px_to_world(0.01);
        assert!(
            (after[0] - before[0]).abs() < tol && (after[1] - before[1]).abs() < tol,
            "drifted from {before:?} to {after:?}"
        );
    }

    #[test]
    fn zoom_is_clamped_rather_than_allowed_to_reach_zero() {
        let mut c = far_camera();
        for _ in 0..10_000 {
            c.zoom_to_point(0.5, [0.0, 0.0]);
        }
        assert_eq!(c.scale(), MIN_SCALE);
        for _ in 0..10_000 {
            c.zoom_to_point(2.0, [0.0, 0.0]);
        }
        assert_eq!(c.scale(), MAX_SCALE);
    }

    #[test]
    fn zoom_to_fit_frames_the_rectangle_inside_the_padding() {
        let mut c = far_camera();
        let rect = WorldRect::from_corners([1.0e9, 1.0e9], [1.0e9 + 2.1e8, 1.0e9 + 2.97e8]);
        c.zoom_to_fit(&rect, 20.0);

        let tl = c.world_to_screen_f64(rect.min);
        let br = c.world_to_screen_f64(rect.max);
        for v in [tl[0], tl[1], br[0], br[1]] {
            assert!(v > -0.5, "content escaped the viewport: {v}");
        }
        assert!(br[0] <= c.viewport()[0] + 0.5);
        assert!(br[1] <= c.viewport()[1] + 0.5);
        // One axis must actually touch the padding, or the fit was not tight.
        let touches = (tl[0] - 20.0).abs() < 0.5 || (tl[1] - 20.0).abs() < 0.5;
        assert!(touches, "fit was not tight: {tl:?} {br:?}");
    }

    #[test]
    fn zoom_to_fit_tolerates_a_degenerate_rectangle() {
        let mut c = far_camera();
        let point = WorldRect::from_corners([5.0, 5.0], [5.0, 5.0]);
        c.zoom_to_fit(&point, 10.0);
        assert!(c.scale().is_finite());
        assert_eq!(c.center(), [5.0, 5.0]);

        let mut c = far_camera();
        c.zoom_to_fit(&WorldRect::EMPTY, 10.0);
        assert!(c.scale().is_finite());
    }

    #[test]
    fn panning_by_pixels_moves_the_content_by_that_many_pixels() {
        let mut c = far_camera();
        let w = [c.center()[0] + 12345.0, c.center()[1] - 999.0];
        let before = c.world_to_screen_f64(w);
        c.pan_by_pixels(30.0, -17.0);
        let after = c.world_to_screen_f64(w);
        assert!((after[0] - before[0] - 30.0).abs() < 1e-6);
        assert!((after[1] - before[1] + 17.0).abs() < 1e-6);
    }

    #[test]
    fn visible_rect_covers_the_viewport_corners() {
        let c = far_camera();
        let r = c.visible_world_rect(0.0);
        let tl = c.screen_to_world([0.0, 0.0]);
        let br = c.screen_to_world(c.viewport());
        assert!((r.min[0] - tl[0]).abs() < 1e-6 && (r.min[1] - tl[1]).abs() < 1e-6);
        assert!((r.max[0] - br[0]).abs() < 1e-6 && (r.max[1] - br[1]).abs() < 1e-6);
        // A margin must only ever grow the rectangle.
        let m = c.visible_world_rect(64.0);
        assert!(m.min[0] < r.min[0] && m.max[0] > r.max[0]);
    }

    #[test]
    fn lod_quantises_zoom_without_losing_it() {
        let mut c = far_camera();
        let mut lods = std::collections::BTreeSet::new();
        for _ in 0..64 {
            c.zoom_to_point(1.01, [0.0, 0.0]);
            lods.insert(c.lod());
            // Whatever the LOD, the residual reconstructs the true scale.
            assert!((c.lod_scale() * c.lod_residual() - c.scale()).abs() < 1e-18);
            // ...and stays within half a step of 1, so a cached tessellation is
            // never stretched noticeably.
            let step = (1.0f64 / LOD_STEPS_PER_OCTAVE).exp2();
            assert!(c.lod_residual() > 1.0 / step && c.lod_residual() < step);
        }
        // 64 clicks of 1% is about 0.9 octaves, so a handful of LOD buckets,
        // not 64 of them: that ratio is what makes the cache worth having.
        assert!(lods.len() > 1 && lods.len() < 24, "{} buckets", lods.len());
    }

    #[test]
    fn non_finite_input_is_ignored_rather_than_propagated() {
        let mut c = far_camera();
        let before = c;
        c.set_scale(f64::NAN);
        c.set_center([f64::NAN, 0.0]);
        c.pan_by_pixels(f64::INFINITY, 0.0);
        c.zoom_to_point(f64::NAN, [0.0, 0.0]);
        c.zoom_to_point(0.0, [0.0, 0.0]);
        c.zoom_to_point(-1.0, [0.0, 0.0]);
        assert_eq!(c, before);
    }

    #[test]
    fn world_rect_union_and_intersection_behave() {
        let mut r = WorldRect::EMPTY;
        assert!(r.is_empty());
        r.extend([1.0, 2.0]);
        r.extend([-3.0, 4.0]);
        assert_eq!(r.min, [-3.0, 2.0]);
        assert_eq!(r.max, [1.0, 4.0]);
        assert!(r.intersects(&WorldRect::from_corners([0.0, 0.0], [0.5, 3.0])));
        assert!(!r.intersects(&WorldRect::from_corners([5.0, 5.0], [6.0, 6.0])));
        assert!(!r.intersects(&WorldRect::EMPTY));

        let mut a = WorldRect::from_corners([0.0, 0.0], [1.0, 1.0]);
        a.union(&WorldRect::EMPTY);
        assert_eq!(a, WorldRect::from_corners([0.0, 0.0], [1.0, 1.0]));
        a.union(&WorldRect::from_corners([-1.0, -1.0], [0.5, 0.5]));
        assert_eq!(a.min, [-1.0, -1.0]);
    }
}
