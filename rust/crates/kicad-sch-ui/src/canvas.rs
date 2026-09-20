// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The drawing surface: the element, the input plumbing, and the chrome drawn
//! around what the renderer paints.
//!
//! # Where the seam actually is
//!
//! The shell does not draw schematics; `kicad-sch-render` does. It owns the
//! draw stream, the tessellation cache, the spatial index and — importantly —
//! the camera. The shell owns everything *around* the drawing: hit testing, the
//! pointer and keyboard vocabulary, the cursor, the grid, the crosshair and the
//! selection band.
//!
//! There is deliberately **one** camera, [`kicad_sch_render::Camera`], living
//! in the renderer. An earlier draft of this module kept a second one in
//! millimetres with an `f32` scale; that was wrong twice over. Two cameras have
//! to be kept in agreement on every pan, zoom, resize and hit test, and a
//! millimetre camera means a unit conversion at a boundary crossed several
//! times per frame. Worse, a coordinate can run to hundreds of millions of
//! internal units — beyond where an `f32` mantissa distinguishes neighbours —
//! so an `f32` world scale produces visible pan jitter. The renderer subtracts
//! the camera origin in `f64` and narrows only
//! the small remainder, and that property only holds if nothing upstream has
//! already thrown the precision away.
//!
//! So: the shell reads and drives the renderer's camera, and converts to
//! millimetres in exactly one place — [`crate::grid::Units::format`], on its
//! way into a string.
//!
//! # Where the geometry comes from
//!
//! A canvas holds either a fixed stream or a [`crate::document::LiveDocument`].
//! With a fixed stream — a recorded `.kgds`, the demonstration geometry — the
//! camera moves over geometry that cannot change, which is a viewer and is
//! correct as one. With a live document the canvas asks for the frame its camera
//! is about to paint, *before* painting it, and only when the answer could have
//! changed: a pan, a zoom, a resize, or [`CanvasState::mark_document_dirty`] for
//! anything the host did that the canvas cannot see. A free-running redraw of an
//! untouched view asks for nothing, which is what [`CanvasState::document_renders`]
//! exists to make checkable.

use std::cell::{Cell, RefCell};
use std::rc::Rc;

use gpui_kit::prelude::*;
use gpui_kit::{
    App, Bounds, DispatchPhase, Element, ElementId, Entity, GlobalElementId, Hitbox,
    HitboxBehavior, Hsla, InspectorElementId, IntoElement, LayoutId, MouseButton, MouseDownEvent,
    MouseMoveEvent, MouseUpEvent, Pixels, Point, ScrollWheelEvent, SharedString, Size, Style,
    StyleRefinement, Styled, Window, point, px, size,
};
use kicad_sch_render::{Camera, FrameStats, SchematicRenderer, WorldRect};

use crate::document::SharedDocument;
use crate::grid::GridState;
use crate::input::{
    Modifiers, PointerButton, ScreenPoint, ScrollDelta, SharedSink, ShellEvent, ViewportState,
    WorldPoint, shared_sink,
};
use crate::theme::CanvasPalette;
use crate::tools::Tool;

/// How far the pointer must travel with a button held before the shell calls it
/// a drag. Below this a press-and-release is a click, however shaky the hand.
pub const DRAG_THRESHOLD_PX: f32 = 4.0;

/// One zoom step for the wheel and the zoom buttons.
const ZOOM_STEP: f64 = 1.25;

/// Margin left around the document by "zoom to fit", in pixels.
const FIT_PADDING_PX: f64 = 24.0;

/// Screen pixels per internal unit at which the schematic is shown 1:1 on a
/// nominal 96 dpi display. The status bar's zoom percentage is relative to this,
/// so "100%" means a millimetre of schematic is a millimetre of glass.
pub const REFERENCE_SCALE: f64 = 96.0 / 25.4 / crate::grid::IU_PER_MM;

#[derive(Clone, Copy, Debug)]
struct Press {
    button: PointerButton,
    origin: Point<Pixels>,
    last: Point<Pixels>,
    modifiers: Modifiers,
    dragging: bool,
}

/// The mutable state behind the canvas: what it is showing, what tool is
/// active, and what the pointer is doing.
///
/// Separate from the shell view so that the canvas element can update it from
/// inside `paint` without reaching through the whole window.
pub struct CanvasState {
    renderer: Rc<RefCell<SchematicRenderer>>,
    grid: GridState,
    palette: CanvasPalette,
    tool: Tool,
    sink: SharedSink,
    /// The document to re-record frames from, if this canvas has a live one.
    ///
    /// `None` is a canvas over a fixed stream — a recorded `.kgds` or the
    /// demonstration geometry — which is a viewer and correct as such: the
    /// camera moves over geometry that cannot change.
    document: Option<SharedDocument>,
    /// Last pointer position, in window coordinates.
    pointer: Option<Point<Pixels>>,
    press: Option<Press>,
    crosshair: bool,
    /// The canvas rectangle in window coordinates, as of the last layout.
    bounds: Bounds<Pixels>,
    /// Set whenever the camera changed, so the next paint reports it once
    /// rather than on every mouse move.
    viewport_dirty: bool,
    /// Set whenever the frame a live document would record could have changed,
    /// so that the next prepaint asks for a new one.
    ///
    /// Separate from `viewport_dirty` because the two are consumed at different
    /// points of the same frame — this one in prepaint, before anything is
    /// painted, and that one in paint, on its way to the host — but they are
    /// always raised together, by [`CanvasState::invalidate_view`].
    ///
    /// A `Cell` because the host raises it too: [`CanvasState::emit`] takes
    /// `&self` — it is called from the paint phase — and has to be able to mark the
    /// document dirty when the sink reports that a tool changed something.
    document_dirty: Cell<bool>,
    /// How many frames have been asked of the live document. The measurement
    /// behind "a redraw of an unchanged view costs nothing".
    document_renders: u64,
    /// Why the last request for a frame failed, if it did.
    document_error: Option<SharedString>,
    /// A fit asked for before the canvas had a size, applied as soon as layout
    /// provides one. The shell fits the document while building the window,
    /// which is necessarily before the first layout.
    fit_pending: bool,
    /// Zoom steps to apply once the deferred fit has run, so that a startup
    /// zoom is not undone by it.
    zoom_after_fit: i32,
    last_frame: FrameStats,
    /// What the last call into the renderer cost on the CPU.
    ///
    /// Worth separating from the frame interval: under the software rasteriser
    /// the screenshots are taken on, the frame interval is dominated by
    /// llvmpipe and says nothing about this crate or the renderer.
    last_paint: std::time::Duration,
}

/// A secondary click, emitted only after release so dragging can pan instead.
pub struct CanvasContextMenu(pub Point<Pixels>);

impl gpui_kit::EventEmitter<CanvasContextMenu> for CanvasState {}

impl CanvasState {
    /// A canvas showing `renderer`, reporting to `sink`.
    pub fn new(
        renderer: Rc<RefCell<SchematicRenderer>>,
        palette: CanvasPalette,
        sink: SharedSink,
    ) -> Self {
        Self {
            renderer,
            grid: GridState::default(),
            palette,
            tool: Tool::Select,
            sink,
            document: None,
            pointer: None,
            press: None,
            crosshair: true,
            bounds: Bounds {
                origin: Point::default(),
                size: size(px(1.), px(1.)),
            },
            viewport_dirty: true,
            document_dirty: Cell::new(true),
            document_renders: 0,
            document_error: None,
            fit_pending: true,
            zoom_after_fit: 0,
            last_frame: FrameStats::default(),
            last_paint: std::time::Duration::ZERO,
        }
    }

    /// A canvas showing the demonstration stream and reporting nowhere.
    pub fn demo(palette: CanvasPalette) -> Self {
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(crate::demo::demo_stream());
        Self::new(
            Rc::new(RefCell::new(renderer)),
            palette,
            shared_sink(crate::input::NullSink),
        )
    }

    /// The renderer, for handing it a new stream or inspecting its cache.
    pub fn renderer(&self) -> &Rc<RefCell<SchematicRenderer>> {
        &self.renderer
    }

    /// A copy of the view transform.
    ///
    /// A copy rather than a borrow because the camera lives behind the
    /// renderer's `RefCell`, and handing out a `Ref` from here would make every
    /// caller responsible for not holding it across a paint.
    pub fn camera(&self) -> Camera {
        *self.renderer.borrow().camera()
    }

    /// The canvas rectangle in window coordinates.
    pub fn bounds(&self) -> Bounds<Pixels> {
        self.bounds
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

    /// What the last painted frame cost, for the status bar.
    pub fn last_frame(&self) -> FrameStats {
        self.last_frame
    }

    /// CPU time in the renderer for the last frame.
    pub fn last_paint(&self) -> std::time::Duration {
        self.last_paint
    }

    /// Zoom by `steps` once the canvas has been laid out and fitted.
    ///
    /// A zoom applied before the first layout would be undone by the pending
    /// fit, which is why this is queued rather than done now.
    pub fn zoom_after_fit(&mut self, steps: i32) {
        self.zoom_after_fit = steps;
    }

    /// Zoom as a fraction of 1:1 on a nominal 96 dpi display.
    pub fn zoom(&self) -> f64 {
        self.camera().scale() / REFERENCE_SCALE
    }

    /// Where the pointer last was, in world coordinates, snapped to the grid.
    ///
    /// Snapped because that is the position an edit would actually use, and a
    /// status bar that disagrees with where the wire lands is worse than none.
    pub fn cursor_world(&self) -> Option<WorldPoint> {
        self.pointer
            .map(|position| self.grid.snap(self.world(position)))
    }

    /// How many items the host reports as selected.
    ///
    /// The selection belongs to the C++ selection tool, so the shell has none of
    /// its own to count. Zero when no host is attached, and also zero while no
    /// selection tool can run on a non-frame holder — the two are
    /// indistinguishable from here, which is why the status bar says "selected"
    /// rather than claiming a selection exists.
    pub fn selection_count(&self) -> usize {
        self.sink
            .try_borrow()
            .map(|sink| sink.selection_count())
            .unwrap_or(0)
    }

    /// Feedback from the live host, if attached.
    pub fn host_status(&self) -> Option<String> {
        self.sink
            .try_borrow()
            .ok()?
            .status_message()
            .map(str::to_owned)
    }

    /// Unsaved state from the document rather than inferred from UI gestures.
    pub fn modified(&self) -> Option<bool> {
        self.sink.try_borrow().ok()?.modified()
    }

    /// Save through the host, retaining the document if saving fails.
    pub fn save_document(&mut self) -> Result<(), String> {
        let result = self
            .sink
            .try_borrow_mut()
            .map_err(|_| "The document is busy; try saving again".to_string())?
            .save_document();
        self.mark_document_dirty();
        result
    }

    /// Search the host model and center the canvas on the match.
    pub fn search(
        &mut self,
        data: &crate::search::SearchData,
        operation: crate::search::SearchOperation,
    ) -> Result<crate::search::SearchResult, String> {
        let result = self
            .sink
            .try_borrow_mut()
            .map_err(|_| "Document is busy")?
            .search(data, operation)?;
        if result.found && operation != crate::search::SearchOperation::Close {
            self.renderer
                .borrow_mut()
                .camera_mut()
                .set_center([result.center_x, result.center_y]);
        }
        self.invalidate_view();
        Ok(result)
    }

    /// Point the events somewhere else. Used by tests and by the host once it
    /// attaches.
    pub fn set_sink(&mut self, sink: SharedSink) {
        self.sink = sink;
    }

    /// Draw from `document` from now on, re-recording whenever the view moves.
    ///
    /// The renderer keeps whatever stream it already has until the first
    /// request succeeds, so attaching a document never blanks the canvas.
    pub fn set_document(&mut self, document: SharedDocument) {
        self.document = Some(document);
        self.invalidate_view();
    }

    /// Whether this canvas draws from a live document rather than a fixed
    /// stream.
    pub fn has_live_document(&self) -> bool {
        self.document.is_some()
    }

    /// Ask the live document for a fresh frame before the next paint.
    ///
    /// The canvas already does this for anything it can see — a pan, a zoom, a
    /// resize. This is for changes it cannot: an edit, an undo, a sheet change,
    /// anything the host does to the document behind its back.
    pub fn mark_document_dirty(&mut self) {
        self.document_dirty.set(true);
    }

    /// How many frames the live document has been asked for.
    ///
    /// The number behind the claim that an unchanged view costs nothing: it does
    /// not move while the window free-runs over a document nobody is touching.
    pub fn document_renders(&self) -> u64 {
        self.document_renders
    }

    /// Why the last attempt to record a frame failed, if it did.
    ///
    /// Cleared by the next attempt that succeeds. The shell shows it in the
    /// status bar: a canvas silently showing the last frame it managed to get is
    /// indistinguishable from one that is working.
    pub fn document_error(&self) -> Option<&SharedString> {
        self.document_error.as_ref()
    }

    /// Ask the live document for the frame this camera would show.
    ///
    /// Called from prepaint, after the viewport and any deferred fit have been
    /// settled, so that the camera the document is told about is the one the
    /// frame is painted with. Getting that order wrong shows as geometry missing
    /// along the edges for one frame, because the document culls to what it was
    /// told.
    fn refresh_document(&mut self) {
        if !self.document_dirty.get() {
            return;
        }
        let Some(document) = self.document.clone() else {
            // Nothing to ask. Clearing the flag anyway keeps a canvas over a
            // fixed stream from retrying every frame.
            self.document_dirty.set(false);
            return;
        };
        // A document that reached back into the shell would find this borrowed;
        // dropping the request beats aborting the frame, and the flag stays up
        // so the next frame tries again.
        let Ok(mut document) = document.try_borrow_mut() else {
            return;
        };
        self.document_dirty.set(false);
        self.document_renders += 1;

        let viewport = self.viewport_state();
        let mut renderer = self.renderer.borrow_mut();
        match document.render(viewport, &mut renderer) {
            Ok(()) => self.document_error = None,
            Err(reason) => self.document_error = Some(reason.into()),
        }
    }

    /// The camera, in the shape the host and the sink are told about.
    fn viewport_state(&self) -> ViewportState {
        let camera = self.camera();
        let viewport = camera.viewport();
        ViewportState {
            width: viewport[0],
            height: viewport[1],
            scale: camera.scale(),
            center: WorldPoint::from_array(camera.center()),
        }
    }

    /// Note that the camera moved: the host has to be told, and a live document
    /// has to re-record.
    fn invalidate_view(&mut self) {
        self.viewport_dirty = true;
        self.document_dirty.set(true);
    }

    /// Post an event to the sink.
    pub fn emit(&self, event: ShellEvent) {
        // A sink that panics would take the frame with it, so the contract on
        // `InputSink` forbids it; borrowing can still fail if a sink re-enters
        // the shell, and dropping the event beats aborting the frame.
        if let Ok(mut sink) = self.sink.try_borrow_mut() {
            sink.handle(event);

            // A host tool that moved an item, changed the selection or moved the
            // view has changed what the next frame looks like, and the canvas
            // cannot see any of that: it re-records when it moves the camera
            // itself, and an edit is not that. So the sink is asked, every time.
            if sink.take_dirty() {
                self.document_dirty.set(true);
            }
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

    /// Invoke a tool through the host's hotkey dispatcher, preserving immediate
    /// placement and repeated hotkeys (W while drawing is a synthetic click).
    pub fn tool_hotkey(&mut self, tool: Tool, hotkey: &str) {
        self.tool = tool;
        let parts: Vec<_> = hotkey.split('-').collect();
        self.emit(ShellEvent::KeyDown {
            key: parts.last().unwrap_or(&hotkey).to_string(),
            modifiers: Modifiers {
                ctrl: parts.contains(&"ctrl"),
                shift: parts.contains(&"shift"),
                alt: parts.contains(&"alt"),
                meta: parts.contains(&"cmd"),
            },
            repeat: false,
        });
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

    /// Slide the view by a screen-space delta, as a middle-button drag does.
    pub fn pan(&mut self, dx_px: f64, dy_px: f64) {
        self.renderer.borrow_mut().pan(dx_px, dy_px);
        self.invalidate_view();
    }

    /// Zoom in one step about the canvas centre.
    pub fn zoom_in(&mut self) {
        self.zoom_about_centre(ZOOM_STEP);
    }

    /// Zoom out one step about the canvas centre.
    pub fn zoom_out(&mut self) {
        self.zoom_about_centre(1.0 / ZOOM_STEP);
    }

    fn zoom_about_centre(&mut self, factor: f64) {
        let viewport = self.camera().viewport();
        self.renderer
            .borrow_mut()
            .zoom_to_point(factor, [viewport[0] * 0.5, viewport[1] * 0.5]);
        self.invalidate_view();
    }

    /// Frame the whole document. Deferred until the canvas has a size.
    pub fn zoom_to_fit(&mut self) {
        self.invalidate_view();
        if self.has_a_usable_viewport() {
            self.fit_pending = false;
            self.renderer.borrow_mut().zoom_to_fit(FIT_PADDING_PX);
        } else {
            self.fit_pending = true;
        }
    }

    /// Frame the drawn items.
    ///
    /// The renderer's document bounds are the drawn extent already, so this is
    /// the same view as [`Self::zoom_to_fit`] until a separate sheet outline
    /// exists to fit instead.
    pub fn zoom_to_objects(&mut self) {
        self.zoom_to_fit();
    }

    /// Return to 1:1.
    pub fn zoom_actual_size(&mut self) {
        self.renderer
            .borrow_mut()
            .camera_mut()
            .set_scale(REFERENCE_SCALE);
        self.invalidate_view();
    }

    /// The extent of the document, in internal units.
    pub fn document_bounds(&self) -> WorldRect {
        self.renderer.borrow().document_bounds()
    }

    fn has_a_usable_viewport(&self) -> bool {
        f32::from(self.bounds.size.width) > 2.0 && f32::from(self.bounds.size.height) > 2.0
    }

    fn report_viewport_if_changed(&mut self) {
        if !self.viewport_dirty {
            return;
        }
        self.viewport_dirty = false;
        self.emit(ShellEvent::ViewportChanged(self.viewport_state()));
    }

    /// A window coordinate as the canvas-local pixels the camera works in.
    fn local(&self, position: Point<Pixels>) -> ScreenPoint {
        ScreenPoint::new(
            f32::from(position.x) - f32::from(self.bounds.origin.x),
            f32::from(position.y) - f32::from(self.bounds.origin.y),
        )
    }

    /// A window coordinate in world units.
    fn world(&self, position: Point<Pixels>) -> WorldPoint {
        let local = self.local(position);
        WorldPoint::from_array(
            self.camera()
                .screen_to_world([local.x as f64, local.y as f64]),
        )
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
            if state.bounds != bounds {
                state.bounds = bounds;
                state
                    .renderer
                    .borrow_mut()
                    .set_viewport([bounds.size.width.to_f64(), bounds.size.height.to_f64()]);
                state.invalidate_view();
            }
            // A live document that has not produced geometry yet leaves nothing
            // to frame, so in that one case the frame has to be asked for before
            // the fit rather than after it. Not reachable when a host renders
            // once before opening the window, which is what the binary does.
            if state.fit_pending && state.document_bounds().is_empty() {
                state.refresh_document();
            }
            if state.fit_pending && state.has_a_usable_viewport() {
                state.zoom_to_fit();
                let steps = std::mem::take(&mut state.zoom_after_fit);
                for _ in 0..steps.abs() {
                    if steps > 0 {
                        state.zoom_in();
                    } else {
                        state.zoom_out();
                    }
                }
            }

            // Last, deliberately: everything above can still move the camera,
            // and a document culls to the camera it is given. Asking before the
            // fit had run would paint one frame culled to the wrong view, which
            // shows as geometry missing along the edges.
            state.refresh_document();
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
        let (camera, palette, grid, tool, crosshair, pointer, press, renderer) =
            self.state.update(cx, |state, _| {
                state.report_viewport_if_changed();
                (
                    state.camera(),
                    state.palette,
                    state.grid,
                    state.tool,
                    state.crosshair,
                    state.pointer,
                    state.press,
                    state.renderer.clone(),
                )
            });

        // One layer for the whole canvas: one draw order, so the grid, the
        // schematic and the overlays merge into a single gpui path batch and
        // therefore a single render pass. `paint_frame` paints into the layer
        // the caller has opened rather than opening its own, for exactly this
        // reason.
        let stats = window.paint_layer(bounds, |window| {
            window.paint_quad(gpui_kit::fill(bounds, palette.background));
            paint_grid(bounds, &camera, &grid, &palette, window);

            let started = std::time::Instant::now();
            let stats = renderer.borrow_mut().paint_frame(bounds, window);
            let elapsed = started.elapsed();

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
            (stats, elapsed)
        });

        self.state.update(cx, |state, _| {
            let (stats, elapsed) = stats;
            state.last_frame = stats;
            state.last_paint = elapsed;
        });

        window.set_cursor_style(tool.spec().cursor, &hitbox);
        install_mouse_handlers(&self.state, &hitbox, window);
    }
}

fn install_mouse_handlers(state: &Entity<CanvasState>, hitbox: &Hitbox, window: &mut Window) {
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

                // The press is copied out and written back rather than held as
                // a mutable borrow, because everything below needs the whole
                // state to convert coordinates and to reach the sink.
                let Some(mut press) = state.press else {
                    state.emit(ShellEvent::PointerMove {
                        screen,
                        world,
                        modifiers,
                    });
                    cx.notify();
                    return;
                };

                let travelled = (f32::from(event.position.x) - f32::from(press.origin.x))
                    .hypot(f32::from(event.position.y) - f32::from(press.origin.y));
                let previous = press.last;
                press.last = event.position;

                if !press.dragging && travelled >= DRAG_THRESHOLD_PX {
                    press.dragging = true;
                    state.press = Some(press);
                    state.emit(ShellEvent::DragBegin {
                        button: press.button,
                        origin_screen: state.local(press.origin),
                        origin_world: state.world(press.origin),
                        // The modifiers that were held when the button went
                        // down, not the ones held now: a shift-drag that lets
                        // go of shift halfway is still a shift-drag.
                        modifiers: press.modifiers,
                    });
                }
                state.press = Some(press);

                if press.dragging {
                    let delta = ScreenPoint::new(
                        f32::from(event.position.x) - f32::from(previous.x),
                        f32::from(event.position.y) - f32::from(previous.y),
                    );
                    // Both navigation buttons pan. A secondary click opens its
                    // menu only on release, and never after a pan.
                    if matches!(press.button, PointerButton::Middle | PointerButton::Right) {
                        state.pan(delta.x as f64, delta.y as f64);
                    }
                    state.emit(ShellEvent::DragUpdate {
                        button: press.button,
                        screen,
                        world,
                        delta_screen: delta,
                        modifiers,
                    });
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
                let Some(button) = map_button(event.button) else {
                    return;
                };
                let modifiers = map_modifiers(event.modifiers);
                let screen = state.local(event.position);
                let world = state.world(event.position);

                // `press` tracks one button, so with two held it describes the second.
                // The release of the *first* therefore finds no matching press — and
                // must be reported anyway: a host that keeps per-button state would
                // otherwise believe that button is held for the rest of the session and
                // turn every later pointer move into a drag from a stale origin.
                // Only the drag bookkeeping depends on the match.
                let secondary_click = state.press.is_some_and(|press| {
                    press.button == PointerButton::Right
                        && button == PointerButton::Right
                        && !press.dragging
                        && state.bounds.contains(&event.position)
                });
                let dragging = match state.press {
                    Some(press) if press.button == button => {
                        state.press = None;
                        press.dragging
                    }
                    _ => false,
                };

                if dragging {
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
                if secondary_click {
                    cx.emit(CanvasContextMenu(event.position));
                }
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
                let local = state.local(event.position);
                state.emit(ShellEvent::Scroll {
                    screen: local,
                    world: state.world(event.position),
                    delta,
                    modifiers,
                });

                // KiCad's default: the wheel zooms, and the modifiers turn it
                // into scrolling. Shift is vertical, Ctrl is horizontal.
                let detents = delta.detents_y() as f64;
                {
                    let mut renderer = state.renderer.borrow_mut();
                    if modifiers.shift {
                        renderer.pan(0.0, detents * 40.0);
                    } else if modifiers.ctrl {
                        renderer.pan(detents * 40.0, 0.0);
                    } else if detents != 0.0 {
                        renderer.zoom_to_point(
                            ZOOM_STEP.powf(detents),
                            [local.x as f64, local.y as f64],
                        );
                    }
                }
                state.invalidate_view();
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
    let mut spacing = grid.spacing_iu();
    let major_every = 10i64;
    let mut on_screen = camera.world_to_px(spacing);
    if on_screen < 7.0 {
        // Too dense to read: show only what would have been the major points.
        spacing *= major_every as f64;
        on_screen *= major_every as f64;
    }
    if on_screen < 5.0 || spacing <= 0.0 {
        return;
    }

    let visible = camera.visible_world_rect(0.0);
    let first_x = (visible.min[0] / spacing).floor() as i64;
    let last_x = (visible.max[0] / spacing).ceil() as i64;
    let first_y = (visible.min[1] / spacing).floor() as i64;
    let last_y = (visible.max[1] / spacing).ceil() as i64;

    // A pathological zoom could ask for millions of dots; cap the work rather
    // than dropping the frame.
    const MAX_DOTS: i64 = 40_000;
    let columns = (last_x - first_x + 1).max(0);
    let rows = (last_y - first_y + 1).max(0);
    if columns.saturating_mul(rows) > MAX_DOTS {
        return;
    }

    let dot = if on_screen > 26.0 { 1.5 } else { 1.0 };
    let origin = bounds.origin;
    for ix in first_x..=last_x {
        for iy in first_y..=last_y {
            let major = ix.rem_euclid(major_every) == 0 && iy.rem_euclid(major_every) == 0;
            let color = if major {
                palette.grid_major
            } else {
                palette.grid
            };
            let radius = if major { dot + 0.5 } else { dot };
            let screen = camera.world_to_screen([ix as f64 * spacing, iy as f64 * spacing]);
            let center = point(origin.x + px(screen[0]), origin.y + px(screen[1]));
            let rect = Bounds {
                origin: point(center.x - px(radius), center.y - px(radius)),
                size: size(px(radius * 2.), px(radius * 2.)),
            };
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

fn paint_selection_band(from: Point<Pixels>, to: Point<Pixels>, color: Hsla, window: &mut Window) {
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
    window.paint_quad(gpui_kit::outline(rect, color, gpui_kit::BorderStyle::Solid));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn one_to_one_is_ninety_six_dots_per_inch() {
        // At the reference scale a 25.4 mm ruler spans 96 pixels.
        let span_iu = 25.4 * crate::grid::IU_PER_MM;
        assert!((span_iu * REFERENCE_SCALE - 96.0).abs() < 1e-9);
    }
}
