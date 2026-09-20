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

    /// Toggle a live ERC marker exclusion.
    pub fn exclude_erc(&mut self, _id: &str, _excluded: bool) -> Result<(), Error> {
        Err(Error::NoHost)
    }
    /// Run the native ERC engine.
    pub fn run_erc(&mut self) -> Result<Vec<crate::ErcViolation>, Error> {
        Err(Error::NoHost)
    }

    /// Copy search terms to the host and update match highlighting.
    pub fn item_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        Err(Error::NoHost)
    }
    /// Hierarchical label and sheet pin synchronization.
    pub fn sheet_pin_properties(&mut self, _all: bool) -> Result<crate::ItemProperties, Error> {
        Err(Error::NoHost)
    }

    /// Hierarchical label and sheet pin synchronization.
    pub fn apply_sheet_pin_properties(
        &mut self,
        _all: bool,
        _data: &crate::ItemProperties,
    ) -> Result<(), Error> {
        Err(Error::NoHost)
    }

    /// Read or persist application preferences.
    pub fn preferences(&mut self) -> Result<crate::ItemProperties, Error> {
        Err(Error::NoHost)
    }
    pub fn graphics_import_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        Err(Error::NoHost)
    }
    pub fn image_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        Err(Error::NoHost)
    }

    /// Read or persist application preferences.
    pub fn apply_preferences(&mut self, _data: &crate::ItemProperties) -> Result<(), Error> {
        Err(Error::NoHost)
    }
    pub fn apply_graphics_import(&mut self, _data: &crate::ItemProperties) -> Result<(), Error> {
        Err(Error::NoHost)
    }
    pub fn apply_image_properties(&mut self, _data: &crate::ItemProperties) -> Result<(), Error> {
        Err(Error::NoHost)
    }

    /// Consume an asynchronous properties request from the active tool.
    pub fn take_pending_properties(&mut self) -> Result<bool, Error> {
        Err(Error::NoHost)
    }

    /// Relink a hierarchical sheet file, clearing undo history.
    pub fn relink_sheet(&mut self, _item_id: &str, _path: &str) -> Result<(), Error> {
        Err(Error::NoHost)
    }

    /// Create or delete a selected item custom field.
    pub fn edit_custom_field(
        &mut self,
        _item_id: &str,
        _name: &str,
        _value: Option<&str>,
    ) -> Result<(), Error> {
        Err(Error::NoHost)
    }

    pub fn apply_properties(&mut self, _data: &crate::ItemProperties) -> Result<(), Error> {
        Err(Error::NoHost)
    }

    /// Cached library IDs (requires the host).
    pub fn symbol_libraries(&mut self) -> Result<Vec<String>, Error> {
        Err(Error::NoHost)
    }
    /// Browse a symbol library.
    pub fn browse_symbols(
        &mut self,
        _library: &str,
        _power_only: bool,
    ) -> Result<Vec<String>, Error> {
        Err(Error::NoHost)
    }
    /// List cached symbols.
    pub fn list_symbols(&mut self) -> Result<Vec<String>, Error> {
        Err(Error::NoHost)
    }
    /// Record a chooser preview.
    pub fn preview_symbol(
        &mut self,
        _id: &str,
        _unit: u32,
        _body: u32,
    ) -> Result<(kicad_gal::Stream, u32, u32), Error> {
        Err(Error::NoHost)
    }
    /// Place a chosen unit and body style.
    pub fn place_symbol_variant(&mut self, _id: &str, _unit: u32, _body: u32) -> Result<(), Error> {
        Err(Error::NoHost)
    }
    /// Begin placement (requires the host).
    pub fn place_symbol(&mut self, _id: &str) -> Result<(), Error> {
        Err(Error::NoHost)
    }
    /// Configure search.
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

impl Session {
    /// Unavailable without the native host.
    pub fn document_workflow(&mut self, _kind: u32) -> Result<Vec<(String, String)>, Error> {
        match self.0 {}
    }
    /// Unavailable without the native host.
    pub fn apply_document_workflow(&mut self, _kind: u32, _values: &[String]) -> Result<(), Error> {
        match self.0 {}
    }
}

impl Session {
    /// Read a symbol library table.
    pub fn library_table(&mut self, _global: bool) -> Result<Vec<crate::LibraryRow>, Error> {
        Err(Error::NoHost)
    }
    /// Save a symbol library table.
    pub fn save_library_table(
        &mut self,
        _global: bool,
        _rows: &[crate::LibraryRow],
    ) -> Result<(), Error> {
        Err(Error::NoHost)
    }
}

impl Session {
    /// Read simulation workflow values.
    pub fn simulation_workflow(&mut self, _kind: u32) -> Result<Vec<(String, String)>, Error> {
        Err(Error::NoHost)
    }
    /// Apply simulation workflow values.
    pub fn apply_simulation_workflow(
        &mut self,
        _kind: u32,
        _values: &[String],
    ) -> Result<(), Error> {
        Err(Error::NoHost)
    }
}

impl Session {
    /// Read schematic setup.
    pub fn setup_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        Err(Error::NoHost)
    }
    /// Save schematic setup.
    pub fn apply_setup_properties(&mut self, _data: &crate::ItemProperties) -> Result<(), Error> {
        Err(Error::NoHost)
    }
}

impl Session {
    /// Read Database or HTTP library connection settings.
    pub fn configure_library(
        &mut self,
        _global: bool,
        _nickname: &str,
    ) -> Result<Vec<(String, String)>, Error> {
        Err(Error::NoHost)
    }
}
