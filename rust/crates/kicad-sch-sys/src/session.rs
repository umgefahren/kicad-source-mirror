// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The session, when the C++ host is linked.
//!
//! Its counterpart is `unavailable.rs`, which presents the same API in a build
//! that has no host. Keep the two signatures identical: a caller must not need
//! `cfg` of its own.

use std::ffi::{c_int, CStr, CString};
use std::marker::PhantomData;
use std::mem::MaybeUninit;
use std::path::Path;
use std::ptr::NonNull;
use std::sync::OnceLock;

use kicad_gal::abi::kgds_stream_view;
use kicad_gal::{Stream, StreamView};

use crate::ffi;
use crate::{
    BBox, DocumentInfo, EditorState, Error, InputEvent, InputOutcome, Modifiers, PointerButton,
    SheetInfo, Status, Viewport,
};

/// A schematic editor session: a document, a view and a recording canvas.
///
/// Owns the C++ side and destroys it on drop. Not [`Send`] or [`Sync`], because
/// neither the session nor the process-wide state it reads through is thread
/// safe.
pub struct Session {
    raw: NonNull<ffi::ksch_session>,

    /// Makes the type `!Send` and `!Sync` explicitly, rather than relying on
    /// `NonNull` happening to do it.
    _not_thread_safe: PhantomData<*const ()>,
}

impl Session {
    /// An empty session, initialising the process on first use.
    pub fn new() -> Result<Session, Error> {
        ensure_runtime()?;

        // SAFETY: no arguments, and the runtime is up.
        let raw = unsafe { ffi::ksch_session_create() };

        match NonNull::new(raw) {
            Some(raw) => Ok(Session {
                raw,
                _not_thread_safe: PhantomData,
            }),
            // The only documented failure. The message is global because there
            // is no session to have recorded it.
            None => Err(Error::Runtime(global_error())),
        }
    }

    /// A session with `path` loaded.
    pub fn open(path: &Path) -> Result<Session, Error> {
        let mut session = Session::new()?;
        session.load_file(path)?;
        Ok(session)
    }

    /// Load a `.kicad_sch`, replacing whatever was held.
    ///
    /// Goes through eeschema's own reader, so symbol links, instance data,
    /// page numbering and connectivity resolve exactly as they do for
    /// `kicad-cli`. On failure the session is left empty, not as it was.
    pub fn load_file(&mut self, path: &Path) -> Result<(), Error> {
        let path_c = c_path(path)?;

        // SAFETY: a live handle, and a NUL-terminated string borrowed for the
        // duration of the call, which is what the ABI asks for.
        self.check(unsafe { ffi::ksch_session_load_file(self.raw.as_ptr(), path_c.as_ptr()) })
    }

    /// Drop the document, keeping the session reusable.
    pub fn unload(&mut self) -> Result<(), Error> {
        // SAFETY: a live handle.
        self.check(unsafe { ffi::ksch_session_unload(self.raw.as_ptr()) })
    }

    /// Whether a document is loaded.
    pub fn is_loaded(&self) -> bool {
        // SAFETY: a live handle.
        unsafe { ffi::ksch_session_is_loaded(self.raw.as_ptr()) != 0 }
    }

    /// Sheet count, current sheet, item count and modified flag.
    pub fn document_info(&self) -> Result<DocumentInfo, Error> {
        let mut info = ffi::ksch_document_info::default();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_document_info(self.raw.as_ptr(), &mut info) })?;

        Ok(DocumentInfo {
            sheet_count: info.sheet_count,
            current_sheet: info.current_sheet,
            item_count: info.item_count,
            modified: info.modified != 0,
        })
    }

    /// The current sheet's bounding box.
    ///
    /// `include_all_visible` asks for the whole page rectangle, which is what a
    /// zoom-to-fit frames; otherwise it is the union of the items' own boxes,
    /// excluding the drawing sheet.
    pub fn bbox(&self, include_all_visible: bool) -> Result<BBox, Error> {
        let mut bbox = ffi::ksch_bbox::default();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe {
            ffi::ksch_session_bbox(self.raw.as_ptr(), include_all_visible.into(), &mut bbox)
        })?;

        Ok(BBox {
            x: bbox.x,
            y: bbox.y,
            width: bbox.width,
            height: bbox.height,
        })
    }

    /// Sheets in the hierarchy, ordered by page number.
    pub fn sheet_count(&self) -> Result<u32, Error> {
        let mut count = 0u32;

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_sheet_count(self.raw.as_ptr(), &mut count) })?;

        Ok(count)
    }

    /// Describe one sheet, copying its strings out.
    pub fn sheet(&self, index: u32) -> Result<SheetInfo, Error> {
        let mut info = ffi::ksch_sheet_info::default();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_sheet_info(self.raw.as_ptr(), index, &mut info) })?;

        // SAFETY: on success the ABI guarantees three NUL-terminated UTF-8
        // strings, borrowed from the session until its next call. They are
        // copied here and then never referred to again.
        unsafe {
            Ok(SheetInfo {
                name: borrowed_string(info.name),
                path: borrowed_string(info.path),
                page_number: borrowed_string(info.page_number),
                item_count: info.item_count,
            })
        }
    }

    /// Make a sheet current, repopulating the view from its screen.
    ///
    /// Every retained group in the stream is invalidated: the previous sheet's
    /// items are gone, so a renderer caching group ids must discard them.
    pub fn set_sheet(&mut self, index: u32) -> Result<(), Error> {
        // SAFETY: a live handle.
        self.check(unsafe { ffi::ksch_session_set_sheet(self.raw.as_ptr(), index) })
    }

    /// Read the camera back.
    pub fn viewport(&self) -> Result<Viewport, Error> {
        let mut viewport = ffi::ksch_viewport::default();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_get_viewport(self.raw.as_ptr(), &mut viewport) })?;

        Ok(Viewport {
            width_px: viewport.width_px,
            height_px: viewport.height_px,
            center_x: viewport.center_x,
            center_y: viewport.center_y,
            scale: viewport.scale,
        })
    }

    /// Point the camera. Dimensions and scale must be positive.
    pub fn set_viewport(&mut self, viewport: &Viewport) -> Result<(), Error> {
        let raw = ffi::ksch_viewport {
            width_px: viewport.width_px,
            height_px: viewport.height_px,
            center_x: viewport.center_x,
            center_y: viewport.center_y,
            scale: viewport.scale,
        };

        // SAFETY: a live handle and a struct we own for the call.
        self.check(unsafe { ffi::ksch_session_set_viewport(self.raw.as_ptr(), &raw) })
    }

    /// Frame the whole page, keeping the viewport size.
    pub fn zoom_to_fit(&mut self) -> Result<(), Error> {
        // SAFETY: a live handle.
        self.check(unsafe { ffi::ksch_session_zoom_to_fit(self.raw.as_ptr()) })
    }

    /// Record a frame and borrow it.
    ///
    /// The view points into buffers the session owns and overwrites on the next
    /// recording pass. That is why this borrows `&mut self` for as long as the
    /// view lives: rendering again, changing sheet or dropping the session while
    /// holding one does not compile.
    ///
    /// Everything about the stream is validated before it is returned, so a host
    /// that recorded nonsense is [`Error::Decode`] rather than a bad read later.
    pub fn render(&mut self) -> Result<StreamView<'_>, Error> {
        let mut view = empty_view();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_render(self.raw.as_ptr(), &mut view) })?;

        // SAFETY: the sections belong to the session, which this borrow keeps
        // alive and un-rendered for as long as the returned view exists, so the
        // memory outlives it and nothing mutates it in the meantime.
        Ok(unsafe { StreamView::from_raw(&view) }?)
    }

    /// Borrow the frame the last [`Session::render`] recorded, without
    /// recording another.
    pub fn published(&self) -> Result<StreamView<'_>, Error> {
        let mut view = empty_view();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_publish(self.raw.as_ptr(), &mut view) })?;

        // SAFETY: as in `render`, with a shared borrow: the mutating calls all
        // take `&mut self`, so none can run while this view is alive.
        Ok(unsafe { StreamView::from_raw(&view) }?)
    }

    /// Record a frame and copy it out.
    ///
    /// One copy, and then the caller owns it for as long as it likes. This is
    /// what a renderer that keeps a frame between paints wants; [`Session::render`]
    /// is for reading one and letting it go.
    pub fn render_owned(&mut self) -> Result<Stream, Error> {
        Ok(self.render()?.to_owned_stream())
    }

    /// Serialise the last recorded frame in the `.kgds` file format.
    ///
    /// The same bytes `kicad-sch-dump` writes, which is how the golden fixtures
    /// in `qa/data/draw_streams/` are made.
    pub fn write_stream(&mut self, path: &Path) -> Result<(), Error> {
        let path_c = c_path(path)?;

        // SAFETY: a live handle, and a string borrowed for the call.
        self.check(unsafe { ffi::ksch_session_write_stream(self.raw.as_ptr(), path_c.as_ptr()) })
    }

    /// Give one input event to the C++ tool framework.
    ///
    /// Takes `&mut self` because it can change the document, the selection and the
    /// view — which is also why the outcome carries [`InputOutcome::redraw`]: a
    /// frame recorded before this call may no longer match.
    pub fn dispatch_input(&mut self, event: &InputEvent<'_>) -> Result<InputOutcome, Error> {
        // The key name is borrowed for the duration of the call, which is what the
        // header promises, so a CString that lives to the end of this function is
        // exactly the right lifetime. Declared here rather than inside the match so
        // that it outlives the raw struct pointing into it.
        let key = match event {
            InputEvent::KeyDown { key, .. } | InputEvent::KeyUp { key, .. } => {
                Some(CString::new(*key).map_err(|_| Error::Failed {
                    status: Status::InvalidArg,
                    message: "a key name may not contain a NUL".to_string(),
                })?)
            }
            _ => None,
        };

        let raw = raw_input(event, key.as_deref());
        let mut flags: u32 = 0;

        // SAFETY: a live handle, a struct we own for the call, and an out-parameter
        // we own. The string `raw.key` points into outlives the call.
        self.check(unsafe {
            ffi::ksch_session_dispatch_input(self.raw.as_ptr(), &raw, &mut flags)
        })?;

        Ok(outcome(flags))
    }

    /// Forget which buttons are down, because the UI lost focus.
    ///
    /// Without it, a button released while the window was not focused leaves a tool
    /// believing its drag is still running. The cursor position is kept.
    pub fn reset_input(&mut self) -> Result<(), Error> {
        // SAFETY: a live handle.
        self.check(unsafe { ffi::ksch_session_reset_input(self.raw.as_ptr()) })
    }

    /// Run a registered action by its dotted name, as a menu or a toolbar does.
    ///
    /// An action no registered tool handles is `Ok` with
    /// [`InputOutcome::handled`] false, not an error: a UI built from the whole
    /// action registry will legitimately offer plenty of them.
    pub fn run_action(&mut self, name: &str) -> Result<InputOutcome, Error> {
        let name_c = CString::new(name).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "an action name may not contain a NUL".to_string(),
        })?;

        let mut flags: u32 = 0;

        // SAFETY: a live handle, a string borrowed for the call, and an
        // out-parameter we own.
        self.check(unsafe {
            ffi::ksch_session_run_action(self.raw.as_ptr(), name_c.as_ptr(), &mut flags)
        })?;

        Ok(outcome(flags))
    }

    /// Toggle a still-live ERC marker exclusion.
    pub fn exclude_erc(&mut self, id: &str, excluded: bool) -> Result<(), Error> {
        let id = CString::new(id).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "Marker ID contains NUL".into(),
        })?;
        // SAFETY: live session and owned C string.
        self.check(unsafe {
            ffi::ksch_session_exclude_erc(self.raw.as_ptr(), id.as_ptr(), excluded as u32)
        })
    }
    /// Run KiCad's ERC engine and copy an owned result snapshot.
    pub fn run_erc(&mut self) -> Result<Vec<crate::ErcViolation>, Error> {
        let mut raw = std::ptr::null_mut();
        self.check(unsafe { ffi::ksch_session_run_erc(self.raw.as_ptr(), &mut raw) })?;
        struct Snapshot(*mut ffi::ksch_erc_result);
        impl Drop for Snapshot {
            fn drop(&mut self) {
                unsafe { ffi::ksch_erc_result_destroy(self.0) };
            }
        }
        let snapshot = Snapshot(raw);
        let count = unsafe { ffi::ksch_erc_result_count(snapshot.0) };
        let mut results = Vec::with_capacity(count as usize);
        for index in 0..count {
            let mut item = MaybeUninit::<ffi::ksch_erc_violation>::uninit();
            self.check(unsafe { ffi::ksch_erc_result_get(snapshot.0, index, item.as_mut_ptr()) })?;
            let item = unsafe { item.assume_init() };
            results.push(crate::ErcViolation {
                marker_id: unsafe { CStr::from_ptr(item.marker_id) }
                    .to_string_lossy()
                    .into_owned(),
                message: unsafe { CStr::from_ptr(item.message) }
                    .to_string_lossy()
                    .into_owned(),
                severity: item.severity,
                sheet_index: item.sheet_index,
                x: item.x,
                y: item.y,
            });
        }
        Ok(results)
    }

    /// Read text properties, copying all callback strings before returning.
    pub fn item_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        let mut data = self.read_properties(false)?;
        // SAFETY: the session and output pointer remain live for the call.
        self.check(unsafe {
            ffi::ksch_session_property_capabilities(self.raw.as_ptr(), &mut data.capabilities)
        })?;
        Ok(data)
    }

    /// Read the current project's schematic setup controls.
    pub fn setup_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        self.read_properties(true)
    }

    fn read_properties(&mut self, setup: bool) -> Result<crate::ItemProperties, Error> {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            id: *const std::ffi::c_char,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
            kind: u32,
            choices: *const *const std::ffi::c_char,
            choice_count: u32,
        ) {
            // SAFETY: the host calls synchronously with valid strings and our owned output pointer.
            unsafe {
                let out = &mut *context.cast::<crate::ItemProperties>();
                out.item_id = CStr::from_ptr(id).to_string_lossy().into_owned();
                out.entries.push(crate::PropertyEntry {
                    name: CStr::from_ptr(name).to_string_lossy().into_owned(),
                    value: CStr::from_ptr(value).to_string_lossy().into_owned(),
                    kind,
                    choices: (0..choice_count as usize)
                        .map(|index| {
                            CStr::from_ptr(*choices.add(index))
                                .to_string_lossy()
                                .into_owned()
                        })
                        .collect(),
                });
            }
        }
        let mut result = crate::ItemProperties::default();
        // SAFETY: result remains alive for the synchronous callback.
        self.check(unsafe {
            if setup {
                ffi::ksch_session_setup(
                    self.raw.as_ptr(),
                    Some(visit),
                    (&mut result as *mut crate::ItemProperties).cast(),
                )
            } else {
                ffi::ksch_session_item_properties(
                    self.raw.as_ptr(),
                    Some(visit),
                    (&mut result as *mut crate::ItemProperties).cast(),
                )
            }
        })?;
        Ok(result)
    }

    /// Apply all text values as one undo transaction.
    pub fn apply_properties(&mut self, data: &crate::ItemProperties) -> Result<(), Error> {
        let string = |value: &str| {
            CString::new(value).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Property text may not contain a NUL".into(),
            })
        };
        let id = string(&data.item_id)?;
        let values = data
            .entries
            .iter()
            .map(|entry| string(&entry.value))
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = values
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        // SAFETY: all strings and their pointer array remain valid for the call.
        self.check(unsafe {
            ffi::ksch_session_apply_properties(
                self.raw.as_ptr(),
                id.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }

    /// Validate and save schematic setup to the document's project file.
    pub fn apply_setup_properties(&mut self, data: &crate::ItemProperties) -> Result<(), Error> {
        let values = data
            .entries
            .iter()
            .map(|entry| {
                CString::new(entry.value.as_str()).map_err(|_| Error::Failed {
                    status: Status::InvalidArg,
                    message: "Settings cannot contain NUL".into(),
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = values
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        self.check(unsafe {
            ffi::ksch_session_apply_setup(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }

    /// Configured library nicknames without loading all their symbols.
    pub fn symbol_libraries(&mut self) -> Result<Vec<String>, Error> {
        let mut names = std::ptr::null();
        // SAFETY: live session and writable output, copied before another call.
        self.check(unsafe { ffi::ksch_session_symbol_libraries(self.raw.as_ptr(), &mut names) })?;
        Ok(unsafe { std::ffi::CStr::from_ptr(names) }
            .to_string_lossy()
            .lines()
            .map(str::to_owned)
            .collect())
    }
    /// Browse one installed library, or cached symbols when the library is empty.
    pub fn browse_symbols(
        &mut self,
        library: &str,
        power_only: bool,
    ) -> Result<Vec<String>, Error> {
        let library = CString::new(library).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "library may not contain NUL".into(),
        })?;
        let mut ids = std::ptr::null();
        // SAFETY: input string and output pointer valid for the call; copy the result immediately.
        self.check(unsafe {
            ffi::ksch_session_browse_symbols(
                self.raw.as_ptr(),
                library.as_ptr(),
                power_only as u32,
                &mut ids,
            )
        })?;
        Ok(unsafe { std::ffi::CStr::from_ptr(ids) }
            .to_string_lossy()
            .lines()
            .map(str::to_owned)
            .collect())
    }
    /// Library IDs already cached in the loaded schematic.
    pub fn list_symbols(&mut self) -> Result<Vec<String>, Error> {
        let mut ids = std::ptr::null();
        // SAFETY: live session and writable output; copied before another call.
        self.check(unsafe { ffi::ksch_session_list_symbols(self.raw.as_ptr(), &mut ids) })?;
        let text = unsafe { std::ffi::CStr::from_ptr(ids) }.to_string_lossy();
        Ok(text.lines().map(str::to_owned).collect())
    }

    /// Record an isolated chooser preview and report available units/body styles.
    pub fn preview_symbol(
        &mut self,
        id: &str,
        unit: u32,
        body: u32,
    ) -> Result<(Stream, u32, u32), Error> {
        let id = CString::new(id).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "Symbol ID contains NUL".into(),
        })?;
        let mut view = empty_view();
        let (mut units, mut bodies) = (0, 0);
        // SAFETY: live session and owned out parameters; copy before another call.
        self.check(unsafe {
            ffi::ksch_session_preview_symbol(
                self.raw.as_ptr(),
                id.as_ptr(),
                unit,
                body,
                &mut units,
                &mut bodies,
                &mut view,
            )
        })?;
        // SAFETY: preview buffer remains owned by this exclusively borrowed session.
        Ok((
            unsafe { StreamView::from_raw(&view) }?.to_owned_stream(),
            units,
            bodies,
        ))
    }
    /// Place a chosen unit and body style.
    pub fn place_symbol_variant(&mut self, id: &str, unit: u32, body: u32) -> Result<(), Error> {
        let id = CString::new(id).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "Symbol ID contains NUL".into(),
        })?;
        // SAFETY: live session and valid UTF-8 C string.
        self.check(unsafe {
            ffi::ksch_session_place_symbol_variant(self.raw.as_ptr(), id.as_ptr(), unit, body)
        })
    }
    /// Begin interactive placement of an installed or cached library symbol.
    pub fn place_symbol(&mut self, id: &str) -> Result<(), Error> {
        let id = CString::new(id).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "symbol ID may not contain NUL".into(),
        })?;
        // SAFETY: live session and string valid throughout the call.
        self.check(unsafe { ffi::ksch_session_place_symbol(self.raw.as_ptr(), id.as_ptr()) })
    }

    /// Copy search terms to the host and update match highlighting.
    pub fn set_search_data(&mut self, data: &crate::SearchData) -> Result<(), Error> {
        let string = |value: &str| {
            CString::new(value).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "search text may not contain a NUL".into(),
            })
        };
        let find = string(&data.find)?;
        let replace = string(&data.replace)?;
        let raw = ffi::ksch_search_data {
            find: find.as_ptr(),
            replace: replace.as_ptr(),
            match_case: data.match_case as u32,
            whole_word: data.whole_word as u32,
            current_sheet_only: data.current_sheet_only as u32,
            selected_only: data.selected_only as u32,
            replace_references: data.replace_references as u32,
            search_all_fields: data.search_all_fields as u32,
            search_all_pins: data.search_all_pins as u32,
            replace_mode: data.replace_mode as u32,
            active: data.active as u32,
        };
        // SAFETY: all strings remain alive for the call; the host copies them.
        self.check(unsafe { ffi::ksch_session_set_search_data(self.raw.as_ptr(), &raw) })
    }

    /// Read the result after dispatching a Find/Replace action.
    pub fn search_result(&mut self) -> Result<crate::SearchResult, Error> {
        let mut raw = MaybeUninit::<ffi::ksch_search_result>::uninit();
        // SAFETY: live session and writable output, initialized on success.
        self.check(unsafe {
            ffi::ksch_session_search_result(self.raw.as_ptr(), raw.as_mut_ptr())
        })?;
        let raw = unsafe { raw.assume_init() };
        Ok(crate::SearchResult {
            found: raw.found != 0,
            wrapped: raw.wrapped != 0,
            center_x: raw.center_x,
            center_y: raw.center_y,
            replaced: raw.replaced,
        })
    }

    /// Undo the newest command.
    ///
    /// Deliberately not `run_action("common.Interactive.undo")`: that action belongs to
    /// `SCH_EDITOR_CONTROL`, which still declines an editing context that is not a
    /// `wxFrame`. Undo itself needs no frame, so the ABI carries it directly.
    ///
    /// Returns whether anything was undone; false means the stack was empty.
    pub fn undo(&mut self) -> Result<bool, Error> {
        let mut undone: c_int = 0;

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_undo(self.raw.as_ptr(), &mut undone) })?;

        Ok(undone != 0)
    }

    /// Redo the newest undone command. See [`Session::undo`].
    pub fn redo(&mut self) -> Result<bool, Error> {
        let mut redone: c_int = 0;

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_redo(self.raw.as_ptr(), &mut redone) })?;

        Ok(redone != 0)
    }

    /// Write every sheet back to the file it was loaded from.
    ///
    /// The `.kicad_sch` files and nothing else — not the project file, the symbol library
    /// table, a backup or the embedded-file cache, each of which is a decision about the
    /// project rather than about the document.
    pub fn save(&mut self) -> Result<(), Error> {
        // SAFETY: a live handle.
        self.check(unsafe { ffi::ksch_session_save(self.raw.as_ptr()) })
    }

    /// What the editor is doing: the cursor, the selection and the status text.
    ///
    /// `&mut self` because the C++ side is not const about it — reading the
    /// selection asks the selection tool for it.
    pub fn editor_state(&mut self) -> Result<EditorState, Error> {
        let mut state = ffi::ksch_editor_state::default();

        // SAFETY: a live handle and an out-parameter we own.
        self.check(unsafe { ffi::ksch_session_editor_state(self.raw.as_ptr(), &mut state) })?;

        // SAFETY: both strings are non-null and session-scoped, and are copied
        // before anything else touches the session.
        let tool_name = unsafe { borrowed_string(state.tool_name) };
        let status_text = unsafe { borrowed_string(state.status_text) };

        Ok(EditorState {
            cursor: (state.cursor_x, state.cursor_y),
            selection_count: state.selection_count,
            undo_count: state.undo_count,
            redo_count: state.redo_count,
            pointer_over_canvas: state.flags
                & ffi::ksch_editor_flag_KSCH_EDITOR_POINTER_OVER_CANVAS
                != 0,
            modified: state.flags & ffi::ksch_editor_flag_KSCH_EDITOR_MODIFIED != 0,
            tool_name,
            status_text,
        })
    }

    /// Turn a status code into a result, attaching the session's error string.
    fn check(&self, status: ffi::ksch_status) -> Result<(), Error> {
        if status == ffi::ksch_status_KSCH_OK {
            return Ok(());
        }

        Err(Error::Failed {
            status: Status::from_code(status),
            message: self.last_error(),
        })
    }

    /// The session's last error message, or an empty string.
    fn last_error(&self) -> String {
        // SAFETY: a live handle. The ABI never returns null here, and the string
        // is valid until the next call on the session — it is copied before this
        // returns.
        unsafe { borrowed_string(ffi::ksch_session_last_error(self.raw.as_ptr())) }
    }
}

impl Drop for Session {
    fn drop(&mut self) {
        // SAFETY: a live handle, destroyed exactly once. Every borrowed view is
        // gone by now, because they borrow from `self`.
        unsafe { ffi::ksch_session_destroy(self.raw.as_ptr()) };
    }
}

impl std::fmt::Debug for Session {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Session")
            .field("loaded", &self.is_loaded())
            .finish()
    }
}

/// Unpack the result flags both input entry points return.
fn outcome(flags: u32) -> InputOutcome {
    InputOutcome {
        handled: flags & ffi::ksch_input_result_KSCH_INPUT_HANDLED != 0,
        redraw: flags & ffi::ksch_input_result_KSCH_INPUT_REDRAW != 0,
    }
}

fn raw_modifiers(modifiers: Modifiers) -> u32 {
    let mut bits = 0;

    if modifiers.shift {
        bits |= ffi::ksch_modifier_KSCH_MOD_SHIFT;
    }
    if modifiers.ctrl {
        bits |= ffi::ksch_modifier_KSCH_MOD_CTRL;
    }
    if modifiers.alt {
        bits |= ffi::ksch_modifier_KSCH_MOD_ALT;
    }
    if modifiers.meta {
        bits |= ffi::ksch_modifier_KSCH_MOD_META;
    }

    bits
}

fn raw_button(button: PointerButton) -> i32 {
    let value = match button {
        PointerButton::Left => ffi::ksch_pointer_button_KSCH_BUTTON_LEFT,
        PointerButton::Right => ffi::ksch_pointer_button_KSCH_BUTTON_RIGHT,
        PointerButton::Middle => ffi::ksch_pointer_button_KSCH_BUTTON_MIDDLE,
        PointerButton::Back => ffi::ksch_pointer_button_KSCH_BUTTON_BACK,
        PointerButton::Forward => ffi::ksch_pointer_button_KSCH_BUTTON_FORWARD,
    };

    value as i32
}

/// Flatten an [`InputEvent`] into the ABI's tagged struct.
///
/// `key` is the NUL-terminated name for a key event, which the caller owns for the
/// duration of the call — this only borrows it.
fn raw_input(event: &InputEvent<'_>, key: Option<&CStr>) -> ffi::ksch_input_event {
    let mut raw = ffi::ksch_input_event {
        type_: ffi::ksch_input_type_KSCH_INPUT_POINTER_MOTION as i32,
        button: ffi::ksch_pointer_button_KSCH_BUTTON_NONE as i32,
        modifiers: 0,
        flags: 0,
        key: std::ptr::null(),
        x: 0.0,
        y: 0.0,
        scroll_x: 0.0,
        scroll_y: 0.0,
    };

    let pointer = |raw: &mut ffi::ksch_input_event, screen: (f64, f64), modifiers: Modifiers| {
        raw.x = screen.0;
        raw.y = screen.1;
        raw.modifiers = raw_modifiers(modifiers);
    };

    match event {
        InputEvent::PointerMotion { screen, modifiers } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_POINTER_MOTION as i32;
            pointer(&mut raw, *screen, *modifiers);
        }
        InputEvent::PointerDown {
            button,
            screen,
            modifiers,
        } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_POINTER_DOWN as i32;
            raw.button = raw_button(*button);
            pointer(&mut raw, *screen, *modifiers);
        }
        InputEvent::PointerUp {
            button,
            screen,
            modifiers,
        } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_POINTER_UP as i32;
            raw.button = raw_button(*button);
            pointer(&mut raw, *screen, *modifiers);
        }
        InputEvent::PointerDoubleClick {
            button,
            screen,
            modifiers,
        } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_POINTER_DBLCLICK as i32;
            raw.button = raw_button(*button);
            pointer(&mut raw, *screen, *modifiers);
        }
        InputEvent::PointerLeave => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_POINTER_LEAVE as i32;
        }
        InputEvent::Scroll {
            screen,
            delta,
            modifiers,
        } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_SCROLL as i32;
            pointer(&mut raw, *screen, *modifiers);
            raw.scroll_x = delta.0;
            raw.scroll_y = delta.1;
        }
        InputEvent::KeyDown {
            modifiers,
            auto_repeat,
            ..
        } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_KEY_DOWN as i32;
            raw.modifiers = raw_modifiers(*modifiers);

            if *auto_repeat {
                raw.flags |= ffi::ksch_input_flag_KSCH_INPUT_FLAG_AUTOREPEAT;
            }
        }
        InputEvent::KeyUp { modifiers, .. } => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_KEY_UP as i32;
            raw.modifiers = raw_modifiers(*modifiers);
        }
        InputEvent::Cancel => {
            raw.type_ = ffi::ksch_input_type_KSCH_INPUT_CANCEL as i32;
        }
    }

    if let Some(key) = key {
        raw.key = key.as_ptr();
    }

    raw
}

/// Stand up the process, once, and confirm we are on the thread that owns it.
///
/// Two jobs, because they are the same fact: the first caller both initialises
/// the host and becomes the thread it belongs to. wx records that thread as its
/// main thread, and `SCH_CONNECTIVITY::ENGINE::Clear` asserts `wxThread::IsMain()`
/// on every document load — so a second thread is not a race to be serialised
/// with a lock, it is simply not allowed. Reporting it as [`Error::WrongThread`]
/// turns a C++ assertion on someone else's stack into a value.
///
/// The ABI's own guard is not thread safe and is not meant to be; this is.
fn ensure_runtime() -> Result<(), Error> {
    static STATE: OnceLock<Result<std::thread::ThreadId, String>> = OnceLock::new();

    // A `String` rather than an `Error`, because the result is cached and
    // handed out repeatedly, and `Error` is not `Clone` by design.
    let state = STATE.get_or_init(|| {
        // SAFETY: no arguments, no state read.
        let library = unsafe { ffi::ksch_abi_version() };

        if library != ffi::KSCH_ABI_VERSION {
            // Reported through the same channel as a runtime failure so that
            // the version check cannot be forgotten by a caller that only
            // matches on `Error::Runtime`; `Error::AbiMismatch` says it exactly.
            return Err(format!(
                "ABI version {library}, expected {}",
                ffi::KSCH_ABI_VERSION
            ));
        }

        // SAFETY: called under `OnceLock`, so never concurrently with itself,
        // which is the ABI's only requirement.
        let status = unsafe { ffi::ksch_runtime_init() };

        if status != ffi::ksch_status_KSCH_OK {
            return Err(format!(
                "{}: {}",
                Status::from_code(status).name(),
                global_error()
            ));
        }

        Ok(std::thread::current().id())
    });

    // The mismatch case is worth its own variant, and this is the one place it
    // can be recovered from the message.
    match state {
        Ok(owner) if *owner == std::thread::current().id() => Ok(()),
        Ok(_) => Err(Error::WrongThread),
        Err(message) if message.starts_with("ABI version ") => {
            // SAFETY: no arguments, no state read.
            Err(Error::AbiMismatch {
                library: unsafe { ffi::ksch_abi_version() },
                expected: ffi::KSCH_ABI_VERSION,
            })
        }
        Err(message) => Err(Error::Runtime(message.clone())),
    }
}

/// The error string from a call that had no session to record it on.
fn global_error() -> String {
    // SAFETY: never null, and process-lived, so copying it out is always safe.
    unsafe { borrowed_string(ffi::ksch_last_global_error()) }
}

/// Copy a borrowed C string out, tolerating null and invalid UTF-8.
///
/// # Safety
///
/// `raw` must be null or point at a NUL-terminated string that stays put and
/// unmodified for the duration of the call.
unsafe fn borrowed_string(raw: *const std::os::raw::c_char) -> String {
    if raw.is_null() {
        return String::new();
    }

    // SAFETY: the caller guarantees a NUL-terminated string.
    unsafe { CStr::from_ptr(raw) }
        .to_string_lossy()
        .into_owned()
}

/// A path as the ABI wants it: NUL-terminated UTF-8.
fn c_path(path: &Path) -> Result<CString, Error> {
    let text = path.to_str().ok_or_else(|| Error::BadPath(path.into()))?;

    CString::new(text).map_err(|_| Error::BadPath(path.into()))
}

/// A zeroed `kgds_stream_view`, for the ABI to fill in.
///
/// An all-zero view is the empty one — null section pointers with zero counts —
/// so this is also what is left behind if a call fails, the ABI having promised
/// not to touch out-parameters in that case.
fn empty_view() -> kgds_stream_view {
    // SAFETY: the struct is plain data: integers and raw pointers, for all of
    // which all-zero is a valid value.
    unsafe { MaybeUninit::<kgds_stream_view>::zeroed().assume_init() }
}

impl Session {
    /// Snapshot editable document workflow fields. Strings are copied during the callback.
    pub fn document_workflow(&mut self, kind: u32) -> Result<Vec<(String, String)>, Error> {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
        ) {
            // SAFETY: callback is synchronous, with live UTF-8 strings and our vector context.
            unsafe {
                (&mut *context.cast::<Vec<(String, String)>>()).push((
                    CStr::from_ptr(name).to_string_lossy().into_owned(),
                    CStr::from_ptr(value).to_string_lossy().into_owned(),
                ));
            }
        }
        let mut result = Vec::<(String, String)>::new();
        // SAFETY: the vector and session live throughout the callback.
        self.check(unsafe {
            ffi::ksch_session_document_workflow(
                self.raw.as_ptr(),
                kind,
                Some(visit),
                (&mut result as *mut Vec<(String, String)>).cast(),
            )
        })?;
        Ok(result)
    }
    /// Apply a validated document workflow through the native model service.
    pub fn apply_document_workflow(&mut self, kind: u32, values: &[String]) -> Result<(), Error> {
        let values = values
            .iter()
            .map(|s| {
                CString::new(s.as_str()).map_err(|_| Error::Failed {
                    status: Status::InvalidArg,
                    message: "Text may not contain NUL".into(),
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = values.iter().map(|s| s.as_ptr()).collect::<Vec<_>>();
        // SAFETY: pointer array and its strings remain live for the synchronous call.
        self.check(unsafe {
            ffi::ksch_session_apply_document_workflow(
                self.raw.as_ptr(),
                kind,
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }
}

impl Session {
    /// Read the project (false) or global (true) symbol library table.
    pub fn library_table(&mut self, global: bool) -> Result<Vec<crate::LibraryRow>, Error> {
        unsafe extern "C" fn collect(
            context: *mut std::ffi::c_void,
            row: *const ffi::ksch_library_row,
        ) {
            // SAFETY: the host calls synchronously with borrowed valid strings and our Vec pointer.
            unsafe {
                let output = &mut *context.cast::<Vec<crate::LibraryRow>>();
                let row = &*row;
                let text = |ptr| CStr::from_ptr(ptr).to_string_lossy().into_owned();
                output.push(crate::LibraryRow {
                    name: text(row.name),
                    kind: text(row.kind),
                    uri: text(row.uri),
                    options: text(row.options),
                    description: text(row.description),
                    enabled: row.enabled != 0,
                    visible: row.visible != 0,
                });
            }
        }
        let mut rows = Vec::new();
        // SAFETY: live session, synchronous callback, correctly typed context.
        self.check(unsafe {
            ffi::ksch_session_library_table(
                self.raw.as_ptr(),
                global as u32,
                Some(collect),
                (&mut rows as *mut Vec<crate::LibraryRow>).cast(),
            )
        })?;
        Ok(rows)
    }
    /// Validate and persist a symbol library table through KiCad's atomic writer.
    pub fn save_library_table(
        &mut self,
        global: bool,
        rows: &[crate::LibraryRow],
    ) -> Result<(), Error> {
        let strings = rows
            .iter()
            .map(|r| {
                [&r.name, &r.kind, &r.uri, &r.options, &r.description]
                    .into_iter()
                    .map(|s| CString::new(s.as_str()))
                    .collect::<Result<Vec<_>, _>>()
            })
            .collect::<Result<Vec<_>, _>>()
            .map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Library values may not contain NUL".into(),
            })?;
        let raw: Vec<_> = rows
            .iter()
            .zip(&strings)
            .map(|(r, s)| ffi::ksch_library_row {
                name: s[0].as_ptr(),
                kind: s[1].as_ptr(),
                uri: s[2].as_ptr(),
                options: s[3].as_ptr(),
                description: s[4].as_ptr(),
                enabled: r.enabled as u32,
                visible: r.visible as u32,
            })
            .collect();
        // SAFETY: strings and rows remain live for the synchronous native call.
        self.check(unsafe {
            ffi::ksch_session_save_library_table(
                self.raw.as_ptr(),
                global as u32,
                raw.as_ptr(),
                raw.len() as u32,
            )
        })
    }
}

impl Session {
    /// Read simulation model, analysis configuration, or result vectors.
    pub fn simulation_workflow(&mut self, kind: u32) -> Result<Vec<(String, String)>, Error> {
        unsafe extern "C" fn collect(
            context: *mut std::ffi::c_void,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
        ) {
            // SAFETY: synchronous visitor receives valid borrowed strings and Vec context.
            unsafe {
                (&mut *context.cast::<Vec<(String, String)>>()).push((
                    CStr::from_ptr(name).to_string_lossy().into_owned(),
                    CStr::from_ptr(value).to_string_lossy().into_owned(),
                ));
            }
        }
        let mut rows = Vec::new();
        // SAFETY: live session and visitor context outlive the call.
        self.check(unsafe {
            ffi::ksch_session_simulation_workflow(
                self.raw.as_ptr(),
                kind,
                Some(collect),
                (&mut rows as *mut Vec<(String, String)>).cast(),
            )
        })?;
        Ok(rows)
    }
    /// Apply model fields, run an analysis, or stop simulation.
    pub fn apply_simulation_workflow(&mut self, kind: u32, values: &[String]) -> Result<(), Error> {
        let strings: Vec<_> = values
            .iter()
            .map(|s| CString::new(s.as_str()))
            .collect::<Result<_, _>>()
            .map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Simulation values may not contain NUL".into(),
            })?;
        let pointers: Vec<_> = strings.iter().map(|s| s.as_ptr()).collect();
        // SAFETY: all owned strings remain live throughout the synchronous call.
        self.check(unsafe {
            ffi::ksch_session_apply_simulation_workflow(
                self.raw.as_ptr(),
                kind,
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }
}

impl Session {
    /// Create a selected item's custom field, or delete it when value is None.
    pub fn edit_custom_field(
        &mut self,
        item_id: &str,
        name: &str,
        value: Option<&str>,
    ) -> Result<(), Error> {
        let string = |text: &str| {
            CString::new(text).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Field text may not contain NUL".into(),
            })
        };
        let id = string(item_id)?;
        let name = string(name)?;
        let value = value.map(string).transpose()?;
        // SAFETY: session and optional NUL-terminated strings outlive this synchronous call.
        self.check(unsafe {
            ffi::ksch_session_edit_custom_field(
                self.raw.as_ptr(),
                id.as_ptr(),
                name.as_ptr(),
                value
                    .as_ref()
                    .map_or(std::ptr::null(), |value| value.as_ptr()),
            )
        })
    }
}

impl Session {
    /// Load Database or HTTP connection settings for a configured library.
    pub fn configure_library(
        &mut self,
        global: bool,
        nickname: &str,
    ) -> Result<Vec<(String, String)>, Error> {
        unsafe extern "C" fn collect(
            context: *mut std::ffi::c_void,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
        ) {
            // SAFETY: synchronous visitor with borrowed valid strings and our typed Vec context.
            unsafe {
                (&mut *context.cast::<Vec<(String, String)>>()).push((
                    CStr::from_ptr(name).to_string_lossy().into_owned(),
                    CStr::from_ptr(value).to_string_lossy().into_owned(),
                ));
            }
        }
        let nickname = CString::new(nickname).map_err(|_| Error::Failed {
            status: Status::InvalidArg,
            message: "Library name contains NUL".into(),
        })?;
        let mut rows = Vec::new();
        // SAFETY: valid session, nickname and context outlive the synchronous call.
        self.check(unsafe {
            ffi::ksch_session_configure_library(
                self.raw.as_ptr(),
                global as u32,
                nickname.as_ptr(),
                Some(collect),
                (&mut rows as *mut Vec<(String, String)>).cast(),
            )
        })?;
        Ok(rows)
    }
}

impl Session {
    /// Relink a hierarchical sheet to a file, clearing undo history after validation.
    pub fn relink_sheet(&mut self, item_id: &str, path: &str) -> Result<(), Error> {
        let string = |text: &str| {
            CString::new(text).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Sheet text may not contain NUL".into(),
            })
        };
        let id = string(item_id)?;
        let path = string(path)?;
        // SAFETY: the session and NUL-terminated strings outlive the synchronous call.
        self.check(unsafe {
            ffi::ksch_session_relink_sheet(self.raw.as_ptr(), id.as_ptr(), path.as_ptr())
        })
    }
}

impl Session {
    /// Consume a properties request from the active placement tool.
    pub fn take_pending_properties(&mut self) -> Result<bool, Error> {
        let mut pending = 0;
        // SAFETY: output and session remain valid throughout the call.
        self.check(unsafe {
            ffi::ksch_session_take_pending_properties(self.raw.as_ptr(), &mut pending)
        })?;
        Ok(pending != 0)
    }
}

impl Session {
    /// Snapshot sheet-pin synchronization choices, with a stable model fingerprint.
    pub fn sheet_pin_properties(&mut self, all: bool) -> Result<crate::ItemProperties, Error> {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            id: *const std::ffi::c_char,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
            kind: u32,
            choices: *const *const std::ffi::c_char,
            count: u32,
        ) {
            // SAFETY: the host calls synchronously with borrowed strings and our owned output.
            unsafe {
                let data = &mut *context.cast::<crate::ItemProperties>();
                let text = |ptr| CStr::from_ptr(ptr).to_string_lossy().into_owned();
                data.item_id = text(id);
                data.entries.push(crate::PropertyEntry {
                    name: text(name),
                    value: text(value),
                    kind,
                    choices: (0..count as usize).map(|i| text(*choices.add(i))).collect(),
                });
            }
        }
        let mut data = crate::ItemProperties::default();
        // SAFETY: the session and output live throughout the synchronous visitor.
        self.check(unsafe {
            ffi::ksch_session_sheet_pin_properties(
                self.raw.as_ptr(),
                all.into(),
                Some(visit),
                (&mut data as *mut crate::ItemProperties).cast(),
            )
        })?;
        Ok(data)
    }
    /// Apply reviewed synchronization choices as one undo transaction.
    pub fn apply_sheet_pin_properties(
        &mut self,
        all: bool,
        data: &crate::ItemProperties,
    ) -> Result<(), Error> {
        let string = |s: &str| {
            CString::new(s).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Synchronization text may not contain NUL".into(),
            })
        };
        let id = string(&data.item_id)?;
        let strings = data
            .entries
            .iter()
            .map(|entry| string(&entry.value))
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = strings
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        // SAFETY: the session, string array and fingerprint live for this synchronous call.
        self.check(unsafe {
            ffi::ksch_session_apply_sheet_pin_properties(
                self.raw.as_ptr(),
                all.into(),
                id.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }
}

impl Session {
    /// Snapshot application preferences.
    pub fn preferences(&mut self) -> Result<crate::ItemProperties, Error> {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            id: *const std::ffi::c_char,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
            kind: u32,
            choices: *const *const std::ffi::c_char,
            count: u32,
        ) {
            // SAFETY: the host calls synchronously with borrowed strings and our owned output.
            unsafe {
                let data = &mut *context.cast::<crate::ItemProperties>();
                let text = |ptr| CStr::from_ptr(ptr).to_string_lossy().into_owned();
                data.item_id = text(id);
                data.entries.push(crate::PropertyEntry {
                    name: text(name),
                    value: text(value),
                    kind,
                    choices: (0..count as usize).map(|i| text(*choices.add(i))).collect(),
                });
            }
        }
        let mut data = crate::ItemProperties::default();
        // SAFETY: the session and output live throughout the synchronous visitor.
        self.check(unsafe {
            ffi::ksch_session_preferences(
                self.raw.as_ptr(),
                Some(visit),
                (&mut data as *mut crate::ItemProperties).cast(),
            )
        })?;
        Ok(data)
    }
    pub fn graphics_import_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            id: *const std::ffi::c_char,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
            kind: u32,
            choices: *const *const std::ffi::c_char,
            count: u32,
        ) {
            // SAFETY: the host calls synchronously with borrowed strings and our owned output.
            unsafe {
                let data = &mut *context.cast::<crate::ItemProperties>();
                let text = |ptr| CStr::from_ptr(ptr).to_string_lossy().into_owned();
                data.item_id = text(id);
                data.entries.push(crate::PropertyEntry {
                    name: text(name),
                    value: text(value),
                    kind,
                    choices: (0..count as usize).map(|i| text(*choices.add(i))).collect(),
                });
            }
        }
        let mut data = crate::ItemProperties::default();
        // SAFETY: the session and output live throughout the synchronous visitor.
        self.check(unsafe {
            ffi::ksch_session_graphics_import_properties(
                self.raw.as_ptr(),
                Some(visit),
                (&mut data as *mut crate::ItemProperties).cast(),
            )
        })?;
        Ok(data)
    }
    pub fn image_properties(&mut self) -> Result<crate::ItemProperties, Error> {
        unsafe extern "C" fn visit(
            context: *mut std::ffi::c_void,
            id: *const std::ffi::c_char,
            name: *const std::ffi::c_char,
            value: *const std::ffi::c_char,
            kind: u32,
            choices: *const *const std::ffi::c_char,
            count: u32,
        ) {
            // SAFETY: the host calls synchronously with borrowed strings and our owned output.
            unsafe {
                let data = &mut *context.cast::<crate::ItemProperties>();
                let text = |ptr| CStr::from_ptr(ptr).to_string_lossy().into_owned();
                data.item_id = text(id);
                data.entries.push(crate::PropertyEntry {
                    name: text(name),
                    value: text(value),
                    kind,
                    choices: (0..count as usize).map(|i| text(*choices.add(i))).collect(),
                });
            }
        }
        let mut data = crate::ItemProperties::default();
        // SAFETY: the session and output live throughout the synchronous visitor.
        self.check(unsafe {
            ffi::ksch_session_image_properties(
                self.raw.as_ptr(),
                Some(visit),
                (&mut data as *mut crate::ItemProperties).cast(),
            )
        })?;
        Ok(data)
    }
    /// Apply application preferences to their native stores.
    pub fn apply_preferences(&mut self, data: &crate::ItemProperties) -> Result<(), Error> {
        let string = |s: &str| {
            CString::new(s).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Synchronization text may not contain NUL".into(),
            })
        };

        let strings = data
            .entries
            .iter()
            .map(|entry| string(&entry.value))
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = strings
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        // SAFETY: the session, string array and fingerprint live for this synchronous call.
        self.check(unsafe {
            ffi::ksch_session_apply_preferences(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }
    pub fn apply_graphics_import(&mut self, data: &crate::ItemProperties) -> Result<(), Error> {
        let string = |s: &str| {
            CString::new(s).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Synchronization text may not contain NUL".into(),
            })
        };

        let strings = data
            .entries
            .iter()
            .map(|entry| string(&entry.value))
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = strings
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        // SAFETY: the session, string array and fingerprint live for this synchronous call.
        self.check(unsafe {
            ffi::ksch_session_apply_graphics_import(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }
    pub fn apply_image_properties(&mut self, data: &crate::ItemProperties) -> Result<(), Error> {
        let string = |s: &str| {
            CString::new(s).map_err(|_| Error::Failed {
                status: Status::InvalidArg,
                message: "Synchronization text may not contain NUL".into(),
            })
        };

        let strings = data
            .entries
            .iter()
            .map(|entry| string(&entry.value))
            .collect::<Result<Vec<_>, _>>()?;
        let pointers = strings
            .iter()
            .map(|value| value.as_ptr())
            .collect::<Vec<_>>();
        // SAFETY: the session, string array and fingerprint live for this synchronous call.
        self.check(unsafe {
            ffi::ksch_session_apply_image_properties(
                self.raw.as_ptr(),
                pointers.as_ptr(),
                pointers.len() as u32,
            )
        })
    }
}
