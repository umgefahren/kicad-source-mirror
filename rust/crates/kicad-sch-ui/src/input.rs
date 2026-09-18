// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The vocabulary of everything the shell produces and the host consumes.
//!
//! This module is the whole contract between the Rust presentation layer and
//! whatever eventually drives KiCad's `TOOL_MANAGER`. It is deliberately free
//! of gpui types, of FFI, and of any KiCad type: a [`ShellEvent`] is a plain
//! Rust value that can be recorded, compared, serialised or turned into a
//! `TOOL_EVENT` on the far side of a C ABI without any of those layers knowing
//! about each other.
//!
//! Three rules shaped it, and are worth keeping:
//!
//! * **Positions are reported twice.** Every pointer event carries both the
//!   screen position (device-independent pixels, canvas-local) and the world
//!   position (KiCad internal units). The C++ tools work in world space, but
//!   hit-test tolerances and drag thresholds are screen-space quantities, so
//!   throwing either away costs the host information it cannot recover.
//! * **World coordinates are internal units in `f64`, never millimetres and
//!   never `f32`.** A sheet can sit more than 1.2e9 internal units from the
//!   origin, which is past the point where an `f32` can tell neighbouring units
//!   apart; storing world coordinates in one produces pan jitter that looks
//!   like a renderer bug and is not one. Millimetres appear only where a number
//!   is formatted for a human, in [`crate::grid::Units`].
//! * **Drags are explicit.** gpui reports a move with a button held; KiCad's
//!   tools want a distinct "a drag began" moment after a threshold has been
//!   passed. The shell owns that threshold and emits
//!   [`ShellEvent::DragBegin`] / [`DragUpdate`](ShellEvent::DragUpdate) /
//!   [`DragEnd`](ShellEvent::DragEnd), so every consumer agrees on what a drag
//!   is.
//! * **Commands are names, not numbers.** [`ActionId`] and [`ToolId`] are
//!   strings matching KiCad's `TOOL_ACTION` names (`eeschema.EditorControl.save`
//!   and friends), because `ACTION_REGISTRY` can enumerate those headlessly.
//!   A name survives both sides being rebuilt independently; an ordinal does
//!   not.

use std::cell::RefCell;
use std::rc::Rc;

/// A point in schematic world space, in KiCad internal units (nanometres).
///
/// The same units the draw stream and the renderer's camera use, so a position
/// can travel from a mouse event to `TOOL_MANAGER` without a single conversion.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct WorldPoint {
    /// Distance right of the sheet origin, in internal units.
    pub x: f64,
    /// Distance below the sheet origin, in internal units.
    pub y: f64,
}

impl WorldPoint {
    /// A world point at the given internal-unit coordinates.
    pub const fn new(x: f64, y: f64) -> Self {
        Self { x, y }
    }

    /// The pair the renderer's camera speaks in.
    pub const fn to_array(self) -> [f64; 2] {
        [self.x, self.y]
    }

    /// From the pair the renderer's camera speaks in.
    pub const fn from_array(p: [f64; 2]) -> Self {
        Self { x: p[0], y: p[1] }
    }
}

/// A point in canvas-local screen space, in device-independent pixels.
///
/// The origin is the top-left corner of the canvas element, not of the window,
/// so a host that never learns the window layout can still reason about it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ScreenPoint {
    /// Distance right of the canvas origin, in pixels.
    pub x: f32,
    /// Distance below the canvas origin, in pixels.
    pub y: f32,
}

impl ScreenPoint {
    /// A screen point at the given pixel offsets from the canvas origin.
    pub const fn new(x: f32, y: f32) -> Self {
        Self { x, y }
    }
}

/// Modifier keys held when an event was produced.
///
/// A mirror of gpui's `Modifiers` so that nothing downstream of the shell has
/// to link gpui. `meta` is Command on macOS and Super elsewhere.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct Modifiers {
    /// Control.
    pub ctrl: bool,
    /// Shift.
    pub shift: bool,
    /// Alt / Option.
    pub alt: bool,
    /// Command on macOS, Super elsewhere.
    pub meta: bool,
}

impl Modifiers {
    /// No modifiers held.
    pub const NONE: Self = Self {
        ctrl: false,
        shift: false,
        alt: false,
        meta: false,
    };

    /// Whether no modifier at all is held.
    pub fn is_empty(&self) -> bool {
        *self == Self::NONE
    }
}

/// A pointer button, in the order KiCad's tools care about them.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum PointerButton {
    /// Primary button: select, place, confirm.
    Left,
    /// Middle button: pan.
    Middle,
    /// Secondary button: context menu.
    Right,
    /// Extra button 1, if the device has one.
    Back,
    /// Extra button 2, if the device has one.
    Forward,
}

/// One wheel or trackpad scroll step.
///
/// Kept as two cases because the two need different handling: a line delta is
/// a discrete detent and should map to a fixed zoom ratio, while a pixel delta
/// is a continuous gesture and should map proportionally. Collapsing them to
/// one number is the usual cause of trackpad zoom feeling wrong.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ScrollDelta {
    /// A discrete wheel, in detents. Positive `y` is scroll up / zoom in.
    Lines {
        /// Horizontal detents.
        x: f32,
        /// Vertical detents.
        y: f32,
    },
    /// A continuous gesture, in device-independent pixels.
    Pixels {
        /// Horizontal travel.
        x: f32,
        /// Vertical travel.
        y: f32,
    },
}

impl ScrollDelta {
    /// The vertical component expressed in detents, for consumers that only
    /// want one number. A pixel gesture is divided by a typical detent height.
    pub fn detents_y(&self) -> f32 {
        match *self {
            ScrollDelta::Lines { y, .. } => y,
            // 20 px per detent is gpui's own conversion for line scrolling,
            // so using it here keeps wheel and trackpad zoom consistent.
            ScrollDelta::Pixels { y, .. } => y / 20.0,
        }
    }
}

/// A key, named the way gpui names it.
///
/// Stored as a string rather than an enum on purpose: KiCad's hotkey table is
/// open-ended (it includes every printable character), and an enum here would
/// have to be kept in step with two other layers for no gain. The host maps
/// these names onto `WXK_*` codes in exactly one place.
pub type KeyName = String;

/// The identity of a tool the user can activate, as a KiCad `TOOL_ACTION` name.
///
/// Example: `"eeschema.InteractiveDrawingLineWireBus.drawWires"`.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ToolId(pub &'static str);

impl ToolId {
    /// The underlying action name.
    pub const fn as_str(&self) -> &'static str {
        self.0
    }
}

impl std::fmt::Display for ToolId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(self.0)
    }
}

/// The identity of an invocable action, as a KiCad `TOOL_ACTION` name.
///
/// Example: `"eeschema.EditorControl.save"`. Shell-local actions that will
/// never reach C++ — toggling a dock, say — use a `kicad.ui.` prefix so the
/// two populations stay distinguishable at the seam.
#[derive(Clone, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct ActionId(pub String);

impl ActionId {
    /// Borrow the action name.
    pub fn as_str(&self) -> &str {
        &self.0
    }
}

impl From<&str> for ActionId {
    fn from(value: &str) -> Self {
        Self(value.to_string())
    }
}

impl std::fmt::Display for ActionId {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

/// How the viewport is currently looking at the schematic.
///
/// Reported whenever it changes so that a host doing its own rendering (or
/// culling, or snapping) never has to ask.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewportState {
    /// Canvas width in device-independent pixels.
    pub width: f64,
    /// Canvas height in device-independent pixels.
    pub height: f64,
    /// Screen pixels per internal unit.
    pub scale: f64,
    /// The world point at the centre of the canvas.
    pub center: WorldPoint,
}

/// Everything the shell can tell a host about.
///
/// New variants are expected; a sink should treat unknown ones as no-ops
/// rather than exhaustively matching, which is why the enum is
/// `#[non_exhaustive]`.
#[derive(Clone, Debug, PartialEq)]
#[non_exhaustive]
pub enum ShellEvent {
    /// The pointer moved over the canvas with no button held.
    PointerMove {
        /// Where, relative to the canvas origin.
        screen: ScreenPoint,
        /// Where, in schematic world space.
        world: WorldPoint,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// A button went down on the canvas.
    PointerDown {
        /// Which button.
        button: PointerButton,
        /// Where, relative to the canvas origin.
        screen: ScreenPoint,
        /// Where, in schematic world space.
        world: WorldPoint,
        /// Modifiers held at the time.
        modifiers: Modifiers,
        /// 1 for a single click, 2 for a double click, and so on.
        click_count: usize,
    },
    /// A button came up. Emitted whether or not a drag happened in between.
    PointerUp {
        /// Which button.
        button: PointerButton,
        /// Where, relative to the canvas origin.
        screen: ScreenPoint,
        /// Where, in schematic world space.
        world: WorldPoint,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// The pointer left the canvas. Tools use this to drop hover previews.
    PointerLeave,
    /// A drag passed the movement threshold and has begun.
    ///
    /// The position reported is where the button originally went down, not
    /// where the threshold was crossed, because that is the anchor a rubber
    /// band or a move delta has to be measured from.
    DragBegin {
        /// Which button is dragging.
        button: PointerButton,
        /// Where the press started, relative to the canvas origin.
        origin_screen: ScreenPoint,
        /// Where the press started, in schematic world space.
        origin_world: WorldPoint,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// The pointer moved while a drag was in progress.
    DragUpdate {
        /// Which button is dragging.
        button: PointerButton,
        /// Where, relative to the canvas origin.
        screen: ScreenPoint,
        /// Where, in schematic world space.
        world: WorldPoint,
        /// Movement since the previous `DragUpdate`, or since `DragBegin`.
        delta_screen: ScreenPoint,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// The drag finished because the button came up.
    DragEnd {
        /// Which button was dragging.
        button: PointerButton,
        /// Where, relative to the canvas origin.
        screen: ScreenPoint,
        /// Where, in schematic world space.
        world: WorldPoint,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// The wheel turned or a scroll gesture happened over the canvas.
    Scroll {
        /// Where, relative to the canvas origin.
        screen: ScreenPoint,
        /// Where, in schematic world space.
        world: WorldPoint,
        /// How far, and in which units.
        delta: ScrollDelta,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// A key went down while the canvas had focus.
    KeyDown {
        /// The key, named the way gpui names it.
        key: KeyName,
        /// Modifiers held at the time.
        modifiers: Modifiers,
        /// Whether this is an auto-repeat rather than a fresh press.
        repeat: bool,
    },
    /// A key came up while the canvas had focus.
    KeyUp {
        /// The key, named the way gpui names it.
        key: KeyName,
        /// Modifiers held at the time.
        modifiers: Modifiers,
    },
    /// The user picked a tool, from the palette, a menu or a hotkey.
    ToolActivated(ToolId),
    /// The user asked to abandon the current tool, typically with Escape.
    ToolCancelled,
    /// A named action was invoked, from anywhere in the shell.
    ActionInvoked(ActionId),
    /// The canvas was resized, panned or zoomed.
    ViewportChanged(ViewportState),
}

/// Where the shell posts what it produced.
///
/// Implementations must not panic and must not block: this is called from
/// inside gpui's paint phase, where a panic aborts the frame and a block drops
/// it. A real host queues the event and returns.
pub trait InputSink {
    /// Consume one event.
    fn handle(&mut self, event: ShellEvent);
}

/// A sink that throws everything away.
///
/// The shell always holds a sink, so this is what it holds before a host is
/// attached, and what a screenshot run uses.
#[derive(Clone, Copy, Debug, Default)]
pub struct NullSink;

impl InputSink for NullSink {
    fn handle(&mut self, _event: ShellEvent) {}
}

/// A sink that keeps everything, for tests.
///
/// Cloning shares the same log, so a test can hand one clone to the shell and
/// keep the other to assert on.
#[derive(Clone, Debug, Default)]
pub struct RecordingSink {
    events: Rc<RefCell<Vec<ShellEvent>>>,
}

impl RecordingSink {
    /// An empty recorder.
    pub fn new() -> Self {
        Self::default()
    }

    /// Every event seen so far, in order.
    pub fn events(&self) -> Vec<ShellEvent> {
        self.events.borrow().clone()
    }

    /// How many events have been seen.
    pub fn len(&self) -> usize {
        self.events.borrow().len()
    }

    /// Whether nothing has been seen yet.
    pub fn is_empty(&self) -> bool {
        self.events.borrow().is_empty()
    }

    /// Forget everything seen so far, so a test can assert on one interaction
    /// at a time without counting past events.
    pub fn clear(&self) {
        self.events.borrow_mut().clear();
    }

    /// Whether any recorded event satisfies `predicate`.
    pub fn any(&self, predicate: impl FnMut(&ShellEvent) -> bool) -> bool {
        self.events.borrow().iter().any(predicate)
    }

    /// The names of every [`ShellEvent::ActionInvoked`] seen, in order.
    pub fn invoked_actions(&self) -> Vec<String> {
        self.events
            .borrow()
            .iter()
            .filter_map(|event| match event {
                ShellEvent::ActionInvoked(id) => Some(id.0.clone()),
                _ => None,
            })
            .collect()
    }

    /// The tools activated, in order.
    pub fn activated_tools(&self) -> Vec<ToolId> {
        self.events
            .borrow()
            .iter()
            .filter_map(|event| match event {
                ShellEvent::ToolActivated(tool) => Some(*tool),
                _ => None,
            })
            .collect()
    }
}

impl InputSink for RecordingSink {
    fn handle(&mut self, event: ShellEvent) {
        self.events.borrow_mut().push(event);
    }
}

/// A shared, reference-counted sink.
///
/// The shell hands clones of this to its canvas element, its toolbars and its
/// menu handlers, all of which are `'static` closures, so shared ownership is
/// the only workable shape. Single-threaded because gpui's UI state is.
pub type SharedSink = Rc<RefCell<dyn InputSink>>;

/// Wrap a sink for sharing across the shell.
pub fn shared_sink(sink: impl InputSink + 'static) -> SharedSink {
    Rc::new(RefCell::new(sink))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn recording_sink_clones_share_one_log() {
        let sink = RecordingSink::new();
        let other = sink.clone();
        let mut writable = sink.clone();
        writable.handle(ShellEvent::ToolCancelled);
        assert_eq!(other.len(), 1);
        assert_eq!(sink.len(), 1);
    }

    #[test]
    fn pixel_scroll_is_converted_to_detents() {
        assert_eq!(ScrollDelta::Lines { x: 0., y: 3. }.detents_y(), 3.);
        assert_eq!(ScrollDelta::Pixels { x: 0., y: 40. }.detents_y(), 2.);
    }

    #[test]
    fn recording_sink_reports_actions_and_tools_in_order() {
        let mut sink = RecordingSink::new();
        sink.handle(ShellEvent::ActionInvoked(ActionId::from("a")));
        sink.handle(ShellEvent::ToolActivated(ToolId("t")));
        sink.handle(ShellEvent::ActionInvoked(ActionId::from("b")));
        assert_eq!(sink.invoked_actions(), vec!["a".to_string(), "b".to_string()]);
        assert_eq!(sink.activated_tools(), vec![ToolId("t")]);
        sink.clear();
        assert!(sink.is_empty());
    }
}
