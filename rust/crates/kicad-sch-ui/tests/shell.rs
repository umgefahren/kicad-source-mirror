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
use kicad_sch_ui::input::{PointerButton, RecordingSink, ShellEvent, shared_sink};
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
    cx.update(shell::init);

    let sink = RecordingSink::new();
    let shared = shared_sink(sink.clone());
    let captured: Rc<RefCell<Option<Entity<SchematicShell>>>> = Rc::new(RefCell::new(None));
    let slot = captured.clone();

    let handle = cx.open_window(size(WINDOW.width, WINDOW.height), move |window, cx| {
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(demo_stream());
        let renderer = std::rc::Rc::new(std::cell::RefCell::new(renderer));
        let view = cx.new(|cx| SchematicShell::new_with_renderer(renderer, shared, window, cx));
        *slot.borrow_mut() = Some(view.clone());
        Root::new(view, window, cx)
    });
    let shell = captured.borrow_mut().take().expect("the shell was built");
    let harness = Harness {
        window: handle.into(),
        shell,
        sink,
    };

    // Give the shell keyboard focus so key bindings resolve the way they do
    // when the user has clicked into the window.
    let focus = harness.shell.clone();
    cx.update_window(harness.window, |_, window, cx| {
        focus.read(cx).focus_handle(cx).focus(window, cx);
        window.render_frame(cx);
    })
    .expect("window is live");
    cx.run_until_parked();
    frame(cx, &harness);
    harness.sink.clear();
    harness
}

fn frame(cx: &mut TestAppContext, harness: &Harness) {
    cx.update_window(harness.window, |_, window, cx| window.render_frame(cx))
        .expect("window is live");
}

/// Click an element, then let the deferred action dispatch run and redraw.
fn click(cx: &mut TestAppContext, harness: &Harness, id: impl Into<ElementId>) {
    let id = id.into();
    cx.update_window(harness.window, move |_, window, cx| window.click(id, cx))
        .expect("window is live");
    cx.run_until_parked();
    frame(cx, harness);
}

/// Press a key, then settle.
fn press(cx: &mut TestAppContext, harness: &Harness, key: &str) {
    let key = key.to_string();
    cx.update_window(harness.window, move |_, window, cx| {
        window.press(&key, cx)
    })
    .expect("window is live");
    cx.run_until_parked();
    frame(cx, harness);
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
            "menu-row",
            "menu-bar",
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
            assert!(
                found.bounds().size.width > px(0.),
                "{id} has no width"
            );
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
        if spec.tool == Tool::Select {
            continue;
        }
        harness.sink.clear();
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
    assert_eq!(harness.sink.activated_tools(), vec![Tool::DrawBus.id()]);

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
        ("tb-erc", "eeschema.InspectionTool.runERC"),
        ("tb-annotate", "eeschema.EditorControl.annotate"),
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
    assert!((fitted - start).abs() < start * 0.01, "fit should return to the opening view");

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
    assert!(after > before, "scrolling up should zoom in: {before} -> {after}");
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
            harness
                .shell
                .read(cx)
                .canvas()
                .read(cx)
                .cursor_world()
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

#[gpui_kit::test]
fn picking_a_sheet_in_the_hierarchy_changes_the_properties_panel(cx: &mut TestAppContext) {
    let harness = open(cx);
    let before = cx
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
    assert_eq!(before, "Root Sheet");

    click(cx, &harness, "sym-u1");

    let after = cx
        .update_window(harness.window, |_, _, cx| {
            let design = harness.shell.read(cx).design().read(cx);
            (
                design.selected_label().to_string(),
                design.selected_kind().to_string(),
            )
        })
        .expect("window is live");
    assert_eq!(after.0, "U1  MCU-48");
    assert_eq!(after.1, "Symbol");
}

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

/// Right-clicking the canvas opens gpui-component's `ContextMenu`, which keeps
/// the `PopupMenu` entity it builds in element state for the life of the
/// window and never drops it — dismissing only clears an open flag. That is
/// harmless in an application and fatal to gpui's leaked-handle check, which
/// fires when the test's `App` is dropped; removing the window first does not
/// help. So this is recorded and skipped rather than deleted, and the context
/// menu is verified in the headless screenshots instead.
#[ignore = "gpui-component's ContextMenu retains its PopupMenu, which trips gpui's leak detector"]
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
}

#[gpui_kit::test]
async fn the_command_palette_filters_and_runs_a_command(cx: &mut TestAppContext) {
    let harness = open(cx);

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
    }).expect("live");

    cx.update_window(harness.window, |_, window, cx| {
        window.press("enter", cx);
    })
    .expect("window is live");
    cx.run_until_parked();

    assert!(
        harness
            .sink
            .invoked_actions()
            .contains(&"eeschema.EditorControl.annotate".to_string()),
        "the palette should have run the annotate command, got {:?}",
        harness.sink.invoked_actions()
    );
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
            harness
                .shell
                .read(cx)
                .canvas()
                .read(cx)
                .grid()
                .size()
                .label
        })
        .expect("window is live");

    click(cx, &harness, "tb-grid");
    let grid_after = cx
        .update_window(harness.window, |_, _, cx| {
            harness
                .shell
                .read(cx)
                .canvas()
                .read(cx)
                .grid()
                .size()
                .label
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
