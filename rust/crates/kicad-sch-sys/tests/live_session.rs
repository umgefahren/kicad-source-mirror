// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The linked host, end to end: open a real `.kicad_sch`, render it, and check
//! the result against the stream `kicad-sch-dump` recorded from the same file.
//!
//! That comparison is the point of this file. `qa/data/draw_streams/` is what the
//! renderer's own tests are written against, and it was produced by the C++ tool
//! driving this same ABI. If a live render paints the same picture as the fixture,
//! then the two halves of the migration are looking at the same geometry, and
//! every test written against a fixture also covers the live path.
//!
//! # Two things that look like compromises and are not
//!
//! **There is no test harness** (`harness = false`). libtest runs each `#[test]`
//! on a worker thread, and this API is main-thread-only: wx takes the thread that
//! initialises it to be its main thread, and `SCH_CONNECTIVITY::ENGINE::Clear`
//! asserts `wxThread::IsMain()` on every document load. Under the normal harness
//! the first test to run claims the host and the other eight fail
//! [`Error::WrongThread`] — correctly. A plain `main()` is the honest shape for a
//! main-thread API, so that is what this is.
//!
//! **The fixture comparison ignores ordering.** On this file a live render is
//! byte-identical to what `kicad-sch-dump` writes on the same machine — one ABI,
//! two callers, same bytes. It is not byte-identical to the *checked-in* fixture,
//! which was recorded on Linux: the group table and all 2,587 group commands
//! match exactly, and four of the 222 group bodies hold the same geometry under
//! different ids. What differs is the order `KIGFX::VIEW` visited items in, which
//! libstdc++ and libc++ are entitled to disagree about — an unstable sort does not
//! fix an order for equal keys. Asserting byte equality against the fixture would
//! assert the standard library, not KiCad. See `docs/rust-migration/04-host-seam.md`
//! §8.
//!
//! What is asserted exactly, because it is environment-independent: rendering the
//! same document twice produces the same bytes, and a stream written to disk reads
//! back equal.
//!
//! Prints and does nothing in a build with no host library — see the crate docs.

#[cfg(not(ksch_linked))]
fn main() {
    println!("no host library is linked into this build; nothing to check");
}

#[cfg(ksch_linked)]
fn main() {
    // Every check runs here, on the main thread, in order. A panic is caught so
    // that one failure does not hide the other eight results; its message has
    // already gone to stderr by then.
    let checks: &[(&str, fn())] = &[
        (
            "loading reports the hierarchy the fixture recorded",
            live::loading_reports_the_hierarchy,
        ),
        (
            "a live render paints the recorded fixture",
            live::a_live_render_paints_the_recorded_fixture,
        ),
        (
            "rendering twice produces the same frame",
            live::rendering_twice_produces_the_same_frame,
        ),
        (
            "moving the camera re-records the frame and not the geometry",
            live::moving_the_camera_re_records_the_frame_and_not_the_geometry,
        ),
        (
            "a written stream reads back as the one rendered",
            live::a_written_stream_reads_back_as_rendered,
        ),
        (
            "the camera reads back and frames the page",
            live::the_camera_reads_back_and_frames_the_page,
        ),
        (
            "a viewport with no area is refused",
            live::a_viewport_with_no_area_is_refused,
        ),
        (
            "a missing file is an error with a message",
            live::a_missing_file_is_an_error_with_a_message,
        ),
        (
            "rendering without a document is an error",
            live::rendering_without_a_document_is_an_error,
        ),
        (
            "a session can be reused for another document",
            live::a_session_can_be_reused,
        ),
        (
            "a second thread is refused, not raced",
            live::a_second_thread_is_refused,
        ),
        (
            "host input moves the cursor the tools read",
            live::host_input_moves_the_cursor_the_tools_read,
        ),
        (
            "a whole click gesture crosses the ABI and edits nothing",
            live::a_click_gesture_crosses_and_edits_nothing,
        ),
        (
            "an action no tool handles is reported, not an error",
            live::an_unhandled_action_is_reported,
        ),
    ];

    let mut failed = 0;

    for (name, check) in checks {
        match std::panic::catch_unwind(*check) {
            Ok(()) => println!("ok   {name}"),
            Err(_) => {
                failed += 1;
                println!("FAIL {name}");
            }
        }
    }

    println!("\n{} passed, {failed} failed", checks.len() - failed);

    if failed > 0 {
        std::process::exit(1);
    }
}

#[cfg(ksch_linked)]
mod live {
    use std::fmt::Debug;
    use std::fs::File;
    use std::path::{Path, PathBuf};

    use kicad_gal::Command;
    use kicad_sch_sys::{
        Error, InputEvent, Modifiers, PointerButton, Session, Status, Stream, Viewport,
    };

    /// The KiCad tree this crate lives in: three levels up from the manifest.
    fn tree_root() -> PathBuf {
        Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(3)
            .expect("rust/crates/kicad-sch-sys is three levels down from the root")
            .to_path_buf()
    }

    fn kitchen_sink() -> PathBuf {
        tree_root().join("qa/data/eeschema/api_kitchen_sink.kicad_sch")
    }

    /// The viewport `qa/data/draw_streams/generate.sh` pins, so that a live render
    /// is comparable with the fixtures.
    const FIXTURE_VIEWPORT: Viewport = Viewport {
        width_px: 1920,
        height_px: 1080,
        center_x: 0.0,
        center_y: 0.0,
        scale: 1.0,
    };

    /// Load, frame and record, the way the fixture generator does.
    fn record(path: &Path) -> (Session, Stream) {
        let mut session = Session::open(path).expect("the schematic loads");

        session.set_sheet(0).expect("sheet 0 is selectable");
        session
            .set_viewport(&FIXTURE_VIEWPORT)
            .expect("a positive viewport is accepted");
        session.zoom_to_fit().expect("a loaded page can be framed");

        let stream = session.render_owned().expect("recording a frame");

        (session, stream)
    }

    /// Compare one section, reporting *where* it differs rather than dumping both.
    ///
    /// These streams are tens of thousands of elements; an `assert_eq!` on a whole
    /// one produces a megabyte of `Debug` output and says nothing useful.
    fn same_section<T: PartialEq + Debug>(what: &str, live: &[T], golden: &[T]) {
        assert_eq!(live.len(), golden.len(), "{what}: section length");

        if let Some((at, (live, golden))) = live
            .iter()
            .zip(golden)
            .enumerate()
            .find(|(_, (live, golden))| live != golden)
        {
            panic!(
                "{what}: first difference at index {at}\n  live   {live:?}\n  golden {golden:?}"
            );
        }
    }

    /// The same, for coordinates, compared by bit pattern: a lost sign on a zero
    /// is a difference.
    fn same_coords(what: &str, live: &[f64], golden: &[f64]) {
        assert_eq!(live.len(), golden.len(), "{what}: section length");

        if let Some((at, (live, golden))) = live
            .iter()
            .zip(golden)
            .enumerate()
            .find(|(_, (live, golden))| live.to_bits() != golden.to_bits())
        {
            panic!("{what}: first difference at index {at}: live {live} golden {golden}");
        }
    }

    /// What a stream actually paints, as sorted text.
    ///
    /// Every frame command, with each group reference replaced by the body it
    /// draws. That removes the two things a platform is entitled to decide — the
    /// order items were visited in, and therefore which group id each item got —
    /// and leaves the picture. Text rather than a typed multiset because `Command`
    /// is not `Ord`, and because a difference is then legible.
    fn painted(stream: &Stream) -> Vec<String> {
        let mut painted: Vec<String> = stream
            .view()
            .frame()
            .map(|instruction| match instruction.command {
                Command::DrawGroup { id, .. } => format!("draw {}", body_text(stream, id)),
                other => format!("{other:?}"),
            })
            .collect();

        painted.sort_unstable();
        painted
    }

    /// Every retained body a stream holds, as sorted text, drawn or not.
    fn bodies(stream: &Stream) -> Vec<String> {
        let mut bodies: Vec<String> = stream
            .groups()
            .iter()
            .map(|group| body_text(stream, group.id))
            .collect();

        bodies.sort_unstable();
        bodies
    }

    /// One group body, decoded, with its coordinates resolved.
    fn body_text(stream: &Stream, id: u32) -> String {
        match stream.view().group_body(id) {
            Some(body) => format!("{:?}", body.map(|i| i.command).collect::<Vec<_>>()),
            None => format!("<no group {id}>"),
        }
    }

    /// Compare two sorted lists of text, reporting the first difference.
    fn same_text(what: &str, live: &[String], golden: &[String]) {
        assert_eq!(live.len(), golden.len(), "{what}: count");

        if let Some((at, (live, golden))) = live
            .iter()
            .zip(golden)
            .enumerate()
            .find(|(_, (live, golden))| live != golden)
        {
            panic!(
                "{what}: first difference at {at} of the sorted list\n  live   {live}\n  golden {golden}"
            );
        }
    }

    pub fn loading_reports_the_hierarchy() {
        let session = Session::open(&kitchen_sink()).expect("the kitchen sink loads");

        assert!(session.is_loaded());

        let info = session.document_info().expect("a loaded document has info");

        // The numbers are the ones in qa/data/draw_streams/api_kitchen_sink.txt,
        // which kicad-sch-dump recorded from this file.
        assert_eq!(info.sheet_count, 2);
        assert_eq!(info.current_sheet, 0);
        assert_eq!(info.item_count, 77);
        assert!(!info.modified, "loading must not dirty the document");

        assert_eq!(session.sheet_count().unwrap(), 2);

        let root = session.sheet(0).expect("sheet 0 exists");
        assert_eq!(root.path, "/");
        assert_eq!(root.page_number, "1");
        assert_eq!(root.item_count, 77);

        // Past the end is an error, not a crash or a garbage struct.
        let error = session.sheet(99).expect_err("there is no sheet 99");
        assert!(
            matches!(
                error,
                Error::Failed {
                    status: Status::OutOfRange,
                    ..
                }
            ),
            "{error}"
        );
    }

    pub fn a_live_render_paints_the_recorded_fixture() {
        let (session, live) = record(&kitchen_sink());

        let golden = Stream::read(
            File::open(tree_root().join("qa/data/draw_streams/api_kitchen_sink.kgds"))
                .expect("the golden fixture is checked in"),
        )
        .expect("the golden fixture decodes");

        assert_eq!(live.version(), golden.version(), "stream version");
        assert_eq!(live.flags(), golden.flags(), "stream flags");

        // Exact: the group table, and every group command with its coordinate
        // indices. 2,587 of them, so a changed drawing rule lands here.
        same_section("groups", live.groups(), golden.groups());
        same_section("group commands", live.group_cmds(), golden.group_cmds());
        same_section("images", live.images(), golden.images());
        assert_eq!(
            live.view().image_pixels(0),
            golden.view().image_pixels(0),
            "embedded image pixels"
        );

        // And the picture, which is what the ordering does not change.
        same_text("painted frame", &painted(&live), &painted(&golden));
        same_text("retained bodies", &bodies(&live), &bodies(&golden));

        drop(session);
    }

    pub fn rendering_twice_produces_the_same_frame() {
        let (mut session, first) = record(&tree_root().join("demos/ecc83/ecc83-pp_v2.kicad_sch"));

        let second = session.render_owned().expect("the second frame");

        // Byte equality this time, with nothing about the environment in the way:
        // one document, one camera, two passes. This is the property the
        // renderer's group cache rests on — an unchanged item must come back with
        // the same serial and the same body, or every pan re-uploads everything.
        same_section("groups", second.groups(), first.groups());
        same_section("group commands", second.group_cmds(), first.group_cmds());
        same_coords(
            "group coordinates",
            second.group_coords(),
            first.group_coords(),
        );
        same_section("frame commands", second.frame_cmds(), first.frame_cmds());
        same_coords(
            "frame coordinates",
            second.frame_coords(),
            first.frame_coords(),
        );

        assert!(
            first == second,
            "a re-render of an unchanged document must match"
        );
    }

    /// The property the live canvas is built on: moving the camera re-records the
    /// frame and leaves the retained geometry alone.
    ///
    /// This is what makes asking the host for a frame per view change affordable.
    /// The frame body is a list of group references that `KIGFX::VIEW::Redraw`
    /// culls to the viewport, so it has to change; the group bodies those
    /// references point at are recorded once and replayed, so they must not. If
    /// they did, the renderer's `(group id, serial)` cache would miss on every pan
    /// and the whole design would collapse into re-tessellating the sheet at the
    /// display rate.
    pub fn moving_the_camera_re_records_the_frame_and_not_the_geometry() {
        let (mut session, framed) = record(&tree_root().join("demos/ecc83/ecc83-pp_v2.kicad_sch"));

        let mut viewport = session.viewport().expect("the camera reads back");

        // What the canvas does on a pan: move the centre, ask for a frame. Far
        // enough that the cull result genuinely differs — a third of the viewport.
        viewport.center_x += viewport.width_px as f64 / 3.0 / viewport.scale;
        session
            .set_viewport(&viewport)
            .expect("a panned camera is still a valid one");

        let panned = session.render_owned().expect("a frame after the pan");

        same_section("groups after a pan", panned.groups(), framed.groups());
        same_section(
            "group commands after a pan",
            panned.group_cmds(),
            framed.group_cmds(),
        );
        same_coords(
            "group coordinates after a pan",
            panned.group_coords(),
            framed.group_coords(),
        );
        assert_ne!(
            panned.frame_cmds(),
            framed.frame_cmds(),
            "panning a third of the viewport has to change what the frame draws"
        );

        // A zoom is the other half, and the interesting one: scale reaches
        // SCH_PAINTER, so this is the case where retained bodies could plausibly
        // have been re-recorded. They are not — eeschema's cached geometry is in
        // world units.
        viewport.scale *= 2.0;
        session
            .set_viewport(&viewport)
            .expect("a zoomed camera is still a valid one");

        let zoomed = session.render_owned().expect("a frame after the zoom");

        same_section("groups after a zoom", zoomed.groups(), framed.groups());
        same_section(
            "group commands after a zoom",
            zoomed.group_cmds(),
            framed.group_cmds(),
        );
        same_coords(
            "group coordinates after a zoom",
            zoomed.group_coords(),
            framed.group_coords(),
        );
        assert_ne!(
            zoomed.frame_cmds(),
            panned.frame_cmds(),
            "doubling the scale has to change what the frame draws"
        );
    }

    pub fn a_written_stream_reads_back_as_rendered() {
        let (mut session, rendered) =
            record(&tree_root().join("demos/ecc83/ecc83-pp_v2.kicad_sch"));

        let path = std::env::temp_dir().join(format!("kicad-sch-sys-{}.kgds", std::process::id()));

        session.write_stream(&path).expect("writing the stream");

        let written = Stream::read(File::open(&path).expect("the file was written"))
            .expect("and it decodes as a stream");

        let _ = std::fs::remove_file(&path);

        // The golden fixtures are produced by this path, so this is the check
        // that a fixture file is the frame it claims to be.
        assert!(
            rendered == written,
            "the serialised frame must be the one that was rendered"
        );
    }

    pub fn the_camera_reads_back_and_frames_the_page() {
        let (session, _stream) = record(&kitchen_sink());

        let viewport = session.viewport().expect("the camera reads back");

        assert_eq!(viewport.width_px, 1920);
        assert_eq!(viewport.height_px, 1080);

        // Zoom to fit frames the page rectangle, so the centre is its middle.
        let page = session.bbox(true).expect("a loaded sheet has a page box");

        assert!(page.width > 0.0 && page.height > 0.0);
        assert!((viewport.center_x - (page.x + page.width / 2.0)).abs() < 1.0);
        assert!((viewport.center_y - (page.y + page.height / 2.0)).abs() < 1.0);

        // The item box is inside the page box, and smaller: this fixture's items
        // do not fill the sheet.
        let items = session.bbox(false).expect("and an item box");

        assert!(items.width < page.width && items.height < page.height);

        // And the scale is what the header says it is: pixels per internal unit,
        // so the framed page spans the viewport. It is not `KIGFX::VIEW`'s notion
        // of a scale, which is the GAL zoom factor and differs from this by the
        // screen DPI times eeschema's world unit length — some three orders of
        // magnitude. Reporting that instead was a bug: the number came back
        // clamped to eeschema's zoom limits, the session's cull rectangle was tens
        // of metres wide, and nothing it recorded ever depended on the camera.
        let spanned = page.width.max(page.height) * viewport.scale;
        let viewport_px = f64::from(viewport.width_px.max(viewport.height_px));

        assert!(
            spanned > viewport_px * 0.5 && spanned <= viewport_px,
            "zoom to fit should span the viewport: {spanned:.1} px of {viewport_px} \
             at scale {}",
            viewport.scale
        );
    }

    pub fn a_viewport_with_no_area_is_refused() {
        let mut session = Session::new().expect("an empty session");

        let error = session
            .set_viewport(&Viewport::new(0, 1080))
            .expect_err("a zero width is outside the documented domain");

        assert!(
            matches!(
                error,
                Error::Failed {
                    status: Status::InvalidArg,
                    ..
                }
            ),
            "{error}"
        );
    }

    pub fn a_missing_file_is_an_error_with_a_message() {
        let error = Session::open(Path::new("/nonexistent/definitely/not/here.kicad_sch"))
            .expect_err("that file does not exist");

        let Error::Failed { status, message } = &error else {
            panic!("expected a failed call, got {error}");
        };

        assert_eq!(*status, Status::FileNotFound);
        assert!(
            message.contains("does not exist"),
            "the session's own message should say so: {message}"
        );
    }

    pub fn rendering_without_a_document_is_an_error() {
        let mut session = Session::new().expect("an empty session");

        assert!(!session.is_loaded());

        let error = session.render().expect_err("there is nothing to render");

        assert!(
            matches!(
                error,
                Error::Failed {
                    status: Status::NoDocument,
                    ..
                }
            ),
            "{error}"
        );
    }

    pub fn a_session_can_be_reused() {
        let root = tree_root();
        let mut session = Session::new().expect("an empty session");

        session.load_file(&kitchen_sink()).expect("the first load");

        let first = session.document_info().unwrap().item_count;

        session
            .load_file(&root.join("demos/ecc83/ecc83-pp_v2.kicad_sch"))
            .expect("the second load replaces the first");

        let second = session.document_info().unwrap();

        assert_eq!(second.current_sheet, 0);
        assert_ne!(second.item_count, first, "a different document, seriously");

        session.unload().expect("unloading is allowed");
        assert!(!session.is_loaded());

        // And unloading twice is documented as idempotent.
        session.unload().expect("unloading again is a no-op");
    }

    /// Input goes in as screen pixels and the cursor the *tools* read comes back in
    /// internal units, having gone through the view transform and the grid.
    ///
    /// This is the round trip Stage 4 exists to make, exercised against the real
    /// host rather than against a C++ test double.
    pub fn host_input_moves_the_cursor_the_tools_read() {
        let mut session = Session::open(&kitchen_sink()).expect("the fixture loads");

        session
            .set_viewport(&FIXTURE_VIEWPORT)
            .expect("a viewport the fixtures use");
        session.zoom_to_fit().expect("framing the page");

        let before = session.editor_state().expect("the editor state");

        assert!(
            !before.pointer_over_canvas,
            "nothing has reported a pointer yet"
        );
        assert_eq!(before.selection_count, 0);

        let outcome = session
            .dispatch_input(&InputEvent::PointerMotion {
                screen: (100.0, 50.0),
                modifiers: Modifiers::default(),
            })
            .expect("a pointer move is accepted");

        // No eeschema tool runs on a holder that is not a frame, so nothing claims
        // a motion event. That is the state of the seam, asserted rather than
        // described.
        assert!(!outcome.handled);

        let after = session.editor_state().expect("the editor state");

        assert!(after.pointer_over_canvas);
        assert_ne!(
            after.cursor, before.cursor,
            "the cursor followed the pointer"
        );

        // In internal units, so on the scale of a page rather than of a pixel: a
        // page is millions of internal units across, and a failure to convert would
        // leave this at 100.
        assert!(
            after.cursor.0.abs() > 1000.0 || after.cursor.1.abs() > 1000.0,
            "the cursor should be in internal units, got {:?}",
            after.cursor
        );

        session
            .dispatch_input(&InputEvent::PointerLeave)
            .expect("losing the pointer is accepted");

        let gone = session.editor_state().expect("the editor state");
        assert!(!gone.pointer_over_canvas);
    }

    /// A whole gesture, and a key, across the boundary — and the document is
    /// untouched by all of it, which is what "input reaches the framework and stops
    /// there" means concretely.
    pub fn a_click_gesture_crosses_and_edits_nothing() {
        let mut session = Session::open(&kitchen_sink()).expect("the fixture loads");

        session.set_viewport(&FIXTURE_VIEWPORT).expect("a viewport");
        session.zoom_to_fit().expect("framing the page");

        let before = session.document_info().expect("document info");

        let gesture = [
            InputEvent::PointerMotion {
                screen: (400.0, 300.0),
                modifiers: Modifiers::default(),
            },
            InputEvent::PointerDown {
                button: PointerButton::Left,
                screen: (400.0, 300.0),
                modifiers: Modifiers::default(),
            },
            InputEvent::PointerMotion {
                screen: (460.0, 300.0),
                modifiers: Modifiers::default(),
            },
            InputEvent::PointerUp {
                button: PointerButton::Left,
                screen: (460.0, 300.0),
                modifiers: Modifiers::default(),
            },
            InputEvent::Scroll {
                screen: (460.0, 300.0),
                delta: (0.0, 1.0),
                modifiers: Modifiers {
                    ctrl: true,
                    alt: true,
                    ..Modifiers::default()
                },
            },
            InputEvent::KeyDown {
                key: "w",
                modifiers: Modifiers::default(),
                auto_repeat: false,
            },
            // A name the host's table does not know is dropped rather than sent as
            // key zero, and that is not an error: a UI forwards its whole key
            // stream and some of it has no KiCad meaning.
            InputEvent::KeyDown {
                key: "no such key",
                modifiers: Modifiers::default(),
                auto_repeat: false,
            },
            InputEvent::KeyUp {
                key: "w",
                modifiers: Modifiers::default(),
            },
            InputEvent::Cancel,
        ];

        for event in &gesture {
            session
                .dispatch_input(event)
                .unwrap_or_else(|error| panic!("{event:?} was refused: {error}"));
        }

        session.reset_input().expect("resetting the input state");

        let after = session.document_info().expect("document info");

        assert_eq!(after.modified, before.modified);
        assert_eq!(after.item_count, before.item_count);

        // And the session is still usable afterwards, which is the thing a crash
        // in the dispatcher would take away.
        let frame = session.render().expect("a frame after all that input");
        assert!(!frame.groups().is_empty());
    }

    /// A UI built from the whole 440-action registry will offer plenty of actions
    /// nothing handles. That has to be a report rather than an error.
    pub fn an_unhandled_action_is_reported() {
        let mut session = Session::open(&kitchen_sink()).expect("the fixture loads");

        let missing = session
            .run_action("no.such.action")
            .expect("an unknown action is not an error");
        assert!(!missing.handled);

        let declined = session
            .run_action("eeschema.InteractiveSelection.selectionActivate")
            .expect("a real action with no tool behind it is not an error either");
        assert!(!declined.handled);
    }

    /// The main-thread rule, as a check rather than a comment.
    ///
    /// This is the failure mode the module comment is about: without the guard in
    /// `ensure_runtime` this would trip `wxASSERT( wxThread::IsMain() )` somewhere
    /// inside the load instead.
    pub fn a_second_thread_is_refused() {
        // Make sure the host is up and owned by *this* thread first.
        let _session = Session::new().expect("an empty session on the main thread");

        let error = std::thread::spawn(|| Session::new().expect_err("not this thread"))
            .join()
            .expect("the thread itself does not panic");

        assert!(matches!(error, Error::WrongThread), "{error}");
    }
}
