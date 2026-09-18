// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Turning draw-stream commands into [`Batch`]es.
//!
//! This is the part of the renderer worth testing hardest, because it is where
//! every drawing rule SCH_PAINTER encoded has to survive the crossing. It is
//! pure: a stream in, geometry out, no window, no GPU, no gpui types.

use std::collections::HashMap;

use kicad_gal::{Affine, Color, Command, GridStyle, Instruction, StreamView, Target};

use crate::camera::WorldRect;
use crate::geometry::{
    flatten_arc, flatten_circle, flatten_cubic, flatten_ellipse_arc, Projector,
};
use crate::scene::{signed_area2, BatchBuilder, Contour, Geometry, PackedColor, PixelRect, Polyline};

/// A grid whose marks are closer together than this on screen is not drawn.
///
/// `GAL::DrawGrid` does the same thing with `m_gridMinSpacing`: past a certain
/// density the grid stops conveying anything and starts costing everything.
pub const GRID_MIN_SPACING_PX: f64 = 10.0;

/// Ceiling on the marks a single grid may produce, whatever the viewport.
///
/// With the minimum spacing above, a 4K viewport asks for about 80 000; the
/// limit is there so that an absurd viewport or a corrupt pitch cannot turn
/// one command into unbounded work.
pub const MAX_GRID_MARKS: usize = 200_000;

/// Half-length, in pixels, of each arm of the cursor crosshair.
pub const CURSOR_ARM_PX: f32 = 8.0;

/// Half-length, in pixels, of each arm of a `SMALL_CROSS` grid mark.
pub const GRID_CROSS_ARM_PX: f32 = 3.0;

/// The `KIGFX::GAL` state a stream mutates as it is replayed.
///
/// Defaults match `GAL`'s own constructor: stroking on, filling off, a line
/// width of one internal unit and a one-pixel floor under it.
#[derive(Clone, Debug, PartialEq)]
pub struct GalState {
    /// Whether shapes are filled.
    pub is_fill: bool,
    /// Whether shapes are stroked.
    pub is_stroke: bool,
    /// Fill colour.
    pub fill_color: Color,
    /// Stroke colour.
    pub stroke_color: Color,
    /// Hover highlight colour. Recorded but not used by any geometry opcode.
    pub hover_color: Color,
    /// Stroke width in world units.
    pub line_width: f64,
    /// Floor applied to stroke widths after scaling, in pixels.
    pub min_line_width_px: f64,
    /// Painter's-algorithm depth. Smaller is nearer the viewer.
    pub layer_depth: f64,
    /// The current transform.
    pub transform: Affine,
    /// The current render target.
    pub target: Target,
    /// Negative draw mode, a pcbnew feature with no schematic effect.
    pub negative: bool,
}

impl Default for GalState {
    fn default() -> Self {
        GalState {
            is_fill: false,
            is_stroke: true,
            fill_color: Color::BLACK,
            stroke_color: Color::BLACK,
            hover_color: Color::WHITE,
            line_width: 1.0,
            min_line_width_px: 1.0,
            layer_depth: 0.0,
            transform: Affine::IDENTITY,
            target: Target::Cached,
            negative: false,
        }
    }
}

/// What a translation pass did, for benchmarks and for diagnosing a frame.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Stats {
    /// Commands walked.
    pub commands: usize,
    /// Primitives produced before batching.
    pub primitives: usize,
    /// Batches produced after it.
    pub batches: usize,
    /// Points across every batch.
    pub points: usize,
    /// Commands that this renderer does not draw. Today that is only
    /// `KGDS_OP_BITMAP`; see [`crate::translate`]'s notes.
    pub unsupported: usize,
}

impl Stats {
    /// Fold another pass's counts in.
    pub fn add(&mut self, other: &Stats) {
        self.commands += other.commands;
        self.primitives += other.primitives;
        self.batches += other.batches;
        self.points += other.points;
        self.unsupported += other.unsupported;
    }
}

/// One primitive, before batching, tagged with the depth it was drawn at.
#[derive(Clone, Debug)]
enum Prim {
    Stroke {
        color: PackedColor,
        width_px: f32,
        polyline: Polyline,
    },
    Fill {
        color: PackedColor,
        contours: Vec<Contour>,
    },
    Quad {
        color: PackedColor,
        rect: PixelRect,
    },
}

#[derive(Clone, Debug)]
struct Pending {
    depth: f64,
    prim: Prim,
}

/// Accumulates primitives from one scope, then batches them.
struct Emitter {
    proj: Projector,
    pending: Vec<Pending>,
    bounds: WorldRect,
    stats: Stats,
    scratch: Vec<[f64; 2]>,
    /// The nearest depth anything was drawn at, which is what decides where a
    /// whole group sits relative to its neighbours.
    min_depth: f64,
    /// Index in `pending` of the filled outline that subsequent `HOLE`
    /// contours belong to.
    ///
    /// Tracked explicitly rather than looked for at the back of the list,
    /// because with stroking on an outline emits a fill *and* a stroke, and
    /// each hole emits a stroke of its own, so the fill is soon several
    /// entries back.
    open_fill: Option<usize>,
}

impl Emitter {
    fn new(proj: Projector) -> Emitter {
        Emitter {
            proj,
            pending: Vec::new(),
            bounds: WorldRect::EMPTY,
            stats: Stats::default(),
            scratch: Vec::new(),
            min_depth: f64::INFINITY,
            open_fill: None,
        }
    }

    /// Project world points into pixel space, growing the world bounds as it
    /// goes. The bounds are kept in world units because they outlive any one
    /// camera: culling and the spatial index both work in world space.
    fn project(&mut self, world: &[[f64; 2]]) -> Vec<[f32; 2]> {
        let mut out = Vec::with_capacity(world.len());
        for w in world {
            self.bounds.extend(*w);
            out.push(self.proj.point(*w));
        }
        out
    }

    /// True when nothing has been emitted into this scope yet.
    fn is_empty(&self) -> bool {
        self.pending.is_empty()
    }

    fn push(&mut self, depth: f64, prim: Prim) {
        self.stats.primitives += 1;
        if depth < self.min_depth {
            self.min_depth = depth;
        }
        self.pending.push(Pending { depth, prim });
    }

    /// Batch the collected primitives.
    ///
    /// `sort_by_depth` reorders stably by layer depth, farthest first. That is
    /// safe inside a group body — a group is one item's own geometry, and depth
    /// is exactly the ordering the painter asked for — and it lets a symbol's
    /// fills and strokes collapse into two batches instead of alternating. It
    /// is *not* safe across the frame body, where `KIGFX::VIEW` has already
    /// chosen the order.
    fn finish(mut self, sort_by_depth: bool) -> (Geometry, WorldRect, Stats, f64) {
        if sort_by_depth {
            // Larger depth is farther away and must be painted first. A stable
            // sort keeps stream order within one depth.
            self.pending
                .sort_by(|a, b| b.depth.partial_cmp(&a.depth).unwrap_or(std::cmp::Ordering::Equal));
        }

        let mut builder = BatchBuilder::new();
        for p in self.pending {
            match p.prim {
                Prim::Stroke {
                    color,
                    width_px,
                    polyline,
                } => builder.stroke(color, width_px, polyline),
                Prim::Fill { color, contours } => builder.fill(color, contours),
                Prim::Quad { color, rect } => builder.quad(color, rect),
            }
        }
        let geometry = builder.finish();
        self.stats.batches = geometry.batches.len();
        self.stats.points = geometry.point_count();
        (geometry, self.bounds, self.stats, self.min_depth)
    }
}

/// Stroke width in pixels for the current state, with the GAL floor applied.
///
/// `GAL` treats a width of zero as "the thinnest line the device can draw", and
/// applies `m_minLineWidth` as a floor in device pixels so that a hairline does
/// not vanish when zoomed out.
fn stroke_width_px(width_world: f64, state: &GalState, proj: &Projector) -> f32 {
    let scaled = width_world * state.transform.uniform_scale() * proj.scale();
    let floored = if scaled.is_finite() {
        scaled.max(state.min_line_width_px.max(0.0))
    } else {
        state.min_line_width_px.max(1.0)
    };
    floored.max(f64::MIN_POSITIVE) as f32
}

/// Replay one command run into an emitter.
///
/// `depth` is carried through `state`; `view` is needed so that a `DRAW_GROUP`
/// inside a group body can be replayed inline. Recursion is bounded because
/// `StreamView` construction already proved the group graph acyclic and no
/// deeper than `kicad_gal::MAX_GROUP_DEPTH`.
fn replay<'a>(
    view: &StreamView<'a>,
    instructions: impl Iterator<Item = Instruction<'a>>,
    state: &mut GalState,
    stack: &mut Vec<Affine>,
    emitter: &mut Emitter,
    visible: &WorldRect,
) {
    for inst in instructions {
        emitter.stats.commands += 1;
        apply(view, inst, state, stack, emitter, visible);
    }
}

fn apply<'a>(
    view: &StreamView<'a>,
    inst: Instruction<'a>,
    state: &mut GalState,
    stack: &mut Vec<Affine>,
    em: &mut Emitter,
    visible: &WorldRect,
) {
    let t = state.transform;
    let depth = state.layer_depth;

    match inst.command {
        // -- state ---------------------------------------------------------
        Command::SetIsFill { enabled } => state.is_fill = enabled,
        Command::SetIsStroke { enabled } => state.is_stroke = enabled,
        Command::SetFillColor { color } => state.fill_color = color,
        Command::SetStrokeColor { color } => state.stroke_color = color,
        Command::SetHoverColor { color } => state.hover_color = color,
        Command::SetLineWidth { width } => state.line_width = width,
        Command::SetMinLineWidth { width } => state.min_line_width_px = width,
        Command::SetLayerDepth { depth } => state.layer_depth = depth,
        Command::SetNegativeDrawMode { enabled } => state.negative = enabled,
        Command::SetTarget { target } => state.target = target,
        // Depth testing is a GPU concept the gpui path does not have: ordering
        // comes from paint order plus the depth sort inside a group.
        Command::EnableDepthTest { .. } => {}

        // -- transforms ----------------------------------------------------
        // `GAL::Transform` composes onto the current matrix rather than
        // replacing it, and so do the three shorthands.
        Command::Transform { matrix } => state.transform = matrix.then(state.transform),
        Command::Rotate { radians } => {
            state.transform = Affine::rotation(radians).then(state.transform)
        }
        Command::Translate { offset } => {
            state.transform = Affine::translation(offset[0], offset[1]).then(state.transform)
        }
        Command::Scale { factor } => {
            state.transform = Affine::scale(factor[0], factor[1]).then(state.transform)
        }
        Command::Save => stack.push(state.transform),
        Command::Restore => {
            if let Some(m) = stack.pop() {
                state.transform = m;
            }
        }

        // -- structural ----------------------------------------------------
        Command::DrawGroup {
            id,
            color_override,
            depth_override,
        } => {
            if let Some(body) = view.group_body(id) {
                // A nested group inherits the state at the reference point,
                // which is what `GAL::DrawGroup` does, plus whatever the
                // replay overrides.
                let saved = state.clone();
                if let Some(c) = color_override {
                    state.stroke_color = c;
                    state.fill_color = c;
                }
                if let Some(d) = depth_override {
                    state.layer_depth = d;
                }
                replay(view, body, state, stack, em, visible);
                *state = saved;
            }
        }
        Command::ClearScreen { color } => {
            // The viewport, expressed in the emitter's own pixel space.
            let rect = PixelRect::from_corners(
                [f32::MIN / 4.0, f32::MIN / 4.0],
                [f32::MAX / 4.0, f32::MAX / 4.0],
            );
            em.push(
                f64::INFINITY,
                Prim::Quad {
                    color: color.to_packed(),
                    rect,
                },
            );
        }
        Command::Nop
        | Command::BeginFrame { .. }
        | Command::EndFrame
        | Command::ClearTarget { .. }
        // Difference and negative layers are pcbnew overlay features with no
        // schematic equivalent, and gpui has no blend-mode primitive to carry
        // them anyway.
        | Command::StartDiffLayer
        | Command::EndDiffLayer
        | Command::StartNegativesLayer
        | Command::EndNegativesLayer => {}

        // -- geometry ------------------------------------------------------
        Command::Line { p0, p1 } => {
            let w = stroke_width_px(state.line_width, state, &em.proj);
            let pts = [t.apply(p0), t.apply(p1)];
            let points = em.project(&pts);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points,
                        closed: false,
                    },
                },
            );
        }
        Command::Segment { p0, p1, width } => {
            let w = stroke_width_px(width, state, &em.proj);
            let pts = [t.apply(p0), t.apply(p1)];
            let points = em.project(&pts);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points,
                        closed: false,
                    },
                },
            );
        }
        Command::SegmentChain { points, width } => {
            let w = stroke_width_px(width, state, &em.proj);
            let world: Vec<[f64; 2]> = points.iter().map(|p| t.apply(p)).collect();
            let projected = em.project(&world);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points: projected,
                        closed: false,
                    },
                },
            );
        }
        Command::Polyline { points, closed } => {
            let w = stroke_width_px(state.line_width, state, &em.proj);
            let world: Vec<[f64; 2]> = points.iter().map(|p| t.apply(p)).collect();
            let projected = em.project(&world);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points: projected,
                        closed,
                    },
                },
            );
        }
        Command::Polygon { points, hole } => {
            let world: Vec<[f64; 2]> = points.iter().map(|p| t.apply(p)).collect();
            let projected = em.project(&world);
            emit_contour(em, state, depth, projected, hole);
        }
        Command::Circle { center, radius } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_circle(center, radius, t, em.proj.scale(), &mut pts);
            let projected = em.project(&pts);
            em.scratch = pts;
            emit_closed_shape(em, state, depth, projected);
        }
        Command::Arc {
            center,
            radius,
            start_angle,
            end_angle,
        } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_arc(center, radius, start_angle, end_angle, t, em.proj.scale(), &mut pts);
            let projected = em.project(&pts);
            em.scratch = pts;
            // An arc is stroked with the current width. When only filling is
            // enabled it is closed into a chord, matching how the GAL backends
            // fill an arc.
            if state.is_stroke || !state.is_fill {
                let w = stroke_width_px(state.line_width, state, &em.proj);
                em.push(
                    depth,
                    Prim::Stroke {
                        color: state.stroke_color.to_packed(),
                        width_px: w,
                        polyline: Polyline {
                            points: projected,
                            closed: false,
                        },
                    },
                );
            } else {
                emit_contour(em, state, depth, projected, false);
            }
        }
        Command::ArcSegment {
            center,
            radius,
            start_angle,
            end_angle,
            width,
        } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_arc(center, radius, start_angle, end_angle, t, em.proj.scale(), &mut pts);
            let projected = em.project(&pts);
            em.scratch = pts;
            let w = stroke_width_px(width, state, &em.proj);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points: projected,
                        closed: false,
                    },
                },
            );
        }
        Command::Rectangle { p0, p1 } => {
            let corners = [
                t.apply(p0),
                t.apply([p1[0], p0[1]]),
                t.apply(p1),
                t.apply([p0[0], p1[1]]),
            ];
            let projected = em.project(&corners);
            emit_closed_shape(em, state, depth, projected);
        }
        Command::Curve {
            start,
            control_a,
            control_b,
            end,
        } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_cubic(start, control_a, control_b, end, t, em.proj.scale(), &mut pts);
            let projected = em.project(&pts);
            em.scratch = pts;
            let w = stroke_width_px(state.line_width, state, &em.proj);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points: projected,
                        closed: false,
                    },
                },
            );
        }
        Command::Ellipse {
            center,
            major_radius,
            minor_radius,
            rotation,
        } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_ellipse_arc(
                center,
                major_radius,
                minor_radius,
                rotation,
                0.0,
                std::f64::consts::TAU,
                t,
                em.proj.scale(),
                &mut pts,
            );
            // The parameterisation is inclusive of both ends, which for a full
            // turn repeats the first point.
            pts.pop();
            let projected = em.project(&pts);
            em.scratch = pts;
            emit_closed_shape(em, state, depth, projected);
        }
        Command::EllipseArc {
            center,
            major_radius,
            minor_radius,
            rotation,
            start_angle,
            end_angle,
            width,
        } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_ellipse_arc(
                center,
                major_radius,
                minor_radius,
                rotation,
                start_angle,
                end_angle,
                t,
                em.proj.scale(),
                &mut pts,
            );
            let projected = em.project(&pts);
            em.scratch = pts;
            let w = stroke_width_px(width, state, &em.proj);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points: projected,
                        closed: false,
                    },
                },
            );
        }
        Command::HoleWall {
            center,
            radius,
            width,
        } => {
            em.scratch.clear();
            let mut pts = std::mem::take(&mut em.scratch);
            flatten_circle(center, radius, t, em.proj.scale(), &mut pts);
            let projected = em.project(&pts);
            em.scratch = pts;
            let w = stroke_width_px(width, state, &em.proj);
            em.push(
                depth,
                Prim::Stroke {
                    color: state.stroke_color.to_packed(),
                    width_px: w,
                    polyline: Polyline {
                        points: projected,
                        closed: true,
                    },
                },
            );
        }

        // gpui's only image primitive is `PolychromeSprite`, which carries no
        // transformation matrix, so a placed bitmap cannot be rotated or
        // sheared — and a schematic's bitmaps generally are. Rather than draw
        // them wrong, they are counted and skipped. See the crate docs.
        Command::Bitmap { .. } => em.stats.unsupported += 1,

        // -- overlays ------------------------------------------------------
        Command::Grid {
            origin,
            size,
            line_width_px,
            style,
            color,
        } => emit_grid(em, depth, origin, size, line_width_px, style, color, visible),
        Command::Cursor { position, color } => {
            let c = em.proj.point(t.apply(position));
            let a = CURSOR_ARM_PX;
            let packed = color.to_packed();
            let w = 1.0f32.max(state.min_line_width_px as f32);
            em.push(
                depth,
                Prim::Stroke {
                    color: packed,
                    width_px: w,
                    polyline: Polyline {
                        points: vec![[c[0] - a, c[1]], [c[0] + a, c[1]]],
                        closed: false,
                    },
                },
            );
            em.push(
                depth,
                Prim::Stroke {
                    color: packed,
                    width_px: w,
                    polyline: Polyline {
                        points: vec![[c[0], c[1] - a], [c[0], c[1] + a]],
                        closed: false,
                    },
                },
            );
        }

        // `Command` is marked non-exhaustive, so a later ABI version can add an
        // opcode this build has never seen. `kicad_gal` refuses to decode one,
        // so this arm is unreachable today; counting it means that if the two
        // crates ever go out of step the result is a number rather than
        // geometry quietly going missing.
        _ => em.stats.unsupported += 1,
    }
}

/// Emit a shape that is closed: filled, stroked, or both, per the state.
fn emit_closed_shape(
    em: &mut Emitter,
    state: &GalState,
    depth: f64,
    points: Vec<[f32; 2]>,
) {
    if state.is_fill {
        em.push(
            depth,
            Prim::Fill {
                color: state.fill_color.to_packed(),
                contours: vec![Contour {
                    points: points.clone(),
                }],
            },
        );
    }
    if state.is_stroke {
        let w = stroke_width_px(state.line_width, state, &em.proj);
        em.push(
            depth,
            Prim::Stroke {
                color: state.stroke_color.to_packed(),
                width_px: w,
                polyline: Polyline {
                    points,
                    closed: true,
                },
            },
        );
    }
}

/// Emit a `POLYGON` contour, attaching holes to the outline they belong to.
///
/// `SHAPE_POLY_SET` reaches the stream as an outline followed by its holes,
/// with no promise about winding. Reversing a hole whose winding matches its
/// outline makes the non-zero fill rule subtract it either way, which is what
/// lets every polygon of one colour share a single tessellation.
fn emit_contour(
    em: &mut Emitter,
    state: &GalState,
    depth: f64,
    points: Vec<[f32; 2]>,
    hole: bool,
) {
    let stroke = |em: &mut Emitter, points: Vec<[f32; 2]>| {
        let w = stroke_width_px(state.line_width, state, &em.proj);
        em.push(
            depth,
            Prim::Stroke {
                color: state.stroke_color.to_packed(),
                width_px: w,
                polyline: Polyline {
                    points,
                    closed: true,
                },
            },
        );
    };

    if hole {
        let mut points = points;
        if state.is_fill {
            let slot = match em.open_fill {
                Some(i) => em.pending.get_mut(i),
                None => None,
            };
            if let Some(Pending {
                prim: Prim::Fill { contours, .. },
                ..
            }) = slot
            {
                let outline_sign = contours
                    .first()
                    .map(|c| signed_area2(&c.points))
                    .unwrap_or(0.0);
                if signed_area2(&points) * outline_sign > 0.0 {
                    points.reverse();
                }
                contours.push(Contour {
                    points: points.clone(),
                });
            }
            // A hole with no outline before it is a malformed contour run.
            // Nothing is added to the fill, so no solid shape appears where a
            // hole was meant; its edge is still stroked below if stroking is on,
            // because that edge is visible either way.
        }
        if state.is_stroke {
            stroke(em, points);
        }
        return;
    }

    if state.is_fill {
        em.open_fill = Some(em.pending.len());
        em.push(
            depth,
            Prim::Fill {
                color: state.fill_color.to_packed(),
                contours: vec![Contour {
                    points: points.clone(),
                }],
            },
        );
    } else {
        em.open_fill = None;
    }
    if state.is_stroke {
        stroke(em, points);
    }
}

/// Expand `KGDS_OP_GRID` into marks covering the visible area.
///
/// The stream carries the grid as parameters rather than as the thousands of
/// lines it stands for, so this is where those lines come from. gpui has no
/// shader hook, so they have to be real primitives — but quads are instanced
/// and a full row of the grid is one polyline, so the cost stays bounded.
#[allow(clippy::too_many_arguments)]
fn emit_grid(
    em: &mut Emitter,
    depth: f64,
    origin: [f64; 2],
    size: [f64; 2],
    line_width_px: f64,
    style: GridStyle,
    color: Color,
    visible: &WorldRect,
) {
    if visible.is_empty() || size[0] <= 0.0 || size[1] <= 0.0 {
        return;
    }
    let scale = em.proj.scale();
    let pitch_px = [size[0] * scale, size[1] * scale];
    if pitch_px[0] < GRID_MIN_SPACING_PX || pitch_px[1] < GRID_MIN_SPACING_PX {
        // Denser than this the grid is a grey wash: GAL hides it too.
        return;
    }

    let first = |min: f64, o: f64, step: f64| ((min - o) / step).ceil();
    let count = |min: f64, max: f64, step: f64| ((max - min) / step).floor() as i64 + 1;

    let i0 = first(visible.min[0], origin[0], size[0]);
    let j0 = first(visible.min[1], origin[1], size[1]);
    let x0 = origin[0] + i0 * size[0];
    let y0 = origin[1] + j0 * size[1];
    let nx = count(x0, visible.max[0], size[0]).max(0) as usize;
    let ny = count(y0, visible.max[1], size[1]).max(0) as usize;
    if nx == 0 || ny == 0 {
        return;
    }

    let packed = color.to_packed();
    let width_px = (line_width_px.max(1.0)) as f32;

    match style {
        GridStyle::Lines => {
            if nx + ny > MAX_GRID_MARKS {
                return;
            }
            // One polyline per line; they all share a batch, so the whole grid
            // is a single tessellation.
            for i in 0..nx {
                let x = x0 + i as f64 * size[0];
                let a = em.proj.point([x, visible.min[1]]);
                let b = em.proj.point([x, visible.max[1]]);
                em.push(
                    depth,
                    Prim::Stroke {
                        color: packed,
                        width_px,
                        polyline: Polyline {
                            points: vec![a, b],
                            closed: false,
                        },
                    },
                );
            }
            for j in 0..ny {
                let y = y0 + j as f64 * size[1];
                let a = em.proj.point([visible.min[0], y]);
                let b = em.proj.point([visible.max[0], y]);
                em.push(
                    depth,
                    Prim::Stroke {
                        color: packed,
                        width_px,
                        polyline: Polyline {
                            points: vec![a, b],
                            closed: false,
                        },
                    },
                );
            }
        }
        GridStyle::Dots => {
            if nx.saturating_mul(ny) > MAX_GRID_MARKS {
                return;
            }
            let half = width_px.max(1.0) * 0.5;
            for j in 0..ny {
                let y = y0 + j as f64 * size[1];
                for i in 0..nx {
                    let x = x0 + i as f64 * size[0];
                    let c = em.proj.point([x, y]);
                    em.push(
                        depth,
                        Prim::Quad {
                            color: packed,
                            rect: PixelRect::from_corners(
                                [c[0] - half, c[1] - half],
                                [c[0] + half, c[1] + half],
                            ),
                        },
                    );
                }
            }
        }
        GridStyle::SmallCross => {
            if nx.saturating_mul(ny).saturating_mul(2) > MAX_GRID_MARKS {
                return;
            }
            let a = GRID_CROSS_ARM_PX;
            for j in 0..ny {
                let y = y0 + j as f64 * size[1];
                for i in 0..nx {
                    let x = x0 + i as f64 * size[0];
                    let c = em.proj.point([x, y]);
                    em.push(
                        depth,
                        Prim::Stroke {
                            color: packed,
                            width_px,
                            polyline: Polyline {
                                points: vec![[c[0] - a, c[1]], [c[0] + a, c[1]]],
                                closed: false,
                            },
                        },
                    );
                    em.push(
                        depth,
                        Prim::Stroke {
                            color: packed,
                            width_px,
                            polyline: Polyline {
                                points: vec![[c[0], c[1] - a], [c[0], c[1] + a]],
                                closed: false,
                            },
                        },
                    );
                }
            }
        }
    }
}

/// Translated geometry for one cached group, in group-local pixel space.
#[derive(Clone, Debug, PartialEq)]
pub struct GroupGeometry {
    /// The batches, in paint order.
    pub geometry: Geometry,
    /// The group's extent in world units, for culling and hit testing.
    pub bounds: WorldRect,
    /// The world point group-local pixel space is measured from.
    pub anchor: [f64; 2],
    /// Pixels per world unit this geometry was flattened at.
    pub scale: f64,
    /// The nearest layer depth anything in the group was drawn at, which is
    /// where the group as a whole sits among its neighbours. Infinite when the
    /// group drew nothing.
    pub min_depth: f64,
    /// What the pass cost.
    pub stats: Stats,
}

/// Translate one group body.
///
/// The geometry comes out in *group-local* pixel space: `anchor` maps to the
/// origin and `scale` pixels stand for one world unit. Nothing about the camera
/// enters, which is what lets the result be cached across pans and reused
/// across an octave-sixteenth of zoom.
///
/// The body is replayed from a default [`GalState`]. That matches how
/// `KIGFX::VIEW` records: a group holds one item's complete geometry including
/// its own colour and width changes, and is replayed with `DrawGroup` rather
/// than inheriting the caller's brush.
pub fn translate_group(view: &StreamView<'_>, id: u32, scale: f64) -> Option<GroupGeometry> {
    let group = view.group(id)?;
    let anchor = group_anchor(view, id).unwrap_or([0.0, 0.0]);
    let proj = Projector::new(anchor, scale, [0.0, 0.0]);

    let mut em = Emitter::new(proj);
    let mut state = GalState::default();
    let mut stack = Vec::new();
    // A group has no viewport of its own; the grid is a frame-body overlay and
    // never appears in one.
    let visible = WorldRect::EMPTY;
    replay(
        view,
        view.group_body_at(group),
        &mut state,
        &mut stack,
        &mut em,
        &visible,
    );

    let (geometry, bounds, stats, min_depth) = em.finish(true);
    Some(GroupGeometry {
        geometry,
        bounds,
        anchor,
        scale,
        min_depth,
        stats,
    })
}

/// A stable world point to measure a group's local pixel space from.
///
/// The first coordinate pair the body references is used rather than the
/// bounding-box centre, because the anchor has to be known *before* the body is
/// walked and it only has to be close to the geometry, not central to it.
/// Anything within a sheet of the geometry keeps the local `f32` coordinates
/// small, which is all the anchor is for.
fn group_anchor(view: &StreamView<'_>, id: u32) -> Option<[f64; 2]> {
    let group = view.group(id)?;
    for inst in view.group_body_at(group) {
        let p = match inst.command {
            Command::Line { p0, .. }
            | Command::Segment { p0, .. }
            | Command::Rectangle { p0, .. } => Some(p0),
            Command::Circle { center, .. }
            | Command::Arc { center, .. }
            | Command::ArcSegment { center, .. }
            | Command::Ellipse { center, .. }
            | Command::EllipseArc { center, .. }
            | Command::HoleWall { center, .. } => Some(center),
            Command::Curve { start, .. } => Some(start),
            Command::SegmentChain { points, .. }
            | Command::Polyline { points, .. }
            | Command::Polygon { points, .. } => points.get(0),
            _ => None,
        };
        if let Some(p) = p {
            return Some(p);
        }
    }
    None
}

/// One step of a translated frame.
///
/// The frame body interleaves its own geometry with references to cached
/// groups, and the order between them is `KIGFX::VIEW`'s, so it is preserved
/// exactly rather than flattened.
#[derive(Clone, Debug, PartialEq)]
pub enum FrameItem {
    /// Geometry recorded directly in the frame body, already in screen pixels.
    Geometry(Geometry),
    /// A reference to a cached group.
    ///
    /// The overrides are carried rather than applied: substituting a colour at
    /// paint time costs nothing, whereas baking it into the geometry would
    /// throw away the cached tessellation every time the selection changed.
    Group {
        /// The group's id.
        id: u32,
        /// Replaces every colour in the group for this replay.
        color_override: Option<Color>,
        /// Replaces the group's layer depth for this replay.
        depth_override: Option<f64>,
    },
}

/// A translated frame.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Frame {
    /// What to paint, in order.
    pub items: Vec<FrameItem>,
    /// What the pass cost, excluding the group bodies.
    pub stats: Stats,
}

impl Frame {
    /// The ids of every group this frame references, in order and with
    /// duplicates kept.
    pub fn referenced_groups(&self) -> impl Iterator<Item = u32> + '_ {
        self.items.iter().filter_map(|i| match i {
            FrameItem::Group { id, .. } => Some(*id),
            FrameItem::Geometry(_) => None,
        })
    }
}

/// Translate the frame body.
///
/// `proj` carries world units to viewport pixels and `visible` is the world
/// rectangle the grid is expanded over. Groups are *not* expanded: they come
/// back as [`FrameItem::Group`] so the caller can serve them from its
/// tessellation cache.
pub fn translate_frame(
    view: &StreamView<'_>,
    proj: Projector,
    visible: &WorldRect,
) -> Frame {
    let mut frame = Frame::default();
    let mut state = GalState::default();
    let mut stack: Vec<Affine> = Vec::new();
    let mut em = Emitter::new(proj);

    for inst in view.frame() {
        frame.stats.commands += 1;
        if let Command::DrawGroup {
            id,
            color_override,
            depth_override,
        } = inst.command
        {
            // Close the run of direct geometry so that the group lands between
            // the commands that surround it, not after all of them. A frame
            // body is mostly a long run of group replays, so the common case is
            // that there is nothing to close and the emitter is left alone.
            if !em.is_empty() {
                let (geometry, _, stats, _) =
                    std::mem::replace(&mut em, Emitter::new(proj)).finish(false);
                frame.stats.add(&stats);
                if !geometry.is_empty() {
                    frame.items.push(FrameItem::Geometry(geometry));
                }
            }
            frame.items.push(FrameItem::Group {
                id,
                color_override,
                depth_override,
            });
            continue;
        }
        apply(view, inst, &mut state, &mut stack, &mut em, visible);
    }

    let (geometry, _, stats, _) = em.finish(false);
    frame.stats.add(&stats);
    if !geometry.is_empty() {
        frame.items.push(FrameItem::Geometry(geometry));
    }
    frame
}

/// World bounds of every group in the stream, by id.
///
/// Walking a body only to measure it is wasteful, so this is computed once per
/// `(id, serial)` and kept by [`crate::cache`]; it is exposed separately
/// because culling needs bounds for groups it is about to *not* tessellate.
pub fn group_bounds(view: &StreamView<'_>) -> HashMap<u32, WorldRect> {
    let mut out = HashMap::with_capacity(view.groups().len());
    for g in view.groups() {
        out.insert(g.id, measure_group(view, g.id));
    }
    out
}

/// World bounds of one group, stroke width included.
pub fn measure_group(view: &StreamView<'_>, id: u32) -> WorldRect {
    let Some(group) = view.group(id) else {
        return WorldRect::EMPTY;
    };
    // Measuring at a unit scale keeps flattening coarse, which is all that is
    // needed: chords lie inside the true curve's bounding box except by less
    // than the flattening tolerance, and the half-width margin below covers it.
    let proj = Projector::new([0.0, 0.0], 1.0, [0.0, 0.0]);
    let mut em = Emitter::new(proj);
    let mut state = GalState::default();
    let mut stack = Vec::new();
    let mut widest = 0.0f64;

    // Replaying is the only way to measure, because the extent depends on the
    // transform state; the emitted primitives are thrown away.
    for inst in view.group_body_at(group) {
        if let Command::SetLineWidth { width } = inst.command {
            widest = widest.max(width);
        }
        if let Command::Segment { width, .. }
        | Command::SegmentChain { width, .. }
        | Command::ArcSegment { width, .. }
        | Command::EllipseArc { width, .. }
        | Command::HoleWall { width, .. } = inst.command
        {
            widest = widest.max(width);
        }
        apply(view, inst, &mut state, &mut stack, &mut em, &WorldRect::EMPTY);
    }
    let (_, bounds, _, _) = em.finish(false);
    // Half a stroke width spills outside the centreline on every side.
    bounds.inflated(widest * 0.5)
}
