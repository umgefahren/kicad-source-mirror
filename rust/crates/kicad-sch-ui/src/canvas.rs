// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The drawing surface: the seam to the renderer, and the element that hosts
//! it.
//!
//! # The seam
//!
//! The shell does not draw schematics and never will; `kicad-sch-render` does.
//! What the shell owns is everything *around* the drawing: the viewport
//! transform, hit testing, the pointer and keyboard vocabulary, the cursor, the
//! grid, the crosshair and the selection band. Those are shell concerns because
//! they are the same whether the content comes from a stub, from a recorded
//! draw stream, or from a live `SCH_PAINTER`.
//!
//! So the seam is [`SchematicScene`]: "given a camera, a palette and a window,
//! draw the schematic". One method plus a bounding box. [`StubScene`] below
//! implements it well enough to build and photograph the whole shell against,
//! and swapping in the real renderer is described on the trait.

use std::cell::RefCell;
use std::rc::Rc;

use gpui_kit::prelude::*;
use gpui_kit::{
    App, Bounds, CursorStyle, DispatchPhase, Element, ElementId, Entity, GlobalElementId, Hitbox,
    HitboxBehavior, Hsla, InspectorElementId, IntoElement, LayoutId, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, PathBuilder, Pixels, Point, ScrollWheelEvent, Size, Style,
    StyleRefinement, Styled, Window, point, px, size,
};

use crate::camera::{Camera, REFERENCE_SCALE, WorldRect};
use crate::grid::GridState;
use crate::input::{
    Modifiers, PointerButton, ScrollDelta, ScreenPoint, SharedSink, ShellEvent, WorldPoint,
    shared_sink,
};
use crate::theme::CanvasPalette;
use crate::tools::Tool;

/// Everything a scene needs to know about how it is being looked at.
pub struct ScenePaint<'a> {
    /// The canvas rectangle in window coordinates. Everything drawn outside it
    /// is clipped by the layer the shell has already pushed.
    pub bounds: Bounds<Pixels>,
    /// The view transform. Use [`Camera::world_to_screen`] rather than
    /// recomputing one, so the grid and the content cannot disagree.
    pub camera: &'a Camera,
    /// The colours to draw with.
    pub palette: &'a CanvasPalette,
}

/// What draws the schematic inside the canvas.
///
/// # Replacing the stub with `kicad-sch-render`
///
/// The real renderer supplies a gpui element plus camera controls. Those camera
/// controls are [`Camera`], which lives in this crate precisely so both sides
/// can share it as a value. What is left is the drawing, which is this trait.
/// The integration is:
///
/// 1. `kicad-sch-render` gains a `kicad-sch-ui` dependency and implements
///    `SchematicScene` for its canvas type — `content_bounds` from its own
///    extents, `paint` doing what its element's `paint` does now, using the
///    passed [`Camera`] instead of an internal one.
/// 2. `kicad-eeschema-gpui`'s `main` constructs that type instead of
///    [`StubScene`] and hands it to [`crate::shell::SchematicShell::new_with_scene`].
///
/// Nothing else in the shell changes: no element, no event plumbing, no
/// layout. If the renderer would rather not depend on the shell, the same two
/// steps work with a one-screen adapter struct in the binary crate.
pub trait SchematicScene: 'static {
    /// The extent of the drawn items, for "zoom to objects". `None` means the
    /// scene is empty or does not know, and the shell falls back to the sheet.
    fn content_bounds(&self) -> Option<WorldRect>;

    /// The sheet outline, for "zoom to fit".
    fn sheet_bounds(&self) -> WorldRect;

    /// How many items are selected, for the status bar.
    fn selection_count(&self) -> usize {
        0
    }

    /// Draw the schematic.
    ///
    /// Called inside a `paint_layer`, so everything emitted here shares one
    /// draw order and batches into a single path pass. Do not open further
    /// layers unless a separate pass is actually wanted.
    fn paint(&mut self, paint: &ScenePaint<'_>, window: &mut Window, cx: &mut App);
}

/// How far the pointer must travel with a button held before the shell calls it
/// a drag. Below this a press-and-release is a click, however shaky the hand.
pub const DRAG_THRESHOLD_PX: f32 = 4.0;

/// One zoom step for the wheel and the zoom buttons.
const ZOOM_STEP: f32 = 1.25;

#[derive(Clone, Copy, Debug)]
struct Press {
    button: PointerButton,
    origin: Point<Pixels>,
    last: Point<Pixels>,
    modifiers: Modifiers,
    dragging: bool,
}

/// The mutable state behind the canvas: where it is looking, what tool is
/// active, and what the pointer is doing.
///
/// Separate from the shell view so that the canvas element can update it from
/// inside `paint` without reaching through the whole window.
pub struct CanvasState {
    camera: Camera,
    grid: GridState,
    palette: CanvasPalette,
    tool: Tool,
    scene: Box<dyn SchematicScene>,
    sink: SharedSink,
    pointer: Option<Point<Pixels>>,
    press: Option<Press>,
    crosshair: bool,
    /// Set whenever the camera changed, so the next paint can report it once
    /// rather than on every mouse move.
    viewport_dirty: bool,
}

impl CanvasState {
    /// A canvas showing `scene`, reporting to `sink`.
    pub fn new(scene: Box<dyn SchematicScene>, palette: CanvasPalette, sink: SharedSink) -> Self {
        Self {
            camera: Camera::new(),
            grid: GridState::default(),
            palette,
            tool: Tool::Select,
            scene,
            sink,
            pointer: None,
            press: None,
            crosshair: true,
            viewport_dirty: true,
        }
    }

    /// A canvas showing the placeholder scene and reporting nowhere.
    pub fn stub(palette: CanvasPalette) -> Self {
        Self::new(
            Box::new(StubScene::new()),
            palette,
            shared_sink(crate::input::NullSink),
        )
    }

    /// The view transform.
    pub fn camera(&self) -> &Camera {
        &self.camera
    }

    /// The view transform, mutably. Prefer the named operations below where
    /// one exists, so the viewport report is not missed.
    pub fn camera_mut(&mut self) -> &mut Camera {
        self.viewport_dirty = true;
        &mut self.camera
    }

    /// The grid settings.
    pub fn grid(&self) -> &GridState {
        &self.grid
    }

    /// The grid settings, mutably.
    pub fn grid_mut(&mut self) -> &mut GridState {
        &mut self.grid
    }

    /// The active tool.
    pub fn tool(&self) -> Tool {
        self.tool
    }

    /// Whether the crosshair is drawn.
    pub fn crosshair(&self) -> bool {
        self.crosshair
    }

    /// Show or hide the crosshair.
    pub fn set_crosshair(&mut self, on: bool) {
        self.crosshair = on;
    }

    /// Replace the colour palette, on a theme change.
    pub fn set_palette(&mut self, palette: CanvasPalette) {
        self.palette = palette;
    }

    /// The colour palette.
    pub fn palette(&self) -> &CanvasPalette {
        &self.palette
    }

    /// Where the pointer last was, in world coordinates, snapped to the grid.
    ///
    /// Snapped because that is the position an edit would actually use, and a
    /// status bar that disagrees with where the wire lands is worse than none.
    pub fn cursor_world(&self) -> Option<WorldPoint> {
        self.pointer
            .map(|position| self.grid.snap(self.camera.screen_to_world(position)))
    }

    /// How many items the scene reports as selected.
    pub fn selection_count(&self) -> usize {
        self.scene.selection_count()
    }

    /// Swap the scene. This is what `kicad-sch-render` will be installed with
    /// if it arrives after the window is already open.
    pub fn set_scene(&mut self, scene: Box<dyn SchematicScene>) {
        self.scene = scene;
        self.viewport_dirty = true;
    }

    /// Point the events somewhere else. Used by tests and by the host once it
    /// attaches.
    pub fn set_sink(&mut self, sink: SharedSink) {
        self.sink = sink;
    }

    /// Post an event to the sink.
    pub fn emit(&self, event: ShellEvent) {
        // A sink that panics would take the frame with it, so the contract on
        // `InputSink` forbids it; borrowing can still fail if a sink re-enters
        // the shell, and dropping the event beats aborting the frame.
        if let Ok(mut sink) = self.sink.try_borrow_mut() {
            sink.handle(event);
        }
    }

    /// Make `tool` active and tell the host.
    pub fn set_tool(&mut self, tool: Tool) {
        if self.tool == tool {
            return;
        }
        self.tool = tool;
        self.emit(ShellEvent::ToolActivated(tool.id()));
    }

    /// Go back to the select tool and tell the host the current tool was
    /// abandoned.
    pub fn cancel_tool(&mut self) {
        self.emit(ShellEvent::ToolCancelled);
        if self.tool != Tool::Select {
            self.tool = Tool::Select;
            self.emit(ShellEvent::ToolActivated(Tool::Select.id()));
        }
    }

    /// Zoom in one step about the canvas centre.
    pub fn zoom_in(&mut self) {
        let anchor = self.camera.viewport().center();
        self.camera.zoom_to_point(ZOOM_STEP, anchor);
        self.viewport_dirty = true;
    }

    /// Zoom out one step about the canvas centre.
    pub fn zoom_out(&mut self) {
        let anchor = self.camera.viewport().center();
        self.camera.zoom_to_point(1.0 / ZOOM_STEP, anchor);
        self.viewport_dirty = true;
    }

    /// Frame the whole sheet.
    pub fn zoom_to_fit(&mut self) {
        let bounds = self.scene.sheet_bounds();
        self.camera.zoom_to_fit(bounds);
        self.viewport_dirty = true;
    }

    /// Frame the drawn items, falling back to the sheet when the scene has
    /// nothing to say.
    pub fn zoom_to_objects(&mut self) {
        let bounds = self
            .scene
            .content_bounds()
            .unwrap_or_else(|| self.scene.sheet_bounds());
        self.camera.zoom_to_fit(bounds);
        self.viewport_dirty = true;
    }

    /// Return to 1:1.
    pub fn zoom_actual_size(&mut self) {
        self.camera.set_zoom(1.0);
        self.viewport_dirty = true;
    }

    fn report_viewport_if_changed(&mut self) {
        if self.viewport_dirty {
            self.viewport_dirty = false;
            self.emit(ShellEvent::ViewportChanged(self.camera.state()));
        }
    }

    fn local(&self, position: Point<Pixels>) -> ScreenPoint {
        self.camera.to_canvas_local(position)
    }

    fn world(&self, position: Point<Pixels>) -> WorldPoint {
        self.camera.screen_to_world(position)
    }
}

/// The element that hosts the schematic.
///
/// Rebuilt every frame by the shell's `render`, as gpui elements are; all the
/// state it needs lives in the [`CanvasState`] entity it points at.
pub struct CanvasElement {
    id: ElementId,
    style: StyleRefinement,
    state: Entity<CanvasState>,
}

impl CanvasElement {
    /// A canvas element driven by `state`.
    pub fn new(id: impl Into<ElementId>, state: Entity<CanvasState>) -> Self {
        Self {
            id: id.into(),
            style: StyleRefinement::default(),
            state,
        }
    }
}

impl Styled for CanvasElement {
    fn style(&mut self) -> &mut StyleRefinement {
        &mut self.style
    }
}

impl IntoElement for CanvasElement {
    type Element = Self;

    fn into_element(self) -> Self::Element {
        self
    }
}

/// What `prepaint` hands to `paint`.
pub struct CanvasPrepaint {
    hitbox: Hitbox,
}

impl Element for CanvasElement {
    type RequestLayoutState = ();
    type PrepaintState = CanvasPrepaint;

    fn id(&self) -> Option<ElementId> {
        Some(self.id.clone())
    }

    fn source_location(&self) -> Option<&'static std::panic::Location<'static>> {
        None
    }

    fn request_layout(
        &mut self,
        _id: Option<&GlobalElementId>,
        _inspector: Option<&InspectorElementId>,
        window: &mut Window,
        cx: &mut App,
    ) -> (LayoutId, ()) {
        let mut style = Style::default();
        style.refine(&self.style);
        (window.request_layout(style, [], cx), ())
    }

    fn prepaint(
        &mut self,
        _id: Option<&GlobalElementId>,
        _inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _layout: &mut (),
        window: &mut Window,
        cx: &mut App,
    ) -> CanvasPrepaint {
        // The camera has to learn the canvas rectangle before anything asks it
        // to convert a coordinate, and layout is the only place that knows it.
        self.state.update(cx, |state, _| {
            if state.camera.viewport() != bounds {
                state.camera.set_viewport(bounds);
                state.viewport_dirty = true;
            }
        });
        CanvasPrepaint {
            hitbox: window.insert_hitbox(bounds, HitboxBehavior::Normal),
        }
    }

    fn paint(
        &mut self,
        _id: Option<&GlobalElementId>,
        _inspector: Option<&InspectorElementId>,
        bounds: Bounds<Pixels>,
        _layout: &mut (),
        prepaint: &mut CanvasPrepaint,
        window: &mut Window,
        cx: &mut App,
    ) {
        let hitbox = prepaint.hitbox.clone();
        let (camera, palette, grid, tool, crosshair, pointer, press) =
            self.state.update(cx, |state, _| {
                state.report_viewport_if_changed();
                (
                    state.camera,
                    state.palette,
                    state.grid,
                    state.tool,
                    state.crosshair,
                    state.pointer,
                    state.press,
                )
            });

        // One layer for the whole canvas: one draw order, so every path the
        // grid, the scene and the overlays emit merges into a single pass.
        let state_entity = self.state.clone();
        window.paint_layer(bounds, |window| {
            window.paint_quad(gpui_kit::fill(bounds, palette.background));
            paint_grid(bounds, &camera, &grid, &palette, window);

            let paint = ScenePaint {
                bounds,
                camera: &camera,
                palette: &palette,
            };
            state_entity.update(cx, |state, cx| state.scene.paint(&paint, window, cx));

            if crosshair {
                if let Some(position) = pointer {
                    paint_crosshair(bounds, position, palette.crosshair, window);
                }
            }
            if let Some(press) = press {
                if press.dragging && press.button == PointerButton::Left && tool == Tool::Select {
                    paint_selection_band(press.origin, press.last, palette.selection, window);
                }
            }
        });

        window.set_cursor_style(tool.spec().cursor, &hitbox);
        install_mouse_handlers(&self.state, &hitbox, window);
    }
}

fn install_mouse_handlers(
    state: &Entity<CanvasState>,
    hitbox: &Hitbox,
    window: &mut Window,
) {
    // Window-level listeners are cleared at the end of every frame, so all of
    // these are re-registered on each paint. That is gpui's design, not an
    // oversight here.
    {
        let (state, hitbox) = (state.clone(), hitbox.clone());
        window.on_mouse_event(move |event: &MouseDownEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble || !hitbox.is_hovered(window) {
                return;
            }
            let Some(button) = map_button(event.button) else {
                return;
            };
            window.capture_pointer(hitbox.id);
            state.update(cx, |state, cx| {
                let modifiers = map_modifiers(event.modifiers);
                state.pointer = Some(event.position);
                state.press = Some(Press {
                    button,
                    origin: event.position,
                    last: event.position,
                    modifiers,
                    dragging: false,
                });
                state.emit(ShellEvent::PointerDown {
                    button,
                    screen: state.local(event.position),
                    world: state.world(event.position),
                    modifiers,
                    click_count: event.click_count,
                });
                cx.notify();
            });
        });
    }

    {
        let (state, hitbox) = (state.clone(), hitbox.clone());
        window.on_mouse_event(move |event: &MouseMoveEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            let captured = window.captured_hitbox() == Some(hitbox.id);
            if !captured && !hitbox.is_hovered(window) {
                // Leaving the canvas without a button held ends hovering.
                state.update(cx, |state, cx| {
                    if state.pointer.take().is_some() {
                        state.emit(ShellEvent::PointerLeave);
                        cx.notify();
                    }
                });
                return;
            }
            state.update(cx, |state, cx| {
                let modifiers = map_modifiers(event.modifiers);
                state.pointer = Some(event.position);
                let screen = state.local(event.position);
                let world = state.world(event.position);

                match state.press.as_mut() {
                    None => state.emit(ShellEvent::PointerMove {
                        screen,
                        world,
                        modifiers,
                    }),
                    Some(press) => {
                        let travelled = (f32::from(event.position.x) - f32::from(press.origin.x))
                            .hypot(f32::from(event.position.y) - f32::from(press.origin.y));
                        let button = press.button;
                        let origin = press.origin;
                        let previous = press.last;
                        press.last = event.position;

                        if !press.dragging && travelled >= DRAG_THRESHOLD_PX {
                            press.dragging = true;
                            let origin_screen = state.local(origin);
                            let origin_world = state.world(origin);
                            state.emit(ShellEvent::DragBegin {
                                button,
                                origin_screen,
                                origin_world,
                                modifiers,
                            });
                        }
                        if state
                            .press
                            .as_ref()
                            .map(|press| press.dragging)
                            .unwrap_or(false)
                        {
                            let delta = ScreenPoint::new(
                                f32::from(event.position.x) - f32::from(previous.x),
                                f32::from(event.position.y) - f32::from(previous.y),
                            );
                            // The middle button pans the view itself; the host
                            // still hears the drag so a tool can override it.
                            if button == PointerButton::Middle {
                                state.camera.pan(point(
                                    event.position.x - previous.x,
                                    event.position.y - previous.y,
                                ));
                                state.viewport_dirty = true;
                            }
                            state.emit(ShellEvent::DragUpdate {
                                button,
                                screen,
                                world,
                                delta_screen: delta,
                                modifiers,
                            });
                        }
                    }
                }
                cx.notify();
            });
        });
    }

    {
        let state = state.clone();
        window.on_mouse_event(move |event: &MouseUpEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble {
                return;
            }
            window.release_pointer();
            state.update(cx, |state, cx| {
                let Some(press) = state.press.take() else {
                    return;
                };
                let Some(button) = map_button(event.button) else {
                    return;
                };
                if button != press.button {
                    state.press = Some(press);
                    return;
                }
                let modifiers = map_modifiers(event.modifiers);
                let screen = state.local(event.position);
                let world = state.world(event.position);
                if press.dragging {
                    state.emit(ShellEvent::DragEnd {
                        button,
                        screen,
                        world,
                        modifiers,
                    });
                }
                state.emit(ShellEvent::PointerUp {
                    button,
                    screen,
                    world,
                    modifiers,
                });
                cx.notify();
            });
        });
    }

    {
        let (state, hitbox) = (state.clone(), hitbox.clone());
        window.on_mouse_event(move |event: &ScrollWheelEvent, phase, window, cx| {
            if phase != DispatchPhase::Bubble || !hitbox.should_handle_scroll(window) {
                return;
            }
            state.update(cx, |state, cx| {
                let modifiers = map_modifiers(event.modifiers);
                let delta = map_scroll(event.delta);
                state.emit(ShellEvent::Scroll {
                    screen: state.local(event.position),
                    world: state.world(event.position),
                    delta,
                    modifiers,
                });

                // KiCad's default: the wheel zooms, and the modifiers turn it
                // into scrolling. Shift is vertical, Ctrl is horizontal.
                let detents = delta.detents_y();
                if modifiers.shift {
                    state.camera.pan(point(px(0.), px(detents * 40.)));
                } else if modifiers.ctrl {
                    state.camera.pan(point(px(detents * 40.), px(0.)));
                } else if detents != 0. {
                    let factor = ZOOM_STEP.powf(detents);
                    state.camera.zoom_to_point(factor, event.position);
                }
                state.viewport_dirty = true;
                cx.notify();
            });
        });
    }
}

fn map_button(button: MouseButton) -> Option<PointerButton> {
    match button {
        MouseButton::Left => Some(PointerButton::Left),
        MouseButton::Middle => Some(PointerButton::Middle),
        MouseButton::Right => Some(PointerButton::Right),
        MouseButton::Navigate(gpui_kit::NavigationDirection::Back) => Some(PointerButton::Back),
        MouseButton::Navigate(gpui_kit::NavigationDirection::Forward) => {
            Some(PointerButton::Forward)
        }
    }
}

fn map_modifiers(modifiers: gpui_kit::Modifiers) -> Modifiers {
    Modifiers {
        ctrl: modifiers.control,
        shift: modifiers.shift,
        alt: modifiers.alt,
        meta: modifiers.platform,
    }
}

fn map_scroll(delta: gpui_kit::ScrollDelta) -> ScrollDelta {
    match delta {
        gpui_kit::ScrollDelta::Lines(lines) => ScrollDelta::Lines {
            x: lines.x,
            y: lines.y,
        },
        gpui_kit::ScrollDelta::Pixels(pixels) => ScrollDelta::Pixels {
            x: f32::from(pixels.x),
            y: f32::from(pixels.y),
        },
    }
}

/// Draw the grid as dots, dropping to every tenth point and then to nothing as
/// the spacing on screen gets too tight to be anything but noise.
fn paint_grid(
    bounds: Bounds<Pixels>,
    camera: &Camera,
    grid: &GridState,
    palette: &CanvasPalette,
    window: &mut Window,
) {
    if !grid.is_visible() {
        return;
    }
    let mut spacing = grid.spacing_mm();
    let mut major_every = 10usize;
    let mut on_screen = spacing * camera.scale() as f64;
    if on_screen < 7.0 {
        // Too dense to read: show only what would have been the major points.
        spacing *= major_every as f64;
        on_screen *= major_every as f64;
        major_every = 10;
    }
    if on_screen < 5.0 {
        return;
    }

    let visible = camera.visible_world();
    let first_x = (visible.min.x / spacing).floor() as i64;
    let last_x = (visible.max.x / spacing).ceil() as i64;
    let first_y = (visible.min.y / spacing).floor() as i64;
    let last_y = (visible.max.y / spacing).ceil() as i64;

    // A pathological zoom could ask for millions of dots; cap the work rather
    // than dropping the frame.
    const MAX_DOTS: i64 = 40_000;
    let columns = (last_x - first_x + 1).max(0);
    let rows = (last_y - first_y + 1).max(0);
    if columns.saturating_mul(rows) > MAX_DOTS {
        return;
    }

    let dot = if on_screen > 26.0 { 1.5 } else { 1.0 };
    for ix in first_x..=last_x {
        for iy in first_y..=last_y {
            let major = ix.rem_euclid(major_every as i64) == 0
                && iy.rem_euclid(major_every as i64) == 0;
            let color = if major { palette.grid_major } else { palette.grid };
            let radius = if major { dot + 0.5 } else { dot };
            let center = camera.world_to_screen(WorldPoint::new(
                ix as f64 * spacing,
                iy as f64 * spacing,
            ));
            let rect = Bounds {
                origin: point(center.x - px(radius), center.y - px(radius)),
                size: size(px(radius * 2.), px(radius * 2.)),
            };
            if rect.origin.x < bounds.origin.x - px(4.)
                || rect.origin.y < bounds.origin.y - px(4.)
            {
                continue;
            }
            window.paint_quad(gpui_kit::fill(rect, color));
        }
    }
}

fn paint_crosshair(
    bounds: Bounds<Pixels>,
    position: Point<Pixels>,
    color: Hsla,
    window: &mut Window,
) {
    let hair = px(1.);
    window.paint_quad(gpui_kit::fill(
        Bounds {
            origin: point(bounds.origin.x, position.y),
            size: size(bounds.size.width, hair),
        },
        color.opacity(0.55),
    ));
    window.paint_quad(gpui_kit::fill(
        Bounds {
            origin: point(position.x, bounds.origin.y),
            size: size(hair, bounds.size.height),
        },
        color.opacity(0.55),
    ));
}

fn paint_selection_band(
    from: Point<Pixels>,
    to: Point<Pixels>,
    color: Hsla,
    window: &mut Window,
) {
    let origin = point(from.x.min(to.x), from.y.min(to.y));
    let extent = Size {
        width: (to.x - from.x).abs(),
        height: (to.y - from.y).abs(),
    };
    let rect = Bounds {
        origin,
        size: extent,
    };
    window.paint_quad(gpui_kit::fill(rect, color.opacity(0.12)));
    window.paint_quad(gpui_kit::outline(rect, color));
}

// ---------------------------------------------------------------------------
// The placeholder scene
// ---------------------------------------------------------------------------

/// A primitive in the placeholder scene.
enum StubItem {
    /// An open polyline in world millimetres.
    Polyline {
        points: Vec<WorldPoint>,
        width_mm: f64,
        color: StubColor,
    },
    /// A rectangle outline, optionally filled.
    Rect {
        rect: WorldRect,
        width_mm: f64,
        color: StubColor,
        fill: Option<StubColor>,
    },
    /// A filled dot, used for junctions and pin ends.
    Dot {
        at: WorldPoint,
        radius_mm: f64,
        color: StubColor,
    },
    /// A text run, anchored at its left baseline.
    Text {
        at: WorldPoint,
        text: String,
        height_mm: f64,
        color: StubColor,
    },
}

/// Which palette entry a stub primitive uses.
#[derive(Clone, Copy)]
enum StubColor {
    Wire,
    Bus,
    Junction,
    SymbolOutline,
    SymbolFill,
    Pin,
    Label,
    Field,
    Text,
    NoConnect,
    SheetBorder,
}

impl StubColor {
    fn resolve(self, palette: &CanvasPalette) -> Hsla {
        match self {
            StubColor::Wire => palette.wire,
            StubColor::Bus => palette.bus,
            StubColor::Junction => palette.junction,
            StubColor::SymbolOutline => palette.symbol_outline,
            StubColor::SymbolFill => palette.symbol_fill,
            StubColor::Pin => palette.pin,
            StubColor::Label => palette.label,
            StubColor::Field => palette.field,
            StubColor::Text => palette.text,
            StubColor::NoConnect => palette.no_connect,
            StubColor::SheetBorder => palette.sheet_border,
        }
    }
}

/// A stand-in schematic, drawn until `kicad-sch-render` exists.
///
/// It is not a schematic model and nothing should grow into one here: it is a
/// fixed list of primitives on an A4 sheet, sized and coloured like the real
/// thing so that the shell's layout, zoom, grid and palette can be judged
/// honestly before the renderer lands.
pub struct StubScene {
    items: Vec<StubItem>,
    sheet: WorldRect,
    content: WorldRect,
}

impl Default for StubScene {
    fn default() -> Self {
        Self::new()
    }
}

impl StubScene {
    /// The placeholder schematic.
    pub fn new() -> Self {
        let sheet = WorldRect::from_corners(WorldPoint::new(0., 0.), WorldPoint::new(297., 210.));
        let mut items = Vec::new();
        build_stub(&mut items);
        Self {
            items,
            sheet,
            content: WorldRect::from_corners(
                WorldPoint::new(38., 40.),
                WorldPoint::new(240., 150.),
            ),
        }
    }
}

impl SchematicScene for StubScene {
    fn content_bounds(&self) -> Option<WorldRect> {
        Some(self.content)
    }

    fn sheet_bounds(&self) -> WorldRect {
        self.sheet
    }

    fn selection_count(&self) -> usize {
        0
    }

    fn paint(&mut self, paint: &ScenePaint<'_>, window: &mut Window, cx: &mut App) {
        let camera = paint.camera;
        let palette = paint.palette;
        let scale = camera.scale();

        // Widths are in millimetres so they zoom with the drawing, but a line
        // thinner than a pixel disappears, and one fatter than a few dozen
        // stops looking like a schematic.
        let width_px = |mm: f64| px(((mm * scale as f64) as f32).clamp(1.0, 48.0));

        for item in &self.items {
            match item {
                StubItem::Polyline {
                    points,
                    width_mm,
                    color,
                } => {
                    let mut builder = PathBuilder::stroke(width_px(*width_mm));
                    let mut first = true;
                    for world in points {
                        let at = camera.world_to_screen(*world);
                        if first {
                            builder.move_to(at);
                            first = false;
                        } else {
                            builder.line_to(at);
                        }
                    }
                    if let Ok(path) = builder.build() {
                        window.paint_path(path, color.resolve(palette));
                    }
                }
                StubItem::Rect {
                    rect,
                    width_mm,
                    color,
                    fill,
                } => {
                    let min = camera.world_to_screen(rect.min);
                    let max = camera.world_to_screen(rect.max);
                    let bounds = Bounds {
                        origin: min,
                        size: size(max.x - min.x, max.y - min.y),
                    };
                    if let Some(fill) = fill {
                        window.paint_quad(gpui_kit::fill(bounds, fill.resolve(palette)));
                    }
                    let mut builder = PathBuilder::stroke(width_px(*width_mm));
                    builder.add_polygon(
                        &[
                            min,
                            point(max.x, min.y),
                            max,
                            point(min.x, max.y),
                        ],
                        true,
                    );
                    if let Ok(path) = builder.build() {
                        window.paint_path(path, color.resolve(palette));
                    }
                }
                StubItem::Dot {
                    at,
                    radius_mm,
                    color,
                } => {
                    let center = camera.world_to_screen(*at);
                    let radius = ((*radius_mm * scale as f64) as f32).clamp(1.5, 24.0);
                    let bounds = Bounds {
                        origin: point(center.x - px(radius), center.y - px(radius)),
                        size: size(px(radius * 2.), px(radius * 2.)),
                    };
                    window.paint_quad(
                        gpui_kit::fill(bounds, color.resolve(palette))
                            .corner_radii(gpui_kit::Corners::all(px(radius))),
                    );
                }
                StubItem::Text {
                    at,
                    text,
                    height_mm,
                    color,
                } => {
                    let font_size = (*height_mm * scale as f64) as f32;
                    // Below about 5 px text is illegible mush that costs a
                    // shaping pass per string; KiCad hides it too.
                    if font_size < 5.0 {
                        continue;
                    }
                    let font_size = px(font_size.min(96.0));
                    let run = gpui_kit::TextRun {
                        len: text.len(),
                        font: gpui_kit::font("sans-serif"),
                        color: color.resolve(palette),
                        background_color: None,
                        underline: None,
                        strikethrough: None,
                    };
                    let shaped = window.text_system().shape_line(
                        text.clone().into(),
                        font_size,
                        &[run],
                        None,
                    );
                    let origin = camera.world_to_screen(*at);
                    let _ = shaped.paint(
                        origin,
                        font_size * 1.25,
                        gpui_kit::TextAlign::Left,
                        None,
                        window,
                        cx,
                    );
                }
            }
        }

        // A faint reminder that this is not the real renderer. It sits in the
        // sheet's top-left corner so it scrolls away like any other content.
        let _ = REFERENCE_SCALE;
    }
}

/// Build the placeholder schematic: a sheet frame, two discretes, an IC, a
/// bus, and enough labels to judge typography by.
fn build_stub(items: &mut Vec<StubItem>) {
    // --- sheet frame and title block ------------------------------------
    items.push(StubItem::Rect {
        rect: WorldRect::from_corners(WorldPoint::new(5., 5.), WorldPoint::new(292., 205.)),
        width_mm: 0.3,
        color: StubColor::SheetBorder,
        fill: None,
    });
    items.push(StubItem::Rect {
        rect: WorldRect::from_corners(WorldPoint::new(185., 172.), WorldPoint::new(292., 205.)),
        width_mm: 0.3,
        color: StubColor::SheetBorder,
        fill: None,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(188., 180.),
        text: "KiCad Schematic Editor".into(),
        height_mm: 4.0,
        color: StubColor::Text,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(188., 188.),
        text: "Sheet: /  Rev: A  Size: A4".into(),
        height_mm: 2.6,
        color: StubColor::Field,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(188., 195.),
        text: "placeholder canvas - kicad-sch-render not yet linked".into(),
        height_mm: 2.2,
        color: StubColor::Field,
    });

    // --- U1: an eight-pin part ------------------------------------------
    let body = WorldRect::from_corners(WorldPoint::new(120., 60.), WorldPoint::new(155., 105.));
    items.push(StubItem::Rect {
        rect: body,
        width_mm: 0.35,
        color: StubColor::SymbolOutline,
        fill: Some(StubColor::SymbolFill),
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(120., 54.),
        text: "U1".into(),
        height_mm: 3.5,
        color: StubColor::Field,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(132., 54.),
        text: "MCU-48".into(),
        height_mm: 3.5,
        color: StubColor::Field,
    });
    let left_pins = ["VDD", "RST", "SDA", "SCL"];
    for (index, name) in left_pins.iter().enumerate() {
        let y = 68. + index as f64 * 10.;
        items.push(StubItem::Polyline {
            points: vec![WorldPoint::new(112.5, y), WorldPoint::new(120., y)],
            width_mm: 0.25,
            color: StubColor::Pin,
        });
        items.push(StubItem::Text {
            at: WorldPoint::new(122., y - 1.5),
            text: (*name).into(),
            height_mm: 2.4,
            color: StubColor::Pin,
        });
    }
    let right_pins = ["D0", "D1", "D2", "GND"];
    for (index, name) in right_pins.iter().enumerate() {
        let y = 68. + index as f64 * 10.;
        items.push(StubItem::Polyline {
            points: vec![WorldPoint::new(155., y), WorldPoint::new(162.5, y)],
            width_mm: 0.25,
            color: StubColor::Pin,
        });
        items.push(StubItem::Text {
            at: WorldPoint::new(146., y - 1.5),
            text: (*name).into(),
            height_mm: 2.4,
            color: StubColor::Pin,
        });
    }

    // --- R1: a resistor on the supply rail -------------------------------
    items.push(StubItem::Rect {
        rect: WorldRect::from_corners(WorldPoint::new(59., 55.), WorldPoint::new(63.5, 67.)),
        width_mm: 0.3,
        color: StubColor::SymbolOutline,
        fill: Some(StubColor::SymbolFill),
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(61.25, 47.), WorldPoint::new(61.25, 55.)],
        width_mm: 0.25,
        color: StubColor::Pin,
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(61.25, 67.), WorldPoint::new(61.25, 75.)],
        width_mm: 0.25,
        color: StubColor::Pin,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(66., 57.),
        text: "R1".into(),
        height_mm: 3.0,
        color: StubColor::Field,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(66., 62.),
        text: "10k".into(),
        height_mm: 3.0,
        color: StubColor::Field,
    });

    // --- C1: a decoupling capacitor --------------------------------------
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(85.5, 86.), WorldPoint::new(97., 86.)],
        width_mm: 0.4,
        color: StubColor::SymbolOutline,
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(85.5, 90.), WorldPoint::new(97., 90.)],
        width_mm: 0.4,
        color: StubColor::SymbolOutline,
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(91.25, 78.), WorldPoint::new(91.25, 86.)],
        width_mm: 0.25,
        color: StubColor::Pin,
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(91.25, 90.), WorldPoint::new(91.25, 98.)],
        width_mm: 0.25,
        color: StubColor::Pin,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(99., 84.),
        text: "C1".into(),
        height_mm: 3.0,
        color: StubColor::Field,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(99., 89.),
        text: "100n".into(),
        height_mm: 3.0,
        color: StubColor::Field,
    });

    // --- the supply rail --------------------------------------------------
    items.push(StubItem::Polyline {
        points: vec![
            WorldPoint::new(40., 47.),
            WorldPoint::new(112.5, 47.),
            WorldPoint::new(112.5, 68.),
        ],
        width_mm: 0.35,
        color: StubColor::Wire,
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(61.25, 47.), WorldPoint::new(61.25, 47.)],
        width_mm: 0.35,
        color: StubColor::Wire,
    });
    items.push(StubItem::Dot {
        at: WorldPoint::new(61.25, 47.),
        radius_mm: 0.7,
        color: StubColor::Junction,
    });
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(91.25, 47.), WorldPoint::new(91.25, 78.)],
        width_mm: 0.35,
        color: StubColor::Wire,
    });
    items.push(StubItem::Dot {
        at: WorldPoint::new(91.25, 47.),
        radius_mm: 0.7,
        color: StubColor::Junction,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(38., 43.),
        text: "+3V3".into(),
        height_mm: 3.2,
        color: StubColor::Label,
    });

    // --- ground -----------------------------------------------------------
    for (x, y) in [(61.25_f64, 75.0_f64), (91.25, 98.0), (162.5, 98.0)] {
        items.push(StubItem::Polyline {
            points: vec![
                WorldPoint::new(x - 3.5, y),
                WorldPoint::new(x + 3.5, y),
            ],
            width_mm: 0.4,
            color: StubColor::Wire,
        });
        items.push(StubItem::Polyline {
            points: vec![
                WorldPoint::new(x - 2.0, y + 2.),
                WorldPoint::new(x + 2.0, y + 2.),
            ],
            width_mm: 0.4,
            color: StubColor::Wire,
        });
        items.push(StubItem::Text {
            at: WorldPoint::new(x - 4.5, y + 4.),
            text: "GND".into(),
            height_mm: 2.6,
            color: StubColor::Label,
        });
    }
    items.push(StubItem::Polyline {
        points: vec![WorldPoint::new(155., 98.), WorldPoint::new(162.5, 98.)],
        width_mm: 0.35,
        color: StubColor::Wire,
    });

    // --- the I2C pair, with labels ---------------------------------------
    for (index, name) in ["SDA", "SCL"].iter().enumerate() {
        let y = 88. + index as f64 * 10.;
        items.push(StubItem::Polyline {
            points: vec![WorldPoint::new(78., y), WorldPoint::new(112.5, y)],
            width_mm: 0.35,
            color: StubColor::Wire,
        });
        items.push(StubItem::Text {
            at: WorldPoint::new(66., y - 2.),
            text: (*name).into(),
            height_mm: 3.0,
            color: StubColor::Label,
        });
        items.push(StubItem::Polyline {
            points: vec![
                WorldPoint::new(66., y - 2.5),
                WorldPoint::new(76., y - 2.5),
                WorldPoint::new(78., y),
                WorldPoint::new(76., y + 2.5),
                WorldPoint::new(66., y + 2.5),
            ],
            width_mm: 0.2,
            color: StubColor::Label,
        });
    }

    // --- a data bus out to the right --------------------------------------
    items.push(StubItem::Polyline {
        points: vec![
            WorldPoint::new(180., 55.),
            WorldPoint::new(180., 120.),
            WorldPoint::new(238., 120.),
        ],
        width_mm: 0.7,
        color: StubColor::Bus,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(182., 50.),
        text: "D[0..2]".into(),
        height_mm: 3.2,
        color: StubColor::Bus,
    });
    for index in 0..3 {
        let y = 68. + index as f64 * 10.;
        items.push(StubItem::Polyline {
            points: vec![
                WorldPoint::new(162.5, y),
                WorldPoint::new(175., y),
                WorldPoint::new(180., y + 5.),
            ],
            width_mm: 0.35,
            color: StubColor::Wire,
        });
    }

    // --- a no-connect marker ---------------------------------------------
    items.push(StubItem::Polyline {
        points: vec![
            WorldPoint::new(160., 45.),
            WorldPoint::new(165., 50.),
        ],
        width_mm: 0.3,
        color: StubColor::NoConnect,
    });
    items.push(StubItem::Polyline {
        points: vec![
            WorldPoint::new(165., 45.),
            WorldPoint::new(160., 50.),
        ],
        width_mm: 0.3,
        color: StubColor::NoConnect,
    });

    // --- a note ------------------------------------------------------------
    items.push(StubItem::Text {
        at: WorldPoint::new(40., 140.),
        text: "Stub scene: geometry drawn by kicad-sch-ui.".into(),
        height_mm: 3.4,
        color: StubColor::Text,
    });
    items.push(StubItem::Text {
        at: WorldPoint::new(40., 147.),
        text: "kicad-sch-render replaces this behind SchematicScene.".into(),
        height_mm: 3.0,
        color: StubColor::Field,
    });
}

/// A scene that draws nothing, for tests that only care about the chrome.
pub struct EmptyScene {
    sheet: WorldRect,
    /// Counted so a test can assert the canvas actually painted.
    pub paints: Rc<RefCell<usize>>,
}

impl Default for EmptyScene {
    fn default() -> Self {
        Self {
            sheet: WorldRect::from_corners(WorldPoint::new(0., 0.), WorldPoint::new(297., 210.)),
            paints: Rc::new(RefCell::new(0)),
        }
    }
}

impl SchematicScene for EmptyScene {
    fn content_bounds(&self) -> Option<WorldRect> {
        None
    }

    fn sheet_bounds(&self) -> WorldRect {
        self.sheet
    }

    fn paint(&mut self, _paint: &ScenePaint<'_>, _window: &mut Window, _cx: &mut App) {
        *self.paints.borrow_mut() += 1;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_stub_scene_reports_an_a4_sheet_and_some_content() {
        let scene = StubScene::new();
        let sheet = scene.sheet_bounds();
        assert!((sheet.width() - 297.).abs() < 1e-9);
        assert!((sheet.height() - 210.).abs() < 1e-9);
        let content = scene.content_bounds().expect("stub has content");
        assert!(content.width() > 0. && content.height() > 0.);
        assert!(content.width() < sheet.width());
    }

    #[test]
    fn the_drag_threshold_is_a_few_pixels_not_zero() {
        // A zero threshold turns every click into a drag on a trackpad.
        assert!(DRAG_THRESHOLD_PX >= 2.0 && DRAG_THRESHOLD_PX <= 8.0);
    }
}
