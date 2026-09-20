// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Interaction tests for the shell.
//!
//! These drive the real widget tree through gpui's own hit testing and
//! accessibility tree — no test-only hooks, no mocked event dispatch — and
//! assert on what the user would observe: the state the shell ends up in, the
//! scene it painted, and the events the host would have received.
//!
//! Two mechanics are worth knowing when reading them:
//!
//! * `Window::dispatch_action` **defers**. An interaction and the assertion
//!   about its effect therefore cannot share one `update_window` call, which
//!   is why every helper below ends its own update and parks the executor.
//! * Only elements marked `.test_support()` and the widgets from `gpui-base`
//!   (every `Button`, for one) appear in the accessibility tree that `find`
//!   searches. gpui-component's popup menu items are not among them, so the
//!   menu test asserts on the menu opening and on the action the menu carries,
//!   rather than clicking a row that cannot be addressed.

type WorkflowCalls = Rc<RefCell<Vec<(u32, Vec<String>)>>>;

use std::cell::RefCell;
use std::rc::Rc;

use gpui_kit::component::Root;
use gpui_kit::component::dock::DockPlacement;
use gpui_kit::component::theme::ThemeMode;
use gpui_kit::test::{TestAppContextExt, TestWindowExt};
use gpui_kit::{
    AnyWindowHandle, AppContext as _, ElementId, Entity, Focusable, MenuItem, Point, ScrollDelta,
    TestAppContext, px, size,
};
use kicad_sch_render::SchematicRenderer;
use kicad_sch_ui::commands;
use kicad_sch_ui::demo::demo_stream;
use kicad_sch_ui::document::{ReplayDocument, SharedDocument};
use kicad_sch_ui::input::{PointerButton, RecordingSink, ShellEvent, shared_sink};
use kicad_sch_ui::panels::DocumentSource;
use kicad_sch_ui::shell::{self, SchematicShell};
use kicad_sch_ui::tools::Tool;

const WINDOW: gpui_kit::Size<gpui_kit::Pixels> = gpui_kit::Size {
    width: px(1440.),
    height: px(900.),
};

struct Harness {
    window: AnyWindowHandle,
    shell: Entity<SchematicShell>,
    sink: RecordingSink,
}

fn open(cx: &mut TestAppContext) -> Harness {
    open_with(cx, None)
}

/// A shell drawing from a live document, plus the document itself so a test can
/// see what it was asked for.
///
/// The `Rc<RefCell<ReplayDocument>>` coerces to the `SharedDocument` the shell
/// wants, which is how a test keeps a handle on something it has handed over.
fn open_live(cx: &mut TestAppContext) -> (Harness, Rc<RefCell<ReplayDocument>>) {
    let document = Rc::new(RefCell::new(ReplayDocument::new(demo_stream())));
    let harness = open_with(cx, Some(document.clone()));
    (harness, document)
}

fn open_with(cx: &mut TestAppContext, document: Option<SharedDocument>) -> Harness {
    cx.update(shell::init);

    let sink = RecordingSink::new();
    let shared = shared_sink(sink.clone());
    let captured: Rc<RefCell<Option<Entity<SchematicShell>>>> = Rc::new(RefCell::new(None));
    let slot = captured.clone();

    let handle = cx.open_window(size(WINDOW.width, WINDOW.height), move |window, cx| {
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(demo_stream());
        let renderer = std::rc::Rc::new(std::cell::RefCell::new(renderer));
        let view = cx.new(|cx| {
            let mut shell = SchematicShell::new_with_document(
                renderer,
                DocumentSource::Demonstration,
                shared,
                window,
                cx,
            );
            if let Some(document) = document {
                shell.set_document(document, cx);
            }
            shell
        });
        *slot.borrow_mut() = Some(view.clone());
        Root::new(view, window, cx)
    });
    let shell = captured.borrow_mut().take().expect("the shell was built");
    let harness = Harness {
        window: handle.into(),
        shell,
        sink,
    };

    cx.run_until_parked();
    frame(cx, &harness);
    harness.sink.clear();
    harness
}

fn frame(cx: &mut TestAppContext, _harness: &Harness) {
    cx.run_until_parked();
    for handle in cx.update(|cx| cx.windows()) {
        cx.update_window(handle, |_, window, cx| window.render_frame(cx))
            .expect("enumerated window is live");
    }
    cx.run_until_parked();
}

/// Resolve controls in their native window, preferring the active dialog when
/// several workflows use the same control IDs.
fn window_for(cx: &mut TestAppContext, id: impl Into<ElementId>) -> AnyWindowHandle {
    let id = id.into();
    let (mut windows, active) = cx.update(|cx| (cx.windows(), cx.active_window()));
    windows.sort_by_key(|handle| Some(*handle) != active);
    for handle in windows {
        let found = cx
            .update_window(handle, |_, window, cx| {
                window.render_frame(cx);
                window.try_find(id.clone()).is_some()
            })
            .unwrap_or(false);
        if found {
            return handle;
        }
    }
    panic!("control {id:?} was not found in any live window");
}

/// Bring a tool-palette button into view.
///
/// The palette holds more tools than fit beside the canvas, so it scrolls.
/// gpui's hit testing refuses a click on a clipped element — correctly, since
/// the user could not click it either — so a test that walks the whole palette
/// has to scroll the way a user would.
fn reveal(cx: &mut TestAppContext, harness: &Harness, id: &'static str) {
    for _ in 0..40 {
        // Fully inside, not merely intersecting: a click lands on the centre of
        // the element's bounds, and for a button hanging half out of the clip
        // that centre is somewhere else entirely.
        let shown = cx
            .update_window(harness.window, move |_, window, cx| {
                window.render_frame(cx);
                let button = window.find(id).bounds();
                let palette = window.find("tool-palette").bounds();
                button.origin.y >= palette.origin.y
                    && button.origin.y + button.size.height
                        <= palette.origin.y + palette.size.height
            })
            .expect("window is live");
        if shown {
            return;
        }
        cx.update_window(harness.window, |_, window, cx| {
            window.scroll(
                "tool-palette",
                ScrollDelta::Lines(Point { x: 0., y: -3. }),
                cx,
            );
        })
        .expect("window is live");
        cx.run_until_parked();
    }
    panic!("{id} never scrolled into view");
}

/// Click an element, then let the deferred action dispatch run and redraw.
fn click(cx: &mut TestAppContext, harness: &Harness, id: impl Into<ElementId>) {
    let id = id.into();
    frame(cx, harness);
    let target = window_for(cx, id.clone());
    cx.update_window(target, move |_, window, cx| {
        window.activate_window();
        window.click(id, cx);
    })
    .expect("window is live");
    cx.run_until_parked();
    frame(cx, harness);
}

/// Press a key, then settle.
fn press(cx: &mut TestAppContext, harness: &Harness, key: &str) {
    let key = key.to_string();
    let target = cx
        .update(|cx| {
            cx.active_window()
                .filter(|handle| cx.windows().contains(handle))
        })
        .unwrap_or(harness.window);
    cx.update_window(target, move |_, window, cx| window.press(&key, cx))
        .expect("window is live");
    cx.run_until_parked();
    frame(cx, harness);
}

/// GPUI's helper invokes the native should-close callback, but does not perform
/// the OS's subsequent removal when that callback accepts the close.
fn native_close(cx: &mut TestAppContext, handle: AnyWindowHandle) {
    let mut visual = gpui_kit::VisualTestContext::from_window(handle, cx);
    assert!(
        visual.simulate_close(),
        "unmodified native window accepts close"
    );
    cx.update_window(handle, |_, window, _| window.remove_window())
        .unwrap();
    cx.run_until_parked();
}

fn active_tool(cx: &mut TestAppContext, harness: &Harness) -> Tool {
    cx.update_window(harness.window, |_, _, cx| {
        harness.shell.read(cx).canvas().read(cx).tool()
    })
    .expect("window is live")
}

fn zoom(cx: &mut TestAppContext, harness: &Harness) -> f64 {
    cx.update_window(harness.window, |_, _, cx| {
        harness.shell.read(cx).canvas().read(cx).zoom()
    })
    .expect("window is live")
}

// ---------------------------------------------------------------------------

#[gpui_kit::test]
fn the_shell_renders_every_region(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        for id in [
            "schematic-shell",
            "toolbar",
            "tool-palette",
            "workspace",
            "canvas",
            "hierarchy-panel",
            "properties-panel",
            "status-tool",
            "status-position",
            "status-zoom",
        ] {
            let found = window.find(id);
            assert!(found.visible(), "{id} is not visible");
            assert!(found.bounds().size.width > px(0.), "{id} has no width");
        }
        if !cfg!(target_os = "macos") {
            assert!(window.find("menu-row").visible());
            assert!(window.find("menu-bar").visible());
        }
        // The canvas actually painted: the background, the grid dots and the
        // stub geometry all land in the scene.
        assert!(
            window.painted_quads().len() > 200,
            "only {} quads painted",
            window.painted_quads().len()
        );
    })
    .expect("window is live");
}

#[gpui_kit::test]
fn every_tool_button_activates_its_tool(cx: &mut TestAppContext) {
    let harness = open(cx);
    for spec in kicad_sch_ui::TOOLS {
        if matches!(
            spec.tool,
            Tool::Select | Tool::PlaceSymbol | Tool::PlacePower | Tool::PlaceImage
        ) {
            continue;
        }
        harness.sink.clear();
        reveal(cx, &harness, spec.button_id);
        click(cx, &harness, spec.button_id);
        assert_eq!(
            active_tool(cx, &harness),
            spec.tool,
            "clicking {} did not activate it",
            spec.button_id
        );
        assert_eq!(
            harness.sink.activated_tools(),
            vec![spec.id],
            "{} reported the wrong tool id",
            spec.button_id
        );
    }
}

#[gpui_kit::test]
fn symbol_buttons_require_a_live_library_instead_of_activating_empty_tools(
    cx: &mut TestAppContext,
) {
    let harness = open(cx);
    for tool in [Tool::PlaceSymbol, Tool::PlacePower] {
        let spec = kicad_sch_ui::TOOLS
            .iter()
            .find(|spec| spec.tool == tool)
            .unwrap();
        harness.sink.clear();
        reveal(cx, &harness, spec.button_id);
        click(cx, &harness, spec.button_id);
        assert_eq!(active_tool(cx, &harness), Tool::Select);
        assert!(harness.sink.activated_tools().is_empty());
        assert!(harness.sink.invoked_actions().is_empty());
        cx.update(|cx| {
            assert_eq!(
                harness.shell.read(cx).status().as_ref(),
                "Symbol libraries unavailable"
            );
        });
    }
}

#[gpui_kit::test]
fn image_button_requires_a_live_decoder_instead_of_an_empty_tool(cx: &mut TestAppContext) {
    let harness = open(cx);
    reveal(cx, &harness, "tool-image");
    click(cx, &harness, "tool-image");
    assert_eq!(active_tool(cx, &harness), Tool::Select);
    assert!(harness.sink.activated_tools().is_empty());
    assert!(harness.sink.invoked_actions().is_empty());
    cx.update(|cx| {
        assert!(
            harness
                .shell
                .read(cx)
                .status()
                .contains("requires a live schematic")
        );
    });
}

struct ImagePlacementSink {
    requests: Rc<RefCell<Vec<kicad_sch_ui::properties::ItemProperties>>>,
    events: Rc<RefCell<Vec<ShellEvent>>>,
}
impl kicad_sch_ui::InputSink for ImagePlacementSink {
    fn handle(&mut self, event: ShellEvent) {
        self.events.borrow_mut().push(event);
    }
    fn image_properties(&mut self) -> Result<kicad_sch_ui::properties::ItemProperties, String> {
        use kicad_sch_ui::properties::{ItemProperties, PropertyEntry};
        Ok(ItemProperties {
            item_id: "image-placement".into(),
            capabilities: 0,
            entries: vec![
                PropertyEntry {
                    name: "Image file".into(),
                    value: "image.png".into(),
                    kind: 0,
                    choices: vec![],
                },
                PropertyEntry {
                    name: "Scale".into(),
                    value: "1".into(),
                    kind: 3,
                    choices: vec![],
                },
                PropertyEntry {
                    name: "Placement".into(),
                    value: "Cursor".into(),
                    kind: 7,
                    choices: vec!["Cursor".into(), "Coordinates".into()],
                },
                PropertyEntry {
                    name: "Center X (mm)".into(),
                    value: "0".into(),
                    kind: 4,
                    choices: vec![],
                },
                PropertyEntry {
                    name: "Center Y (mm)".into(),
                    value: "0".into(),
                    kind: 4,
                    choices: vec![],
                },
            ],
        })
    }
    fn apply_image_properties(
        &mut self,
        data: &kicad_sch_ui::properties::ItemProperties,
    ) -> Result<(), String> {
        self.requests.borrow_mut().push(data.clone());
        Ok(())
    }
}

#[gpui_kit::test]
fn gpui_image_options_apply_to_placement_and_restore_canvas_focus(cx: &mut TestAppContext) {
    let harness = open(cx);
    let requests = Rc::new(RefCell::new(Vec::new()));
    let events = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(ImagePlacementSink {
                    requests: requests.clone(),
                    events: events.clone(),
                }));
            });
    });
    reveal(cx, &harness, "tool-image");
    click(cx, &harness, "tool-image");
    click(cx, &harness, "properties-close");
    assert!(
        requests.borrow().is_empty(),
        "closing must not create an image"
    );
    click(cx, &harness, "tool-image");
    events.borrow_mut().clear();
    press(cx, &harness, "w");
    assert_eq!(
        active_tool(cx, &harness),
        Tool::Select,
        "filename input owns typing"
    );
    click(cx, &harness, "properties-apply");
    assert_eq!(requests.borrow().len(), 1);
    assert_eq!(requests.borrow()[0].item_id, "image-placement");
    assert!(requests.borrow()[0].entries[0].value.contains('w'));
    assert_eq!(requests.borrow()[0].entries[2].value, "Cursor");
    assert!(
        !events
            .borrow()
            .iter()
            .any(|event| matches!(event, ShellEvent::ToolActivated(_))),
        "applying options uses the model service, not the wx chooser action"
    );
    press(cx, &harness, "w");
    assert_eq!(
        active_tool(cx, &harness),
        Tool::DrawWire,
        "successful apply restores canvas focus"
    );
}

/// The palette buttons have to announce which tool is active, not merely look
/// different: that is what a screen reader reads, and it is the only handle a
/// test has on the palette's appearance.
#[gpui_kit::test]
fn the_active_tool_button_announces_itself(cx: &mut TestAppContext) {
    let harness = open(cx);
    click(cx, &harness, "tool-wire");
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("tool-wire").checked(), Some(true));
        assert_eq!(window.find("tool-bus").checked(), Some(false));
        // The accessibility label is the tool's name, not its element id.
        assert_eq!(window.find("tool-wire").label(), Some("Draw Wire"));
    })
    .expect("window is live");

    click(cx, &harness, "tool-bus");
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        assert_eq!(window.find("tool-wire").checked(), Some(false));
        assert_eq!(window.find("tool-bus").checked(), Some(true));
    })
    .expect("window is live");
}

#[gpui_kit::test]
fn key_bindings_activate_tools_and_escape_cancels(cx: &mut TestAppContext) {
    let harness = open(cx);

    press(cx, &harness, "b");
    assert_eq!(active_tool(cx, &harness), Tool::DrawBus);
    assert!(
        harness
            .sink
            .any(|event| matches!(event, ShellEvent::KeyDown { key, .. } if key == "b"))
    );

    harness.sink.clear();
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);

    harness.sink.clear();
    press(cx, &harness, "escape");
    assert_eq!(active_tool(cx, &harness), Tool::Select);
    assert!(
        harness
            .sink
            .any(|event| matches!(event, ShellEvent::ToolCancelled)),
        "escape has to report the cancellation: {:?}",
        harness.sink.events()
    );
}

#[gpui_kit::test]
fn toolbar_buttons_dispatch_their_kicad_action(cx: &mut TestAppContext) {
    let harness = open(cx);
    for (button, expected) in [
        ("tb-new", "common.Control.new"),
        ("tb-open", "common.Control.open"),
        ("tb-save", "common.Control.save"),
        ("tb-undo", "common.Interactive.undo"),
        ("tb-redo", "common.Interactive.redo"),
    ] {
        harness.sink.clear();
        click(cx, &harness, button);
        assert_eq!(
            harness.sink.invoked_actions(),
            vec![expected.to_string()],
            "{button} reported the wrong action"
        );
    }
}

#[derive(Clone, Default)]
struct DocumentCalls {
    opened: Rc<RefCell<Vec<u32>>>,
    applied: WorkflowCalls,
}
struct DocumentSink {
    calls: DocumentCalls,
    recording: RecordingSink,
}
impl kicad_sch_ui::input::InputSink for DocumentSink {
    fn handle(&mut self, event: ShellEvent) {
        kicad_sch_ui::input::InputSink::handle(&mut self.recording, event);
    }
    fn document_workflow(&mut self, kind: u32) -> Result<Vec<(String, String)>, String> {
        self.calls.opened.borrow_mut().push(kind);
        assert_eq!(kind, 0);
        Ok([
            ("First number", "1"),
            ("Order", "x"),
            ("Numbering", "incremental"),
            ("Reset existing", "no"),
            ("Scope", "all"),
        ]
        .into_iter()
        .map(|(name, value)| (name.into(), value.into()))
        .collect())
    }
    fn apply_document_workflow(&mut self, kind: u32, values: &[String]) -> Result<(), String> {
        self.calls
            .applied
            .borrow_mut()
            .push((kind, values.to_vec()));
        Ok(())
    }
}
fn install_document_sink(cx: &mut TestAppContext, harness: &Harness) -> DocumentCalls {
    let calls = DocumentCalls::default();
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(DocumentSink {
                    calls: calls.clone(),
                    recording: harness.sink.clone(),
                }));
            });
    });
    calls
}

#[gpui_kit::test]
fn annotation_toolbar_edits_and_applies_gpui_fields(cx: &mut TestAppContext) {
    let harness = open(cx);
    let calls = install_document_sink(cx, &harness);
    click(cx, &harness, "tb-annotate");
    assert_eq!(*calls.opened.borrow(), vec![0]);
    assert!(harness.sink.invoked_actions().is_empty());
    press(
        cx,
        &harness,
        if cfg!(target_os = "macos") {
            "cmd-a"
        } else {
            "ctrl-a"
        },
    );
    press(cx, &harness, "4");
    press(cx, &harness, "2");
    click(cx, &harness, ("document-choice", 33usize));
    click(cx, &harness, "document-apply");
    let applied = calls.applied.borrow();
    assert_eq!(applied.len(), 1);
    assert_eq!(applied[0].0, 0);
    assert_eq!(applied[0].1, vec!["42", "y", "incremental", "no", "all"]);
    drop(applied);
    click(cx, &harness, "document-close");
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);
}

#[gpui_kit::test]
fn erc_toolbar_opens_native_panel_without_dispatching_a_wx_dialog(cx: &mut TestAppContext) {
    let harness = open(cx);
    harness.sink.clear();
    click(cx, &harness, "tb-erc");
    assert!(harness.sink.invoked_actions().is_empty());
    // This control only exists in the native ERC panel.
    click(cx, &harness, "erc-close");
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);
}

#[gpui_kit::test]
fn the_zoom_controls_move_the_camera_and_report_it(cx: &mut TestAppContext) {
    let harness = open(cx);
    let start = zoom(cx, &harness);

    click(cx, &harness, "tb-zoom-in");
    let zoomed_in = zoom(cx, &harness);
    assert!(zoomed_in > start, "{zoomed_in} should exceed {start}");
    assert!(
        harness
            .sink
            .invoked_actions()
            .contains(&"common.Control.zoomIn".to_string()),
        "{:?}",
        harness.sink.invoked_actions()
    );

    harness.sink.clear();
    click(cx, &harness, "tb-zoom-out");
    assert!(zoom(cx, &harness) < zoomed_in);

    harness.sink.clear();
    click(cx, &harness, "tb-zoom-fit");
    let fitted = zoom(cx, &harness);
    assert!(
        (fitted - start).abs() < start * 0.01,
        "fit should return to the opening view"
    );

    // The status bar carries the zoom, so it has to agree with the camera.
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        let shown = window.find("status-zoom");
        assert!(shown.visible());
    })
    .expect("window is live");
}

#[gpui_kit::test]
fn a_canvas_drag_produces_the_full_event_sequence(cx: &mut TestAppContext) {
    let harness = open(cx);
    click(cx, &harness, "tool-select");
    harness.sink.clear();

    let canvas = cx
        .update_window(harness.window, |_, window, cx| {
            window.render_frame(cx);
            window.find("canvas").bounds()
        })
        .expect("window is live");
    let from = Point {
        x: canvas.origin.x + px(120.),
        y: canvas.origin.y + px(120.),
    };
    let to = Point {
        x: canvas.origin.x + px(320.),
        y: canvas.origin.y + px(260.),
    };

    cx.update_window(harness.window, move |_, window, cx| {
        window.drag(from, to, cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    let events = harness.sink.events();
    let kinds: Vec<&str> = events
        .iter()
        .map(|event| match event {
            ShellEvent::PointerMove { .. } => "move",
            ShellEvent::PointerDown { .. } => "down",
            ShellEvent::PointerUp { .. } => "up",
            ShellEvent::DragBegin { .. } => "drag-begin",
            ShellEvent::DragUpdate { .. } => "drag-update",
            ShellEvent::DragEnd { .. } => "drag-end",
            ShellEvent::PointerLeave => "leave",
            ShellEvent::Scroll { .. } => "scroll",
            ShellEvent::ViewportChanged(_) => "viewport",
            _ => "other",
        })
        .collect();

    let down = kinds.iter().position(|kind| *kind == "down");
    let begin = kinds.iter().position(|kind| *kind == "drag-begin");
    let end = kinds.iter().position(|kind| *kind == "drag-end");
    let up = kinds.iter().position(|kind| *kind == "up");
    assert!(down.is_some(), "no pointer down in {kinds:?}");
    assert!(begin > down, "drag has to begin after the press: {kinds:?}");
    assert!(
        kinds.iter().filter(|kind| **kind == "drag-update").count() >= 2,
        "a drag across 200px should update more than once: {kinds:?}"
    );
    assert!(end > begin, "drag has to end after it began: {kinds:?}");
    assert!(up > end, "the button comes up last: {kinds:?}");

    // The world coordinates have to be consistent with the screen ones.
    let begin_event = events
        .iter()
        .find_map(|event| match event {
            ShellEvent::DragBegin {
                button,
                origin_screen,
                origin_world,
                ..
            } => Some((*button, *origin_screen, *origin_world)),
            _ => None,
        })
        .expect("a drag began");
    assert_eq!(begin_event.0, PointerButton::Left);
    assert!((begin_event.1.x - 120.).abs() < 1.0, "{:?}", begin_event.1);
    let end_event = events
        .iter()
        .find_map(|event| match event {
            ShellEvent::DragEnd { world, .. } => Some(*world),
            _ => None,
        })
        .expect("a drag ended");
    assert!(
        end_event.x > begin_event.2.x && end_event.y > begin_event.2.y,
        "dragging right and down has to move the world point the same way: {begin_event:?} -> {end_event:?}"
    );
}

#[gpui_kit::test]
fn a_click_below_the_threshold_is_not_a_drag(cx: &mut TestAppContext) {
    let harness = open(cx);
    harness.sink.clear();
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        window.click("canvas", cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    assert!(
        harness
            .sink
            .any(|event| matches!(event, ShellEvent::PointerDown { .. })),
        "the press should still be reported"
    );
    assert!(
        !harness
            .sink
            .any(|event| matches!(event, ShellEvent::DragBegin { .. })),
        "a stationary click is not a drag: {:?}",
        harness.sink.events()
    );
}

#[gpui_kit::test]
fn scrolling_the_canvas_zooms_about_the_pointer(cx: &mut TestAppContext) {
    let harness = open(cx);
    let before = zoom(cx, &harness);
    harness.sink.clear();

    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        window.scroll("canvas", ScrollDelta::Lines(Point { x: 0., y: 2. }), cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    let after = zoom(cx, &harness);
    assert!(
        after > before,
        "scrolling up should zoom in: {before} -> {after}"
    );
    assert!(
        harness
            .sink
            .any(|event| matches!(event, ShellEvent::Scroll { .. })),
        "the wheel has to reach the host too"
    );
}

#[gpui_kit::test]
fn the_status_bar_follows_the_pointer(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        // Nothing hovered yet: the readout says so rather than lying about 0,0.
        assert!(window.find("status-position").visible());
        window.hover("canvas", cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    let position = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).canvas().read(cx).cursor_world()
        })
        .expect("window is live");
    let position = position.expect("hovering the canvas gives a cursor position");
    // The reported position is in internal units and lands on the 50 mil grid,
    // because that is the position an edit would actually use.
    let spacing = 50.0 * kicad_sch_ui::grid::IU_PER_MIL;
    assert!(
        (position.x / spacing - (position.x / spacing).round()).abs() < 1e-6,
        "{position:?} is not on the grid"
    );
    // A fitted A4 sheet puts the pointer somewhere in the middle of it, which
    // is of the order of a million internal units.
    assert!(
        position.x.abs() > 1.0e5,
        "{position:?} looks like millimetres rather than internal units"
    );
}

#[gpui_kit::test]
fn the_left_dock_collapses_and_comes_back(cx: &mut TestAppContext) {
    let harness = open(cx);
    let open_before = cx
        .update_window(harness.window, |_, _, cx| {
            harness
                .shell
                .read(cx)
                .dock()
                .read(cx)
                .is_dock_open(DockPlacement::Left)
        })
        .expect("window is live");
    assert!(open_before);

    press(cx, &harness, "ctrl-b");
    let collapsed = cx
        .update_window(harness.window, |_, _, cx| {
            harness
                .shell
                .read(cx)
                .dock()
                .read(cx)
                .is_dock_open(DockPlacement::Left)
        })
        .expect("window is live");
    assert!(!collapsed, "ctrl-b should collapse the hierarchy dock");

    press(cx, &harness, "ctrl-b");
    let reopened = cx
        .update_window(harness.window, |_, _, cx| {
            harness
                .shell
                .read(cx)
                .dock()
                .read(cx)
                .is_dock_open(DockPlacement::Left)
        })
        .expect("window is live");
    assert!(reopened);
}

#[gpui_kit::test]
fn dragging_the_dock_handle_resizes_the_panel(cx: &mut TestAppContext) {
    let harness = open(cx);
    // The dock's resize handle is drawn by gpui-base and is not registered in
    // the accessibility tree, so its position is derived: it is the last few
    // pixels of the dock, and the dock starts where the tool palette ends.
    let (palette, before) = cx
        .update_window(harness.window, |_, window, cx| {
            window.render_frame(cx);
            (
                window.find("tool-palette").bounds(),
                harness
                    .shell
                    .read(cx)
                    .dock()
                    .read(cx)
                    .dock_size(DockPlacement::Left),
            )
        })
        .expect("window is live");
    let before = before.expect("the left dock has a size");

    let from = Point {
        x: palette.origin.x + palette.size.width + before - px(1.5),
        y: palette.center().y,
    };
    let to = Point {
        x: from.x + px(90.),
        y: from.y,
    };
    cx.update_window(harness.window, move |_, window, cx| {
        window.drag(from, to, cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    let after = cx
        .update_window(harness.window, |_, _, cx| {
            harness
                .shell
                .read(cx)
                .dock()
                .read(cx)
                .dock_size(DockPlacement::Left)
        })
        .expect("window is live")
        .expect("the left dock still has a size");
    assert!(
        after > before + px(40.),
        "dragging the handle right should widen the dock: {before:?} -> {after:?}"
    );
}

/// The hierarchy panel drives the properties panel. Until the C++ document
/// model is connected the tree summarises the loaded draw stream, so the row
/// clicked here is one of those rows rather than a symbol — which is the point:
/// the panels show what the shell can actually know.
#[gpui_kit::test]
fn picking_a_row_in_the_hierarchy_changes_the_properties_panel(cx: &mut TestAppContext) {
    let harness = open(cx);
    let (connected, before) = cx
        .update_window(harness.window, |_, _, cx| {
            let design = harness.shell.read(cx).design().read(cx);
            (design.is_connected(), design.selected_label().to_string())
        })
        .expect("window is live");
    assert!(
        !connected,
        "the shell must not claim a document model it has no link to"
    );
    assert_eq!(before, "demonstration stream");

    click(cx, &harness, "stream-groups");

    let after = cx
        .update_window(harness.window, |_, _, cx| {
            harness
                .shell
                .read(cx)
                .design()
                .read(cx)
                .selected_label()
                .to_string()
        })
        .expect("window is live");
    assert!(
        after.contains("retained groups"),
        "the properties panel should follow the tree: {after}"
    );
}

/// Nothing in the panels may be invented while the document model is absent.
#[gpui_kit::test]
fn the_panels_say_they_are_not_connected_to_a_document_model(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(window.find("hierarchy-note").visible());
        assert!(window.find("properties-note").visible());
        if !cfg!(target_os = "macos") {
            assert!(window.find("document-name").visible());
        }
    })
    .expect("window is live");
}

#[cfg(not(target_os = "macos"))]
#[gpui_kit::test]
fn opening_a_menu_marks_it_and_draws_a_popup(cx: &mut TestAppContext) {
    let harness = open(cx);
    let quads_before = cx
        .update_window(harness.window, |_, window, cx| {
            window.render_frame(cx);
            window.painted_quads().len()
        })
        .expect("window is live");

    cx.update_window(harness.window, |_, window, cx| {
        let mut bar = window.within("menu-bar");
        let mut file = bar.within(0_usize);
        file.click("menu", cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        // The popup is drawn by gpui-component and is not in the
        // accessibility tree, so the evidence that it opened is the scene: a
        // popover panel, its rows and its keybinding chips are all new quads.
        assert!(
            window.painted_quads().len() > quads_before + 5,
            "an open menu has to add to the scene: {} -> {}",
            quads_before,
            window.painted_quads().len()
        );
    })
    .expect("window is live");

    // Leave the menu closed. gpui's test harness fails a test that exits with
    // a live entity handle, and an open PopupMenu is one. Clicking away is how
    // a user closes it, and it is what the popup's mouse-down-out handler
    // listens for.
    click(cx, &harness, "status-message");
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.painted_quads().len() <= quads_before + 5,
            "the menu should have closed again"
        );
    })
    .expect("window is live");
}

#[gpui_kit::test]
fn the_file_menu_carries_the_actions_it_should(cx: &mut TestAppContext) {
    let harness = open(cx);
    let menus = commands::app_menus();
    let file = menus.first().expect("there is a File menu");
    assert_eq!(file.name.as_ref(), "File");

    let first = file
        .items
        .iter()
        .find_map(|item| match item {
            MenuItem::Action { name, action, .. } if name.as_ref() == "New Schematic" => {
                Some(action.boxed_clone())
            }
            _ => None,
        })
        .expect("File starts with New Schematic");

    cx.update_window(harness.window, move |_, window, cx| {
        window.dispatch_action(first, cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    assert_eq!(
        harness.sink.invoked_actions(),
        vec!["common.Control.new".to_string()],
        "the menu item has to invoke KiCad's own action name"
    );
}

/// A secondary click still opens a menu and reaches the host.
#[gpui_kit::test]
fn a_right_click_on_the_canvas_reaches_the_host(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        window.right_click("canvas", cx);
    })
    .expect("window is live");

    assert!(
        harness.sink.any(|event| matches!(
            event,
            ShellEvent::PointerDown {
                button: PointerButton::Right,
                ..
            }
        )),
        "the host hears the right click: {:?}",
        harness.sink.events()
    );
    cx.run_until_parked();
    frame(cx, &harness);
    press(cx, &harness, "escape");
}

#[gpui_kit::test]
async fn the_command_palette_filters_and_runs_a_command(cx: &mut TestAppContext) {
    let harness = open(cx);
    let calls = install_document_sink(cx, &harness);

    cx.update_window(harness.window, |_, window, cx| {
        window.press("ctrl-shift-p", cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    cx.wait_for(
        harness.window,
        std::time::Duration::from_secs(2),
        |window, _| window.try_find("command-palette").is_some(),
    )
    .await;

    let opened = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).is_palette_open()
        })
        .expect("window is live");
    assert!(opened);

    harness.sink.clear();
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        window.input("Annotate", cx);
    })
    .expect("window is live");
    cx.run_until_parked();
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        let state = harness.shell.read(cx).command_state().clone();
        let state = state.read(cx);
        eprintln!(
            "DEBUG query={:?} matched={} selected={:?}",
            state.query(cx),
            state.matched_count(),
            state.selected_index()
        );
    })
    .expect("live");

    cx.update_window(harness.window, |_, window, cx| {
        window.press("enter", cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    assert_eq!(*calls.opened.borrow(), vec![0]);
    assert!(
        harness.sink.invoked_actions().is_empty(),
        "annotation must not dispatch a wx dialog action"
    );
    frame(cx, &harness);
    let dialog = window_for(cx, "document-apply");
    assert_ne!(
        dialog, harness.window,
        "annotation has its own native window"
    );
    cx.update_window(dialog, |_, window, _| {
        assert!(window.try_find("document-dialog").is_some())
    })
    .unwrap();
    click(cx, &harness, "document-apply");
    assert_eq!(calls.applied.borrow()[0].1[0], "1");
}

#[gpui_kit::test]
fn escape_closes_the_palette_before_it_cancels_the_tool(cx: &mut TestAppContext) {
    let harness = open(cx);
    click(cx, &harness, "tool-wire");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);

    click(cx, &harness, "tb-palette");
    let opened = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).is_palette_open()
        })
        .expect("window is live");
    assert!(opened, "the toolbar button opens the palette");

    press(cx, &harness, "escape");
    let (still_open, tool) = cx
        .update_window(harness.window, |_, _, cx| {
            let shell = harness.shell.read(cx);
            (shell.is_palette_open(), shell.canvas().read(cx).tool())
        })
        .expect("window is live");
    assert!(!still_open, "escape closes the palette");
    assert_eq!(
        tool,
        Tool::DrawWire,
        "and leaves the tool alone while it is doing so"
    );
}

#[gpui_kit::test]
fn the_grid_and_unit_controls_change_what_the_status_bar_shows(cx: &mut TestAppContext) {
    let harness = open(cx);
    let grid_before = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).canvas().read(cx).grid().size().label
        })
        .expect("window is live");

    click(cx, &harness, "tb-grid");
    let grid_after = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).canvas().read(cx).grid().size().label
        })
        .expect("window is live");
    assert_ne!(grid_before, grid_after);

    click(cx, &harness, "tb-units");
    let units = cx
        .update_window(harness.window, |_, _, cx| harness.shell.read(cx).units())
        .expect("window is live");
    assert_eq!(units.suffix(), "mil");
}

#[gpui_kit::test]
fn the_theme_toggle_switches_the_canvas_palette_too(cx: &mut TestAppContext) {
    let harness = open(cx);
    let dark = cx
        .update_window(harness.window, |_, _, cx| {
            let shell = harness.shell.read(cx);
            (
                shell.theme_mode(),
                shell.canvas().read(cx).palette().background,
            )
        })
        .expect("window is live");
    assert_eq!(dark.0, ThemeMode::Dark);

    click(cx, &harness, "tb-theme");
    let light = cx
        .update_window(harness.window, |_, _, cx| {
            let shell = harness.shell.read(cx);
            (
                shell.theme_mode(),
                shell.canvas().read(cx).palette().background,
            )
        })
        .expect("window is live");
    assert_eq!(light.0, ThemeMode::Light);
    assert!(
        light.1.l > dark.1.l + 0.5,
        "the light canvas has to be lighter: {:?} vs {:?}",
        light.1,
        dark.1
    );
}

#[gpui_kit::test]
fn the_frame_time_readout_appears_once_it_is_switched_on(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        assert!(
            window.try_find("status-frame-time").is_none(),
            "the readout is off by default"
        );
    })
    .expect("window is live");

    press(cx, &harness, "ctrl-shift-f");
    // Two more frames so the window has at least one interval to average.
    frame(cx, &harness);
    frame(cx, &harness);

    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        let readout = window.find("status-frame-time");
        assert!(readout.visible(), "the readout should be on screen");
    })
    .expect("window is live");

    let counted = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).stats().frame_count()
        })
        .expect("window is live");
    assert!(counted > 3, "the shell should have counted its frames");
}

#[gpui_kit::test]
fn the_shell_reports_its_viewport_when_the_camera_moves(cx: &mut TestAppContext) {
    let harness = open(cx);
    harness.sink.clear();
    click(cx, &harness, "tb-zoom-in");

    let reported = harness
        .sink
        .events()
        .into_iter()
        .find_map(|event| match event {
            ShellEvent::ViewportChanged(state) => Some(state),
            _ => None,
        })
        .expect("zooming has to report a new viewport");
    assert!(reported.width > 100., "{reported:?}");
    assert!(reported.scale > 0., "{reported:?}");
}

// --- live re-render --------------------------------------------------------
//
// The property these are about: a canvas over a live document draws the frame
// its camera is about to paint, asks for one exactly when the answer could have
// changed, and does not throw away tessellated geometry when it gets one.

/// How many frames the live document has been asked for.
fn renders(cx: &mut TestAppContext, harness: &Harness) -> u64 {
    cx.update_window(harness.window, |_, _, cx| {
        harness.shell.read(cx).canvas().read(cx).document_renders()
    })
    .expect("window is live")
}

fn cache(cx: &mut TestAppContext, harness: &Harness) -> kicad_sch_render::CacheStats {
    cx.update_window(harness.window, |_, _, cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .read(cx)
            .renderer()
            .borrow()
            .cache_stats()
    })
    .expect("window is live")
}

#[gpui_kit::test]
fn opening_asks_the_document_for_one_frame_and_then_stops(cx: &mut TestAppContext) {
    let (harness, _document) = open_live(cx);

    // One request, for the camera the opening fit settled on — not one per
    // layout pass on the way there.
    assert_eq!(renders(cx, &harness), 1);

    // The window free-runs, so the redraws keep coming. Nothing touched the
    // view, so nothing should be re-recorded: this is the measurement behind
    // "a redraw of an unchanged document costs nothing".
    for _ in 0..8 {
        frame(cx, &harness);
    }
    assert_eq!(
        renders(cx, &harness),
        1,
        "a free-running redraw of an untouched view re-recorded the frame"
    );
}

#[gpui_kit::test]
fn panning_and_zooming_re_record_from_the_document(cx: &mut TestAppContext) {
    let (harness, document) = open_live(cx);
    let opening = document
        .borrow()
        .last_viewport()
        .expect("the opening frame was asked for");

    click(cx, &harness, "tb-zoom-in");
    assert_eq!(renders(cx, &harness), 2, "zooming has to re-record");

    let zoomed = document.borrow().last_viewport().expect("a second frame");
    assert!(
        zoomed.scale > opening.scale,
        "the document was told the old scale: {opening:?} -> {zoomed:?}"
    );

    // And the camera the document was told about is the camera the canvas
    // painted with. If these drift, the document culls to one view and the
    // renderer projects for another, which shows as geometry missing at the
    // edges.
    let painted = cx
        .update_window(harness.window, |_, _, cx| {
            harness.shell.read(cx).canvas().read(cx).camera()
        })
        .expect("window is live");
    assert_eq!(zoomed.scale, painted.scale());
    assert_eq!(zoomed.center.to_array(), painted.center());
    assert_eq!([zoomed.width, zoomed.height], painted.viewport());

    // Panning is the other half of the criterion. The test helpers only drag
    // with the left button, so this drives the same `CanvasState::pan` the
    // middle-button drag handler calls rather than a parallel path.
    let before = document.borrow().renders();
    cx.update_window(harness.window, |_, _, cx| {
        let canvas = harness.shell.read(cx).canvas().clone();
        canvas.update(cx, |canvas, cx| {
            canvas.pan(120.0, 60.0);
            cx.notify();
        });
    })
    .expect("window is live");
    frame(cx, &harness);

    let panned = document.borrow().last_viewport().expect("a panned frame");
    assert!(
        document.borrow().renders() > before,
        "panning has to re-record"
    );
    assert_ne!(
        panned.center.to_array(),
        zoomed.center.to_array(),
        "the pan did not reach the document"
    );
    assert_eq!(
        panned.scale, zoomed.scale,
        "a pan is not a zoom: {zoomed:?} -> {panned:?}"
    );
}

/// The load-bearing half of Stage 2: re-recording an unchanged document must not
/// cost the renderer its tessellation. Retained geometry is keyed on
/// `(group id, serial)` on both sides of the boundary, so a stream that arrives
/// again with the same groups re-uses every cached one.
///
/// The view is deliberately left where it is. Cached tessellation is also keyed
/// by level of detail, so a *zoom* legitimately re-tessellates once per LOD
/// bucket it passes through — adding one here would make this assertion fail for
/// a reason that is not a bug.
#[gpui_kit::test]
fn re_recording_unchanged_geometry_re_uploads_nothing(cx: &mut TestAppContext) {
    let (harness, document) = open_live(cx);
    let warm = cache(cx, &harness);
    assert!(warm.misses > 0, "the opening frame has to tessellate");
    let recorded = document.borrow().renders();

    // Three more frames from the document, all at the same camera.
    for _ in 0..3 {
        cx.update_window(harness.window, |_, _, cx| {
            let canvas = harness.shell.read(cx).canvas().clone();
            canvas.update(cx, |canvas, cx| {
                canvas.mark_document_dirty();
                cx.notify();
            });
        })
        .expect("window is live");
        frame(cx, &harness);
    }
    assert_eq!(document.borrow().renders(), recorded + 3);

    let after = cache(cx, &harness);
    assert!(after.hits > warm.hits, "the cache was never consulted");
    assert_eq!(
        after.misses,
        warm.misses,
        "re-recording identical geometry re-tessellated {} groups",
        after.misses - warm.misses
    );
    assert_eq!(
        after.evictions, warm.evictions,
        "and it should not have evicted anything either"
    );
}

/// A host change the canvas cannot see — an edit, an undo, a sheet switch — is
/// what `mark_document_dirty` is for. Nothing produces one yet; the path is
/// tested so that Stage 4 has somewhere to plug in.
#[gpui_kit::test]
fn a_document_change_the_canvas_cannot_see_still_re_records(cx: &mut TestAppContext) {
    let (harness, _document) = open_live(cx);
    let before = renders(cx, &harness);

    cx.update_window(harness.window, |_, _, cx| {
        let canvas = harness.shell.read(cx).canvas().clone();
        canvas.update(cx, |canvas, cx| {
            canvas.mark_document_dirty();
            cx.notify();
        });
    })
    .expect("window is live");
    frame(cx, &harness);

    assert_eq!(renders(cx, &harness), before + 1);
}

/// A canvas that silently keeps showing the last frame it managed to record is
/// indistinguishable from one that is working, so the failure has to be visible.
#[gpui_kit::test]
fn a_document_that_cannot_record_says_so_in_the_status_bar(cx: &mut TestAppContext) {
    let document = Rc::new(RefCell::new(ReplayDocument::failing("no document loaded")));
    let harness = open_with(cx, Some(document.clone()));

    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        let shown = window.find("status-document-error");
        assert!(shown.visible(), "the failure has to be on screen");
        assert_eq!(
            harness
                .shell
                .read(cx)
                .canvas()
                .read(cx)
                .document_error()
                .map(|reason| reason.to_string()),
            Some("no document loaded".to_string())
        );
    })
    .expect("window is live");

    // And it does not retry every frame, which would turn a broken session into
    // a busy loop.
    let asked = document.borrow().renders();
    for _ in 0..4 {
        frame(cx, &harness);
    }
    assert_eq!(document.borrow().renders(), asked);
}

/// A canvas over a recorded stream has no document to ask, and must not behave
/// as though it had one.
#[gpui_kit::test]
fn a_canvas_over_a_fixed_stream_asks_for_nothing(cx: &mut TestAppContext) {
    let harness = open(cx);
    click(cx, &harness, "tb-zoom-in");
    for _ in 0..4 {
        frame(cx, &harness);
    }
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        let canvas = harness.shell.read(cx).canvas().read(cx);
        assert!(!canvas.has_live_document());
        assert_eq!(canvas.document_renders(), 0);
        assert!(canvas.document_error().is_none());
        assert!(window.try_find("status-document-error").is_none());
    })
    .expect("window is live");
}

/// A sink that reports what a host with a live tool would: it claimed the event,
/// something changed, and here is how big the selection now is.
///
/// Nothing in the shipping path can report this yet, because no eeschema tool runs
/// on a holder that is not a frame — so the two properties below are asserted
/// against a stand-in rather than left untested until one does.
#[derive(Clone, Default)]
struct BusyHostSink {
    dirty: Rc<std::cell::Cell<bool>>,
    selection: usize,
}

impl kicad_sch_ui::input::InputSink for BusyHostSink {
    fn handle(&mut self, _event: ShellEvent) {
        self.dirty.set(true);
    }

    fn take_dirty(&mut self) -> bool {
        self.dirty.replace(false)
    }

    fn selection_count(&self) -> usize {
        self.selection
    }
}

/// The arrow that makes an edit visible.
///
/// A tool that moves an item or changes the selection has changed what the next
/// frame looks like, and the canvas cannot see any of it: it re-records when *it*
/// moves the camera, and an edit is not that. So the sink is asked after every
/// event, and a host that says something changed gets a fresh frame.
#[gpui_kit::test]
fn a_host_that_changed_something_gets_a_fresh_frame(cx: &mut TestAppContext) {
    let (harness, document) = open_live(cx);

    let sink = BusyHostSink {
        dirty: Rc::new(std::cell::Cell::new(false)),
        selection: 4,
    };

    cx.update_window(harness.window, |_, _, cx| {
        let canvas = harness.shell.read(cx).canvas().clone();
        canvas.update(cx, |canvas, _| canvas.set_sink(shared_sink(sink.clone())));
    })
    .expect("window is live");

    let before = renders(cx, &harness);
    let asked = document.borrow().renders();

    // A tool activation is the cheapest event to post; what is under test is the
    // sink's answer to it, not the event itself.
    cx.update_window(harness.window, |_, _, cx| {
        let canvas = harness.shell.read(cx).canvas().clone();
        canvas.update(cx, |canvas, cx| {
            canvas.emit(ShellEvent::ToolCancelled);
            cx.notify();
        });
    })
    .expect("window is live");
    frame(cx, &harness);

    assert_eq!(renders(cx, &harness), before + 1);
    assert_eq!(document.borrow().renders(), asked + 1);

    // ...and only once. The flag is consumed, so one edit does not re-record for
    // ever, which would turn every click into a permanent recording loop.
    frame(cx, &harness);
    assert_eq!(renders(cx, &harness), before + 1);
}

/// The selection belongs to the C++ selection tool, so the shell has none of its
/// own to count and the status bar reads the host's answer through the sink.
#[gpui_kit::test]
fn the_selection_count_comes_from_the_host(cx: &mut TestAppContext) {
    let harness = open(cx);

    cx.update_window(harness.window, |_, _, cx| {
        let canvas = harness.shell.read(cx).canvas().clone();
        // The default sink is the recorder, which has no host and says zero.
        assert_eq!(canvas.read(cx).selection_count(), 0);

        canvas.update(cx, |canvas, _| {
            canvas.set_sink(shared_sink(BusyHostSink {
                dirty: Rc::new(std::cell::Cell::new(false)),
                selection: 9,
            }))
        });
        assert_eq!(canvas.read(cx).selection_count(), 9);
    })
    .expect("window is live");
}

/// Two buttons held at once, released in the order they were pressed.
///
/// The shell tracks one press at a time — which is all its own selection band needs
/// — so the second press overwrites the first, and the release of the *first* finds
/// no matching record. It has to be reported anyway. A host that keeps per-button
/// state would otherwise believe that button is still held for the rest of the
/// session and turn every later pointer move into a drag from a stale origin, which
/// is exactly the failure `HOST_TOOL_DISPATCHER` gave up wx's mouse-state poll on
/// the promise that "the host delivers every up".
#[gpui_kit::test]
fn every_press_gets_a_release_even_with_two_buttons_held(cx: &mut TestAppContext) {
    let harness = open(cx);

    let canvas = cx
        .update_window(harness.window, |_, window, cx| {
            window.render_frame(cx);
            window.find("canvas").bounds()
        })
        .expect("window is live");
    let at = Point {
        x: canvas.origin.x + px(150.),
        y: canvas.origin.y + px(150.),
    };

    harness.sink.clear();

    cx.update_window(harness.window, move |_, window, cx| {
        // Left and Middle rather than Left and Right: a right click opens
        // gpui-component's context menu, whose retained `PopupMenu` trips gpui's leak
        // detector — the reason the right-click test in this file is `#[ignore]`d. The
        // property under test is about two buttons, not about which two.
        for button in [gpui_kit::MouseButton::Left, gpui_kit::MouseButton::Middle] {
            window.dispatch_event(
                gpui_kit::PlatformInput::MouseDown(gpui_kit::MouseDownEvent {
                    button,
                    position: at,
                    modifiers: Default::default(),
                    click_count: 1,
                    first_mouse: false,
                }),
                cx,
            );
        }
        // Released first-pressed-first, which is the order that loses the record.
        for button in [gpui_kit::MouseButton::Left, gpui_kit::MouseButton::Middle] {
            window.dispatch_event(
                gpui_kit::PlatformInput::MouseUp(gpui_kit::MouseUpEvent {
                    button,
                    position: at,
                    modifiers: Default::default(),
                    click_count: 1,
                }),
                cx,
            );
        }
        window.render_frame(cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    let mut downs = Vec::new();
    let mut ups = Vec::new();

    for event in harness.sink.events() {
        match event {
            ShellEvent::PointerDown { button, .. } => downs.push(button),
            ShellEvent::PointerUp { button, .. } => ups.push(button),
            _ => {}
        }
    }

    downs.sort_by_key(|button| format!("{button:?}"));
    ups.sort_by_key(|button| format!("{button:?}"));

    assert_eq!(downs, vec![PointerButton::Left, PointerButton::Middle]);
    assert_eq!(
        ups, downs,
        "every button that went down has to come up: {ups:?}"
    );
}

/// Do not give the test a focus handle that a real user never receives.
#[gpui_kit::test]
fn canvas_receives_unbound_host_keys_at_startup_and_after_palette(cx: &mut TestAppContext) {
    let harness = open(cx);
    press(cx, &harness, "r");
    assert!(harness.sink.events().iter().any(|event| matches!(
        event, ShellEvent::KeyDown { key, .. } if key == "r"
    )));
    press(cx, &harness, "ctrl-shift-p");
    press(cx, &harness, "escape");
    harness.sink.clear();
    press(cx, &harness, "r");
    assert!(harness.sink.events().iter().any(|event| matches!(
        event, ShellEvent::KeyDown { key, .. } if key == "r"
    )));
}

#[gpui_kit::test]
fn clicking_canvas_restores_host_keyboard_input(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, cx| {
        harness.shell.read(cx).focus_handle(cx).focus(window, cx);
    })
    .expect("window is live");
    click(cx, &harness, "canvas");
    harness.sink.clear();
    press(cx, &harness, "r");
    assert!(harness.sink.events().iter().any(|event| matches!(
        event, ShellEvent::KeyDown { key, .. } if key == "r"
    )));
}

#[cfg(target_os = "macos")]
#[gpui_kit::test]
fn mac_command_save_and_redo_reach_the_host(cx: &mut TestAppContext) {
    let harness = open(cx);
    press(cx, &harness, "cmd-s");
    press(cx, &harness, "cmd-shift-z");
    for expected in ["common.Control.save", "common.Interactive.redo"] {
        assert!(harness.sink.events().iter().any(|event| matches!(
            event, ShellEvent::ActionInvoked(id) if id.as_str() == expected
        )));
    }
}

#[gpui_kit::test]
fn named_host_edits_repaint_the_cached_canvas_panel(cx: &mut TestAppContext) {
    let (harness, _document) = open_live(cx);
    cx.update_window(harness.window, |_, _, cx| {
        let canvas = harness.shell.read(cx).canvas().clone();
        canvas.update(cx, |canvas, _| {
            canvas.set_sink(shared_sink(BusyHostSink::default()))
        });
    })
    .expect("window is live");
    let before = renders(cx, &harness);
    press(cx, &harness, "ctrl-z");
    assert!(
        renders(cx, &harness) > before,
        "undo must repaint without a mouse move"
    );
}

#[gpui_kit::test]
fn secondary_drag_pans_without_opening_a_menu(cx: &mut TestAppContext) {
    let (harness, document) = open_live(cx);
    let before = document.borrow().last_viewport().unwrap();
    let start = cx
        .update_window(harness.window, |_, window, cx| {
            window.render_frame(cx);
            window.find("canvas").bounds().center()
        })
        .unwrap();
    let end = start + gpui_kit::point(px(100.), px(60.));
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_event(
            gpui_kit::PlatformInput::MouseDown(gpui_kit::MouseDownEvent {
                button: gpui_kit::MouseButton::Right,
                position: start,
                modifiers: Default::default(),
                click_count: 1,
                first_mouse: false,
            }),
            cx,
        );
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    // A menu on mouse-down would steal the following motion.
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_event(
            gpui_kit::PlatformInput::MouseMove(gpui_kit::MouseMoveEvent {
                position: end,
                pressed_button: Some(gpui_kit::MouseButton::Right),
                modifiers: Default::default(),
            }),
            cx,
        );
        window.render_frame(cx);
        window.dispatch_event(
            gpui_kit::PlatformInput::MouseUp(gpui_kit::MouseUpEvent {
                button: gpui_kit::MouseButton::Right,
                position: end,
                modifiers: Default::default(),
                click_count: 1,
            }),
            cx,
        );
        window.render_frame(cx);
    })
    .unwrap();
    cx.run_until_parked();
    frame(cx, &harness);
    let after = document.borrow().last_viewport().unwrap();
    assert_eq!(after.scale, before.scale);
    assert!((after.center.x - (before.center.x - 100. / before.scale)).abs() < 0.01);
    assert!((after.center.y - (before.center.y - 60. / before.scale)).abs() < 0.01);
    // No menu stole focus: an ordinary canvas hotkey still works on release.
    harness.sink.clear();
    press(cx, &harness, "r");
    assert!(
        harness
            .sink
            .any(|event| matches!(event, ShellEvent::KeyDown { key, .. } if key == "r"))
    );
}

/// The live host's metadata reaches the rendered chrome, and missing registry
/// actions cannot dispatch even though their disabled buttons retain layout.
#[gpui_kit::test]
fn live_registry_controls_toolbar_and_tool_palette(cx: &mut TestAppContext) {
    let registry = commands::ActionRegistry::new(vec![
        commands::ActionInfo {
            name: "common.Control.save".into(),
            label: "Save from host".into(),
            description: "Host save help".into(),
            hotkey: if cfg!(target_os = "macos") {
                "Cmd+S"
            } else {
                "Ctrl+S"
            }
            .into(),
            ..Default::default()
        },
        commands::ActionInfo {
            name: Tool::DrawWire.id().as_str().into(),
            label: "Wire from host".into(),
            description: "Host wire help".into(),
            hotkey: "W".into(),
            ..Default::default()
        },
    ]);
    cx.update(|cx| cx.set_global(registry));
    let harness = open(cx);
    cx.update_window(harness.window, |_, window, _| {
        assert_eq!(window.find("tb-save").label(), Some("Save from host"));
        assert_eq!(window.find("tool-wire").label(), Some("Wire from host"));
        let wire =
            window.bindings_for_action(&commands::RunAction::new(Tool::DrawWire.id().as_str()));
        assert_eq!(
            wire.last().expect("wire shortcut hint").keystrokes()[0]
                .inner()
                .key,
            "w"
        );
        let save = window.bindings_for_action(&commands::RunAction::new("common.Control.save"));
        assert_eq!(
            save.first().unwrap().keystrokes(),
            save.last().unwrap().keystrokes()
        );
        assert_eq!(
            save.last().unwrap().keystrokes()[0]
                .inner()
                .modifiers
                .platform,
            cfg!(target_os = "macos")
        );
    })
    .unwrap();

    click(cx, &harness, "tb-open");
    click(cx, &harness, "tool-symbol");
    assert!(
        harness.sink.invoked_actions().is_empty(),
        "missing actions must not dispatch"
    );
    click(cx, &harness, "tb-save");
    assert!(harness.sink.events().iter().any(|event| matches!(
        event, ShellEvent::ActionInvoked(id) if id.as_str() == "common.Control.save"
    )));
    click(cx, &harness, "tb-palette");
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        window.input("Wire from host", cx);
    })
    .unwrap();
    cx.run_until_parked();
    cx.update_window(harness.window, |_, window, cx| {
        window.render_frame(cx);
        let state = harness.shell.read(cx).command_state().read(cx);
        assert_eq!(state.matched_count(), 1, "palette searches the host label");
    })
    .unwrap();
}

/// A dirty document whose save fails, to exercise the close dialog against
/// the same host interface used by the real editor.
struct UnsavedSink(Rc<RefCell<usize>>);

impl kicad_sch_ui::input::InputSink for UnsavedSink {
    fn handle(&mut self, _: ShellEvent) {}
    fn modified(&self) -> Option<bool> {
        Some(true)
    }
    fn save_document(&mut self) -> Result<(), String> {
        *self.0.borrow_mut() += 1;
        Err("Disk is full".into())
    }
}

#[gpui_kit::test]
fn cancel_and_failed_save_keep_the_unsaved_editor_open(cx: &mut TestAppContext) {
    let harness = open(cx);
    let saves = Rc::new(RefCell::new(0));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(UnsavedSink(saves.clone())));
            });
    });
    cx.simulate_keystrokes(
        harness.window,
        if cfg!(target_os = "macos") {
            "cmd-w"
        } else {
            "ctrl-w"
        },
    );
    cx.run_until_parked();
    assert!(cx.has_pending_prompt());
    cx.simulate_prompt_answer("Cancel");
    cx.run_until_parked();
    assert_eq!(*saves.borrow(), 0);
    frame(cx, &harness);

    let quit = |cx: &mut TestAppContext| {
        cx.update_window(harness.window, |_, window, cx| {
            window.dispatch_action(Box::new(commands::Quit), cx);
        })
        .unwrap();
        cx.run_until_parked();
    };
    quit(cx);
    assert!(cx.has_pending_prompt());
    cx.simulate_prompt_answer("Cancel");
    cx.run_until_parked();
    assert_eq!(*saves.borrow(), 0);
    frame(cx, &harness);

    quit(cx);
    assert!(cx.has_pending_prompt());
    cx.simulate_prompt_answer("Save");
    cx.run_until_parked();
    assert_eq!(*saves.borrow(), 1);
    assert!(cx.has_pending_prompt());
    cx.simulate_prompt_answer("OK");
    cx.run_until_parked();
    frame(cx, &harness);
    cx.update(|cx| assert_eq!(harness.shell.read(cx).status().as_ref(), "Disk is full"));
    // The cancelled attempt and failure must both allow a fresh close request.
    quit(cx);
    assert!(cx.has_pending_prompt());
    cx.simulate_prompt_answer("Cancel");
    cx.run_until_parked();
}

#[derive(Clone)]
struct SearchSink(
    Rc<
        RefCell<
            Vec<(
                kicad_sch_ui::search::SearchData,
                kicad_sch_ui::search::SearchOperation,
            )>,
        >,
    >,
);

impl kicad_sch_ui::input::InputSink for SearchSink {
    fn handle(&mut self, _: ShellEvent) {}
    fn search(
        &mut self,
        data: &kicad_sch_ui::search::SearchData,
        operation: kicad_sch_ui::search::SearchOperation,
    ) -> Result<kicad_sch_ui::search::SearchResult, String> {
        self.0.borrow_mut().push((data.clone(), operation));
        Ok(kicad_sch_ui::search::SearchResult {
            found: true,
            center_x: 1234.,
            center_y: 5678.,
            ..Default::default()
        })
    }
}

#[gpui_kit::test]
fn find_replace_uses_host_data_and_keeps_typing_out_of_the_canvas(cx: &mut TestAppContext) {
    use kicad_sch_ui::search::SearchOperation;
    let harness = open(cx);
    let requests = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(SearchSink(requests.clone())));
            });
    });
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_action(
            Box::new(commands::RunAction::new(
                "common.Interactive.findAndReplace",
            )),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    frame(cx, &harness);
    click(cx, &harness, "find-next");
    assert!(
        requests.borrow().is_empty(),
        "empty queries never reach the host"
    );
    // Opening Find focuses its real text input. W must enter text, not start wire placement.
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_action(
            Box::new(commands::RunAction::new(
                "common.Interactive.findAndReplace",
            )),
            cx,
        );
    })
    .unwrap();
    cx.run_until_parked();
    frame(cx, &harness);
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::Select);
    press(cx, &harness, "enter");
    assert_eq!(requests.borrow()[0].0.find, "w");
    assert!(requests.borrow()[0].0.replace_mode);
    assert_eq!(requests.borrow()[0].1, SearchOperation::Next);
    cx.update(|cx| {
        assert_eq!(
            harness.shell.read(cx).canvas().read(cx).camera().center(),
            [1234., 5678.]
        );
    });
    click(cx, &harness, "find-case");
    click(cx, &harness, "find-replace-all");
    assert_eq!(requests.borrow()[1].1, SearchOperation::ReplaceAll);
    assert!(requests.borrow()[1].0.match_case);
    assert_eq!(
        requests.borrow()[1].0.replace,
        "",
        "empty replacement deletes matching text"
    );
    click(cx, &harness, "find-close");
    assert_eq!(requests.borrow()[2].1, SearchOperation::Close);
    assert!(!requests.borrow()[2].0.active);
    press(cx, &harness, "w");
    assert_eq!(
        active_tool(cx, &harness),
        Tool::DrawWire,
        "closing restores canvas focus"
    );
}

struct SavableSink {
    saves: Rc<RefCell<usize>>,
    modified: bool,
}
impl kicad_sch_ui::input::InputSink for SavableSink {
    fn handle(&mut self, _: ShellEvent) {}
    fn modified(&self) -> Option<bool> {
        Some(self.modified)
    }
    fn save_document(&mut self) -> Result<(), String> {
        *self.saves.borrow_mut() += 1;
        self.modified = false;
        Ok(())
    }
}

#[gpui_kit::test]
fn native_close_saves_or_discards_only_after_confirmation(cx: &mut TestAppContext) {
    for answer in ["Save", "Discard"] {
        let harness = open(cx);
        let saves = Rc::new(RefCell::new(0));
        cx.update(|cx| {
            harness
                .shell
                .read(cx)
                .canvas()
                .clone()
                .update(cx, |canvas, _| {
                    canvas.set_sink(shared_sink(SavableSink {
                        saves: saves.clone(),
                        modified: true,
                    }));
                });
        });
        let mut visual = gpui_kit::VisualTestContext::from_window(harness.window, cx);
        assert!(
            !visual.simulate_close(),
            "dirty window must veto initial close"
        );
        cx.run_until_parked();
        assert!(cx.has_pending_prompt());
        // A repeated close while the dialog is up cannot open a second prompt.
        assert!(!visual.simulate_close());
        cx.simulate_prompt_answer(answer);
        cx.run_until_parked();
        assert_eq!(*saves.borrow(), usize::from(answer == "Save"));
        assert!(cx.update_window(harness.window, |_, _, _| ()).is_err());
    }
}

struct ItemPropertiesSink(
    Rc<RefCell<Vec<kicad_sch_ui::properties::ItemProperties>>>,
    Rc<RefCell<Vec<ShellEvent>>>,
);
impl kicad_sch_ui::input::InputSink for ItemPropertiesSink {
    fn handle(&mut self, event: ShellEvent) {
        self.1.borrow_mut().push(event);
    }
    fn item_properties(&mut self) -> Result<kicad_sch_ui::properties::ItemProperties, String> {
        use kicad_sch_ui::properties::*;
        if let Some(data) = self.0.borrow().last() {
            return Ok(data.clone());
        }
        Ok(ItemProperties {
            item_id: "symbol-1".into(),
            capabilities: 1,
            entries: vec![
                PropertyEntry {
                    name: "Value".into(),
                    value: "10k".into(),
                    kind: 0,
                    choices: vec![],
                },
                PropertyEntry {
                    name: "Exclude From BOM".into(),
                    value: "false".into(),
                    kind: 1,
                    choices: vec![],
                },
                PropertyEntry {
                    name: "Orientation".into(),
                    value: "Horizontal".into(),
                    kind: 7,
                    choices: vec!["Horizontal".into(), "Vertical".into()],
                },
            ],
        })
    }
    fn apply_properties(
        &mut self,
        data: &kicad_sch_ui::properties::ItemProperties,
    ) -> Result<(), String> {
        self.0.borrow_mut().push(data.clone());
        Ok(())
    }
}

#[gpui_kit::test]
fn item_properties_edits_apply_and_close_restores_canvas(cx: &mut TestAppContext) {
    let harness = open(cx);
    let requests = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(ItemPropertiesSink(
                    requests.clone(),
                    Rc::new(RefCell::new(Vec::new())),
                )));
            });
    });
    press(cx, &harness, "e");
    frame(cx, &harness);
    press(cx, &harness, "w");
    click(cx, &harness, "properties-close");
    assert!(
        requests.borrow().is_empty(),
        "closing discards uncommitted edits"
    );
    press(cx, &harness, "e");
    frame(cx, &harness);
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::Select);
    press(cx, &harness, "enter");
    assert_eq!(
        requests.borrow().len(),
        1,
        "Enter applies the focused input"
    );
    assert!(requests.borrow()[0].entries[0].value.contains('w'));
    click(cx, &harness, "properties-apply");
    assert_eq!(requests.borrow().len(), 2);
    assert_eq!(requests.borrow()[0], requests.borrow()[1]);
    click(cx, &harness, "properties-close");
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);
}

struct RejectedPropertiesSink(ItemPropertiesSink);
impl kicad_sch_ui::input::InputSink for RejectedPropertiesSink {
    fn handle(&mut self, event: ShellEvent) {
        self.0.handle(event);
    }
    fn item_properties(&mut self) -> Result<kicad_sch_ui::properties::ItemProperties, String> {
        self.0.item_properties()
    }
    fn apply_properties(
        &mut self,
        data: &kicad_sch_ui::properties::ItemProperties,
    ) -> Result<(), String> {
        self.0.0.borrow_mut().push(data.clone());
        Err("Item changed; reopen Properties before applying edits".into())
    }
}

#[gpui_kit::test]
fn failed_property_apply_preserves_draft_and_keeps_shortcuts_in_dialog(cx: &mut TestAppContext) {
    let harness = open(cx);
    let requests = Rc::new(RefCell::new(Vec::new()));
    let events = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(RejectedPropertiesSink(ItemPropertiesSink(
                    requests.clone(),
                    events.clone(),
                ))));
            });
    });
    press(cx, &harness, "e");
    frame(cx, &harness);
    press(cx, &harness, "w");
    click(cx, &harness, "properties-apply");
    assert_eq!(requests.borrow().len(), 1);
    assert!(requests.borrow()[0].entries[0].value.contains('w'));
    click(cx, &harness, "properties-apply");
    assert_eq!(requests.borrow().len(), 2);
    assert_eq!(requests.borrow()[0], requests.borrow()[1]);
    assert_eq!(active_tool(cx, &harness), Tool::Select);
    click(cx, &harness, "properties-close");
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);
}

#[gpui_kit::test]
fn properties_text_editing_shortcuts_do_not_reach_schematic(cx: &mut TestAppContext) {
    let harness = open(cx);
    let requests = Rc::new(RefCell::new(Vec::new()));
    let events = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(ItemPropertiesSink(
                    requests.clone(),
                    events.clone(),
                )));
            });
    });
    press(cx, &harness, "e");
    frame(cx, &harness);
    events.borrow_mut().clear();
    let modifier = if cfg!(target_os = "macos") {
        "cmd"
    } else {
        "ctrl"
    };
    press(cx, &harness, &format!("{modifier}-a"));
    for key in ["4", "7", "k"] {
        press(cx, &harness, key);
    }
    press(cx, &harness, "enter");
    assert_eq!(
        requests.borrow()[0].entries[0].value,
        "47k",
        "Select All must select input text"
    );
    // Clipboard and history shortcuts are owned by the input too.
    for key in ["a", "c", "x", "v", "z", "shift-z"] {
        press(cx, &harness, &format!("{modifier}-{key}"));
    }
    assert!(
        events
            .borrow()
            .iter()
            .all(|event| matches!(event, ShellEvent::ViewportChanged(_))),
        "input shortcuts reached schematic (only panel resize notifications are allowed): {:?}",
        events.borrow()
    );
    press(cx, &harness, &format!("{modifier}-a"));
    press(cx, &harness, "1");
    press(cx, &harness, "k");
    press(cx, &harness, "enter");
    assert_eq!(requests.borrow()[1].entries[0].value, "1k");
    click(cx, &harness, "properties-close");
}

#[gpui_kit::test]
fn item_properties_boolean_editor_applies_typed_value(cx: &mut TestAppContext) {
    let harness = open(cx);
    let requests = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(ItemPropertiesSink(
                    requests.clone(),
                    Rc::new(RefCell::new(Vec::new())),
                )));
            });
    });
    press(cx, &harness, "e");
    frame(cx, &harness);
    click(cx, &harness, ("property-boolean", 1usize));
    click(cx, &harness, "properties-apply");
    assert_eq!(requests.borrow()[0].entries[1].value, "true");
    assert_eq!(requests.borrow()[0].entries[2].value, "Horizontal");
}

struct LibrarySimulationSink {
    erc_runs: Rc<RefCell<usize>>,
    libraries: Rc<RefCell<Vec<Vec<kicad_sch_ui::library_workflows::LibraryRow>>>>,
    simulations: WorkflowCalls,
}
impl kicad_sch_ui::InputSink for LibrarySimulationSink {
    fn run_erc(&mut self) -> Result<Vec<kicad_sch_ui::erc::ErcViolation>, String> {
        *self.erc_runs.borrow_mut() += 1;
        Ok(Vec::new())
    }
    fn handle(&mut self, _: ShellEvent) {}
    fn library_table(
        &mut self,
        _: bool,
    ) -> Result<Vec<kicad_sch_ui::library_workflows::LibraryRow>, String> {
        Ok(vec![kicad_sch_ui::library_workflows::LibraryRow {
            name: "ProjectLib".into(),
            kind: "KiCad".into(),
            uri: "${KIPRJMOD}/test.kicad_sym".into(),
            enabled: true,
            visible: true,
            ..Default::default()
        }])
    }
    fn save_library_table(
        &mut self,
        _: bool,
        rows: &[kicad_sch_ui::library_workflows::LibraryRow],
    ) -> Result<(), String> {
        self.libraries.borrow_mut().push(rows.to_vec());
        Ok(())
    }
    fn simulation_workflow(&mut self, kind: u32) -> Result<Vec<(String, String)>, String> {
        if kind == 2 {
            return Ok(vec![("Status".into(), "Finished".into())]);
        }
        assert_eq!(kind, 1);
        Ok(vec![
            ("Analysis command".into(), ".op".into()),
            ("Netlist option flags".into(), "240".into()),
            ("Compatibility".into(), "1".into()),
        ])
    }
    fn apply_simulation_workflow(&mut self, kind: u32, values: &[String]) -> Result<(), String> {
        self.simulations.borrow_mut().push((kind, values.to_vec()));
        Ok(())
    }
}

#[gpui_kit::test]
fn gpui_library_and_simulation_controls_commit_owned_values(cx: &mut TestAppContext) {
    let harness = open(cx);
    let libraries = Rc::new(RefCell::new(Vec::new()));
    let simulations = Rc::new(RefCell::new(Vec::new()));
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(LibrarySimulationSink {
                    erc_runs: Rc::new(RefCell::new(0)),
                    libraries: libraries.clone(),
                    simulations: simulations.clone(),
                }));
            })
    });
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_action(
            Box::new(commands::RunAction::new(
                "common.SuiteControl.showSymbolLibTable",
            )),
            cx,
        );
    })
    .unwrap();
    frame(cx, &harness);
    click(cx, &harness, ("library-enabled", 0usize));
    click(cx, &harness, "library-save");
    assert_eq!(libraries.borrow().len(), 1);
    assert!(!libraries.borrow()[0][0].enabled);
    assert_eq!(libraries.borrow()[0][0].uri, "${KIPRJMOD}/test.kicad_sym");
    click(cx, &harness, "library-close");
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_action(
            Box::new(commands::RunAction::new(
                "eeschema.EditorControl.showSimulator",
            )),
            cx,
        );
    })
    .unwrap();
    frame(cx, &harness);
    click(cx, &harness, ("simulation-flag", 0x10usize));
    click(cx, &harness, "simulation-apply");
    assert_eq!(
        simulations.borrow().as_slice(),
        &[(1, vec![".op".into(), "224".into(), "1".into()])]
    );
    click(cx, &harness, ("simulation-property", 0usize));
    click(cx, &harness, "simulation-results");
    press(cx, &harness, "ctrl-shift-p");
    assert!(
        cx.update(|cx| harness.shell.read(cx).is_palette_open()),
        "Replacing a focused input panel must keep menu and keyboard actions dispatchable"
    );
    press(cx, &harness, "escape");
    click(cx, &harness, "simulation-close");
    press(cx, &harness, "w");
    assert_eq!(active_tool(cx, &harness), Tool::DrawWire);
}

/// These are distinct native windows, not mutually-exclusive panels in the
/// editor. Test real text focus, the native close callback and the key binding.
#[gpui_kit::test]
fn independent_dialog_windows_preserve_siblings_and_scope_text_shortcuts(cx: &mut TestAppContext) {
    for use_native_close in [false, true] {
        let harness = open(cx);
        let simulations = Rc::new(RefCell::new(Vec::new()));
        let erc_runs = Rc::new(RefCell::new(0));
        cx.update(|cx| {
            harness
                .shell
                .read(cx)
                .canvas()
                .clone()
                .update(cx, |canvas, _| {
                    canvas.set_sink(shared_sink(LibrarySimulationSink {
                        erc_runs: erc_runs.clone(),
                        libraries: Rc::new(RefCell::new(Vec::new())),
                        simulations: simulations.clone(),
                    }));
                });
        });
        click(cx, &harness, "tb-erc");
        let erc = window_for(cx, "erc-close");
        cx.update_window(harness.window, |_, window, cx| {
            window.activate_window();
            window.dispatch_action(
                Box::new(commands::RunAction::new(
                    "eeschema.EditorControl.showSimulator",
                )),
                cx,
            );
        })
        .unwrap();
        frame(cx, &harness);
        let simulation = window_for(cx, "simulation-close");
        assert_ne!(erc, simulation);
        assert_ne!(erc, harness.window);
        assert_ne!(simulation, harness.window);
        assert_eq!(cx.update(|cx| cx.windows().len()), 3);
        cx.update_window(harness.window, |_, window, _| {
            assert!(window.try_find("erc-close").is_none());
            assert!(window.try_find("simulation-close").is_none());
        })
        .unwrap();

        click(cx, &harness, ("simulation-property", 0usize));
        let modifier = if cfg!(target_os = "macos") {
            "cmd"
        } else {
            "ctrl"
        };
        press(cx, &harness, &format!("{modifier}-a"));
        press(cx, &harness, "w");
        assert_eq!(
            active_tool(cx, &harness),
            Tool::Select,
            "typing W in a dialog cannot start wire placement"
        );
        click(cx, &harness, "erc-run");
        assert_eq!(*erc_runs.borrow(), 1);
        assert_eq!(
            window_for(cx, "simulation-close"),
            simulation,
            "running ERC preserves the open simulator and its draft"
        );
        cx.update_window(harness.window, |_, window, cx| {
            window.dispatch_action(
                Box::new(commands::RunAction::new(
                    "eeschema.EditorControl.showSimulator",
                )),
                cx,
            );
        })
        .unwrap();
        frame(cx, &harness);
        assert_eq!(
            window_for(cx, "simulation-close"),
            simulation,
            "reopening raises the existing simulator"
        );
        click(cx, &harness, "simulation-apply");
        assert_eq!(
            simulations.borrow()[0].1[0],
            "w",
            "Select All belongs to the input in its own window and ERC cannot reset it"
        );
        if use_native_close {
            native_close(cx, simulation);
        } else {
            press(cx, &harness, &format!("{modifier}-w"));
        }
        frame(cx, &harness);
        assert!(cx.update_window(simulation, |_, _, _| ()).is_err());
        assert!(cx.update_window(harness.window, |_, _, _| ()).is_ok());
        assert_eq!(
            window_for(cx, "erc-close"),
            erc,
            "closing simulation leaves ERC alive"
        );
        click(cx, &harness, "erc-run");
        assert_eq!(*erc_runs.borrow(), 2, "the sibling remains interactive");
        click(cx, &harness, "erc-close");
        press(cx, &harness, "w");
        assert_eq!(active_tool(cx, &harness), Tool::DrawWire);
        cx.update_window(harness.window, |_, window, _| window.remove_window())
            .unwrap();
        frame(cx, &harness);
        assert!(cx.update(|cx| cx.windows().is_empty()));
    }
}

#[gpui_kit::test]
fn closing_the_editor_closes_all_owned_dialog_windows(cx: &mut TestAppContext) {
    let harness = open(cx);
    cx.update(|cx| {
        harness
            .shell
            .read(cx)
            .canvas()
            .clone()
            .update(cx, |canvas, _| {
                canvas.set_sink(shared_sink(LibrarySimulationSink {
                    erc_runs: Rc::new(RefCell::new(0)),
                    libraries: Rc::new(RefCell::new(Vec::new())),
                    simulations: Rc::new(RefCell::new(Vec::new())),
                }));
            });
    });
    click(cx, &harness, "tb-erc");
    let erc = window_for(cx, "erc-close");
    cx.update_window(harness.window, |_, window, cx| {
        window.dispatch_action(
            Box::new(commands::RunAction::new(
                "eeschema.EditorControl.showSimulator",
            )),
            cx,
        );
    })
    .unwrap();
    frame(cx, &harness);
    let simulation = window_for(cx, "simulation-close");
    native_close(cx, harness.window);
    frame(cx, &harness);
    for handle in [harness.window, erc, simulation] {
        assert!(
            cx.update_window(handle, |_, _, _| ()).is_err(),
            "owner and dependents must close together"
        );
    }
}
