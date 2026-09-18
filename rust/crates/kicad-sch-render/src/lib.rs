// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Renders a recorded KiCad draw stream through gpui.
//!
//! `SCH_PAINTER` still decides how a schematic looks; `RECORDING_GAL` records
//! the calls it makes, [`kicad_gal`] decodes them, and this crate turns them
//! into pixels.
//!
//! # Why there is no wgpu in here
//!
//! The original plan was a wgpu pipeline of our own, with analytic SDF
//! primitives. gpui turns out to expose no way to reach its device, queue or
//! render pass — `gpui-pre` has no wgpu dependency at all, the live GPU context
//! is private to the platform clients, and `WgpuRenderer`'s only entry point
//! takes a finished `Scene`. Handing gpui a texture instead means a CPU round
//! trip of some 25–33 MB per 1080p frame plus a readback stall, which is not a
//! 120 Hz proposition.
//!
//! What gpui does expose is its own primitive set, and for schematic line work
//! it is enough: `PathBuilder` is a full lyon 1.0 front end, and paths are
//! rasterised through gpui's wgpu renderer at 4x MSAA. So the geometry is
//! tessellated on the CPU and the antialiasing is better than a single-sampled
//! custom pipeline would have given. See
//! `docs/rust-migration/02-gpui-kit-cookbook.md` §3 for the source reading
//! behind that.
//!
//! # How a frame is spent
//!
//! ```text
//!   StreamView ──translate──▶ Geometry ──tessellate──▶ Tessellated ──place──▶ gpui
//!                  (pure)      (batches)    (lyon)       (Path)      (vertex pass)
//! ```
//!
//! Everything up to `Tessellated` is cached per group, keyed by
//! `(id, serial, level of detail)`. `serial` changes only when the producer
//! re-records a body, so:
//!
//! * a pan re-tessellates nothing — it only moves finished vertices;
//! * a small zoom re-tessellates nothing, because the level of detail is
//!   quantised to sixteenths of an octave and the residual is applied to the
//!   vertices;
//! * selecting an item re-tessellates nothing, because `KIGFX::VIEW` recolours
//!   a cached item in place and the stream carries that as an override on the
//!   replay command.
//!
//! # Precision
//!
//! Stream coordinates are `f64` in nanometres, and a sheet can sit more than
//! 1.2e9 of them from the origin — past where an `f32` can tell neighbouring
//! units apart. The camera origin is subtracted in `f64` and only the small
//! remainder is narrowed. [`camera`] is arranged so that this is the only way
//! to reach screen space.
//!
//! # What is not drawn
//!
//! `KGDS_OP_BITMAP`. gpui's image primitive carries no transformation matrix,
//! so a placed bitmap cannot be rotated or sheared, and schematic bitmaps
//! generally are. They are counted in [`translate::Stats::unsupported`] rather
//! than drawn wrong. Difference and negative layers are pcbnew overlay
//! features with no gpui equivalent and are likewise ignored.
//!
//! # Testing
//!
//! gpui supplies no headless renderer on Linux, so golden images are not
//! available and the useful assertions are on the geometry. Almost everything
//! here is a pure function for that reason: [`translate`] and [`scene`] need no
//! window at all, and [`canvas::SchematicRenderer::prepare`] does a whole
//! frame's CPU work — cull, cache, tessellate — without one.
//!
//! ```
//! use kicad_gal::{Color, StreamBuilder};
//! use kicad_sch_render::{Camera, SchematicRenderer};
//!
//! let mut b = StreamBuilder::new();
//! b.group(0, 1, |g| {
//!     g.set_stroke_color(Color::new(0.0, 0.8, 0.0, 1.0));
//!     g.segment([0.0, 0.0], [2_540_000.0, 0.0], 152_400.0);
//! });
//! b.draw_group(0);
//! let stream = b.finish().expect("valid");
//!
//! let mut r = SchematicRenderer::new();
//! r.set_stream(stream);
//! r.set_viewport([800.0, 600.0]);
//! r.zoom_to_fit(20.0);
//!
//! let frame = r.prepare([0.0, 0.0]);
//! assert_eq!(frame.stats.groups_drawn, 1);
//! assert!(frame.stats.vertices > 0);
//!
//! // Panning re-uses the tessellation rather than rebuilding it.
//! r.pan(10.0, 0.0);
//! let frame = r.prepare([0.0, 0.0]);
//! assert_eq!(frame.stats.cache_misses, 0);
//! ```

#![deny(unsafe_op_in_unsafe_fn)]
#![deny(missing_docs)]

pub mod cache;
pub mod camera;
pub mod canvas;
pub mod geometry;
pub mod index;
pub mod paint;
pub mod scene;
pub mod translate;

pub use cache::{CacheStats, CachedGroup, GroupKey, TessellationCache};
pub use camera::{Camera, WorldRect};
pub use canvas::{FrameStats, PreparedFrame, SchematicCanvas, SchematicRenderer};
pub use geometry::Projector;
pub use index::BoundsIndex;
pub use paint::{tessellate, DrawItem, Placement, Tessellated};
pub use scene::{Batch, Contour, Geometry, PackedColor, PixelRect, Polyline};
pub use translate::{translate_frame, translate_group, Frame, FrameItem, GalState, Stats};
