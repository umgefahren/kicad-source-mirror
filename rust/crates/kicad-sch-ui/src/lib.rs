// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The gpui-kit application shell for KiCad's schematic editor.
//!
//! This crate is the window and everything in it except the schematic: the
//! menu bar, the toolbars, the tool palette, the docks, the status bar, the
//! command palette, the key map and the canvas host. It owns no document, no
//! file format and no connectivity; KiCad's C++ keeps all of that.
//!
//! # The seams
//!
//! * **Drawing** is `kicad-sch-render`. The shell holds its
//!   [`SchematicRenderer`](kicad_sch_render::SchematicRenderer), hands it a
//!   draw stream and drives its camera; the renderer paints into the layer the
//!   shell's canvas element has already opened. There is exactly one camera and
//!   it lives in the renderer — see [`canvas`] for why a second one in the
//!   shell was a precision bug as well as a duplication.
//! * **Input** is [`input::InputSink`], where the shell posts what the user did
//!   in the vocabulary of [`input::ShellEvent`]. [`input::RecordingSink`]
//!   implements it for tests; a queue feeding `TOOL_MANAGER` implements it
//!   later. No FFI is involved yet on either side.
//! * **Geometry** is [`document::LiveDocument`], which is the return path: the
//!   canvas hands a document the camera it is about to paint with and gets back
//!   the frame for it. [`document::ReplayDocument`] implements it over a recorded
//!   stream, and the binary implements it over a real `SCH_HOST` session. The
//!   shell knows nothing about either.
//!
//! World coordinates are KiCad internal units — nanometres, as `f64` —
//! everywhere. Millimetres exist only in [`grid::Units`], which formats a
//! number on its way into a string.
//!
//! # Getting a window open
//!
//! ```no_run
//! use gpui_kit::AppContext as _;
//! use gpui_kit::component::Root;
//! use kicad_sch_ui::shell::{self, SchematicShell};
//!
//! gpui_kit::application()
//!     .with_assets(gpui_kit::assets::AllAssets)
//!     .run(|cx| {
//!         shell::init(cx);
//!         cx.spawn(async move |cx| {
//!             let _ = cx.open_window(gpui_kit::WindowOptions::default(), |window, cx| {
//!                 let view = cx.new(|cx| SchematicShell::new(window, cx));
//!                 cx.new(|cx| Root::new(view, window, cx))
//!             });
//!         })
//!         .detach();
//!     });
//! ```

#![deny(missing_docs)]

pub mod canvas;
pub mod commands;
pub mod demo;
pub mod document;
pub mod grid;
pub mod input;
pub mod panels;
pub mod shell;
pub mod stats;
pub mod theme;
pub mod tools;

pub use canvas::{CanvasElement, CanvasState};
pub use commands::{MENUS, RunAction, ShellCommand};
pub use document::{LiveDocument, ReplayDocument, SharedDocument, shared_document};
pub use grid::{GridState, Units};
pub use input::{
    ActionId, InputSink, Modifiers, PointerButton, RecordingSink, ScreenPoint, ShellEvent, ToolId,
    WorldPoint,
};
pub use kicad_sch_render::{Camera, SchematicRenderer, WorldRect};
pub use shell::{SchematicShell, init};
pub use stats::FrameStats;
pub use theme::CanvasPalette;
pub use tools::{TOOLS, Tool};
