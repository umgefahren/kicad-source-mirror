// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The Rust half of the KiCad draw-stream contract.
//!
//! `RECORDING_GAL` on the C++ side implements `KIGFX::GAL` by appending
//! commands to a flat buffer instead of rasterising them. This crate decodes
//! that buffer. The contract itself lives in
//! `include/gal/recording/draw_stream_abi.h` and is the single source of truth;
//! [`abi`] mirrors it and `tests/header_sync.rs` proves the two have not
//! drifted.
//!
//! # What this crate guarantees
//!
//! A [`StreamView`] cannot be constructed without the whole stream passing
//! validation: every coordinate index in range, every opcode known, every group
//! reference resolvable and acyclic, every image extent inside its arena, no
//! non-finite coordinates, and the ABI version matched. Decoding therefore
//! never panics and never reads out of bounds, which matters because the data
//! arrives across an FFI boundary from another process's memory.
//!
//! # Getting a stream
//!
//! ```
//! use kicad_gal::{Color, Command, StreamBuilder};
//!
//! let mut b = StreamBuilder::new();
//! b.group(7, 1, |g| {
//!     g.set_stroke_color(Color::new(0.0, 0.8, 0.0, 1.0));
//!     g.segment([0.0, 0.0], [2_540_000.0, 0.0], 152_400.0);
//! });
//! b.begin_frame(800, 600);
//! b.draw_group(7);
//! b.end_frame();
//!
//! let stream = b.finish().expect("builder produces a valid stream");
//! let view = stream.view();
//!
//! // The frame refers to the group; the group holds the geometry.
//! let ops: Vec<_> = view.frame().map(|i| i.command).collect();
//! assert!(matches!(ops[1], Command::DrawGroup { id: 7, .. }));
//! assert_eq!(view.group_body(7).map(|b| b.count()), Some(2));
//! ```
//!
//! Streams also round-trip through the on-disk format with [`Stream::write`]
//! and [`Stream::read`], which is how golden fixtures recorded from real
//! schematics reach the renderer's tests without a C++ build.

#![deny(unsafe_op_in_unsafe_fn)]
#![deny(missing_docs)]
#![forbid(clippy::undocumented_unsafe_blocks)]

pub mod abi;
mod builder;
mod command;
mod error;
mod io;
mod stream;

pub use abi::{
    coord_refs, pack_color, Color, CoordRef, CoordRefs, GridStyle, ImageFormat, Op, Target,
    KGDS_MAGIC, KGDS_VERSION, MAX_COORD_REFS,
};
pub use builder::{BuildError, StreamBuilder};
pub use command::{Affine, Command, CommandIter, Instruction, Points};
pub use error::{DecodeError, Section};
pub use stream::{Arena, Stream, StreamParts, StreamView, MAX_GROUP_DEPTH};

/// The `GAL` depth range, from `include/gal/definitions.h`:
/// `MIN_DEPTH = -2 * MAX_LAYERS_FOR_VIEW`, `MAX_DEPTH = 2 * MAX_LAYERS_FOR_VIEW - 1`.
///
/// Smaller depths are nearer the viewer — `GAL::AdvanceDepth` decrements.
/// Renderers need this to map `SET_LAYER_DEPTH` onto a depth buffer, and it is
/// not in the ABI header because the header carries the value, not its range.
pub const MIN_LAYER_DEPTH: f64 = -4096.0;

/// The far end of the `GAL` depth range. See [`MIN_LAYER_DEPTH`].
pub const MAX_LAYER_DEPTH: f64 = 4095.0;
