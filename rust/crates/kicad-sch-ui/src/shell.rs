// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The window: menu bar, toolbars, docks, canvas, status bar and palette.

use std::cell::RefCell;
use std::rc::Rc;

use gpui_kit::TestSupportExt;
use gpui_kit::assets::IconName;
use gpui_kit::base::{Disableable, GlobalState};
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::command::{Command, CommandGroup, CommandItem, CommandState};
use gpui_kit::component::dock::{
    DockArea, DockLayout, DockPlacement, DockSkin, Panel, PanelEvent, panel_handle,
};
use gpui_kit::component::menu::{AppMenuBar, PopupMenu};
use gpui_kit::component::scroll::ScrollableElement;
use gpui_kit::component::status_bar::StatusBar;
use gpui_kit::component::theme::{Theme, ThemeMode};
use gpui_kit::component::{ActiveTheme, Icon, Selectable, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{
    Action, App, ClickEvent, Context, Entity, EventEmitter, FocusHandle, Focusable, KeyDownEvent,
    KeyUpEvent, SharedString, Window, div, px,
};

use crate::canvas::{CanvasContextMenu, CanvasElement, CanvasState};
use crate::commands::{
    self, ActionRegistry, CancelTool, CycleGrid, OpenCommandPalette, Quit, RunAction,
    ToggleFrameStats, ToggleGrid, ToggleLeftPanel, ToggleRightPanel, ToggleTheme, ToggleUnits,
    ZoomActualSize, ZoomIn, ZoomOut, ZoomToFit, ZoomToObjects,
};
use crate::document::SharedDocument;
use crate::grid::Units;
use crate::input::{Modifiers, SharedSink, ShellEvent, shared_sink};
use crate::panels::{DesignState, DocumentSource, HierarchyPanel, PropertiesPanel, StreamFacts};
use crate::stats::FrameStats;
use crate::theme::{self, CanvasPalette};
use crate::tools::TOOLS;
use kicad_sch_render::SchematicRenderer;

/// Initialise gpui-kit, the theme, the key map and the menu bar.
///
/// Call once, before opening the window.
pub fn init(cx: &mut App) {
    gpui_kit::init(cx);
    theme::apply(ThemeMode::Dark, None, cx);
    install_key_bindings(cx);
    install_menus(cx);
}

/// Initialise the live shell from the host's action metadata.
///
/// Install before creating any windows, so menus, shortcuts and toolbars all
/// resolve against the same snapshot. `init` remains the recorded-stream mode.
pub fn init_with_registry(cx: &mut App, registry: ActionRegistry) {
    cx.set_global(registry);
    init(cx);
}

/// Bind the default key map.
pub fn install_key_bindings(cx: &mut App) {
    let (bindings, rejected) = if let Some(registry) = cx.try_global::<ActionRegistry>() {
        commands::key_bindings_with_registry(registry)
    } else {
        let (bindings, rejected) = commands::key_bindings();
        (bindings, rejected.into_iter().map(str::to_owned).collect())
    };
    debug_assert!(
        rejected.is_empty(),
        "unparsable default keystrokes: {rejected:?}"
    );
    cx.bind_keys(bindings);
}

/// Publish the menu bar so [`AppMenuBar`] can draw it.
pub fn install_menus(cx: &mut App) {
    let menus = if let Some(registry) = cx.try_global::<ActionRegistry>() {
        commands::app_menus_with_registry(registry)
    } else {
        commands::app_menus()
    };
    let menus = menus.into_iter().map(|menu| menu.owned()).collect();
    GlobalState::global_mut(cx).set_app_menus(menus);
}

/// The canvas, wrapped as a dock panel so the docking machinery can treat it
/// like any other region.
pub struct CanvasPanel {
    focus_handle: FocusHandle,
    state: Entity<CanvasState>,
    /// What the tab says. The document's own name, not a placeholder: a
    /// screenshot that claims to be untitled while showing a real schematic
    /// is worse than no caption at all.
    title: SharedString,
    context_menu: Option<(Entity<PopupMenu>, gpui_kit::Point<gpui_kit::Pixels>)>,
    menu_subscription: Option<gpui_kit::Subscription>,
}

impl CanvasPanel {
    /// Wrap `state` as the centre panel, captioned `title`.
    pub fn new(
        state: Entity<CanvasState>,
        title: impl Into<SharedString>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        // Dock panels cache their rendered element. Invalidating the shell alone
        // does not repaint the nested canvas after a menu/keyboard edit.
        cx.observe(&state, |_, _, cx| cx.notify()).detach();
        cx.subscribe_in(
            &state,
            window,
            |this, _, event: &CanvasContextMenu, window, cx| {
                this.open_context_menu(event.0, window, cx);
            },
        )
        .detach();
        Self {
            focus_handle: cx.focus_handle(),
            state,
            title: title.into(),
            context_menu: None,
            menu_subscription: None,
        }
    }

    fn open_context_menu(
        &mut self,
        position: gpui_kit::Point<gpui_kit::Pixels>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let menu = PopupMenu::build(window, cx, |mut menu, _, cx| {
            for group in [
                &[
                    ("Cut", "common.Interactive.cut"),
                    ("Copy", "common.Interactive.copy"),
                    ("Paste", "common.Interactive.paste"),
                ][..],
                &[
                    ("Properties...", "eeschema.InteractiveEdit.properties"),
                    ("Delete", "common.Interactive.delete"),
                ][..],
                &[("Select All", "common.Interactive.selectAll")][..],
            ] {
                let mut populated = false;
                for (fallback, id) in group {
                    let presentation = action_presentation(cx, id, fallback, "", None);
                    if presentation.available {
                        menu = menu.menu(presentation.label, Box::new(RunAction::new(*id)));
                        populated = true;
                    }
                }
                if populated {
                    menu = menu.separator();
                }
            }
            menu.menu("Zoom to Fit", Box::new(ZoomToFit))
                .action_context(self.focus_handle.clone())
        });

        self.menu_subscription = Some(cx.subscribe_in(
            &menu,
            window,
            |this, _, _: &gpui_kit::DismissEvent, window, cx| {
                this.context_menu = None;
                this.menu_subscription = None;
                this.focus_handle.focus(window, cx);
                cx.notify();
            },
        ));
        menu.read(cx).focus_handle(cx).focus(window, cx);
        self.context_menu = Some((menu, position));
        cx.notify();
    }

    /// Re-caption the tab, when the document changes.
    pub fn set_title(&mut self, title: impl Into<SharedString>, cx: &mut Context<Self>) {
        self.title = title.into();
        cx.notify();
    }

    /// The canvas state behind the panel.
    pub fn state(&self) -> &Entity<CanvasState> {
        &self.state
    }

    fn on_key_down(&mut self, event: &KeyDownEvent, _window: &mut Window, cx: &mut Context<Self>) {
        let keystroke = &event.keystroke;
        let modifiers = Modifiers {
            ctrl: keystroke.modifiers.control,
            shift: keystroke.modifiers.shift,
            alt: keystroke.modifiers.alt,
            meta: keystroke.modifiers.platform,
        };
        self.state.update(cx, |state, cx| {
            state.emit(ShellEvent::KeyDown {
                key: keystroke.key.to_string(),
                modifiers,
                repeat: event.is_held,
            });
            cx.notify();
        });
        // As every mouse handler in `canvas.rs` does. A host that claimed the key may
        // have changed the document, which sets the canvas' dirty flag — and without a
        // repaint scheduled nothing consumes it until an unrelated event happens to
        // cause one, so the window would keep showing the pre-keystroke frame.
        cx.notify();
    }

    fn on_key_up(&mut self, event: &KeyUpEvent, _window: &mut Window, cx: &mut Context<Self>) {
        let keystroke = &event.keystroke;
        let modifiers = Modifiers {
            ctrl: keystroke.modifiers.control,
            shift: keystroke.modifiers.shift,
            alt: keystroke.modifiers.alt,
            meta: keystroke.modifiers.platform,
        };
        self.state.read(cx).emit(ShellEvent::KeyUp {
            key: keystroke.key.to_string(),
            modifiers,
        });
        cx.notify();
    }
}

impl Focusable for CanvasPanel {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl EventEmitter<PanelEvent> for CanvasPanel {}

impl gpui_kit::component::dock::BasePanel for CanvasPanel {
    fn panel_name(&self) -> &'static str {
        "SchematicCanvas"
    }

    fn closable(&self, _cx: &App) -> bool {
        // Closing the drawing surface would leave an editor with nothing to
        // edit, and the dock offers no way back.
        false
    }
}

impl Panel for CanvasPanel {
    fn title(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        self.title.clone()
    }

    fn inner_padding(&self, _cx: &App) -> bool {
        false
    }
}

impl Render for CanvasPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let state = self.state.clone();
        div()
            .id("canvas")
            .test_support()
            .track_focus(&self.focus_handle)
            .key_context("SchematicCanvas")
            .size_full()
            .relative()
            .overflow_hidden()
            .on_mouse_down(
                gpui_kit::MouseButton::Left,
                cx.listener(|this, _, window, cx| {
                    this.focus_handle.focus(window, cx);
                }),
            )
            .on_key_down(cx.listener(Self::on_key_down))
            .on_key_up(cx.listener(Self::on_key_up))
            .child(CanvasElement::new("canvas-surface", state).size_full())
            .when_some(self.context_menu.clone(), |element, (menu, position)| {
                element.child(gpui_kit::deferred(
                    gpui_kit::anchored()
                        .position(position)
                        .snap_to_window_with_margin(px(8.))
                        .child(menu),
                ))
            })
    }
}

/// The schematic editor window.
pub struct SchematicShell {
    focus_handle: FocusHandle,
    menu_bar: Entity<AppMenuBar>,
    dock: Entity<DockArea>,
    _dock_skin: Rc<DockSkin>,
    canvas: Entity<CanvasState>,
    canvas_panel: Entity<CanvasPanel>,
    design: Entity<DesignState>,
    hierarchy: Entity<HierarchyPanel>,
    properties: Entity<PropertiesPanel>,
    command_state: Entity<CommandState>,
    palette_open: bool,
    units: Units,
    theme_mode: ThemeMode,
    stats: FrameStats,
    /// Free-running redraw, so the frame-time readout measures the display
    /// rate rather than how often something happened to change.
    free_run: bool,
    status: SharedString,
}

impl SchematicShell {
    /// The shell showing the demonstration draw stream and reporting nowhere.
    pub fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(crate::demo::demo_stream());
        Self::new_with_document(
            Rc::new(RefCell::new(renderer)),
            DocumentSource::Demonstration,
            shared_sink(crate::input::NullSink),
            window,
            cx,
        )
    }

    /// The shell driving `renderer` and reporting to `sink`.
    ///
    /// This is the entry point a host uses: build a
    /// [`SchematicRenderer`](kicad_sch_render::SchematicRenderer), give it a
    /// draw stream, and hand it here along with the sink that will feed
    /// `TOOL_MANAGER`. The shell keeps the `Rc` and drives the renderer's
    /// camera; it never copies the view transform anywhere.
    pub fn new_with_renderer(
        renderer: Rc<RefCell<SchematicRenderer>>,
        sink: SharedSink,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        Self::new_with_document(renderer, DocumentSource::Empty, sink, window, cx)
    }

    /// The shell driving `renderer`, captioned by `source`.
    ///
    /// The caption reaches the canvas tab and both docked panels, so a window
    /// showing a real schematic says which one it is.
    pub fn new_with_document(
        renderer: Rc<RefCell<SchematicRenderer>>,
        source: DocumentSource,
        sink: SharedSink,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let theme_mode = Theme::global(cx).mode;
        let palette = CanvasPalette::for_mode(theme_mode);

        // Everything the panels show is derived from the stream that was
        // actually loaded. Nothing here is invented.
        let (facts, origin, extent) = {
            let renderer = renderer.borrow();
            let facts = renderer
                .stream()
                .map(|stream| StreamFacts {
                    groups: stream.groups().len(),
                    group_commands: stream.group_cmds().len(),
                    frame_commands: stream.frame_cmds().len(),
                    images: stream.images().len(),
                })
                .unwrap_or_default();
            let bounds = renderer.document_bounds();
            (facts, bounds.min, bounds.size())
        };
        let design_state = DesignState::from_stream(source.clone(), facts, origin, extent);
        let title = design_state.source().title();

        let canvas = cx.new(|_| CanvasState::new(renderer, palette, sink));
        let canvas_panel = cx.new(|cx| CanvasPanel::new(canvas.clone(), title, window, cx));
        let design = cx.new(|_| design_state);
        let hierarchy = cx.new(|cx| HierarchyPanel::new(design.clone(), cx));
        let properties = cx.new(|cx| PropertiesPanel::new(design.clone(), cx));
        let command_state = cx.new(|cx| CommandState::new(window, cx));
        let menu_bar = AppMenuBar::new(cx);

        let (dock, dock_skin) = DockSkin::dock_area("schematic", Some(1), window, cx);
        dock.update(cx, |dock, cx| {
            dock.set_center(
                DockLayout::tabs().panel_view(panel_handle(canvas_panel.clone()), cx),
                window,
                cx,
            );
            dock.set_dock(
                DockPlacement::Left,
                DockLayout::tabs().panel_view(panel_handle(hierarchy.clone()), cx),
                window,
                cx,
            );
            dock.set_dock(
                DockPlacement::Right,
                DockLayout::tabs().panel_view(panel_handle(properties.clone()), cx),
                window,
                cx,
            );
            dock.set_dock_size(DockPlacement::Left, px(248.), window, cx);
            dock.set_dock_size(DockPlacement::Right, px(268.), window, cx);
        });

        // A redraw of the shell has to follow a redraw of the canvas, or the
        // status bar shows last frame's cursor position.
        cx.observe(&canvas, |_, _, cx| cx.notify()).detach();

        let shell = Self {
            focus_handle: cx.focus_handle(),
            menu_bar,
            dock,
            _dock_skin: dock_skin,
            canvas,
            canvas_panel,
            design,
            hierarchy,
            properties,
            command_state,
            palette_open: false,
            units: Units::Millimetres,
            theme_mode,
            stats: FrameStats::default(),
            free_run: true,
            status: "Ready".into(),
        };
        shell.canvas.update(cx, |canvas, _| canvas.zoom_to_fit());
        shell
            .canvas_panel
            .read(cx)
            .focus_handle(cx)
            .focus(window, cx);
        shell
    }

    /// Draw from a live document from now on.
    ///
    /// The one call that turns the shell from a viewer of a recorded frame into
    /// a window onto a document: from here the canvas re-records whenever the
    /// view moves rather than panning a camera over a fixed copy. Attach it after
    /// construction — the shell frames whatever the renderer already holds while
    /// being built, and a document is free to have produced that first frame.
    pub fn set_document(&mut self, document: SharedDocument, cx: &mut Context<Self>) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.set_document(document);
            cx.notify();
        });
        cx.notify();
    }

    /// The canvas state, for tests and for the host.
    pub fn canvas(&self) -> &Entity<CanvasState> {
        &self.canvas
    }

    /// The canvas panel.
    pub fn canvas_panel(&self) -> &Entity<CanvasPanel> {
        &self.canvas_panel
    }

    /// The shared design state the two docks read.
    pub fn design(&self) -> &Entity<DesignState> {
        &self.design
    }

    /// The hierarchy panel.
    pub fn hierarchy(&self) -> &Entity<HierarchyPanel> {
        &self.hierarchy
    }

    /// The properties panel.
    pub fn properties(&self) -> &Entity<PropertiesPanel> {
        &self.properties
    }

    /// The dock area.
    pub fn dock(&self) -> &Entity<DockArea> {
        &self.dock
    }

    /// Whether the command palette is open.
    pub fn is_palette_open(&self) -> bool {
        self.palette_open
    }

    /// The command palette's interaction state: its query, its selection and
    /// how many commands survived the query.
    pub fn command_state(&self) -> &Entity<CommandState> {
        &self.command_state
    }

    /// The display units.
    pub fn units(&self) -> Units {
        self.units
    }

    /// The theme in force.
    pub fn theme_mode(&self) -> ThemeMode {
        self.theme_mode
    }

    /// Frame pacing measurements.
    pub fn stats(&self) -> &FrameStats {
        &self.stats
    }

    /// Show or hide the frame-time readout.
    pub fn set_frame_stats_enabled(&mut self, enabled: bool) {
        self.stats.set_enabled(enabled);
    }

    /// Open the command palette without a keystroke.
    ///
    /// The headless compositor the screenshots are taken under has a seat with
    /// no input devices, so nothing can be typed at the window; this drives the
    /// same state the `ctrl-shift-p` binding does so that the palette can be
    /// photographed.
    pub fn open_palette(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        self.on_open_palette(&OpenCommandPalette, window, cx);
    }

    /// Apply `steps` zoom steps, positive in, negative out. Same reason.
    ///
    /// Queued behind the opening fit rather than applied now, because the fit
    /// itself has to wait for the first layout.
    pub fn zoom_steps(&mut self, steps: i32, cx: &mut Context<Self>) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.zoom_after_fit(steps);
            cx.notify();
        });
        cx.notify();
    }

    /// Whether the window redraws every display frame. On by default so the
    /// frame-time readout is meaningful; a host that would rather idle can
    /// turn it off.
    pub fn free_run(&self) -> bool {
        self.free_run
    }

    /// Turn free-running redraw on or off.
    pub fn set_free_run(&mut self, on: bool) {
        self.free_run = on;
    }

    /// The status-bar message.
    pub fn status(&self) -> &SharedString {
        &self.status
    }

    fn set_status(&mut self, message: impl Into<SharedString>) {
        self.status = message.into();
    }

    fn emit(&self, event: ShellEvent, cx: &mut Context<Self>) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.emit(event);
            cx.notify();
        });
    }

    fn report(&self, command: commands::ShellCommand, cx: &mut Context<Self>) {
        self.emit(ShellEvent::ActionInvoked(command.reported_id().into()), cx);
    }

    // --- action handlers -------------------------------------------------

    fn on_run_action(&mut self, action: &RunAction, _window: &mut Window, cx: &mut Context<Self>) {
        let id = action.id.to_string();
        if let Some(tool) = commands::tool_for_action(&id) {
            self.canvas.update(cx, |canvas, cx| {
                if let Some(hotkey) = &action.hotkey {
                    canvas.tool_hotkey(tool, hotkey);
                } else {
                    canvas.set_tool(tool);
                }
                cx.notify();
            });
            self.set_status(format!("{} tool", tool.label()));
        } else {
            self.emit(ShellEvent::ActionInvoked(id.as_str().into()), cx);
            self.set_status(id);
        }
        cx.notify();
    }

    fn on_quit(&mut self, _: &Quit, _window: &mut Window, cx: &mut Context<Self>) {
        self.report(commands::ShellCommand::Quit, cx);
        cx.quit();
    }

    fn on_zoom_in(&mut self, _: &ZoomIn, _window: &mut Window, cx: &mut Context<Self>) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.zoom_in();
            cx.notify();
        });
        self.report(commands::ShellCommand::ZoomIn, cx);
        cx.notify();
    }

    fn on_zoom_out(&mut self, _: &ZoomOut, _window: &mut Window, cx: &mut Context<Self>) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.zoom_out();
            cx.notify();
        });
        self.report(commands::ShellCommand::ZoomOut, cx);
        cx.notify();
    }

    fn on_zoom_to_fit(&mut self, _: &ZoomToFit, _window: &mut Window, cx: &mut Context<Self>) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.zoom_to_fit();
            cx.notify();
        });
        self.report(commands::ShellCommand::ZoomToFit, cx);
        cx.notify();
    }

    fn on_zoom_to_objects(
        &mut self,
        _: &ZoomToObjects,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.zoom_to_objects();
            cx.notify();
        });
        self.report(commands::ShellCommand::ZoomToObjects, cx);
        cx.notify();
    }

    fn on_zoom_actual_size(
        &mut self,
        _: &ZoomActualSize,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.canvas.update(cx, |canvas, cx| {
            canvas.zoom_actual_size();
            cx.notify();
        });
        self.report(commands::ShellCommand::ZoomActualSize, cx);
        cx.notify();
    }

    fn on_toggle_grid(&mut self, _: &ToggleGrid, _window: &mut Window, cx: &mut Context<Self>) {
        let visible = self.canvas.update(cx, |canvas, cx| {
            let visible = canvas.grid_mut().toggle_visible();
            cx.notify();
            visible
        });
        self.set_status(if visible { "Grid shown" } else { "Grid hidden" });
        self.report(commands::ShellCommand::ToggleGrid, cx);
        cx.notify();
    }

    fn on_cycle_grid(&mut self, _: &CycleGrid, _window: &mut Window, cx: &mut Context<Self>) {
        let label = self.canvas.update(cx, |canvas, cx| {
            canvas.grid_mut().cycle();
            cx.notify();
            canvas.grid().size().label
        });
        self.set_status(format!("Grid {label}"));
        self.report(commands::ShellCommand::CycleGrid, cx);
        cx.notify();
    }

    fn on_toggle_units(&mut self, _: &ToggleUnits, _window: &mut Window, cx: &mut Context<Self>) {
        self.units = self.units.next();
        self.set_status(format!("Units: {}", self.units.suffix()));
        self.report(commands::ShellCommand::ToggleUnits, cx);
        cx.notify();
    }

    fn on_toggle_theme(&mut self, _: &ToggleTheme, window: &mut Window, cx: &mut Context<Self>) {
        self.theme_mode = if self.theme_mode.is_dark() {
            ThemeMode::Light
        } else {
            ThemeMode::Dark
        };
        theme::apply(self.theme_mode, Some(window), cx);
        let palette = CanvasPalette::for_mode(self.theme_mode);
        self.canvas.update(cx, |canvas, cx| {
            canvas.set_palette(palette);
            cx.notify();
        });
        self.report(commands::ShellCommand::ToggleTheme, cx);
        cx.notify();
    }

    fn on_toggle_left_panel(
        &mut self,
        _: &ToggleLeftPanel,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.dock.update(cx, |dock, cx| {
            dock.toggle_dock(DockPlacement::Left, window, cx);
        });
        self.report(commands::ShellCommand::ToggleLeftPanel, cx);
        cx.notify();
    }

    fn on_toggle_right_panel(
        &mut self,
        _: &ToggleRightPanel,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.dock.update(cx, |dock, cx| {
            dock.toggle_dock(DockPlacement::Right, window, cx);
        });
        self.report(commands::ShellCommand::ToggleRightPanel, cx);
        cx.notify();
    }

    fn on_toggle_frame_stats(
        &mut self,
        _: &ToggleFrameStats,
        _window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let on = self.stats.toggle();
        self.set_status(if on {
            "Frame time readout on"
        } else {
            "Frame time readout off"
        });
        self.report(commands::ShellCommand::ToggleFrameStats, cx);
        cx.notify();
    }

    fn on_open_palette(
        &mut self,
        _: &OpenCommandPalette,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.palette_open = true;
        self.command_state.update(cx, |state, cx| {
            state.focus(window, cx);
        });
        self.report(commands::ShellCommand::OpenCommandPalette, cx);
        cx.notify();
    }

    fn close_palette(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        if self.palette_open {
            self.palette_open = false;
            self.canvas_panel
                .read(cx)
                .focus_handle(cx)
                .focus(window, cx);
            cx.notify();
        }
    }

    fn on_cancel_tool(&mut self, _: &CancelTool, window: &mut Window, cx: &mut Context<Self>) {
        if self.palette_open {
            self.close_palette(window, cx);
            return;
        }
        self.canvas.update(cx, |canvas, cx| {
            canvas.cancel_tool();
            cx.notify();
        });
        self.set_status("Select tool");
        cx.notify();
    }

    // --- rendering -------------------------------------------------------

    fn render_menu_row(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        div()
            .id("menu-row")
            .test_support()
            .h_flex()
            .h(px(34.))
            .flex_shrink_0()
            .items_center()
            .gap_3()
            .px_2()
            .bg(theme.title_bar)
            .border_b_1()
            .border_color(theme.title_bar_border)
            .child(
                div()
                    .h_flex()
                    .items_center()
                    .gap_1p5()
                    .pl_1()
                    .pr_2()
                    .child(Icon::new(IconName::Cpu).size_4().text_color(theme.primary))
                    .child(
                        div()
                            .text_xs()
                            .font_semibold()
                            .text_color(theme.muted_foreground)
                            .child("SCHEMATIC"),
                    ),
            )
            .child(
                div()
                    .id("menu-bar")
                    .test_support()
                    .flex_1()
                    .h_full()
                    .child(self.menu_bar.clone()),
            )
            .child(
                div()
                    .id("document-name")
                    .test_support()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(self.design.read(cx).source().title()),
            )
    }

    fn render_toolbar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let grid_label = self.canvas.read(cx).grid().size().label;
        let zoom = self.canvas.read(cx).zoom();
        let stats_on = self.stats.is_enabled();
        let dark = self.theme_mode.is_dark();

        div()
            .id("toolbar")
            .test_support()
            .h_flex()
            .h(px(40.))
            .flex_shrink_0()
            .items_center()
            .gap_1()
            .px_2()
            .bg(theme.background)
            .border_b_1()
            .border_color(theme.border)
            .child(registry_action_button(
                "tb-new",
                IconName::FilePlus,
                "New Schematic",
                "common.Control.new",
                cx,
            ))
            .child(registry_action_button(
                "tb-open",
                IconName::FolderOpen,
                "Open...",
                "common.Control.open",
                cx,
            ))
            .child(registry_action_button(
                "tb-save",
                IconName::Save,
                "Save",
                "common.Control.save",
                cx,
            ))
            .child(toolbar_separator(cx))
            .child(registry_action_button(
                "tb-undo",
                IconName::Undo2,
                "Undo",
                "common.Interactive.undo",
                cx,
            ))
            .child(registry_action_button(
                "tb-redo",
                IconName::Redo2,
                "Redo",
                "common.Interactive.redo",
                cx,
            ))
            .child(toolbar_separator(cx))
            .child(action_button(
                "tb-zoom-in",
                IconName::ZoomIn,
                "Zoom In",
                Box::new(ZoomIn),
                cx,
            ))
            .child(action_button(
                "tb-zoom-out",
                IconName::ZoomOut,
                "Zoom Out",
                Box::new(ZoomOut),
                cx,
            ))
            .child(action_button(
                "tb-zoom-fit",
                IconName::Scan,
                "Zoom to Fit Sheet",
                Box::new(ZoomToFit),
                cx,
            ))
            .child(toolbar_separator(cx))
            .child(registry_action_button(
                "tb-erc",
                IconName::CircleCheck,
                "Electrical Rules Checker",
                "eeschema.InspectionTool.runERC",
                cx,
            ))
            .child(registry_action_button(
                "tb-annotate",
                IconName::Hash,
                "Annotate Schematic",
                "eeschema.EditorControl.annotate",
                cx,
            ))
            .child(registry_action_button(
                "tb-pcb",
                IconName::CircuitBoard,
                "Update PCB from Schematic",
                "common.Control.updatePcbFromSchematic",
                cx,
            ))
            .child(div().flex_1())
            .child(
                Button::new("tb-grid")
                    .ghost()
                    .with_size(px(30.))
                    .icon(IconName::Grid2x2)
                    .label(grid_label)
                    .tooltip("Next grid size")
                    .on_click(dispatch(Box::new(CycleGrid))),
            )
            .child(
                Button::new("tb-units")
                    .ghost()
                    .with_size(px(30.))
                    .icon(IconName::Ruler)
                    .label(self.units.suffix())
                    .tooltip("Switch units")
                    .on_click(dispatch(Box::new(ToggleUnits))),
            )
            .child(
                Button::new("tb-zoom-level")
                    .ghost()
                    .with_size(px(30.))
                    .label(format!("{:.0}%", zoom * 100.))
                    .tooltip("Actual size")
                    .on_click(dispatch(Box::new(ZoomActualSize))),
            )
            .child(toolbar_separator(cx))
            .child(
                Button::new("tb-stats")
                    .ghost()
                    .with_size(px(30.))
                    .icon(IconName::Activity)
                    .selected(stats_on)
                    .tooltip("Frame time readout")
                    .on_click(dispatch(Box::new(ToggleFrameStats))),
            )
            .child(
                Button::new("tb-theme")
                    .ghost()
                    .with_size(px(30.))
                    .icon(if dark { IconName::Sun } else { IconName::Moon })
                    .tooltip("Dark / light theme")
                    .on_click(dispatch(Box::new(ToggleTheme))),
            )
            .child(
                Button::new("tb-palette")
                    .ghost()
                    .with_size(px(30.))
                    .icon(IconName::Search)
                    .tooltip("Command palette")
                    .on_click(dispatch(Box::new(OpenCommandPalette))),
            )
    }

    fn render_tool_palette(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let active = self.canvas.read(cx).tool();
        div()
            .id("tool-palette")
            .test_support()
            .v_flex()
            .w(px(46.))
            .flex_shrink_0()
            // The workspace row centres its items, so without a definite
            // height the palette takes its content's height — 30 tools is
            // taller than the window — and half of it ends up off-screen
            // rather than scrolling. `h_full` pins it to the row instead.
            .h_full()
            .min_h_0()
            .overflow_hidden()
            .bg(theme.sidebar)
            .border_r_1()
            .border_color(theme.sidebar_border)
            // Thirty tools are taller than the window, so the column scrolls
            // inside its fixed-width frame instead of clipping the last few.
            // The scroll region is an inner element on purpose: `Scrollable`
            // re-ids what it wraps, and re-iding the outer div would take
            // `tool-palette` out of reach of the tests that look it up by name.
            // `overflow_y_scrollbar` keeps its own scroll handle, and puts the
            // `overflow: hidden` a flex item needs on its own wrapper, so
            // neither has to be spelled out here. `flex_1` is set before it
            // because the wrapper snapshots the flex properties at that call.
            .child(
                div()
                    .v_flex()
                    .flex_1()
                    .overflow_y_scrollbar()
                    .items_center()
                    .gap_0p5()
                    .py_2()
                    .children(TOOLS.iter().map(|spec| {
                        let action: Box<dyn Action> = Box::new(RunAction::new(spec.id.as_str()));
                        let presentation = action_presentation(
                            cx,
                            spec.id.as_str(),
                            spec.label,
                            spec.description,
                            spec.shortcut,
                        );
                        div()
                            .v_flex()
                            // Without this the buttons compress to fit rather
                            // than overflowing, and nothing ever scrolls.
                            .flex_shrink_0()
                            .items_center()
                            .when(spec.group_break, |this| {
                                this.child(
                                    div().my_1().w(px(22.)).h(px(1.)).bg(theme.sidebar_border),
                                )
                            })
                            .child(
                                Button::new(spec.button_id)
                                    .ghost()
                                    .with_size(px(32.))
                                    .icon(spec.icon)
                                    // `selected` styles it; `toggled` is what
                                    // reaches the accessibility tree as
                                    // aria-pressed, which is both correct for a
                                    // tool button and the only way a test can
                                    // see which tool is active from outside.
                                    .selected(active == spec.tool)
                                    .toggled(active == spec.tool)
                                    .accessibility_label(presentation.label)
                                    .tooltip(presentation.tooltip)
                                    .disabled(!presentation.available)
                                    .on_click(dispatch(action)),
                            )
                    })),
            )
    }

    fn render_status_bar(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        let canvas = self.canvas.read(cx);
        let units = self.units;
        let position = canvas
            .cursor_world()
            .map(|world| {
                format!(
                    "X {}  Y {} {}",
                    units.format(world.x),
                    units.format(world.y),
                    units.suffix()
                )
            })
            .unwrap_or_else(|| format!("X --  Y -- {}", units.suffix()));
        let selection = canvas.selection_count();
        let grid = canvas.grid().size().label;
        let zoom = format!("{:.0}%", canvas.zoom() * 100.);
        let tool = canvas.tool().label();
        let readout = if self.stats.is_enabled() {
            self.stats.readout()
        } else {
            None
        };
        let meets_target = self.stats.meets_target();

        StatusBar::new()
            .left(
                div()
                    .id("status-tool")
                    .test_support()
                    .h_flex()
                    .items_center()
                    .gap_1p5()
                    .text_xs()
                    .child(Icon::new(IconName::MousePointer2).size_3p5())
                    .child(tool),
            )
            .left(status_divider(cx))
            .left(
                div()
                    .id("status-position")
                    .test_support()
                    .text_xs()
                    .font_family("monospace")
                    .child(position),
            )
            .left(status_divider(cx))
            .left(
                div()
                    .id("status-message")
                    .test_support()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(
                        canvas
                            .host_status()
                            .map(SharedString::from)
                            .unwrap_or_else(|| self.status.clone()),
                    ),
            )
            .when_some(canvas.modified(), |bar, modified| {
                bar.right(
                    div()
                        .id("status-modified")
                        .test_support()
                        .text_xs()
                        .child(if modified { "Unsaved changes" } else { "Saved" }),
                )
            })
            // A live canvas that quietly keeps showing the last frame it managed
            // to record is indistinguishable from one that is working, so a
            // failed frame says so where the user is already looking.
            .when_some(canvas.document_error().cloned(), |bar, reason| {
                bar.left(status_divider(cx)).left(
                    div()
                        .id("status-document-error")
                        .test_support()
                        .text_xs()
                        .text_color(theme.danger)
                        .child(format!("Frame not recorded: {reason}")),
                )
            })
            .right(
                div()
                    .id("status-selection")
                    .test_support()
                    .text_xs()
                    .text_color(theme.muted_foreground)
                    .child(format!("{selection} selected")),
            )
            .right(status_divider(cx))
            .right(
                div()
                    .id("status-grid")
                    .test_support()
                    .text_xs()
                    .child(format!("Grid {grid}")),
            )
            .right(status_divider(cx))
            .right(
                div()
                    .id("status-zoom")
                    .test_support()
                    .text_xs()
                    .child(format!("Zoom {zoom}")),
            )
            .when_some(readout, |bar, readout| {
                // The renderer's own numbers sit next to the frame timing,
                // because "60 fps" means something different when it is drawing
                // forty groups than when it is drawing five thousand.
                let frame = canvas.last_frame();
                bar.right(status_divider(cx))
                    .right(
                        div()
                            .id("status-render")
                            .test_support()
                            .text_xs()
                            .font_family("monospace")
                            .text_color(theme.muted_foreground)
                            .child(format!(
                                "{} drawn  {} culled  {} paths  canvas {:.2} ms",
                                frame.groups_drawn,
                                frame.groups_culled,
                                frame.paths,
                                canvas.last_paint().as_secs_f64() * 1000.0
                            )),
                    )
                    .right(status_divider(cx))
                    .right(
                        div()
                            .id("status-frame-time")
                            .test_support()
                            .text_xs()
                            .font_family("monospace")
                            .text_color(if meets_target {
                                theme.success
                            } else {
                                theme.warning
                            })
                            .child(readout),
                    )
            })
    }

    fn render_palette(&self, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        // The palette's callbacks outlive this render, so they reach the shell
        // through its own entity rather than borrowing `self`.
        let on_confirm = cx.entity();
        let on_cancel = cx.entity();
        let mut command = Command::new(&self.command_state)
            .placeholder("Type a command...")
            .searchable(true)
            .filterable(true)
            .max_h(px(420.))
            .on_confirm(move |_, window, cx| {
                on_confirm.update(cx, |shell, cx| shell.close_palette(window, cx));
            })
            .on_cancel(move |window, cx| {
                on_cancel.update(cx, |shell, cx| shell.close_palette(window, cx));
            });

        let mut grouped: Vec<(String, Vec<CommandItem>)> = Vec::new();
        let commands = if let Some(registry) = cx.try_global::<ActionRegistry>() {
            commands::all_commands_with_registry(registry)
                .into_iter()
                .map(|(path, spec)| (path, spec.label.clone(), spec.action()))
                .collect::<Vec<_>>()
        } else {
            commands::all_commands()
                .into_iter()
                .map(|(path, spec)| (path, commands::label_of(&spec).to_owned(), spec.action()))
                .collect()
        };
        for (path, label, action) in commands {
            let keywords = [label.to_lowercase(), path.to_lowercase()];
            let item = CommandItem::new()
                .label(label)
                .keywords(keywords)
                .action(action);
            match grouped.iter_mut().find(|(name, _)| *name == path) {
                Some((_, items)) => items.push(item),
                None => grouped.push((path, vec![item])),
            }
        }
        for (path, items) in grouped {
            command = command.group(CommandGroup::new().label(path).items(items));
        }

        div()
            .id("command-palette")
            .test_support()
            .absolute()
            .inset_0()
            .flex()
            .justify_center()
            .bg(theme.overlay)
            .on_mouse_down(
                gpui_kit::MouseButton::Left,
                cx.listener(|this, _event, window, cx| this.close_palette(window, cx)),
            )
            .child(
                div()
                    .id("command-palette-panel")
                    .test_support()
                    .mt(px(96.))
                    .w(px(560.))
                    .max_h(px(480.))
                    .occlude()
                    .child(command),
            )
    }
}

impl Focusable for SchematicShell {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl Render for SchematicShell {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        self.stats.tick();

        // The theme is a gpui global and anything can change it — a host, a
        // settings dialog, the system appearance. The canvas palette is not
        // part of that global, so the shell reconciles the two here rather than
        // only when its own toggle is used.
        let mode = Theme::global(cx).mode;
        if mode != self.theme_mode {
            self.theme_mode = mode;
            let palette = CanvasPalette::for_mode(mode);
            self.canvas.update(cx, |canvas, cx| {
                canvas.set_palette(palette);
                cx.notify();
            });
        }

        if self.free_run {
            // Free-runs at the display rate: Wayland drives this from real
            // frame callbacks, X11 from the RandR refresh timer.
            window.request_animation_frame();
        }

        let theme = cx.theme();
        div()
            .id("schematic-shell")
            .test_support()
            .track_focus(&self.focus_handle)
            .key_context("SchematicEditor")
            .size_full()
            .relative()
            .v_flex()
            .font_family(theme.font_family.clone())
            .text_color(theme.foreground)
            .bg(theme.background)
            .on_action(cx.listener(Self::on_run_action))
            .on_action(cx.listener(Self::on_quit))
            .on_action(cx.listener(Self::on_zoom_in))
            .on_action(cx.listener(Self::on_zoom_out))
            .on_action(cx.listener(Self::on_zoom_to_fit))
            .on_action(cx.listener(Self::on_zoom_to_objects))
            .on_action(cx.listener(Self::on_zoom_actual_size))
            .on_action(cx.listener(Self::on_toggle_grid))
            .on_action(cx.listener(Self::on_cycle_grid))
            .on_action(cx.listener(Self::on_toggle_units))
            .on_action(cx.listener(Self::on_toggle_theme))
            .on_action(cx.listener(Self::on_toggle_left_panel))
            .on_action(cx.listener(Self::on_toggle_right_panel))
            .on_action(cx.listener(Self::on_toggle_frame_stats))
            .on_action(cx.listener(Self::on_open_palette))
            .on_action(cx.listener(Self::on_cancel_tool))
            .child(self.render_menu_row(cx))
            .child(self.render_toolbar(cx))
            .child(
                div()
                    .id("workspace")
                    .test_support()
                    .h_flex()
                    .flex_1()
                    .min_h(px(120.))
                    .overflow_hidden()
                    .child(self.render_tool_palette(cx))
                    .child(div().flex_1().size_full().child(self.dock.clone())),
            )
            .child(self.render_status_bar(cx))
            .when(self.palette_open, |this| {
                this.child(self.render_palette(cx))
            })
    }
}

/// Presentation is resolved once for every surface from the same registry.
struct ActionPresentation {
    label: String,
    tooltip: String,
    available: bool,
}

fn action_presentation(
    cx: &App,
    id: &str,
    fallback_label: &str,
    fallback_description: &str,
    fallback_key: Option<&str>,
) -> ActionPresentation {
    let (label, description, key, available) = match cx.try_global::<ActionRegistry>() {
        Some(registry) => match registry.get(id) {
            Some(info) => (
                info.label.as_str(),
                info.description.as_str(),
                (!info.hotkey.is_empty()).then_some(info.hotkey.as_str()),
                true,
            ),
            None => (
                fallback_label,
                "Action unavailable in this host",
                None,
                false,
            ),
        },
        None => (fallback_label, fallback_description, fallback_key, true),
    };
    let mut tooltip = match key {
        Some(key) => format!("{label} ({})", pretty_key(key)),
        None => label.to_owned(),
    };
    if !description.is_empty() && description != label {
        tooltip.push('\n');
        tooltip.push_str(description);
    }
    ActionPresentation {
        label: label.to_owned(),
        tooltip,
        available,
    }
}

fn registry_action_button(
    id: &'static str,
    icon: IconName,
    fallback_label: &'static str,
    action_id: &'static str,
    cx: &App,
) -> Button {
    let presentation = action_presentation(cx, action_id, fallback_label, "", None);
    Button::new(id)
        .ghost()
        .with_size(px(30.))
        .icon(icon)
        .accessibility_label(presentation.label)
        .tooltip(presentation.tooltip)
        .disabled(!presentation.available)
        .on_click(dispatch(Box::new(RunAction::new(action_id))))
}

/// A ghost icon button that dispatches `action` when clicked.
fn action_button(
    id: &'static str,
    icon: IconName,
    tooltip: &'static str,
    action: Box<dyn Action>,
    cx: &App,
) -> Button {
    let resolved = cx.try_global::<ActionRegistry>().and_then(|registry| {
        commands::all_commands().into_iter().find_map(|(_, spec)| {
            (spec.action().name() == action.name())
                .then(|| registry.resolve(&spec))
                .flatten()
        })
    });
    let label = resolved
        .as_ref()
        .map_or(tooltip, |spec| spec.label.as_str())
        .to_owned();
    let tooltip = resolved.map_or_else(
        || tooltip.to_owned(),
        |spec| {
            if spec.description.is_empty() {
                spec.label
            } else {
                format!("{}\n{}", spec.label, spec.description)
            }
        },
    );
    let for_tooltip = action.boxed_clone();
    Button::new(id)
        .ghost()
        // An explicit size rather than `small()`: the icon is three quarters of
        // it, and the library's small preset leaves a 12 px glyph that is all
        // but invisible on a light background.
        .with_size(px(30.))
        .icon(icon)
        .accessibility_label(label)
        .tooltip_with_action(tooltip, for_tooltip.as_ref(), None)
        .on_click(dispatch(action))
}

/// A click handler that dispatches `action` through the window.
///
/// Everything the chrome does goes through the action system rather than
/// calling into the shell directly, so a toolbar button, a menu item, a
/// palette row and a hotkey are all the same event by the time anything
/// observes them.
fn dispatch(action: Box<dyn Action>) -> impl Fn(&ClickEvent, &mut Window, &mut App) + 'static {
    move |_event, window, cx| {
        window.dispatch_action(action.boxed_clone(), cx);
    }
}

fn toolbar_separator(cx: &App) -> impl IntoElement {
    div().w(px(1.)).h(px(18.)).mx_1().bg(cx.theme().border)
}

fn status_divider(cx: &App) -> impl IntoElement {
    div().w(px(1.)).h(px(12.)).bg(cx.theme().border)
}

/// Render a gpui keystroke string the way a menu would.
fn pretty_key(key: &str) -> String {
    key.split('-')
        .map(|part| match part {
            "ctrl" => "Ctrl".to_string(),
            "shift" => "Shift".to_string(),
            "alt" => "Alt".to_string(),
            "cmd" | "platform" => "Cmd".to_string(),
            "escape" => "Esc".to_string(),
            other => {
                let mut chars = other.chars();
                match chars.next() {
                    Some(first) => first.to_uppercase().collect::<String>() + chars.as_str(),
                    None => String::new(),
                }
            }
        })
        .collect::<Vec<_>>()
        .join("+")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn keystrokes_render_the_way_a_menu_shows_them() {
        assert_eq!(pretty_key("ctrl-shift-p"), "Ctrl+Shift+P");
        assert_eq!(pretty_key("w"), "W");
        assert_eq!(pretty_key("escape"), "Esc");
    }
}
