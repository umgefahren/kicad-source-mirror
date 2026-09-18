// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Construction of draw streams from Rust.
//!
//! The renderer's whole development and test story rests on this type: with a
//! builder that mirrors the GAL calls, the wgpu backend, its golden images and
//! its benchmarks can all be written and run before any of the C++ producer
//! exists — and stay runnable in CI without it.
//!
//! The builder reproduces the ABI's memory layout faithfully rather than
//! conveniently. In particular group bodies are emitted into the prefix of the
//! command array and the frame body after them, exactly as `RECORDING_GAL`
//! does, so a stream built here exercises the same index arithmetic a recorded
//! one will.

use std::fmt;

use crate::abi::{flags, kgds_cmd, kgds_group, kgds_image, Color, GridStyle, ImageFormat, Op,
                 Target, KGDS_VERSION};
use crate::command::Affine;
use crate::error::DecodeError;
use crate::stream::{Stream, StreamParts};

/// Something a [`StreamBuilder`] was asked to record that cannot be expressed.
#[derive(Debug)]
#[non_exhaustive]
// Each variant's own doc says what its fields are; naming them twice would only
// be one more thing to let drift.
#[allow(missing_docs)]
pub enum BuildError {
    /// More than `u32::MAX` coordinates were recorded, so an index would no
    /// longer fit the ABI's command slots.
    CoordArenaOverflow,
    /// A point run held more than `u32::MAX` points.
    TooManyPoints { points: usize },
    /// `begin_group` was called while another group was open. The GAL
    /// interface does not nest groups and neither does the stream.
    NestedGroup { open: u32, attempted: u32 },
    /// `end_group` was called with no group open.
    EndGroupWithoutBegin,
    /// `finish` was called with a group still open.
    UnterminatedGroup { id: u32 },
    /// Two groups were given the same id.
    DuplicateGroupId { id: u32 },
    /// An image's pixel data length did not match its dimensions.
    ImageSizeMismatch { expected: usize, found: usize },
    /// The finished stream failed validation, which means the builder itself
    /// has a bug.
    Invalid(DecodeError),
}

impl fmt::Display for BuildError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BuildError::CoordArenaOverflow => {
                f.write_str("coordinate arena exceeded the u32 index space")
            }
            BuildError::TooManyPoints { points } => {
                write!(f, "point run of {points} points exceeds the u32 count field")
            }
            BuildError::NestedGroup { open, attempted } => write!(
                f,
                "cannot begin group {attempted} while group {open} is still open"
            ),
            BuildError::EndGroupWithoutBegin => f.write_str("end_group with no group open"),
            BuildError::UnterminatedGroup { id } => write!(f, "group {id} was never ended"),
            BuildError::DuplicateGroupId { id } => write!(f, "group id {id} used twice"),
            BuildError::ImageSizeMismatch { expected, found } => write!(
                f,
                "image pixel data is {found} bytes but its dimensions need {expected}"
            ),
            BuildError::Invalid(e) => write!(f, "built stream failed validation: {e}"),
        }
    }
}

impl std::error::Error for BuildError {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            BuildError::Invalid(e) => Some(e),
            _ => None,
        }
    }
}

impl From<DecodeError> for BuildError {
    fn from(e: DecodeError) -> Self {
        BuildError::Invalid(e)
    }
}

/// Builds a draw stream with methods that mirror the `KIGFX::GAL` calls.
///
/// Methods return `&mut Self` so that recording reads like the drawing code it
/// stands in for. Errors are latched rather than returned per call and surface
/// from [`StreamBuilder::finish`].
///
/// Deliberately not `Clone`: it holds a latched [`BuildError`], which wraps a
/// `std::io::Error` and cannot be duplicated, and a builder whose error was
/// silently dropped by a clone would be worse than one that cannot be cloned.
#[derive(Debug)]
pub struct StreamBuilder {
    flags: u32,
    group_cmds: Vec<kgds_cmd>,
    group_coords: Vec<f64>,
    groups: Vec<kgds_group>,
    frame_cmds: Vec<kgds_cmd>,
    frame_coords: Vec<f64>,
    strings: Vec<u8>,
    images: Vec<kgds_image>,
    image_data: Vec<u8>,
    /// `(id, serial, first command index within `group_cmds`)`.
    open_group: Option<(u32, u32, usize)>,
    /// Sticky flag bits OR'd into subsequent geometry commands.
    sticky_flags: u16,
    error: Option<BuildError>,
}

impl Default for StreamBuilder {
    fn default() -> Self {
        StreamBuilder::new()
    }
}

impl StreamBuilder {
    /// A builder for an empty stream.
    pub fn new() -> StreamBuilder {
        StreamBuilder {
            flags: 0,
            group_cmds: Vec::new(),
            group_coords: Vec::new(),
            groups: Vec::new(),
            frame_cmds: Vec::new(),
            frame_coords: Vec::new(),
            strings: Vec::new(),
            images: Vec::new(),
            image_data: Vec::new(),
            open_group: None,
            sticky_flags: 0,
            error: None,
        }
    }

    /// Reserve space for `cmds` group commands and `coords` group coordinates.
    pub fn reserve(&mut self, cmds: usize, coords: usize) -> &mut Self {
        self.group_cmds.reserve(cmds);
        self.group_coords.reserve(coords);
        self
    }

    /// Set the producer-defined stream flags word.
    pub fn set_stream_flags(&mut self, flags: u32) -> &mut Self {
        self.flags = flags;
        self
    }

    /// Mark subsequent geometry as coming from a glyph outline.
    pub fn set_glyph(&mut self, glyph: bool) -> &mut Self {
        if glyph {
            self.sticky_flags |= flags::GLYPH;
        } else {
            self.sticky_flags &= !flags::GLYPH;
        }
        self
    }

    // -- arena helpers -----------------------------------------------------

    fn fail(&mut self, e: BuildError) {
        if self.error.is_none() {
            self.error = Some(e);
        }
    }

    /// The coordinate arena the command being recorded indexes into: the group
    /// arena while a group is open, the frame arena otherwise. Keeping these
    /// apart is the whole point of the two-arena layout — a frame is discarded
    /// and re-recorded without touching a single group index.
    fn arena(&mut self) -> &mut Vec<f64> {
        if self.open_group.is_some() {
            &mut self.group_coords
        } else {
            &mut self.frame_coords
        }
    }

    /// Append coordinates and return the index of the first, or `u32::MAX` on
    /// overflow — in which case the error is already latched and `finish` will
    /// report it rather than producing a stream with a bogus index.
    fn push_coords(&mut self, values: &[f64]) -> u32 {
        let arena = self.arena();
        let first = arena.len();
        if first > u32::MAX as usize || (u32::MAX as usize - first) < values.len() {
            self.fail(BuildError::CoordArenaOverflow);
            return u32::MAX;
        }
        arena.extend_from_slice(values);
        first as u32
    }

    fn push_points(&mut self, points: &[[f64; 2]]) -> (u32, u32) {
        if points.len() > u32::MAX as usize {
            self.fail(BuildError::TooManyPoints {
                points: points.len(),
            });
            return (u32::MAX, 0);
        }
        let n = points.len();
        let arena = self.arena();
        let first = arena.len();
        if first > u32::MAX as usize || (u32::MAX as usize - first) < n * 2 {
            self.fail(BuildError::CoordArenaOverflow);
            return (u32::MAX, 0);
        }
        arena.reserve(n * 2);
        for p in points {
            arena.push(p[0]);
            arena.push(p[1]);
        }
        (first as u32, n as u32)
    }

    fn emit(&mut self, op: Op, extra_flags: u16, args: [u32; 5]) -> &mut Self {
        let cmd = kgds_cmd {
            op: op as u16,
            flags: extra_flags,
            arg0: args[0],
            arg1: args[1],
            arg2: args[2],
            arg3: args[3],
            arg4: args[4],
        };
        if self.open_group.is_some() {
            self.group_cmds.push(cmd);
        } else {
            self.frame_cmds.push(cmd);
        }
        self
    }

    /// Emit a geometry command, folding in the sticky glyph flag.
    fn emit_geom(&mut self, op: Op, extra_flags: u16, args: [u32; 5]) -> &mut Self {
        let f = extra_flags | self.sticky_flags;
        self.emit(op, f, args)
    }

    // -- structural --------------------------------------------------------

    /// `GAL::BeginDrawing` — record the pixel viewport for this frame.
    pub fn begin_frame(&mut self, width: u32, height: u32) -> &mut Self {
        self.emit(Op::BeginFrame, 0, [width, height, 0, 0, 0])
    }

    /// `GAL::EndDrawing`.
    pub fn end_frame(&mut self) -> &mut Self {
        self.emit(Op::EndFrame, 0, [0, 0, 0, 0, 0])
    }

    /// `GAL::ClearScreen`.
    pub fn clear_screen(&mut self, color: Color) -> &mut Self {
        self.emit(Op::ClearScreen, 0, [color.to_packed(), 0, 0, 0, 0])
    }

    /// `GAL::SetTarget`.
    pub fn set_target(&mut self, target: Target) -> &mut Self {
        self.emit(Op::SetTarget, 0, [target as u32, 0, 0, 0, 0])
    }

    /// `GAL::ClearTarget`.
    pub fn clear_target(&mut self, target: Target) -> &mut Self {
        self.emit(Op::ClearTarget, 0, [target as u32, 0, 0, 0, 0])
    }

    /// `GAL::StartDiffLayer`.
    pub fn start_diff_layer(&mut self) -> &mut Self {
        self.emit(Op::StartDiffLayer, 0, [0; 5])
    }

    /// `GAL::EndDiffLayer`.
    pub fn end_diff_layer(&mut self) -> &mut Self {
        self.emit(Op::EndDiffLayer, 0, [0; 5])
    }

    /// `GAL::StartNegativesLayer`.
    pub fn start_negatives_layer(&mut self) -> &mut Self {
        self.emit(Op::StartNegativesLayer, 0, [0; 5])
    }

    /// `GAL::EndNegativesLayer`.
    pub fn end_negatives_layer(&mut self) -> &mut Self {
        self.emit(Op::EndNegativesLayer, 0, [0; 5])
    }

    /// A no-op command.
    pub fn nop(&mut self) -> &mut Self {
        self.emit(Op::Nop, 0, [0; 5])
    }

    // -- render state ------------------------------------------------------

    /// `GAL::SetIsFill`.
    pub fn set_is_fill(&mut self, enabled: bool) -> &mut Self {
        self.emit(Op::SetIsFill, 0, [enabled as u32, 0, 0, 0, 0])
    }

    /// `GAL::SetIsStroke`.
    pub fn set_is_stroke(&mut self, enabled: bool) -> &mut Self {
        self.emit(Op::SetIsStroke, 0, [enabled as u32, 0, 0, 0, 0])
    }

    /// `GAL::SetFillColor`.
    pub fn set_fill_color(&mut self, color: Color) -> &mut Self {
        self.emit(Op::SetFillColor, 0, [color.to_packed(), 0, 0, 0, 0])
    }

    /// `GAL::SetStrokeColor`.
    pub fn set_stroke_color(&mut self, color: Color) -> &mut Self {
        self.emit(Op::SetStrokeColor, 0, [color.to_packed(), 0, 0, 0, 0])
    }

    /// `GAL::SetHoverColor`.
    pub fn set_hover_color(&mut self, color: Color) -> &mut Self {
        self.emit(Op::SetHoverColor, 0, [color.to_packed(), 0, 0, 0, 0])
    }

    /// `GAL::SetLineWidth`, in internal units.
    pub fn set_line_width(&mut self, width: f64) -> &mut Self {
        let i = self.push_coords(&[width]);
        self.emit(Op::SetLineWidth, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::SetMinLineWidth`, in pixels.
    pub fn set_min_line_width(&mut self, width: f64) -> &mut Self {
        let i = self.push_coords(&[width]);
        self.emit(Op::SetMinLineWidth, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::SetLayerDepth`. Smaller depths are nearer the viewer.
    pub fn set_layer_depth(&mut self, depth: f64) -> &mut Self {
        let i = self.push_coords(&[depth]);
        self.emit(Op::SetLayerDepth, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::SetNegativeDrawMode`.
    pub fn set_negative_draw_mode(&mut self, enabled: bool) -> &mut Self {
        self.emit(Op::SetNegativeDrawMode, 0, [enabled as u32, 0, 0, 0, 0])
    }

    /// `GAL::EnableDepthTest`.
    pub fn enable_depth_test(&mut self, enabled: bool) -> &mut Self {
        self.emit(Op::EnableDepthTest, 0, [enabled as u32, 0, 0, 0, 0])
    }

    // -- transforms --------------------------------------------------------

    /// `GAL::Transform`.
    pub fn transform(&mut self, m: Affine) -> &mut Self {
        let i = self.push_coords(&m.0);
        self.emit(Op::Transform, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::Rotate`, in radians.
    pub fn rotate(&mut self, radians: f64) -> &mut Self {
        let i = self.push_coords(&[radians]);
        self.emit(Op::Rotate, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::Translate`.
    pub fn translate(&mut self, dx: f64, dy: f64) -> &mut Self {
        let i = self.push_coords(&[dx, dy]);
        self.emit(Op::Translate, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::Scale`.
    pub fn scale(&mut self, sx: f64, sy: f64) -> &mut Self {
        let i = self.push_coords(&[sx, sy]);
        self.emit(Op::Scale, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::Save`.
    pub fn save(&mut self) -> &mut Self {
        self.emit(Op::Save, 0, [0; 5])
    }

    /// `GAL::Restore`.
    pub fn restore(&mut self) -> &mut Self {
        self.emit(Op::Restore, 0, [0; 5])
    }

    // -- geometry ----------------------------------------------------------

    /// `GAL::DrawLine`, stroked with the current line width.
    pub fn line(&mut self, p0: [f64; 2], p1: [f64; 2]) -> &mut Self {
        let i = self.push_coords(&[p0[0], p0[1], p1[0], p1[1]]);
        self.emit_geom(Op::Line, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::DrawSegment`, a round-capped segment of an explicit width.
    pub fn segment(&mut self, p0: [f64; 2], p1: [f64; 2], width: f64) -> &mut Self {
        let i = self.push_coords(&[p0[0], p0[1], p1[0], p1[1]]);
        let w = self.push_coords(&[width]);
        self.emit_geom(Op::Segment, 0, [i, w, 0, 0, 0])
    }

    /// `GAL::DrawSegmentChain`.
    pub fn segment_chain(&mut self, points: &[[f64; 2]], width: f64) -> &mut Self {
        let (first, n) = self.push_points(points);
        let w = self.push_coords(&[width]);
        self.emit_geom(Op::SegmentChain, 0, [first, n, w, 0, 0])
    }

    /// `GAL::DrawPolyline`, stroked with the current line width.
    pub fn polyline(&mut self, points: &[[f64; 2]]) -> &mut Self {
        let (first, n) = self.push_points(points);
        self.emit_geom(Op::Polyline, 0, [first, n, 0, 0, 0])
    }

    /// A closed polyline.
    pub fn polyline_closed(&mut self, points: &[[f64; 2]]) -> &mut Self {
        let (first, n) = self.push_points(points);
        self.emit_geom(Op::Polyline, flags::CLOSED, [first, n, 0, 0, 0])
    }

    /// `GAL::DrawPolygon`, an outline contour.
    pub fn polygon(&mut self, points: &[[f64; 2]]) -> &mut Self {
        let (first, n) = self.push_points(points);
        self.emit_geom(Op::Polygon, 0, [first, n, 0, 0, 0])
    }

    /// A contour that is a hole in the preceding outline.
    pub fn polygon_hole(&mut self, points: &[[f64; 2]]) -> &mut Self {
        let (first, n) = self.push_points(points);
        self.emit_geom(Op::Polygon, flags::HOLE, [first, n, 0, 0, 0])
    }

    /// `GAL::DrawCircle`.
    pub fn circle(&mut self, center: [f64; 2], radius: f64) -> &mut Self {
        let i = self.push_coords(&[center[0], center[1], radius]);
        self.emit_geom(Op::Circle, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::DrawArc`, angles in radians.
    pub fn arc(
        &mut self,
        center: [f64; 2],
        radius: f64,
        start_angle: f64,
        end_angle: f64,
    ) -> &mut Self {
        let i = self.push_coords(&[center[0], center[1], radius, start_angle, end_angle]);
        self.emit_geom(Op::Arc, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::DrawArcSegment`, an arc stroked with an explicit width.
    pub fn arc_segment(
        &mut self,
        center: [f64; 2],
        radius: f64,
        start_angle: f64,
        end_angle: f64,
        width: f64,
    ) -> &mut Self {
        let i = self.push_coords(&[center[0], center[1], radius, start_angle, end_angle]);
        let w = self.push_coords(&[width]);
        self.emit_geom(Op::ArcSegment, 0, [i, w, 0, 0, 0])
    }

    /// `GAL::DrawRectangle`, axis aligned.
    pub fn rectangle(&mut self, p0: [f64; 2], p1: [f64; 2]) -> &mut Self {
        let i = self.push_coords(&[p0[0], p0[1], p1[0], p1[1]]);
        self.emit_geom(Op::Rectangle, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::DrawCurve`, a cubic Bezier.
    pub fn curve(
        &mut self,
        start: [f64; 2],
        control_a: [f64; 2],
        control_b: [f64; 2],
        end: [f64; 2],
    ) -> &mut Self {
        let i = self.push_coords(&[
            start[0], start[1], control_a[0], control_a[1], control_b[0], control_b[1], end[0],
            end[1],
        ]);
        self.emit_geom(Op::Curve, 0, [i, 0, 0, 0, 0])
    }

    /// `GAL::DrawEllipse`.
    pub fn ellipse(
        &mut self,
        center: [f64; 2],
        major_radius: f64,
        minor_radius: f64,
        rotation: f64,
    ) -> &mut Self {
        let i = self.push_coords(&[center[0], center[1], major_radius, minor_radius]);
        let r = self.push_coords(&[rotation]);
        self.emit_geom(Op::Ellipse, 0, [i, r, 0, 0, 0])
    }

    /// An elliptical arc stroked with an explicit width.
    #[allow(clippy::too_many_arguments)]
    pub fn ellipse_arc(
        &mut self,
        center: [f64; 2],
        major_radius: f64,
        minor_radius: f64,
        rotation: f64,
        start_angle: f64,
        end_angle: f64,
        width: f64,
    ) -> &mut Self {
        let i = self.push_coords(&[center[0], center[1], major_radius, minor_radius]);
        let a = self.push_coords(&[rotation, start_angle, end_angle]);
        let w = self.push_coords(&[width]);
        self.emit_geom(Op::EllipseArc, 0, [i, a, w, 0, 0])
    }

    /// A plated hole wall. pcbnew only.
    pub fn hole_wall(&mut self, center: [f64; 2], radius: f64, width: f64) -> &mut Self {
        let i = self.push_coords(&[center[0], center[1], radius]);
        let w = self.push_coords(&[width]);
        self.emit_geom(Op::HoleWall, 0, [i, w, 0, 0, 0])
    }

    /// Register an image and return its index in the image table.
    pub fn add_image(
        &mut self,
        width: u32,
        height: u32,
        format: ImageFormat,
        pixels: &[u8],
    ) -> u32 {
        let expected = (width as usize)
            .saturating_mul(height as usize)
            .saturating_mul(format.bytes_per_pixel() as usize);
        if expected != pixels.len() {
            self.fail(BuildError::ImageSizeMismatch {
                expected,
                found: pixels.len(),
            });
            return 0;
        }
        let offset = self.image_data.len() as u64;
        self.image_data.extend_from_slice(pixels);
        self.images.push(kgds_image {
            width,
            height,
            format: format as u32,
            reserved: 0,
            data_offset: offset,
            data_length: pixels.len() as u64,
        });
        (self.images.len() - 1) as u32
    }

    /// `GAL::DrawBitmap`, placing a registered image by an affine transform.
    pub fn bitmap(&mut self, image: u32, placement: Affine, alpha: f64) -> &mut Self {
        let t = self.push_coords(&placement.0);
        let a = self.push_coords(&[alpha]);
        self.emit_geom(Op::Bitmap, 0, [image, t, a, 0, 0])
    }

    // -- overlays ----------------------------------------------------------

    /// `GAL::DrawGrid`, passed as parameters rather than expanded to lines.
    pub fn grid(
        &mut self,
        origin: [f64; 2],
        size: [f64; 2],
        line_width_px: f64,
        style: GridStyle,
        color: Color,
    ) -> &mut Self {
        let i = self.push_coords(&[
            origin[0],
            origin[1],
            size[0],
            size[1],
            line_width_px,
            style as u32 as f64,
        ]);
        self.emit(Op::Grid, 0, [i, color.to_packed(), 0, 0, 0])
    }

    /// `GAL::DrawCursor`.
    pub fn cursor(&mut self, position: [f64; 2], color: Color) -> &mut Self {
        let i = self.push_coords(&[position[0], position[1]]);
        self.emit(Op::Cursor, 0, [i, color.to_packed(), 0, 0, 0])
    }

    // -- groups ------------------------------------------------------------

    /// `GAL::BeginGroup`. Commands recorded until [`StreamBuilder::end_group`]
    /// form the group's body.
    ///
    /// `serial` is what the renderer compares against its cached GPU buffer;
    /// bump it whenever the body changes and leave it alone otherwise.
    pub fn begin_group(&mut self, id: u32, serial: u32) -> &mut Self {
        if let Some((open, _, _)) = self.open_group {
            self.fail(BuildError::NestedGroup {
                open,
                attempted: id,
            });
            return self;
        }
        if self.groups.iter().any(|g| g.id == id) {
            self.fail(BuildError::DuplicateGroupId { id });
            return self;
        }
        self.open_group = Some((id, serial, self.group_cmds.len()));
        self
    }

    /// `GAL::EndGroup`.
    pub fn end_group(&mut self) -> &mut Self {
        let Some((id, serial, first)) = self.open_group.take() else {
            self.fail(BuildError::EndGroupWithoutBegin);
            return self;
        };
        self.groups.push(kgds_group {
            id,
            serial,
            first_cmd: first as u32,
            cmd_count: (self.group_cmds.len() - first) as u32,
        });
        self
    }

    /// `GAL::DrawGroup`.
    pub fn draw_group(&mut self, id: u32) -> &mut Self {
        self.emit(Op::DrawGroup, 0, [id, 0, 0, 0, 0])
    }

    /// `GAL::DrawGroup` with the colour and depth overrides `KIGFX::VIEW`
    /// applies to a cached item when it is selected or highlighted.
    ///
    /// The overrides ride on the replay rather than being recorded into the
    /// body, which is what lets a renderer keep its cached tessellation for the
    /// group across a selection change.
    pub fn draw_group_with(
        &mut self,
        id: u32,
        color: Option<Color>,
        depth: Option<f64>,
    ) -> &mut Self {
        let mut f = 0u16;
        let mut arg1 = 0u32;
        let mut arg2 = 0u32;
        if let Some(c) = color {
            f |= flags::GROUP_COLOR;
            arg1 = c.to_packed();
        }
        if let Some(d) = depth {
            f |= flags::GROUP_DEPTH;
            arg2 = self.push_coords(&[d]);
        }
        self.emit(Op::DrawGroup, f, [id, arg1, arg2, 0, 0])
    }

    /// Record a whole group in one call.
    pub fn group(&mut self, id: u32, serial: u32, body: impl FnOnce(&mut Self)) -> &mut Self {
        self.begin_group(id, serial);
        body(self);
        self.end_group()
    }

    /// Bump an already-recorded group's serial, as a producer does when it
    /// re-records that item. The body is not touched, so this is only useful
    /// in tests that want to force a cache miss.
    pub fn set_group_serial(&mut self, id: u32, serial: u32) -> &mut Self {
        if let Some(g) = self.groups.iter_mut().find(|g| g.id == id) {
            g.serial = serial;
        }
        self
    }

    // -- frame lifecycle ---------------------------------------------------

    /// Discard the frame body and its coordinate arena.
    ///
    /// This is exactly what the producer does between frames. Because the two
    /// arenas are independent, it cannot disturb a group index, which is what
    /// lets the renderer's `(id, serial)` cache survive a pan untouched.
    pub fn clear_frame(&mut self) -> &mut Self {
        self.frame_cmds.clear();
        self.frame_coords.clear();
        self
    }

    /// Number of commands recorded into group bodies so far.
    pub fn group_cmd_count(&self) -> usize {
        self.group_cmds.len()
    }

    /// Number of commands recorded into the frame body so far.
    pub fn frame_cmd_count(&self) -> usize {
        self.frame_cmds.len()
    }

    /// Validate and produce the stream.
    ///
    /// Consumes the builder, because a latched error must not be reportable
    /// only once while the builder stays usable. Clone first if you need to
    /// keep recording.
    pub fn finish(self) -> Result<Stream, BuildError> {
        if let Some(e) = self.error {
            return Err(e);
        }
        if let Some((id, _, _)) = self.open_group {
            return Err(BuildError::UnterminatedGroup { id });
        }

        let parts = StreamParts {
            version: KGDS_VERSION,
            flags: self.flags,
            group_cmds: &self.group_cmds,
            group_coords: &self.group_coords,
            groups: &self.groups,
            frame_cmds: &self.frame_cmds,
            frame_coords: &self.frame_coords,
            strings: &self.strings,
            images: &self.images,
            image_data: &self.image_data,
        };

        Ok(Stream::from_parts(parts)?)
    }
}
