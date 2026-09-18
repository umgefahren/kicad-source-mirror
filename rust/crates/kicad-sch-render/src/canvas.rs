// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The renderer's state, and the gpui element that paints it.
//!
//! [`SchematicRenderer`] holds everything that has to survive between frames:
//! the stream, the camera, the tessellation cache and the spatial index. It is
//! deliberately free of gpui types except at the edges, so that the whole of a
//! frame's work — translation, culling, cache lookup, tessellation — can be run
//! and asserted on without a window.
//!
//! [`SchematicCanvas`] is the thin gpui `Element` on top: it hands the element
//! bounds to the renderer, asks for a prepared frame, and paints it inside a
//! single `paint_layer` so that every path lands in one gpui batch and
//! therefore one render pass.

use std::cell::RefCell;
use std::rc::Rc;

use gpui::{
    App, Bounds, Element, ElementId, GlobalElementId, Hitbox, HitboxBehavior, InspectorElementId,
    IntoElement, LayoutId, Pixels, Refineable, Style, StyleRefinement, Styled, Window,
};
use kicad_gal::{Stream, StreamView};

use crate::cache::{CachedGroup, CacheStats, TessellationCache};
use crate::camera::{Camera, WorldRect};
use crate::geometry::Projector;
use crate::index::BoundsIndex;
use crate::paint::{paint_tessellated, tessellate, Placement, Tessellated};
use crate::scene::PackedColor;
use crate::translate::{group_bounds, translate_frame, FrameItem, Stats};

/// Pixels of slack added to the viewport when culling, so that a wide stroke
/// whose centreline is just off screen still draws its visible half.
pub const CULL_MARGIN_PX: f64 = 64.0;

/// What one prepared frame cost.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FrameStats {
    /// Groups the frame body referenced.
    pub groups_referenced: usize,
    /// Groups that survived culling and were painted.
    pub groups_drawn: usize,
    /// Groups the viewport cull rejected.
    pub groups_culled: usize,
    /// Groups served from the tessellation cache.
    pub cache_hits: usize,
    /// Groups that had to be tessellated.
    pub cache_misses: usize,
    /// gpui paths in the prepared frame.
    pub paths: usize,
    /// gpui quads in the prepared frame.
    pub quads: usize,
    /// Vertices across every path.
    pub vertices: usize,
    /// What translating the frame body itself cost.
    pub frame_translation: Stats,
}

/// One step of a frame that is ready to paint.
enum PreparedItem {
    /// Geometry from the frame body, already in viewport pixels.
    Direct(Tessellated),
    /// A cached group and where to put it.
    Group {
        cached: Rc<CachedGroup>,
        placement: Placement,
        override_color: Option<PackedColor>,
    },
}

/// A frame resolved down to gpui primitives and their placements.
///
/// Producing one needs no window, which is what makes the expensive half of a
/// frame measurable in a benchmark and assertable in a test.
pub struct PreparedFrame {
    items: Vec<PreparedItem>,
    /// What the frame cost.
    pub stats: FrameStats,
}

impl std::fmt::Debug for PreparedFrame {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("PreparedFrame")
            .field("items", &self.items.len())
            .field("stats", &self.stats)
            .finish()
    }
}

/// Everything the renderer keeps between frames.
#[derive(Default)]
pub struct SchematicRenderer {
    stream: Option<Stream>,
    camera: Camera,
    cache: TessellationCache,
    index: BoundsIndex,
    /// The group table the index and bounds were built from, so that a stream
    /// whose groups have not changed does not rebuild either.
    indexed_groups: Vec<kicad_gal::abi::kgds_group>,
    bounds_by_id: std::collections::HashMap<u32, WorldRect>,
    document_bounds: WorldRect,
    last_stats: FrameStats,
}

impl std::fmt::Debug for SchematicRenderer {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("SchematicRenderer")
            .field("camera", &self.camera)
            .field("groups", &self.indexed_groups.len())
            .field("cache", &self.cache)
            .finish()
    }
}

impl SchematicRenderer {
    /// A renderer with no stream.
    pub fn new() -> SchematicRenderer {
        SchematicRenderer::default()
    }

    /// Replace the stream.
    ///
    /// Group bounds and the spatial index are rebuilt only when the group table
    /// has actually changed, so handing over a stream whose frame body is the
    /// only difference — which is what a pan produces — costs one comparison.
    pub fn set_stream(&mut self, stream: Stream) {
        let groups_changed = self.indexed_groups != stream.groups();
        self.stream = Some(stream);
        if groups_changed {
            self.rebuild_index();
        }
    }

    /// Copy a borrowed stream in. The view's memory belongs to the producer and
    /// is only valid until its next recording pass, so it cannot simply be
    /// held.
    pub fn set_stream_view(&mut self, view: &StreamView<'_>) {
        self.set_stream(view.to_owned_stream());
    }

    /// The stream, if one has been set.
    pub fn stream(&self) -> Option<&Stream> {
        self.stream.as_ref()
    }

    /// The camera.
    pub fn camera(&self) -> &Camera {
        &self.camera
    }

    /// The camera, mutably.
    pub fn camera_mut(&mut self) -> &mut Camera {
        &mut self.camera
    }

    /// Resize the viewport, in logical pixels.
    pub fn set_viewport(&mut self, size: [f64; 2]) {
        self.camera.set_viewport(size);
    }

    /// Pan by a screen-space delta.
    pub fn pan(&mut self, dx: f64, dy: f64) {
        self.camera.pan_by_pixels(dx, dy);
    }

    /// Zoom about a point on screen.
    pub fn zoom_to_point(&mut self, factor: f64, screen: [f64; 2]) {
        self.camera.zoom_to_point(factor, screen);
    }

    /// Frame the whole document.
    pub fn zoom_to_fit(&mut self, padding_px: f64) {
        let bounds = self.document_bounds;
        self.camera.zoom_to_fit(&bounds, padding_px);
    }

    /// The union of every group's extent, in world units.
    pub fn document_bounds(&self) -> WorldRect {
        self.document_bounds
    }

    /// The spatial index over group bounds, for culling and hit testing.
    pub fn index(&self) -> &BoundsIndex {
        &self.index
    }

    /// Ids of the groups whose bounds touch the current viewport.
    pub fn visible_groups(&self) -> Vec<u32> {
        self.index
            .query(&self.camera.visible_world_rect(CULL_MARGIN_PX))
    }

    /// Cache counters.
    pub fn cache_stats(&self) -> CacheStats {
        self.cache.stats()
    }

    /// What the last prepared frame cost.
    pub fn last_stats(&self) -> FrameStats {
        self.last_stats
    }

    /// Discard cached tessellation. Only needed when the meaning of group ids
    /// changes under the renderer, which `set_stream` already handles.
    pub fn clear_cache(&mut self) {
        self.cache.clear();
    }

    fn rebuild_index(&mut self) {
        let Some(stream) = &self.stream else {
            self.index = BoundsIndex::default();
            self.indexed_groups.clear();
            self.bounds_by_id.clear();
            self.document_bounds = WorldRect::EMPTY;
            return;
        };
        let view = stream.view();
        self.bounds_by_id = group_bounds(&view);
        self.indexed_groups = stream.groups().to_vec();
        self.index = BoundsIndex::build(self.bounds_by_id.iter().map(|(id, b)| (*id, *b)));
        let mut doc = WorldRect::EMPTY;
        for b in self.bounds_by_id.values() {
            doc.union(b);
        }
        self.document_bounds = doc;
    }

    /// Do a frame's whole CPU side: translate, cull, look up or tessellate.
    ///
    /// `origin` is where the canvas sits in the window, in logical pixels; pass
    /// `[0.0, 0.0]` outside an element.
    pub fn prepare(&mut self, origin: [f64; 2]) -> PreparedFrame {
        let mut stats = FrameStats::default();
        let Some(stream) = self.stream.take() else {
            return PreparedFrame {
                items: Vec::new(),
                stats,
            };
        };

        self.cache.begin_frame();
        let view = stream.view();
        let viewport = self.camera.viewport();
        let proj = Projector::new(
            self.camera.center(),
            self.camera.scale(),
            [
                origin[0] + viewport[0] * 0.5,
                origin[1] + viewport[1] * 0.5,
            ],
        );
        let visible = self.camera.visible_world_rect(CULL_MARGIN_PX);

        let frame = translate_frame(&view, proj, &visible);
        stats.frame_translation = frame.stats;

        let lod = self.camera.lod();
        let lod_scale = self.camera.lod_scale();
        let residual = self.camera.lod_residual() as f32;

        let mut items = Vec::with_capacity(frame.items.len());
        let mut pending_groups: Vec<PreparedItem> = Vec::new();
        let mut pending_depths: Vec<f64> = Vec::new();

        // A run of consecutive group replays is stably sorted by depth, far
        // side first. `KIGFX::VIEW` already emits them in layer order, so this
        // usually changes nothing — but when a selection carries a depth
        // override, this is what floats it above its neighbours. gpui has no
        // depth buffer, so ordering can only come from the order things are
        // painted in.
        let mut flush = |items: &mut Vec<PreparedItem>,
                         groups: &mut Vec<PreparedItem>,
                         depths: &mut Vec<f64>| {
            if groups.is_empty() {
                return;
            }
            let mut order: Vec<usize> = (0..groups.len()).collect();
            order.sort_by(|a, b| {
                depths[*b]
                    .partial_cmp(&depths[*a])
                    .unwrap_or(std::cmp::Ordering::Equal)
            });
            for i in order {
                // `std::mem::replace` keeps the indices valid while the vector
                // is drained out of order.
                let taken = std::mem::replace(
                    &mut groups[i],
                    PreparedItem::Direct(Tessellated::default()),
                );
                items.push(taken);
            }
            groups.clear();
            depths.clear();
        };

        for item in &frame.items {
            match item {
                FrameItem::Geometry(geometry) => {
                    flush(&mut items, &mut pending_groups, &mut pending_depths);
                    let t = tessellate(geometry);
                    stats.paths += t
                        .items
                        .iter()
                        .filter(|i| matches!(i, crate::paint::DrawItem::Path { .. }))
                        .count();
                    stats.quads += t
                        .items
                        .iter()
                        .filter(|i| matches!(i, crate::paint::DrawItem::Quads { .. }))
                        .count();
                    stats.vertices += t.vertices;
                    items.push(PreparedItem::Direct(t));
                }
                FrameItem::Group {
                    id,
                    color_override,
                    depth_override,
                } => {
                    stats.groups_referenced += 1;
                    // Cull before tessellating: gpui culls primitives, but only
                    // after the CPU has already paid to build them.
                    if let Some(b) = self.bounds_by_id.get(id) {
                        if !b.is_empty() && !b.intersects(&visible) {
                            stats.groups_culled += 1;
                            continue;
                        }
                    }
                    let was_cached = view
                        .group(*id)
                        .map(|g| {
                            self.cache.contains(&crate::cache::GroupKey {
                                id: *id,
                                serial: g.serial,
                                lod,
                            })
                        })
                        .unwrap_or(false);
                    let Some(cached) = self.cache.get_or_build(&view, *id, lod, lod_scale) else {
                        continue;
                    };
                    if was_cached {
                        stats.cache_hits += 1;
                    } else {
                        stats.cache_misses += 1;
                    }
                    stats.groups_drawn += 1;
                    stats.vertices += cached.tessellated.vertices;
                    stats.paths += cached
                        .tessellated
                        .items
                        .iter()
                        .filter(|i| matches!(i, crate::paint::DrawItem::Path { .. }))
                        .count();
                    stats.quads += cached
                        .tessellated
                        .items
                        .iter()
                        .filter(|i| matches!(i, crate::paint::DrawItem::Quads { .. }))
                        .count();

                    let anchor = proj.point(cached.anchor);
                    let depth = depth_override.unwrap_or(cached.min_depth);
                    pending_depths.push(depth);
                    pending_groups.push(PreparedItem::Group {
                        cached,
                        placement: Placement {
                            scale: residual,
                            offset: anchor,
                        },
                        override_color: color_override.map(|c| c.to_packed()),
                    });
                }
            }
        }
        flush(&mut items, &mut pending_groups, &mut pending_depths);

        self.stream = Some(stream);
        self.last_stats = stats;
        PreparedFrame { items, stats }
    }

    /// Paint a prepared frame.
    ///
    /// Everything goes inside one `paint_layer`, which is what merges the paths
    /// into a single gpui batch and a single render pass. Splitting this across
    /// layers is the biggest performance cliff available.
    pub fn paint(&self, frame: &PreparedFrame, bounds: Bounds<Pixels>, window: &mut Window) {
        window.paint_layer(bounds, |window| {
            for item in &frame.items {
                match item {
                    PreparedItem::Direct(t) => {
                        paint_tessellated(window, t, Placement::IDENTITY, None)
                    }
                    PreparedItem::Group {
                        cached,
                        placement,
                        override_color,
                    } => paint_tessellated(
                        window,
                        &cached.tessellated,
                        *placement,
                        *override_color,
                    ),
                }
            }
        });
    }
}

/// A gpui element that paints a [`SchematicRenderer`].
///
/// The renderer is shared rather than owned, because a gpui element is rebuilt
/// every frame while the cache and camera have to outlive it. The application
/// shell keeps the `Rc<RefCell<SchematicRenderer>>`, hands it a new stream
/// whenever the document changes, and drives the camera from its own input
/// handling.
pub struct SchematicCanvas {
    id: ElementId,
    style: StyleRefinement,
    renderer: Rc<RefCell<SchematicRenderer>>,
}

impl SchematicCanvas {
    /// A canvas painting the given renderer.
    pub fn new(id: impl Into<ElementId>, renderer: Rc<RefCell<SchematicRenderer>>) -> Self {
        SchematicCanvas {
            id: id.into(),
            style: StyleRefinement::default(),
            renderer,
        }
    }

    /// The renderer this canvas paints, for the shell to drive.
    pub fn renderer(&self) -> &Rc<RefCell<SchematicRenderer>> {
        &self.renderer
    }
}

impl Styled for SchematicCanvas {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

impl IntoElement for SchematicCanvas {
    type Element = Self;
    fn into_element(self) -> Self::Element {
        self
    }
}

/// What the canvas carries from prepaint to paint.
pub struct CanvasPrepaint {
    /// The canvas's hitbox, so the shell can hang input on it.
    pub hitbox: Hitbox,
    frame: PreparedFrame,
}

impl Element for SchematicCanvas {
    type RequestLayoutState = ();
    type PrepaintState = CanvasPrepaint;

    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.refine(&self.style);
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        window: &mut Window,
        _: &mut App,
    ) -> CanvasPrepaint {
        // The whole CPU side of the frame happens here rather than in `paint`,
        // because prepaint is where gpui expects layout-dependent work and
        // because it keeps `paint` down to issuing primitives.
        let frame = {
            let mut r = self.renderer.borrow_mut();
            r.set_viewport([
                bounds.size.width.to_f64(),
                bounds.size.height.to_f64(),
            ]);
            r.prepare([bounds.origin.x.to_f64(), bounds.origin.y.to_f64()])
        };
        CanvasPrepaint {
            hitbox: window.insert_hitbox(bounds, HitboxBehavior::Normal),
            frame,
        }
    }

    fn paint(
        &mut self,
        _: Option<&GlobalElementId>,
        _: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _: &mut (),
        prepaint: &mut CanvasPrepaint,
        window: &mut Window,
        _: &mut App,
    ) {
        self.renderer.borrow().paint(&prepaint.frame, bounds, window);
    }
}
