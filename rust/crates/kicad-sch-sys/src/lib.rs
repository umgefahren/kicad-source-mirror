// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The Rust end of KiCad's headless schematic host.
//!
//! [`include/sch_host/sch_host_abi.h`][abi-header] lets a caller open a session,
//! load a `.kicad_sch` through eeschema's own reader, point a camera at it and
//! read back the recorded draw stream. This crate binds it and wraps it in
//! something safe: one owning handle, errors instead of status codes, and a
//! borrow that the compiler enforces where C's documentation only asked nicely.
//!
//! ```no_run
//! # fn main() -> Result<(), kicad_sch_sys::Error> {
//! use kicad_sch_sys::{Session, Viewport};
//!
//! let mut session = Session::open("demos/video/video.kicad_sch".as_ref())?;
//!
//! session.set_viewport(&Viewport::new(1920, 1080))?;
//! session.zoom_to_fit()?;
//!
//! let frame = session.render()?;
//! println!("{} retained groups", frame.groups().len());
//! # Ok(())
//! # }
//! ```
//!
//! # What "safe" buys here
//!
//! The stream a render hands back points into buffers C++ owns and reuses: the
//! next recording pass invalidates them. [`Session::render`] returns a
//! [`StreamView`] borrowed from `&mut self`, so holding one and rendering again
//! does not compile. That is the whole hazard, closed by the signature rather
//! than by a comment. [`Session::render_owned`] copies out instead, for a caller
//! that wants to keep the frame.
//!
//! # Driving it per frame
//!
//! A session is meant to be held open and asked for a frame whenever the view
//! moves, which is what the gpui shell does. Two properties make that affordable,
//! and both are checked by `tests/live_session.rs`:
//!
//! * Moving the camera re-records the *frame body* — the list of group references
//!   `KIGFX::VIEW::Redraw` culls to the viewport — and leaves the retained group
//!   geometry byte-for-byte alone. A consumer caching tessellation against
//!   `(group id, serial)` therefore keeps all of it across a pan or a zoom.
//! * [`Session::set_viewport`] takes pixels per internal unit, the same unit the
//!   recorded coordinates are in, so a consumer's camera needs no conversion.
//!   It is clamped to eeschema's zoom limits, though, so read it back with
//!   [`Session::viewport`] and adopt what was granted: the session culls to the
//!   scale it holds.
//!
//! Validation is not this crate's job: `kicad-gal` checks every index, opcode
//! and group reference before a [`StreamView`] exists, and that pass runs on a
//! live stream exactly as it does on a recorded one.
//!
//! # Builds without the C++ host
//!
//! The library this binds is built by CMake with `-DKICAD_BUILD_RUST_SCH_UI=ON`,
//! which is a full eeschema build. The rest of the workspace deliberately does
//! not need one — it is developed against recorded streams — so this crate
//! compiles either way: with no host library found, [`is_available`] is `false`
//! and every entry point returns [`Error::NoHost`]. Nothing silently no-ops.
//!
//! # Threading
//!
//! One thread — the one that opened the first session. Not one session per
//! thread: wx takes that first caller to be its main thread, and eeschema's
//! connectivity engine asserts `wxThread::IsMain()` on every document load, so a
//! second thread is not a race to be serialised but simply not allowed. A
//! [`Session`] is therefore neither [`Send`] nor [`Sync`], and asking for one
//! from anywhere else is [`Error::WrongThread`] rather than a C++ assertion on
//! someone else's stack. The process-wide initialisation itself happens once,
//! under a lock, however many threads ask.
//!
//! [abi-header]: https://gitlab.com/kicad/code/kicad/-/blob/master/include/sch_host/sch_host_abi.h

#![deny(unsafe_op_in_unsafe_fn)]

use std::fmt;
use std::path::PathBuf;

use kicad_gal::DecodeError;

/// Re-exported so that a caller needs one dependency, not two, to hold a frame.
pub use kicad_gal::{Stream, StreamView};

#[cfg(ksch_linked)]
pub mod ffi;

#[cfg(ksch_linked)]
mod session;
#[cfg(ksch_linked)]
pub use session::Session;

#[cfg(not(ksch_linked))]
mod unavailable;
#[cfg(not(ksch_linked))]
pub use unavailable::Session;

/// Whether this build has the C++ host linked into it.
///
/// `false` means every [`Session`] call will return [`Error::NoHost`]. A UI can
/// use it to decide between offering to open a `.kicad_sch` and saying why it
/// cannot.
pub const fn is_available() -> bool {
    cfg!(ksch_linked)
}

/// A `ksch_status` other than success.
///
/// The success case is `Ok(())`, so it is not a variant here.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[non_exhaustive]
pub enum Status {
    /// A handle, an out-parameter or an argument was not usable.
    InvalidArg,
    /// The call needs a loaded document and the session has none.
    NoDocument,
    /// The path does not name an existing file.
    FileNotFound,
    /// The file exists but could not be read as a schematic.
    LoadFailed,
    /// An index was past the end of what it addresses.
    OutOfRange,
    /// A write failed.
    Io,
    /// Allocation failed.
    OutOfMemory,
    /// An exception escaped a KiCad call. A bug somewhere.
    Internal,
    /// A code this build does not know, which means the library is newer.
    Unknown(u32),
}

impl Status {
    /// The status code as the ABI numbers it.
    pub fn code(self) -> u32 {
        match self {
            Status::InvalidArg => 1,
            Status::NoDocument => 2,
            Status::FileNotFound => 3,
            Status::LoadFailed => 4,
            Status::OutOfRange => 5,
            Status::Io => 6,
            Status::OutOfMemory => 7,
            Status::Internal => 8,
            Status::Unknown(code) => code,
        }
    }

    /// The status a raw `ksch_status` code stands for.
    ///
    /// Success is not a [`Status`] — it is `Ok(())` — so `0` comes back as
    /// `Unknown(0)`. A caller of the raw [`ffi`](crate::ffi) layer therefore
    /// checks for `KSCH_OK` first, which it has to do anyway.
    pub fn from_code(code: u32) -> Status {
        match code {
            1 => Status::InvalidArg,
            2 => Status::NoDocument,
            3 => Status::FileNotFound,
            4 => Status::LoadFailed,
            5 => Status::OutOfRange,
            6 => Status::Io,
            7 => Status::OutOfMemory,
            8 => Status::Internal,
            other => Status::Unknown(other),
        }
    }

    /// The ABI's own name for the code, as `ksch_status_name` reports it.
    pub fn name(self) -> &'static str {
        match self {
            Status::InvalidArg => "KSCH_ERR_INVALID_ARG",
            Status::NoDocument => "KSCH_ERR_NO_DOCUMENT",
            Status::FileNotFound => "KSCH_ERR_FILE_NOT_FOUND",
            Status::LoadFailed => "KSCH_ERR_LOAD_FAILED",
            Status::OutOfRange => "KSCH_ERR_OUT_OF_RANGE",
            Status::Io => "KSCH_ERR_IO",
            Status::OutOfMemory => "KSCH_ERR_OUT_OF_MEMORY",
            Status::Internal => "KSCH_ERR_INTERNAL",
            Status::Unknown(_) => "KSCH_ERR_UNKNOWN",
        }
    }
}

impl fmt::Display for Status {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(self.name())
    }
}

/// Anything that can go wrong on the way to a frame.
#[derive(Debug)]
#[non_exhaustive]
pub enum Error {
    /// This build has no C++ host linked. See [`is_available`].
    NoHost,

    /// The library and this crate's header disagree about the ABI.
    ///
    /// Almost always a stale build directory: rebuild the host.
    AbiMismatch {
        /// What the linked library reports.
        library: u32,
        /// What the header this crate was generated from says.
        expected: u32,
    },

    /// Standing the process up failed, so no session can exist.
    Runtime(String),

    /// A session was asked for from a thread that does not own the host.
    ///
    /// The first session fixes that thread — wx takes it to be its main thread —
    /// and eeschema's connectivity engine asserts on it thereafter. A
    /// [`Session`] is `!Send`, so this can only happen when *creating* one.
    WrongThread,

    /// A call failed, with whatever the session recorded about it.
    Failed {
        /// The status code.
        status: Status,
        /// The session's error string, which is often the reader's own message.
        message: String,
    },

    /// A path that cannot be handed to a C API: not Unicode, or with an
    /// interior NUL.
    BadPath(PathBuf),

    /// The stream the host produced did not survive validation. A host bug.
    Decode(DecodeError),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::NoHost => f.write_str(
                "this build has no KiCad schematic host linked; \
                 configure CMake with -DKICAD_BUILD_RUST_SCH_UI=ON and rebuild, \
                 or point KICAD_SCH_HOST_DIR at a build that has libkicad_sch_host",
            ),
            Error::AbiMismatch { library, expected } => write!(
                f,
                "the schematic host reports ABI version {library}, this build expects {expected}"
            ),
            Error::Runtime(message) => {
                write!(f, "cannot initialise the schematic host: {message}")
            }
            Error::WrongThread => f.write_str(
                "the schematic host belongs to the thread that opened the first session; \
                 eeschema's connectivity engine asserts wxThread::IsMain() and this is \
                 not that thread",
            ),
            Error::Failed { status, message } if message.is_empty() => write!(f, "{status}"),
            Error::Failed { status, message } => write!(f, "{status}: {message}"),
            Error::BadPath(path) => write!(f, "{} is not a usable UTF-8 path", path.display()),
            Error::Decode(error) => write!(f, "the recorded frame is not valid: {error}"),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Error::Decode(error) => Some(error),
            _ => None,
        }
    }
}

impl From<DecodeError> for Error {
    fn from(error: DecodeError) -> Error {
        Error::Decode(error)
    }
}

/// Summary of the loaded document, from `ksch_document_info`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DocumentInfo {
    /// Sheets in the hierarchy; at least one when loaded.
    pub sheet_count: u32,
    /// Index of the current sheet into the sheet list.
    pub current_sheet: u32,
    /// Items on the current sheet's screen.
    pub item_count: u64,
    /// Whether any screen has unsaved changes.
    pub modified: bool,
}

/// One sheet of the hierarchy, with the strings copied out.
///
/// The ABI lends those strings until the next call on the session; this owns
/// them, so there is nothing to get wrong.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SheetInfo {
    /// The sheet's own name.
    pub name: String,
    /// Hierarchical path, e.g. `/power/regulators`.
    pub path: String,
    /// Page number as shown in the sheet list; not necessarily numeric.
    pub page_number: String,
    /// Items on this sheet's screen.
    pub item_count: u64,
}

/// The camera, in KiCad internal units (100 nm for eeschema).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Viewport {
    /// Viewport width in pixels; must be positive.
    pub width_px: u32,
    /// Viewport height in pixels; must be positive.
    pub height_px: u32,
    /// World x drawn at the middle of the viewport.
    pub center_x: f64,
    /// World y drawn at the middle of the viewport.
    pub center_y: f64,
    /// World-to-screen factor: a world distance times this is pixels.
    pub scale: f64,
}

impl Viewport {
    /// A viewport of that size, at the origin and unit scale.
    ///
    /// Follow it with [`Session::zoom_to_fit`], which is what the recorded
    /// fixtures do: the scale then frames the page rather than being guessed.
    pub fn new(width_px: u32, height_px: u32) -> Viewport {
        Viewport {
            width_px,
            height_px,
            center_x: 0.0,
            center_y: 0.0,
            scale: 1.0,
        }
    }
}

/// Which pointer button an [`InputEvent`] is about.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PointerButton {
    /// The primary button.
    Left,
    /// The secondary button, which opens context menus.
    Right,
    /// The wheel button.
    Middle,
    /// The mouse-side "back" button.
    Back,
    /// The mouse-side "forward" button.
    Forward,
}

/// The modifier keys held when an [`InputEvent`] happened.
///
/// `meta` is Command on macOS and Super elsewhere, matching what the shell calls
/// it. The host maps these onto KiCad's own `MD_*` bits, so nothing here has to
/// know what those are.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Modifiers {
    /// Shift.
    pub shift: bool,
    /// Control.
    pub ctrl: bool,
    /// Alt / Option.
    pub alt: bool,
    /// Command on macOS, Super elsewhere.
    pub meta: bool,
}

/// One input event for the C++ tool framework.
///
/// Positions are in **screen pixels relative to the canvas' top-left**, not in
/// internal units like the rest of this crate. That is the host's requirement
/// rather than a convenience: it derives the world position itself, through the
/// same view controls a tool reads the cursor back from, so that a cursor a tool
/// has placed is the one the following events carry.
///
/// Keys travel as *names* — `"escape"`, `"pagedown"`, `"f11"`, `"w"` — and the
/// host maps them onto KiCad's `WXK_*` codes. That mapping is deliberately on the
/// C++ side, where the numbers come from `wx/defs.h` through a compiler; a table
/// transcribed into Rust would be a silently broken shortcut per wrong entry.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum InputEvent<'a> {
    /// The pointer moved.
    PointerMotion {
        /// Screen position, pixels from the canvas' top-left.
        screen: (f64, f64),
        /// Modifiers held.
        modifiers: Modifiers,
    },
    /// A button went down.
    PointerDown {
        /// Which button.
        button: PointerButton,
        /// Screen position.
        screen: (f64, f64),
        /// Modifiers held.
        modifiers: Modifiers,
    },
    /// A button came up.
    PointerUp {
        /// Which button.
        button: PointerButton,
        /// Screen position.
        screen: (f64, f64),
        /// Modifiers held.
        modifiers: Modifiers,
    },
    /// A double click, sent *instead of* the second press of the pair.
    PointerDoubleClick {
        /// Which button.
        button: PointerButton,
        /// Screen position.
        screen: (f64, f64),
        /// Modifiers held.
        modifiers: Modifiers,
    },
    /// The pointer left the canvas.
    ///
    /// Send it. It is the only notice the host gets that a button may have been
    /// released where it will never hear about it, and without it a tool goes on
    /// believing a drag is running.
    PointerLeave,
    /// The wheel turned, or a two-finger scroll.
    Scroll {
        /// Screen position.
        screen: (f64, f64),
        /// Delta in wheel detents; positive y is away from the user.
        delta: (f64, f64),
        /// Modifiers held.
        modifiers: Modifiers,
    },
    /// A key went down.
    KeyDown {
        /// The key's name, in the shell's vocabulary.
        key: &'a str,
        /// Modifiers held.
        modifiers: Modifiers,
        /// Whether this is an auto-repeat rather than a fresh press.
        auto_repeat: bool,
    },
    /// A key came up. Produces no tool event; KiCad's tools run off presses.
    KeyUp {
        /// The key's name.
        key: &'a str,
        /// Modifiers held.
        modifiers: Modifiers,
    },
    /// Cancel whatever is running, as Escape does.
    Cancel,
}

/// What the host did with an [`InputEvent`] or an action.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct InputOutcome {
    /// A tool or a hotkey claimed it.
    pub handled: bool,
    /// Something asked for a repaint, so the frame the caller holds is stale.
    ///
    /// This is the only notice a UI gets that the document or the view changed
    /// behind its back. Ignoring it shows a frame that no longer matches.
    pub redraw: bool,
}

/// What the editor is doing, for a status bar and overlays.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct EditorState {
    /// The cursor the *tools* see, in internal units: the pointer snapped to the
    /// grid, or wherever a tool has forced it. Not the raw pointer position, which
    /// the UI already knows.
    pub cursor: (f64, f64),
    /// Items currently selected.
    pub selection_count: u32,
    /// Commands on the undo stack, for greying out a menu item.
    pub undo_count: u32,
    /// Commands on the redo stack.
    pub redo_count: u32,
    /// Whether the pointer is over the canvas, so a crosshair should be drawn.
    pub pointer_over_canvas: bool,
    /// Whether any screen has unsaved changes.
    pub modified: bool,
    /// The user-level tool on top of the tool stack; empty if none.
    pub tool_name: String,
    /// The last status text a tool asked to show; empty if none.
    pub status_text: String,
}

/// An axis-aligned box in internal units.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BBox {
    /// Left edge.
    pub x: f64,
    /// Top edge.
    pub y: f64,
    /// Width; never negative.
    pub width: f64,
    /// Height; never negative.
    pub height: f64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn status_codes_round_trip() {
        for code in 1..=8 {
            assert_eq!(Status::from_code(code).code(), code);
        }

        assert_eq!(Status::from_code(99), Status::Unknown(99));
        assert_eq!(Status::from_code(4).name(), "KSCH_ERR_LOAD_FAILED");

        // Documented: success is not a Status, so zero is not one either.
        assert_eq!(Status::from_code(0), Status::Unknown(0));
    }

    #[test]
    fn the_no_host_error_says_what_to_do_about_it() {
        let message = Error::NoHost.to_string();
        assert!(message.contains("KICAD_BUILD_RUST_SCH_UI"), "{message}");
        assert!(message.contains("KICAD_SCH_HOST_DIR"), "{message}");
    }

    #[test]
    fn availability_matches_how_this_was_built() {
        assert_eq!(is_available(), cfg!(ksch_linked));
    }

    /// The point of the stub build: the API exists, and says why it cannot work.
    #[cfg(not(ksch_linked))]
    #[test]
    fn without_a_host_opening_a_session_fails_cleanly() {
        let error = Session::new().expect_err("there is no host in this build");
        assert!(matches!(error, Error::NoHost));
    }
}
