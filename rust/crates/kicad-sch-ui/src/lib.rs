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
//! # The two seams
//!
//! Everything that will eventually cross into C++ goes through one of two
//! places, and neither of them involves FFI yet:
//!
//! * [`canvas::SchematicScene`] — what draws inside the canvas.
//!   [`canvas::StubScene`] implements it today; `kicad-sch-render` implements
//!   it tomorrow. The trait's documentation spells out the swap.
//! * [`input::InputSink`] — where the shell posts what the user did, in the
//!   vocabulary of [`input::ShellEvent`]. [`input::RecordingSink`] implements
//!   it for tests; a queue feeding `TOOL_MANAGER` implements it later.
//!
//! Between them sits [`camera::Camera`], a plain value both sides share, so
//! "where are we looking" never needs to be negotiated.
//!
//! # Getting a window open
//!
//! ```no_run
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

pub mod camera;
pub mod canvas;
pub mod commands;
pub mod grid;
pub mod input;
pub mod panels;
pub mod shell;
pub mod stats;
pub mod theme;
pub mod tools;

pub use camera::{Camera, WorldRect};
pub use canvas::{CanvasState, SchematicScene, ScenePaint, StubScene};
pub use commands::{MENUS, RunAction, ShellCommand};
pub use grid::{GridState, Units};
pub use input::{
    ActionId, InputSink, Modifiers, PointerButton, RecordingSink, ScreenPoint, ShellEvent, ToolId,
    WorldPoint,
};
pub use shell::{SchematicShell, init};
pub use stats::FrameStats;
pub use theme::CanvasPalette;
pub use tools::{TOOLS, Tool};
