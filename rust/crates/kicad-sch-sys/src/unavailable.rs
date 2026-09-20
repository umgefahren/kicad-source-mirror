// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The session, in a build with no C++ host linked.
//!
//! This exists so that a caller never needs a `cfg` of its own: the API is the
//! same shape as `session.rs`, and every way in returns [`Error::NoHost`].
//!
//! [`Session`] is *uninhabited* — its only field is an empty enum — which is how
//! that shape is kept honest for free. A value cannot exist, so every method
//! body is `match self.0 {}`, and the compiler agrees the code is unreachable
//! rather than taking anyone's word for it. It also means the two builds cannot
//! drift in behaviour: there is no second implementation, only no
//! implementation.

use std::fmt;
use std::path::Path;

use crate::{
    BBox, DocumentInfo, EditorState, Error, InputEvent, InputOutcome, SheetInfo, Stream,
    StreamView, Viewport,
};

/// A schematic editor session, which this build cannot open.
///
/// See the [crate documentation](crate#builds-without-the-c-host) for how to get
/// one that can.
pub struct Session(Never);

/// Uninhabited, so `Session` is too.
enum Never {}

impl Session {
    /// Always [`Error::NoHost`] in this build.
    pub fn new() -> Result<Session, Error> {
        Err(Error::NoHost)
    }

    /// Always [`Error::NoHost`] in this build.
    pub fn open(_path: &Path) -> Result<Session, Error> {
        Err(Error::NoHost)
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn load_file(&mut self, _path: &Path) -> Result<(), Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn unload(&mut self) -> Result<(), Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn is_loaded(&self) -> bool {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn document_info(&self) -> Result<DocumentInfo, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn bbox(&self, _include_all_visible: bool) -> Result<BBox, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn sheet_count(&self) -> Result<u32, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn sheet(&self, _index: u32) -> Result<SheetInfo, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn set_sheet(&mut self, _index: u32) -> Result<(), Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn viewport(&self) -> Result<Viewport, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn set_viewport(&mut self, _viewport: &Viewport) -> Result<(), Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn zoom_to_fit(&mut self) -> Result<(), Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn render(&mut self) -> Result<StreamView<'_>, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn published(&self) -> Result<StreamView<'_>, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn render_owned(&mut self) -> Result<Stream, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn write_stream(&mut self, _path: &Path) -> Result<(), Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn dispatch_input(&mut self, _event: &InputEvent<'_>) -> Result<InputOutcome, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn reset_input(&mut self) -> Result<(), Error> {
        match self.0 {}
    }

    /// Copy search terms to the host and update match highlighting.
    pub fn set_search_data(&mut self, _data: &crate::SearchData) -> Result<(), Error> {
        Err(Error::NoHost)
    }

    /// Read the result after dispatching a Find/Replace action.
    pub fn search_result(&mut self) -> Result<crate::SearchResult, Error> {
        Err(Error::NoHost)
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn run_action(&mut self, _name: &str) -> Result<InputOutcome, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn editor_state(&mut self) -> Result<EditorState, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn undo(&mut self) -> Result<bool, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn redo(&mut self) -> Result<bool, Error> {
        match self.0 {}
    }

    /// Unreachable: no `Session` can exist in this build.
    pub fn save(&mut self) -> Result<(), Error> {
        match self.0 {}
    }
}

impl fmt::Debug for Session {
    fn fmt(&self, _f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {}
    }
}
