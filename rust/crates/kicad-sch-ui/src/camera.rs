// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The view transform between schematic world space and the canvas.
//!
//! The camera is a plain value: no gpui state, no interior mutability, no
//! rendering. That matters because it is the piece the real renderer
//! (`kicad-sch-render`) and the shell both have to agree on, and a value with
//! four fields is something two crates can agree on without either owning the
//! other. The controls named in the renderer's brief — `pan`, `zoom_to_point`,
//! `zoom_to_fit`, `set_viewport` — are exactly the mutating methods here.

use gpui_kit::{Bounds, Pixels, Point, Size, px};

use crate::input::{ScreenPoint, ViewportState, WorldPoint};

/// A rectangle in schematic world space, in millimetres.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WorldRect {
    /// Smallest x and y corner.
    pub min: WorldPoint,
    /// Largest x and y corner.
    pub max: WorldPoint,
}

impl WorldRect {
    /// A rectangle from two opposite corners, in either order.
    pub fn from_corners(a: WorldPoint, b: WorldPoint) -> Self {
        Self {
            min: WorldPoint::new(a.x.min(b.x), a.y.min(b.y)),
            max: WorldPoint::new(a.x.max(b.x), a.y.max(b.y)),
        }
    }

    /// Width in millimetres.
    pub fn width(&self) -> f64 {
        self.max.x - self.min.x
    }

    /// Height in millimetres.
    pub fn height(&self) -> f64 {
        self.max.y - self.min.y
    }

    /// The midpoint.
    pub fn center(&self) -> WorldPoint {
        WorldPoint::new(
            (self.min.x + self.max.x) * 0.5,
            (self.min.y + self.max.y) * 0.5,
        )
    }
}

/// Screen pixels per world millimetre at which the schematic is shown 1:1 on
/// a nominal 96 dpi display. Zoom percentages in the status bar are relative to
/// this, so "100%" means "a millimetre of schematic is a millimetre of glass".
pub const REFERENCE_SCALE: f32 = 96.0 / 25.4;

/// Tightest zoom out. Roughly a two-metre-wide sheet across a laptop screen.
pub const MIN_SCALE: f32 = 0.05;

/// Tightest zoom in. Roughly one millimetre filling the canvas.
pub const MAX_SCALE: f32 = 400.0;

/// Where the canvas is looking.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Camera {
    viewport: Bounds<Pixels>,
    center: WorldPoint,
    scale: f32,
}

impl Default for Camera {
    fn default() -> Self {
        Self {
            viewport: Bounds {
                origin: Point::default(),
                size: Size {
                    width: px(1.),
                    height: px(1.),
                },
            },
            center: WorldPoint::new(0., 0.),
            scale: REFERENCE_SCALE,
        }
    }
}

impl Camera {
    /// A camera at the default zoom, centred on the sheet origin.
    pub fn new() -> Self {
        Self::default()
    }

    /// The canvas rectangle in window coordinates.
    pub fn viewport(&self) -> Bounds<Pixels> {
        self.viewport
    }

    /// Tell the camera where and how big the canvas is.
    ///
    /// Called from the canvas element's layout, every frame. Keeping the world
    /// centre fixed rather than the world origin means a window resize grows
    /// the view outwards from the middle, which is what every other editor
    /// does.
    pub fn set_viewport(&mut self, viewport: Bounds<Pixels>) {
        self.viewport = viewport;
    }

    /// Screen pixels per world millimetre.
    pub fn scale(&self) -> f32 {
        self.scale
    }

    /// Zoom as a fraction of 1:1 on a nominal 96 dpi display.
    pub fn zoom(&self) -> f32 {
        self.scale / REFERENCE_SCALE
    }

    /// The world point at the centre of the canvas.
    pub fn center(&self) -> WorldPoint {
        self.center
    }

    /// Put a world point at the centre of the canvas.
    pub fn set_center(&mut self, center: WorldPoint) {
        self.center = center;
    }

    /// Set the zoom directly, clamped to the supported range, keeping the
    /// canvas centre fixed.
    pub fn set_zoom(&mut self, zoom: f32) {
        self.scale = (zoom * REFERENCE_SCALE).clamp(MIN_SCALE, MAX_SCALE);
    }

    /// Move the view so that the content appears to follow a screen-space
    /// delta — the direction a drag with the middle button moves it.
    pub fn pan(&mut self, delta: Point<Pixels>) {
        let scale = self.scale.max(f32::MIN_POSITIVE) as f64;
        self.center.x -= f32::from(delta.x) as f64 / scale;
        self.center.y -= f32::from(delta.y) as f64 / scale;
    }

    /// Zoom by `factor` while keeping the world point under `anchor` still.
    ///
    /// `anchor` is in window coordinates, the same space gpui reports mouse
    /// positions in, so a wheel handler can pass the event position straight
    /// through.
    pub fn zoom_to_point(&mut self, factor: f32, anchor: Point<Pixels>) {
        if !factor.is_finite() || factor <= 0. {
            return;
        }
        let anchored = self.screen_to_world(anchor);
        self.scale = (self.scale * factor).clamp(MIN_SCALE, MAX_SCALE);
        // Re-derive the centre so the anchored world point lands back under
        // the pointer at the new scale.
        let viewport_center = self.viewport.center();
        let scale = self.scale.max(f32::MIN_POSITIVE) as f64;
        self.center = WorldPoint::new(
            anchored.x - (f32::from(anchor.x) - f32::from(viewport_center.x)) as f64 / scale,
            anchored.y - (f32::from(anchor.y) - f32::from(viewport_center.y)) as f64 / scale,
        );
    }

    /// Frame `content` in the canvas with a small margin.
    ///
    /// A degenerate rectangle (a single item, or nothing but a point) would
    /// otherwise divide by zero, so it is treated as a fixed-size box around
    /// its centre and the zoom is left alone.
    pub fn zoom_to_fit(&mut self, content: WorldRect) {
        self.center = content.center();
        let width = content.width();
        let height = content.height();
        if width <= f64::EPSILON || height <= f64::EPSILON {
            return;
        }
        let available_w = f32::from(self.viewport.size.width) as f64;
        let available_h = f32::from(self.viewport.size.height) as f64;
        if available_w <= 0. || available_h <= 0. {
            return;
        }
        // 6% of the smaller axis as breathing room on each side.
        const MARGIN: f64 = 0.94;
        let fit = (available_w / width).min(available_h / height) * MARGIN;
        self.scale = (fit as f32).clamp(MIN_SCALE, MAX_SCALE);
    }

    /// Project a world point into window coordinates.
    pub fn world_to_screen(&self, world: WorldPoint) -> Point<Pixels> {
        let viewport_center = self.viewport.center();
        let scale = self.scale as f64;
        Point {
            x: viewport_center.x + px(((world.x - self.center.x) * scale) as f32),
            y: viewport_center.y + px(((world.y - self.center.y) * scale) as f32),
        }
    }

    /// Unproject a window coordinate into world space.
    pub fn screen_to_world(&self, screen: Point<Pixels>) -> WorldPoint {
        let viewport_center = self.viewport.center();
        let scale = self.scale.max(f32::MIN_POSITIVE) as f64;
        WorldPoint::new(
            self.center.x + (f32::from(screen.x) - f32::from(viewport_center.x)) as f64 / scale,
            self.center.y + (f32::from(screen.y) - f32::from(viewport_center.y)) as f64 / scale,
        )
    }

    /// Express a window coordinate relative to the canvas origin, which is
    /// what [`ScreenPoint`] means.
    pub fn to_canvas_local(&self, screen: Point<Pixels>) -> ScreenPoint {
        ScreenPoint::new(
            f32::from(screen.x) - f32::from(self.viewport.origin.x),
            f32::from(screen.y) - f32::from(self.viewport.origin.y),
        )
    }

    /// The world rectangle currently visible, which is what a renderer culls
    /// against.
    pub fn visible_world(&self) -> WorldRect {
        WorldRect::from_corners(
            self.screen_to_world(self.viewport.origin),
            self.screen_to_world(Point {
                x: self.viewport.origin.x + self.viewport.size.width,
                y: self.viewport.origin.y + self.viewport.size.height,
            }),
        )
    }

    /// This camera as the value reported to an [`InputSink`](crate::input::InputSink).
    pub fn state(&self) -> ViewportState {
        ViewportState {
            width: f32::from(self.viewport.size.width),
            height: f32::from(self.viewport.size.height),
            scale: self.scale,
            center: self.center,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn camera() -> Camera {
        let mut camera = Camera::new();
        camera.set_viewport(Bounds {
            origin: Point {
                x: px(100.),
                y: px(50.),
            },
            size: Size {
                width: px(800.),
                height: px(600.),
            },
        });
        camera
    }

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-3
    }

    #[test]
    fn round_trips_through_screen_space() {
        let camera = camera();
        let world = WorldPoint::new(37.5, -12.25);
        let back = camera.screen_to_world(camera.world_to_screen(world));
        assert!(close(back.x, world.x) && close(back.y, world.y), "{back:?}");
    }

    #[test]
    fn zoom_to_point_keeps_the_anchored_world_point_still() {
        let mut camera = camera();
        let anchor = Point {
            x: px(240.),
            y: px(400.),
        };
        let before = camera.screen_to_world(anchor);
        camera.zoom_to_point(2.5, anchor);
        let after = camera.screen_to_world(anchor);
        assert!(close(before.x, after.x) && close(before.y, after.y), "{after:?}");
        assert!(close(camera.scale() as f64, (REFERENCE_SCALE * 2.5) as f64));
    }

    #[test]
    fn zoom_is_clamped_rather_than_running_away() {
        let mut camera = camera();
        for _ in 0..200 {
            camera.zoom_to_point(2.0, camera.viewport().center());
        }
        assert_eq!(camera.scale(), MAX_SCALE);
        for _ in 0..400 {
            camera.zoom_to_point(0.5, camera.viewport().center());
        }
        assert_eq!(camera.scale(), MIN_SCALE);
    }

    #[test]
    fn pan_moves_the_view_opposite_the_drag() {
        let mut camera = camera();
        let before = camera.center();
        camera.pan(Point {
            x: px(100.),
            y: px(0.),
        });
        // Dragging the content right shows what was to its left.
        assert!(camera.center().x < before.x);
        assert!(close(
            camera.center().x,
            before.x - 100.0 / REFERENCE_SCALE as f64
        ));
    }

    #[test]
    fn zoom_to_fit_frames_the_content() {
        let mut camera = camera();
        let content = WorldRect::from_corners(WorldPoint::new(0., 0.), WorldPoint::new(200., 100.));
        camera.zoom_to_fit(content);
        let visible = camera.visible_world();
        assert!(visible.min.x <= content.min.x && visible.max.x >= content.max.x);
        assert!(visible.min.y <= content.min.y && visible.max.y >= content.max.y);
        assert!(close(camera.center().x, 100.) && close(camera.center().y, 50.));
    }

    #[test]
    fn a_degenerate_rectangle_does_not_produce_an_infinite_scale() {
        let mut camera = camera();
        let before = camera.scale();
        camera.zoom_to_fit(WorldRect::from_corners(
            WorldPoint::new(5., 5.),
            WorldPoint::new(5., 5.),
        ));
        assert_eq!(camera.scale(), before);
        assert!(close(camera.center().x, 5.));
    }

    #[test]
    fn canvas_local_positions_are_relative_to_the_canvas_origin() {
        let camera = camera();
        let local = camera.to_canvas_local(Point {
            x: px(150.),
            y: px(80.),
        });
        assert_eq!(local, ScreenPoint::new(50., 30.));
    }
}
