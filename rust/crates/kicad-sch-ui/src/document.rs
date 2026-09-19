// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The other half of the seam: where the geometry comes from.
//!
//! [`crate::input`] carries what the user did *out* of the shell.
//! [`LiveDocument`] is the return path: the canvas hands a document the camera
//! it is about to paint with and gets back the geometry for it.
//!
//! # Why this is a trait and not a `Session`
//!
//! The only real implementation lives in the binary and wraps
//! `kicad_sch_sys::Session`, which is a full eeschema link. This crate is
//! deliberately buildable — and testable — without any of that, exactly as
//! [`crate::input::InputSink`] keeps `TOOL_MANAGER` out of it. So the shell
//! knows nothing about C++ here either; it knows how to ask for a frame.
//!
//! [`ReplayDocument`] implements the trait over an ordinary recorded stream,
//! which is what the tests drive and what makes the re-render policy assertable
//! with no host linked.
//!
//! # What "live" costs, and what it does not
//!
//! Asking for a frame is not free: the C++ side walks `KIGFX::VIEW`, culls to
//! the viewport and re-records the frame body, and the Rust side copies the
//! result and revalidates it. What it does *not* do is re-record retained
//! geometry or re-tessellate it — those are keyed on `(group id, serial)` on
//! both sides of the boundary. So the canvas asks only when the answer could
//! have changed: a pan, a zoom, a resize, or an edit once edits exist. A
//! free-running redraw of an untouched view asks for nothing.

use std::cell::RefCell;
use std::rc::Rc;

use kicad_gal::{Stream, StreamParts};
use kicad_sch_render::SchematicRenderer;

use crate::input::ViewportState;

/// A document that can record a frame on demand.
///
/// Implementations must not panic and must not block: this runs inside gpui's
/// prepaint phase, where a panic aborts the frame and a block drops it. An
/// implementation that cannot produce a frame returns the reason as a string,
/// which the shell shows in the status bar rather than swallowing.
pub trait LiveDocument {
    /// Point the document at `viewport` and install the resulting geometry in
    /// `renderer`.
    ///
    /// `viewport` is the camera the canvas is about to paint with, so a document
    /// that culls has everything it needs to cull correctly. Installing the
    /// geometry is the implementation's job rather than the caller's because the
    /// frame a host hands back is borrowed from buffers it owns and overwrites,
    /// and [`SchematicRenderer::set_stream_view`] is the only thing that may see
    /// it — a borrow that cannot outlive the call it came from cannot be
    /// returned from this method.
    fn render(
        &mut self,
        viewport: ViewportState,
        renderer: &mut SchematicRenderer,
    ) -> Result<(), String>;
}

/// A shared, reference-counted document.
///
/// Single-threaded for two reasons: gpui's UI state is, and the C++ host behind
/// the real implementation belongs to the thread that initialised it.
pub type SharedDocument = Rc<RefCell<dyn LiveDocument>>;

/// Wrap a document for the canvas to hold.
pub fn shared_document(document: impl LiveDocument + 'static) -> SharedDocument {
    Rc::new(RefCell::new(document))
}

/// A document that hands back one fixed stream, however it is asked.
///
/// The stand-in for a real session: it exercises the whole re-render path — the
/// canvas asking, the stream being installed, the caches deciding what to keep —
/// with no C++ linked. It also records every viewport it was asked for, which is
/// how a test checks that the camera the canvas painted with is the camera the
/// document was told about.
pub struct ReplayDocument {
    stream: Stream,
    viewports: Vec<ViewportState>,
    /// Set to make every call fail, for testing the error path.
    failure: Option<String>,
}

impl ReplayDocument {
    /// A document that replays `stream`.
    pub fn new(stream: Stream) -> Self {
        Self {
            stream,
            viewports: Vec::new(),
            failure: None,
        }
    }

    /// A document that fails every render with `reason`.
    pub fn failing(reason: impl Into<String>) -> Self {
        Self {
            stream: Stream::from_parts(StreamParts::default()).expect("an empty stream is valid"),
            viewports: Vec::new(),
            failure: Some(reason.into()),
        }
    }

    /// How many frames have been asked for.
    pub fn renders(&self) -> usize {
        self.viewports.len()
    }

    /// The viewports asked for, in order.
    pub fn viewports(&self) -> &[ViewportState] {
        &self.viewports
    }

    /// The viewport of the most recent request.
    pub fn last_viewport(&self) -> Option<ViewportState> {
        self.viewports.last().copied()
    }

    /// Forget the history, so a test can count one interaction at a time.
    pub fn clear(&mut self) {
        self.viewports.clear();
    }
}

impl LiveDocument for ReplayDocument {
    fn render(
        &mut self,
        viewport: ViewportState,
        renderer: &mut SchematicRenderer,
    ) -> Result<(), String> {
        self.viewports.push(viewport);
        if let Some(reason) = &self.failure {
            return Err(reason.clone());
        }
        // Through the borrowed path rather than `set_stream`, so that the test
        // and the host exercise the same code: it is the one that decides
        // whether the spatial index has to be rebuilt.
        renderer.set_stream_view(&self.stream.view());
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::input::WorldPoint;

    fn viewport() -> ViewportState {
        ViewportState {
            width: 800.0,
            height: 600.0,
            scale: 1.0e-4,
            center: WorldPoint::new(1.0e6, 2.0e6),
        }
    }

    #[test]
    fn a_replay_document_installs_its_stream_and_records_the_request() {
        let mut document = ReplayDocument::new(crate::demo::demo_stream());
        let mut renderer = SchematicRenderer::new();
        assert_eq!(document.renders(), 0);

        document
            .render(viewport(), &mut renderer)
            .expect("a replay cannot fail");

        assert_eq!(document.renders(), 1);
        assert_eq!(document.last_viewport(), Some(viewport()));
        assert!(renderer.stream().is_some());
        assert!(!renderer.document_bounds().is_empty());
    }

    /// A second render of the same geometry must not invalidate the cache, which
    /// is the property the whole live path rests on.
    #[test]
    fn replaying_the_same_stream_keeps_the_tessellation_cache() {
        let mut document = ReplayDocument::new(crate::demo::demo_stream());
        let mut renderer = SchematicRenderer::new();

        document.render(viewport(), &mut renderer).unwrap();
        renderer.set_viewport([800.0, 600.0]);
        renderer.zoom_to_fit(0.0);
        renderer.prepare([0.0, 0.0]);
        let warm = renderer.cache_stats();
        assert!(warm.misses > 0, "the first frame has to tessellate");

        document.render(viewport(), &mut renderer).unwrap();
        renderer.prepare([0.0, 0.0]);
        let after = renderer.cache_stats();
        assert_eq!(
            after.misses,
            warm.misses,
            "re-installing identical geometry re-tessellated {} groups",
            after.misses - warm.misses
        );
        assert!(after.hits > warm.hits, "and it should have hit the cache");
    }

    #[test]
    fn a_failing_document_reports_why_rather_than_panicking() {
        let mut document = ReplayDocument::failing("no document loaded");
        let mut renderer = SchematicRenderer::new();
        let error = document
            .render(viewport(), &mut renderer)
            .expect_err("this one always fails");
        assert_eq!(error, "no document loaded");
        assert_eq!(document.renders(), 1);
    }
}
