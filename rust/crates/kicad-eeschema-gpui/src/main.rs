// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Starts the gpui-kit schematic editor shell.
//!
//! Everything of substance lives in `kicad-sch-ui`. This binary exists so the
//! shell is runnable — and photographable — while the renderer and the C++
//! host are still being built.
//!
//! There are two ways to give it something to draw. `--stream FILE.kgds` reads a
//! frame recorded earlier by `kicad-sch-dump`, which is a fixed picture and needs
//! no C++ at all. `--schematic FILE.kicad_sch` opens the real thing through the
//! C++ host and keeps the session for the window's lifetime, re-recording the
//! frame whenever the view moves. The second needs a build with the host linked;
//! `kicad-sch-sys` says so plainly if there is none.

use std::path::{Path, PathBuf};
use std::time::Duration;

use gpui_kit::component::Root;
use gpui_kit::component::theme::ThemeMode;
use gpui_kit::{App, AppContext as _, Bounds, WindowBounds, WindowOptions, point, px, size};
use kicad_sch_render::SchematicRenderer;
use kicad_sch_sys::{Session, Stream, Viewport};
use kicad_sch_ui::document::{LiveDocument, SharedDocument, shared_document};
use kicad_sch_ui::input::ViewportState;
use kicad_sch_ui::panels::DocumentSource;
use kicad_sch_ui::shell::{self, SchematicShell};

mod host_sink;

use host_sink::{HostInputSink, SharedSession};

/// Command-line options. Hand-parsed: three flags do not justify a dependency
/// in a crate that will eventually be started by C++ rather than by a shell.
struct Options {
    /// Start in the light theme.
    light: bool,
    /// Start with the frame-time readout showing.
    frame_stats: bool,
    /// Quit after this long. Used by the headless screenshot harness, which
    /// needs the process to exit on its own.
    run_for: Option<Duration>,
    /// Window size in logical pixels.
    width: f32,
    height: f32,
    /// Open the command palette at startup. For the screenshot harness, whose
    /// compositor cannot type at the window.
    open_palette: bool,
    /// Zoom steps to apply at startup, positive in. Same reason.
    zoom_steps: i32,
    /// A recorded draw stream to open, such as one of the fixtures in
    /// `qa/data/draw_streams/`. Without one the built-in demonstration stream
    /// is shown.
    stream: Option<PathBuf>,
    /// A `.kicad_sch` to open through the C++ host, rendered live rather than
    /// read from a recording.
    schematic: Option<PathBuf>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            light: false,
            frame_stats: false,
            run_for: None,
            width: 1600.,
            height: 1000.,
            open_palette: false,
            zoom_steps: 0,
            stream: None,
            schematic: None,
        }
    }
}

fn usage() -> &'static str {
    "kicad-eeschema-gpui [--schematic FILE.kicad_sch | --stream FILE.kgds] \
     [--light] [--frame-stats] [--run-for SECONDS] [--size WIDTHxHEIGHT]"
}

fn parse_args(args: impl Iterator<Item = String>) -> Result<Options, String> {
    let mut options = Options::default();
    let mut args = args.peekable();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--light" => options.light = true,
            "--frame-stats" => options.frame_stats = true,
            "--run-for" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--run-for needs a number".to_string())?;
                let seconds: f64 = value
                    .parse()
                    .map_err(|_| format!("--run-for: {value} is not a number"))?;
                if !seconds.is_finite() || seconds <= 0. {
                    return Err("--run-for must be a positive number".into());
                }
                options.run_for = Some(Duration::from_secs_f64(seconds));
            }
            "--size" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--size needs WIDTHxHEIGHT".to_string())?;
                let (width, height) = value
                    .split_once(['x', 'X'])
                    .ok_or_else(|| format!("--size: {value} is not WIDTHxHEIGHT"))?;
                options.width = width
                    .parse()
                    .map_err(|_| format!("--size: {width} is not a number"))?;
                options.height = height
                    .parse()
                    .map_err(|_| format!("--size: {height} is not a number"))?;
            }
            "--open-palette" => options.open_palette = true,
            "--zoom-in" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--zoom-in needs a count".to_string())?;
                options.zoom_steps = value
                    .parse()
                    .map_err(|_| format!("--zoom-in: {value} is not a whole number"))?;
            }
            "--stream" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--stream needs a path".to_string())?;
                options.stream = Some(PathBuf::from(value));
            }
            "--schematic" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--schematic needs a path".to_string())?;
                options.schematic = Some(PathBuf::from(value));
            }
            "--help" | "-h" => return Err(usage().to_string()),
            other => return Err(format!("unknown argument {other}\n{}", usage())),
        }
    }

    // Both would mean two documents and one canvas. Refuse rather than pick.
    if options.stream.is_some() && options.schematic.is_some() {
        return Err("--stream and --schematic are alternatives; pass one".into());
    }

    Ok(options)
}

/// A document loaded before the window opens, so that a bad path is a message on
/// the terminal rather than an empty canvas.
struct Loaded {
    /// The first frame, which is what the window is framed around.
    stream: Stream,
    /// The session that recorded it, kept so the canvas can ask for more.
    ///
    /// `None` for a recorded stream: there is no document behind it, and the
    /// camera moving over a fixed picture is the whole of what a `.kgds` can
    /// offer.
    document: Option<SharedDocument>,
    /// The same session, for the sink to give input to.
    ///
    /// Two owners rather than one because the two directions are driven at
    /// different points of a frame: the document is asked for geometry in prepaint,
    /// and input is handed over in paint. `None` alongside a `None` document, for
    /// the same reason: a recorded stream has nothing to send input to.
    session: Option<SharedSession>,
    /// What to caption the window and panels with.
    source: DocumentSource,
}

/// A live schematic session, as the shell's canvas consumes one.
///
/// The whole of what Stage 2 needed on this side: hold the session open, point it
/// at the camera the canvas is about to paint with, and hand over the frame it
/// records. `Session::render` lends the stream out of buffers C++ owns and
/// overwrites on the next pass, and `set_stream_view` is the only thing that ever
/// sees it — the borrow cannot escape this method, which is why it is the
/// document's job to install the frame rather than to return it.
struct SchematicSession {
    session: SharedSession,
}

impl LiveDocument for SchematicSession {
    fn render(
        &mut self,
        viewport: ViewportState,
        renderer: &mut SchematicRenderer,
    ) -> Result<(), String> {
        // `KIGFX::VIEW::Redraw` culls to the viewport it is given, so this is not
        // bookkeeping: the camera the session is told about decides which groups
        // the frame body references at all.
        // Borrowed for the whole recording pass. The only other borrower is the
        // input sink, which runs in paint rather than in prepaint, so this never
        // contends — and if something ever makes it contend, the sink drops the
        // event instead of panicking inside a frame.
        let mut session = self.session.try_borrow_mut().map_err(|_| {
            "the session is busy: something is asking for a frame from inside one".to_string()
        })?;

        session
            .set_viewport(&Viewport {
                width_px: viewport.width.max(1.0) as u32,
                height_px: viewport.height.max(1.0) as u32,
                center_x: viewport.center.x,
                center_y: viewport.center.y,
                scale: viewport.scale,
            })
            .map_err(|error| format!("viewport: {error}"))?;

        // The session does not always grant what was asked for: `VIEW::SetScale`
        // clamps to eeschema's zoom limits and `VIEW::SetCenter` clamps to its pan
        // boundary, and the wx editor is bound by exactly the same ones. Adopting
        // what came back matters for more than tidiness — the session culls the
        // frame to the camera it holds, so a canvas showing a wider view than the
        // session believes in would be missing the geometry outside the session's
        // idea of the viewport. A zoom or a pan that runs into a limit therefore
        // simply stops, as it does in the wx editor.
        //
        // This converges rather than oscillating: an unclamped request comes back
        // unchanged, because both sides carry the centre and the scale as `double`
        // and nothing rounds on the way through.
        let granted = session
            .viewport()
            .map_err(|error| format!("reading the viewport back: {error}"))?;

        if granted.scale != viewport.scale {
            renderer.camera_mut().set_scale(granted.scale);
        }
        if (granted.center_x, granted.center_y) != (viewport.center.x, viewport.center.y) {
            renderer
                .camera_mut()
                .set_center([granted.center_x, granted.center_y]);
        }

        let frame = session
            .render()
            .map_err(|error| format!("recording a frame: {error}"))?;
        renderer.set_stream_view(&frame);
        Ok(())
    }
}

/// Read a recorded draw stream.
fn load_stream(path: &Path) -> Result<Loaded, String> {
    let stream = kicad_sch_ui::demo::load_stream(path)
        .map_err(|error| format!("could not read {}: {error}", path.display()))?;

    Ok(Loaded {
        stream,
        document: None,
        session: None,
        source: DocumentSource::RecordedStream {
            file: file_label(path),
        },
    })
}

/// Open a `.kicad_sch` through the C++ host, record the opening frame, and keep
/// the session for the window to draw from.
///
/// This happens before gpui starts: the host initialises wxWidgets and takes the
/// calling thread to be its main thread, which is this one, and a failure here
/// should be a line on stderr rather than a window that opens empty. gpui runs
/// the window on that same thread, so the session stays where it belongs —
/// `Session` is `!Send`, which is what makes that a compile-time fact rather than
/// a convention.
fn load_schematic(path: &Path, width: u32, height: u32) -> Result<Loaded, String> {
    let mut session = Session::open(path)
        .map_err(|error| format!("could not open {}: {error}", path.display()))?;

    // A first frame, so the window has something to frame itself around before
    // the canvas has been laid out and can ask for one of its own. Matching the
    // window size keeps the opening view close to what the first paint shows.
    session
        .set_viewport(&Viewport::new(width.max(1), height.max(1)))
        .map_err(|error| format!("viewport: {error}"))?;
    session
        .zoom_to_fit()
        .map_err(|error| format!("zoom to fit: {error}"))?;

    let stream = session
        .render_owned()
        .map_err(|error| format!("rendering {}: {error}", path.display()))?;

    let session: SharedSession = std::rc::Rc::new(std::cell::RefCell::new(session));

    Ok(Loaded {
        stream,
        document: Some(shared_document(SchematicSession {
            session: session.clone(),
        })),
        session: Some(session),
        source: DocumentSource::Schematic {
            file: file_label(path),
        },
    })
}

/// The file name to caption a window with, so a screenshot cannot misrepresent
/// which document it is showing.
fn file_label(path: &Path) -> gpui_kit::SharedString {
    path.file_name()
        .map(|name| name.to_string_lossy().into_owned())
        .unwrap_or_else(|| path.display().to_string())
        .into()
}

fn main() {
    let options = match parse_args(std::env::args().skip(1)) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(2);
        }
    };

    // The document is loaded before the window opens, so that a bad path or an
    // unreadable schematic is a clear message on the terminal.
    let loaded = match (options.schematic.as_deref(), options.stream.as_deref()) {
        (Some(path), _) => Some(load_schematic(
            path,
            options.width as u32,
            options.height as u32,
        )),
        (None, Some(path)) => Some(load_stream(path)),
        (None, None) => None,
    };

    let loaded = match loaded {
        Some(Ok(loaded)) => Some(loaded),
        Some(Err(message)) => {
            eprintln!("{message}");
            std::process::exit(1);
        }
        None => None,
    };

    gpui_kit::application()
        // The full Lucide catalogue rather than the default hundred: the tool
        // palette alone needs a dozen icons outside the default set.
        .with_assets(gpui_kit::assets::AllAssets)
        .run(move |cx: &mut App| {
            shell::init(cx);
            if options.light {
                kicad_sch_ui::theme::apply(ThemeMode::Light, None, cx);
            }
            if let Some(run_for) = options.run_for {
                // The screenshot harness has no way to close the window, so
                // the app closes itself.
                cx.spawn(async move |cx| {
                    cx.background_executor().timer(run_for).await;
                    cx.update(|cx| cx.quit());
                })
                .detach();
            }

            // The window is captioned with what it is actually showing, so a
            // screenshot cannot misrepresent itself — a live session and a replay
            // of a recorded one look the same on the canvas.
            let document = loaded.map(|loaded| {
                let mut renderer = SchematicRenderer::new();
                renderer.set_stream(loaded.stream);
                (renderer, loaded.source, loaded.document, loaded.session)
            });

            let bounds = Bounds {
                origin: point(px(0.), px(0.)),
                size: size(px(options.width), px(options.height)),
            };
            let frame_stats = options.frame_stats;
            let open_palette = options.open_palette;
            let zoom_steps = options.zoom_steps;
            cx.spawn(async move |cx| {
                let opened = cx.open_window(
                    WindowOptions {
                        window_bounds: Some(WindowBounds::Windowed(bounds)),
                        app_id: Some("org.kicad.eeschema-gpui".into()),
                        titlebar: Some(gpui_kit::TitlebarOptions {
                            title: Some("KiCad Schematic Editor".into()),
                            ..Default::default()
                        }),
                        // Never throttle while unfocused: the frame-time
                        // readout would measure the throttle, not the shell.
                        inactive_frame_interval: None,
                        ..Default::default()
                    },
                    move |window, cx| {
                        let view = cx.new(|cx| {
                            let mut shell = match document {
                                Some((renderer, source, live, session)) => {
                                    // The sink is what makes the window an input
                                    // source rather than a viewer: with a session
                                    // behind it every pointer move, click and key
                                    // press reaches the C++ tool framework. Without
                                    // one — a recorded stream — there is nothing to
                                    // send input to, and the null sink is honest.
                                    let sink = match session {
                                        Some(session) => kicad_sch_ui::input::shared_sink(
                                            HostInputSink::new(session),
                                        ),
                                        None => kicad_sch_ui::input::shared_sink(
                                            kicad_sch_ui::input::NullSink,
                                        ),
                                    };
                                    let mut shell = SchematicShell::new_with_document(
                                        std::rc::Rc::new(std::cell::RefCell::new(renderer)),
                                        source,
                                        sink,
                                        window,
                                        cx,
                                    );
                                    // After construction: the shell frames the
                                    // opening frame while being built, and this
                                    // is what makes every frame after it come
                                    // from the document instead of that copy.
                                    if let Some(live) = live {
                                        shell.set_document(live, cx);
                                    }
                                    shell
                                }
                                None => SchematicShell::new(window, cx),
                            };
                            if frame_stats {
                                shell.set_frame_stats_enabled(true);
                            }
                            if zoom_steps != 0 {
                                shell.zoom_steps(zoom_steps, cx);
                            }
                            if open_palette {
                                shell.open_palette(window, cx);
                            }
                            shell
                        });
                        cx.new(|cx| Root::new(view, window, cx))
                    },
                );
                if let Err(error) = opened {
                    eprintln!("could not open the editor window: {error}");
                    cx.update(|cx| cx.quit());
                }
            })
            .detach();
        });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> Result<Options, String> {
        parse_args(args.iter().map(|arg| arg.to_string()))
    }

    #[test]
    fn defaults_are_a_dark_window_that_stays_open() {
        let options = parse(&[]).expect("no arguments is valid");
        assert!(!options.light);
        assert!(options.run_for.is_none());
        assert!(options.width > 0. && options.height > 0.);
    }

    #[test]
    fn the_screenshot_flags_parse() {
        let options = parse(&[
            "--light",
            "--frame-stats",
            "--run-for",
            "2.5",
            "--size",
            "800x600",
            "--stream",
            "qa/data/draw_streams/ecc83_pp_v2.kgds",
            "--open-palette",
            "--zoom-in",
            "3",
        ])
        .expect("valid arguments");
        assert!(options.open_palette);
        assert_eq!(options.zoom_steps, 3);
        assert_eq!(
            options.stream.as_deref(),
            Some(std::path::Path::new(
                "qa/data/draw_streams/ecc83_pp_v2.kgds"
            ))
        );
        assert!(options.light);
        assert!(options.frame_stats);
        assert_eq!(options.run_for, Some(Duration::from_millis(2500)));
        assert_eq!(options.width, 800.);
        assert_eq!(options.height, 600.);
    }

    #[test]
    fn bad_arguments_are_rejected_rather_than_ignored() {
        assert!(parse(&["--nope"]).is_err());
        assert!(parse(&["--run-for"]).is_err());
        assert!(parse(&["--run-for", "-1"]).is_err());
        assert!(parse(&["--size", "wide"]).is_err());
        assert!(parse(&["--stream"]).is_err());
        assert!(parse(&["--schematic"]).is_err());
        assert!(parse(&["--zoom-in", "lots"]).is_err());
    }

    #[test]
    fn a_schematic_can_be_asked_for_but_not_alongside_a_stream() {
        let options = parse(&["--schematic", "demos/video/video.kicad_sch"]).expect("valid");
        assert_eq!(
            options.schematic.as_deref(),
            Some(Path::new("demos/video/video.kicad_sch"))
        );
        assert!(options.stream.is_none());

        // One canvas, so one document.
        let Err(error) = parse(&["--schematic", "a.kicad_sch", "--stream", "b.kgds"]) else {
            panic!("two documents is not a choice this can make");
        };
        assert!(error.contains("alternatives"), "{error}");
    }

    /// The whole `--schematic` path, short of the window: a real file, through
    /// the C++ host, into something the renderer will draw.
    ///
    /// Skipped in a build with no host linked, which is the default for a
    /// checkout with no CMake build behind it.
    #[test]
    fn a_schematic_loads_into_a_drawable_document() {
        if !kicad_sch_sys::is_available() {
            return;
        }

        let root = Path::new(env!("CARGO_MANIFEST_DIR"))
            .ancestors()
            .nth(3)
            .expect("the crate is three levels below the tree root");

        let loaded = load_schematic(
            &root.join("qa/data/eeschema/api_kitchen_sink.kicad_sch"),
            1920,
            1080,
        )
        .expect("the kitchen sink opens through the host");

        // The fixture's own numbers, so this fails if the seam starts recording
        // something else. qa/data/draw_streams/api_kitchen_sink.txt has them.
        assert_eq!(loaded.stream.groups().len(), 222);
        assert_eq!(loaded.stream.group_cmds().len(), 2587);
        assert!(matches!(loaded.source, DocumentSource::Schematic { .. }));

        // And the renderer makes a page-sized document out of it rather than an
        // empty one, which is what the window then frames.
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(loaded.stream);

        let bounds = renderer.document_bounds();
        assert!(
            bounds.size()[0] > 0.0 && bounds.size()[1] > 0.0,
            "expected a non-empty document, got {bounds:?}"
        );

        // Then the live half: the canvas hands the session a camera and gets the
        // frame for it. Driven here exactly as `CanvasState::refresh_document`
        // does, so that what the window relies on is what is checked.
        let document = loaded.document.expect("a schematic keeps its session");
        let groups = renderer
            .stream()
            .expect("a stream is loaded")
            .groups()
            .len();

        renderer.set_viewport([1920.0, 1080.0]);
        renderer.zoom_to_fit(24.0);

        for step in 0..3 {
            let camera = *renderer.camera();
            let state = ViewportState {
                width: camera.viewport()[0],
                height: camera.viewport()[1],
                scale: camera.scale(),
                center: kicad_sch_ui::input::WorldPoint::from_array(camera.center()),
            };

            document
                .borrow_mut()
                .render(state, &mut renderer)
                .unwrap_or_else(|error| panic!("frame {step}: {error}"));

            // Retained geometry is recorded once and replayed, so the group table
            // survives every view change. If it did not, the tessellation cache
            // would miss on every pan and live re-render would be unaffordable.
            assert_eq!(
                renderer
                    .stream()
                    .expect("a frame was installed")
                    .groups()
                    .len(),
                groups,
                "frame {step} re-recorded the retained geometry"
            );

            let frame = renderer.prepare([0.0, 0.0]);
            assert!(
                frame.stats.groups_drawn > 0,
                "frame {step} drew nothing: {:?}",
                frame.stats
            );
            assert_eq!(
                frame.stats.cache_misses,
                if step == 0 {
                    frame.stats.groups_drawn
                } else {
                    0
                },
                "frame {step} should tessellate only what it has not seen: {:?}",
                frame.stats
            );

            // Pan a quarter of the viewport for the next pass, which is enough to
            // change what the session culls to.
            renderer.pan(480.0, 0.0);
        }
    }
}
