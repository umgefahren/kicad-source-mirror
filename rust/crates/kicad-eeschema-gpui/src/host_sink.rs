// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The arrow that used to be missing: shell input into the C++ tool framework.
//!
//! Until this existed, the binary handed the shell a `NullSink` whose entire body
//! was `fn handle(&mut self, _event: ShellEvent) {}`, so everything a user did to
//! the window was collected and discarded. This is the other end of that: it
//! translates the shell's event vocabulary into the host ABI's and calls it.
//!
//! # Why the shell's drag events are dropped on purpose
//!
//! The shell reports a press-and-move as `PointerDown`, then `DragBegin` once the
//! pointer passes its own threshold, then a `DragUpdate` per move — and *no*
//! `PointerMove` while a drag is in progress. The C++ dispatcher does its own drag
//! detection from presses and motion, with KiCad's threshold and KiCad's
//! semantics, so what it needs is the motion. Forwarding both vocabularies would
//! have the host detect a drag from the motion *and* be told about one, and
//! forwarding `DragBegin` as motion would send the press position over again,
//! after the pointer had left it.
//!
//! So `DragUpdate` becomes motion, and `DragBegin` and `DragEnd` are dropped: the
//! press and the release that bracket them carry everything the host needs.
//!
//! # Threading
//!
//! Everything here runs on the thread that initialised the host, which is the
//! thread gpui runs the window on. `Session` is `!Send`, so that is a compile-time
//! fact rather than a convention.

use std::cell::RefCell;
use std::rc::Rc;

use kicad_sch_sys::{
    EditorState, Error, InputEvent, InputOutcome, Modifiers, PointerButton, Session,
};
use kicad_sch_ui::input::{self, InputSink, ShellEvent};

/// The session, shared by the two things that drive it.
///
/// The live document asks it for frames in prepaint; this sink gives it input in
/// paint. Neither borrow outlives its call, and they happen at different points of
/// the same frame, so a `RefCell` is the whole of the arbitration needed — and
/// the one that can fail, `try_borrow_mut`, drops the event rather than panicking
/// inside a paint.
pub type SharedSession = Rc<RefCell<Session>>;

/// The three calls this sink makes on a session.
///
/// Named as a trait so that the translation — which is the part with rules in it,
/// and the part that is wrong in interesting ways when it is wrong — can be tested
/// against a recorder rather than against a linked C++ host. `Session` implements
/// it by forwarding, so there is no second implementation in the shipping path.
pub trait InputTarget {
    /// Give one event to the tool framework.
    fn dispatch_input(&mut self, event: &InputEvent<'_>) -> Result<InputOutcome, Error>;
    /// Run an action by its dotted name.
    fn run_action(&mut self, name: &str) -> Result<InputOutcome, Error>;
    /// Read the cursor, the selection and the status text back.
    fn editor_state(&mut self) -> Result<EditorState, Error>;
    /// Undo the newest command; false if there was nothing to undo.
    fn undo(&mut self) -> Result<bool, Error>;
    /// Redo the newest undone command; false if there was nothing to redo.
    fn redo(&mut self) -> Result<bool, Error>;
    /// Write the document back to the files it was loaded from.
    fn save(&mut self) -> Result<(), Error>;
}

impl InputTarget for Session {
    fn dispatch_input(&mut self, event: &InputEvent<'_>) -> Result<InputOutcome, Error> {
        Session::dispatch_input(self, event)
    }

    fn run_action(&mut self, name: &str) -> Result<InputOutcome, Error> {
        Session::run_action(self, name)
    }

    fn editor_state(&mut self) -> Result<EditorState, Error> {
        Session::editor_state(self)
    }

    fn undo(&mut self) -> Result<bool, Error> {
        Session::undo(self)
    }

    fn redo(&mut self) -> Result<bool, Error> {
        Session::redo(self)
    }

    fn save(&mut self) -> Result<(), Error> {
        Session::save(self)
    }
}

/// Everything the shell produces, given to the C++ tool framework.
pub struct HostInputSink {
    session: Rc<RefCell<dyn InputTarget>>,

    /// Raised when the host says the frame the canvas is holding is stale.
    ///
    /// Consumed by `CanvasState` after each event, which is what makes an edit or
    /// a selection change visible: the canvas re-records only when it moves the
    /// camera itself, and neither of those is that.
    dirty: bool,

    /// The selection size the host last reported, for the status bar.
    selection_count: usize,

    /// The last failure reported, so a dead session says so once instead of once
    /// per mouse move.
    last_failure: Option<String>,
    status: Option<String>,
    modified: bool,
}

impl HostInputSink {
    /// A sink feeding `session`.
    pub fn new(session: Rc<RefCell<dyn InputTarget>>) -> Self {
        Self {
            session,
            dirty: false,
            selection_count: 0,
            last_failure: None,
            status: None,
            modified: false,
        }
    }

    /// Send one translated event, and record what the host made of it.
    fn dispatch(&mut self, event: InputEvent<'_>) {
        let Ok(mut session) = self.session.try_borrow_mut() else {
            // The document is mid-render, which means we are being called from
            // inside it. Nothing does that today; dropping the event beats
            // panicking inside a paint if something ever does.
            return;
        };

        match session.dispatch_input(&event) {
            Ok(outcome) => {
                drop(session);
                self.absorb(outcome);
            }
            Err(error) => {
                drop(session);
                self.fail(format!("input: {error}"));
            }
        }
    }

    /// Run a KiCad action by name, as a menu item or a toolbar button does.
    fn run_action(&mut self, name: &str) {
        // The shell's own chrome — panels, the theme, the frame readout — reports
        // under a `kicad.ui.` name precisely so that the host can tell the two
        // populations apart. Sending them on would be asking KiCad for actions it
        // has never heard of, once per click.
        if name.starts_with("kicad.ui.") {
            return;
        }

        let Ok(mut session) = self.session.try_borrow_mut() else {
            return;
        };

        match session.run_action(name) {
            Ok(outcome) => {
                drop(session);
                self.status = if outcome.handled {
                    None
                } else {
                    Some(format!("Not available in the GPUI editor: {name}"))
                };
                self.absorb(outcome);
            }
            Err(error) => {
                drop(session);
                self.fail(format!("action {name}: {error}"));
            }
        }
    }

    /// Undo the newest command, for a caller that would rather not go through the registry.
    #[allow(dead_code)]
    fn undo(&mut self) {
        let Ok(mut session) = self.session.try_borrow_mut() else {
            return;
        };

        match session.undo() {
            Ok(undone) => {
                drop(session);

                // Nothing to undo is not a failure and not a reason to re-record.
                if undone {
                    self.absorb(InputOutcome {
                        handled: true,
                        redraw: true,
                    });
                }
            }
            Err(error) => {
                drop(session);
                self.fail(format!("undo: {error}"));
            }
        }
    }

    /// Redo the newest undone command.
    #[allow(dead_code)]
    fn redo(&mut self) {
        let Ok(mut session) = self.session.try_borrow_mut() else {
            return;
        };

        match session.redo() {
            Ok(redone) => {
                drop(session);

                if redone {
                    self.absorb(InputOutcome {
                        handled: true,
                        redraw: true,
                    });
                }
            }
            Err(error) => {
                drop(session);
                self.fail(format!("redo: {error}"));
            }
        }
    }

    /// Write the document back to the files it came from.
    #[allow(dead_code)]
    fn save(&mut self) {
        let Ok(mut session) = self.session.try_borrow_mut() else {
            return;
        };

        match session.save() {
            Ok(()) => {
                drop(session);
                self.last_failure = None;
            }
            Err(error) => {
                drop(session);
                self.fail(format!("save: {error}"));
            }
        }
    }

    /// Note what the host reported, and refresh anything it may have changed.
    fn absorb(&mut self, outcome: InputOutcome) {
        if !outcome.handled && !outcome.redraw {
            return;
        }

        // Conservative on purpose. `redraw` is the host asking for one, which is
        // unambiguous; `handled` only means a tool or a hotkey claimed the event,
        // and a tool that claims an event has usually done something. Re-recording
        // a frame that turns out to be identical costs a recording pass and
        // re-tessellates nothing, because the retained groups come back unchanged.
        self.dirty = true;

        let Ok(mut session) = self.session.try_borrow_mut() else {
            return;
        };

        match session.editor_state() {
            Ok(state) => {
                self.selection_count = state.selection_count as usize;
                self.modified = state.modified;
                drop(session);
                self.last_failure = None;
            }
            Err(error) => {
                drop(session);
                self.fail(format!("editor state: {error}"));
            }
        }
    }

    /// Report a failure once rather than once per event.
    fn fail(&mut self, message: String) {
        if self.last_failure.as_deref() == Some(message.as_str()) {
            return;
        }

        eprintln!("eeschema-gpui: {message}");
        self.status = Some(message.clone());
        self.last_failure = Some(message);
    }
}

impl InputSink for HostInputSink {
    fn handle(&mut self, event: ShellEvent) {
        match &event {
            ShellEvent::PointerMove {
                screen, modifiers, ..
            } => self.dispatch(InputEvent::PointerMotion {
                screen: point(screen),
                modifiers: mods(*modifiers),
            }),

            ShellEvent::PointerDown {
                button,
                screen,
                modifiers,
                click_count,
                ..
            } => {
                // A double click is delivered *instead of* the second press of the
                // pair, which is how the wx dispatcher receives it too — and the
                // host's demotion rule for a fast click-drag depends on the press
                // that preceded it still being on record.
                let event = if *click_count >= 2 {
                    InputEvent::PointerDoubleClick {
                        button: map_button(*button),
                        screen: point(screen),
                        modifiers: mods(*modifiers),
                    }
                } else {
                    InputEvent::PointerDown {
                        button: map_button(*button),
                        screen: point(screen),
                        modifiers: mods(*modifiers),
                    }
                };

                self.dispatch(event);
            }

            ShellEvent::PointerUp {
                button,
                screen,
                modifiers,
                ..
            } => self.dispatch(InputEvent::PointerUp {
                button: map_button(*button),
                screen: point(screen),
                modifiers: mods(*modifiers),
            }),

            ShellEvent::PointerLeave => self.dispatch(InputEvent::PointerLeave),

            // See the module comment: the host detects its own drags from motion,
            // so a drag update is motion and the begin/end brackets are redundant
            // with the press and release around them.
            ShellEvent::DragUpdate {
                screen, modifiers, ..
            } => self.dispatch(InputEvent::PointerMotion {
                screen: point(screen),
                modifiers: mods(*modifiers),
            }),
            ShellEvent::DragBegin { .. } | ShellEvent::DragEnd { .. } => {}

            ShellEvent::Scroll {
                screen,
                delta,
                modifiers,
                ..
            } => self.dispatch(InputEvent::Scroll {
                screen: point(screen),
                // In detents, which is what the host expects and what KiCad's own
                // wheel events are scaled in.
                delta: (0.0, f64::from(delta.detents_y())),
                modifiers: mods(*modifiers),
            }),

            ShellEvent::KeyDown {
                key,
                modifiers,
                repeat,
            } => self.dispatch(InputEvent::KeyDown {
                key,
                modifiers: mods(*modifiers),
                auto_repeat: *repeat,
            }),

            ShellEvent::KeyUp { key, modifiers } => self.dispatch(InputEvent::KeyUp {
                key,
                modifiers: mods(*modifiers),
            }),

            ShellEvent::ToolCancelled => self.dispatch(InputEvent::Cancel),

            // Both of these are real KiCad action names — every `ToolId` in the
            // shell's tool table is one, deliberately — so they go to the action
            // registry rather than to the dispatcher.
            ShellEvent::ToolActivated(tool) => self.run_action(tool.as_str()),
            ShellEvent::ActionInvoked(action) => {
                // Undo, redo and save included: they reach the host through the action
                // registry like everything else, because `SCH_HOST` registers a tool that
                // handles them. That is what makes ⌘Z work as well as a menu item — a
                // hotkey is resolved inside `TOOL_MANAGER`, where the shell cannot
                // intervene. `Session::undo`/`redo`/`save` exist for a UI that would rather
                // ask directly, and the ABI's undo and redo counts are how a menu greys
                // itself out.
                let name = action.as_str().to_string();
                self.run_action(&name);
            }

            // The live document sets the viewport as part of asking for a frame,
            // which is the only place it can be set at the right moment: after the
            // camera has settled and before the frame is culled to it.
            ShellEvent::ViewportChanged(_) => {}

            _ => {}
        }
    }

    fn take_dirty(&mut self) -> bool {
        std::mem::take(&mut self.dirty)
    }

    fn selection_count(&self) -> usize {
        self.selection_count
    }

    fn status_message(&self) -> Option<&str> {
        self.status.as_deref()
    }

    fn modified(&self) -> Option<bool> {
        Some(self.modified)
    }
}

/// The shell's canvas-local screen point, as the ABI's pair of doubles.
fn point(screen: &input::ScreenPoint) -> (f64, f64) {
    (f64::from(screen.x), f64::from(screen.y))
}

fn mods(modifiers: input::Modifiers) -> Modifiers {
    Modifiers {
        shift: modifiers.shift,
        ctrl: modifiers.ctrl,
        alt: modifiers.alt,
        meta: modifiers.meta,
    }
}

fn map_button(button: input::PointerButton) -> PointerButton {
    match button {
        input::PointerButton::Left => PointerButton::Left,
        input::PointerButton::Right => PointerButton::Right,
        input::PointerButton::Middle => PointerButton::Middle,
        input::PointerButton::Back => PointerButton::Back,
        input::PointerButton::Forward => PointerButton::Forward,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kicad_sch_ui::input::{
        ActionId, Modifiers as ShellModifiers, PointerButton as ShellButton, ScreenPoint,
        ScrollDelta, ToolId, WorldPoint,
    };

    /// A target that records what it was given instead of driving C++.
    ///
    /// The events are kept as their debug form, which is exact — every field of
    /// `InputEvent` derives `Debug` — and reads well in a failure.
    #[derive(Default)]
    struct Recorder {
        events: Vec<String>,
        actions: Vec<String>,
        outcome: InputOutcome,
        state: EditorState,
        /// Undo, redo and save, recorded under those names, so a test can tell an action
        /// that went through the registry from one the sink short-circuited.
        commands: Vec<String>,
    }

    impl InputTarget for Recorder {
        fn dispatch_input(&mut self, event: &InputEvent<'_>) -> Result<InputOutcome, Error> {
            self.events.push(format!("{event:?}"));
            Ok(self.outcome)
        }

        fn run_action(&mut self, name: &str) -> Result<InputOutcome, Error> {
            self.actions.push(name.to_string());
            Ok(self.outcome)
        }

        fn editor_state(&mut self) -> Result<EditorState, Error> {
            Ok(self.state.clone())
        }

        fn undo(&mut self) -> Result<bool, Error> {
            self.commands.push("undo".to_string());
            Ok(true)
        }

        fn redo(&mut self) -> Result<bool, Error> {
            self.commands.push("redo".to_string());
            Ok(true)
        }

        fn save(&mut self) -> Result<(), Error> {
            self.commands.push("save".to_string());
            Ok(())
        }
    }

    struct Fixture {
        recorder: Rc<RefCell<Recorder>>,
        sink: HostInputSink,
    }

    fn fixture() -> Fixture {
        Fixture::with(InputOutcome::default(), EditorState::default())
    }

    impl Fixture {
        fn with(outcome: InputOutcome, state: EditorState) -> Fixture {
            let recorder = Rc::new(RefCell::new(Recorder {
                outcome,
                state,
                ..Recorder::default()
            }));
            // The same allocation under both names, which is what the shipping
            // path does too: the document and the sink share one session.
            let target: Rc<RefCell<dyn InputTarget>> = recorder.clone();

            Fixture {
                recorder,
                sink: HostInputSink::new(target),
            }
        }

        fn events(&self) -> Vec<String> {
            self.recorder.borrow().events.clone()
        }

        fn commands(&self) -> Vec<String> {
            self.recorder.borrow().commands.clone()
        }

        fn actions(&self) -> Vec<String> {
            self.recorder.borrow().actions.clone()
        }
    }

    fn at(x: f32, y: f32) -> ScreenPoint {
        ScreenPoint::new(x, y)
    }

    fn expect(event: InputEvent<'_>) -> String {
        format!("{event:?}")
    }

    #[test]
    fn unhandled_actions_are_visible_and_success_clears_the_message() {
        let mut fixture = fixture();
        fixture.sink.run_action("common.Control.open");
        assert!(
            fixture
                .sink
                .status_message()
                .unwrap()
                .contains("Not available")
        );
        fixture.recorder.borrow_mut().outcome.handled = true;
        fixture.sink.run_action("common.Control.save");
        assert!(fixture.sink.status_message().is_none());
    }

    #[test]
    fn unsaved_state_follows_the_document_after_edit_and_save() {
        let mut fixture = Fixture::with(
            InputOutcome {
                handled: true,
                redraw: true,
            },
            EditorState {
                modified: true,
                ..EditorState::default()
            },
        );
        fixture.sink.run_action("common.Interactive.delete");
        assert_eq!(fixture.sink.modified(), Some(true));
        fixture.recorder.borrow_mut().state.modified = false;
        fixture.sink.run_action("common.Control.save");
        assert_eq!(fixture.sink.modified(), Some(false));
    }

    #[test]
    fn a_pointer_move_becomes_host_motion_in_canvas_pixels() {
        let mut fixture = fixture();

        fixture.sink.handle(ShellEvent::PointerMove {
            screen: at(100.0, 50.0),
            world: WorldPoint::new(1.0, 2.0),
            modifiers: ShellModifiers::NONE,
        });

        assert_eq!(
            fixture.events(),
            vec![expect(InputEvent::PointerMotion {
                screen: (100.0, 50.0),
                modifiers: Modifiers::default(),
            })]
        );
    }

    /// The shell reports a drag as `DragBegin` and then `DragUpdate`, and stops
    /// reporting `PointerMove` while it does. The host detects its own drags from
    /// motion with KiCad's threshold, so the updates are what it needs and the
    /// brackets are redundant with the press and release around them — forwarding
    /// `DragBegin` would re-send the press position after the pointer had left it.
    #[test]
    fn a_drag_reaches_the_host_as_motion_and_nothing_else() {
        let mut fixture = fixture();

        fixture.sink.handle(ShellEvent::PointerDown {
            button: ShellButton::Left,
            screen: at(10.0, 10.0),
            world: WorldPoint::default(),
            modifiers: ShellModifiers::NONE,
            click_count: 1,
        });
        fixture.sink.handle(ShellEvent::DragBegin {
            button: ShellButton::Left,
            origin_screen: at(10.0, 10.0),
            origin_world: WorldPoint::default(),
            modifiers: ShellModifiers::NONE,
        });
        fixture.sink.handle(ShellEvent::DragUpdate {
            button: ShellButton::Left,
            screen: at(40.0, 10.0),
            world: WorldPoint::default(),
            delta_screen: at(30.0, 0.0),
            modifiers: ShellModifiers::NONE,
        });
        fixture.sink.handle(ShellEvent::DragEnd {
            button: ShellButton::Left,
            screen: at(40.0, 10.0),
            world: WorldPoint::default(),
            modifiers: ShellModifiers::NONE,
        });
        fixture.sink.handle(ShellEvent::PointerUp {
            button: ShellButton::Left,
            screen: at(40.0, 10.0),
            world: WorldPoint::default(),
            modifiers: ShellModifiers::NONE,
        });

        assert_eq!(
            fixture.events(),
            vec![
                expect(InputEvent::PointerDown {
                    button: PointerButton::Left,
                    screen: (10.0, 10.0),
                    modifiers: Modifiers::default(),
                }),
                expect(InputEvent::PointerMotion {
                    screen: (40.0, 10.0),
                    modifiers: Modifiers::default(),
                }),
                expect(InputEvent::PointerUp {
                    button: PointerButton::Left,
                    screen: (40.0, 10.0),
                    modifiers: Modifiers::default(),
                }),
            ]
        );
    }

    /// A second click of a pair is a double click and not another press, because
    /// that is what the wx dispatcher receives and what the host's
    /// demote-a-fast-drag rule is written against.
    #[test]
    fn a_second_click_is_reported_as_a_double_click() {
        let mut fixture = fixture();

        for count in [1usize, 2] {
            fixture.sink.handle(ShellEvent::PointerDown {
                button: ShellButton::Left,
                screen: at(5.0, 6.0),
                world: WorldPoint::default(),
                modifiers: ShellModifiers::NONE,
                click_count: count,
            });
        }

        assert_eq!(
            fixture.events(),
            vec![
                expect(InputEvent::PointerDown {
                    button: PointerButton::Left,
                    screen: (5.0, 6.0),
                    modifiers: Modifiers::default(),
                }),
                expect(InputEvent::PointerDoubleClick {
                    button: PointerButton::Left,
                    screen: (5.0, 6.0),
                    modifiers: Modifiers::default(),
                }),
            ]
        );
    }

    #[test]
    fn modifiers_and_buttons_survive_the_crossing() {
        let mut fixture = fixture();

        fixture.sink.handle(ShellEvent::PointerDown {
            button: ShellButton::Right,
            screen: at(1.0, 2.0),
            world: WorldPoint::default(),
            modifiers: ShellModifiers {
                ctrl: true,
                shift: true,
                alt: false,
                meta: false,
            },
            click_count: 1,
        });

        assert_eq!(
            fixture.events(),
            vec![expect(InputEvent::PointerDown {
                button: PointerButton::Right,
                screen: (1.0, 2.0),
                modifiers: Modifiers {
                    shift: true,
                    ctrl: true,
                    alt: false,
                    meta: false,
                },
            })]
        );
    }

    /// Scroll deltas cross in detents, because that is the unit KiCad's own wheel
    /// events are scaled in; pixels are the shell's problem and it converts.
    #[test]
    fn scroll_crosses_in_detents_however_the_platform_reported_it() {
        let mut fixture = fixture();

        fixture.sink.handle(ShellEvent::Scroll {
            screen: at(0.0, 0.0),
            world: WorldPoint::default(),
            delta: ScrollDelta::Pixels { x: 0.0, y: 40.0 },
            modifiers: ShellModifiers::NONE,
        });

        assert_eq!(
            fixture.events(),
            vec![expect(InputEvent::Scroll {
                screen: (0.0, 0.0),
                delta: (0.0, 2.0),
                modifiers: Modifiers::default(),
            })]
        );
    }

    #[test]
    fn keys_cross_by_name_and_a_cancel_is_its_own_event() {
        let mut fixture = fixture();

        fixture.sink.handle(ShellEvent::KeyDown {
            key: "escape".to_string(),
            modifiers: ShellModifiers::NONE,
            repeat: true,
        });
        fixture.sink.handle(ShellEvent::KeyUp {
            key: "escape".to_string(),
            modifiers: ShellModifiers::NONE,
        });
        fixture.sink.handle(ShellEvent::ToolCancelled);

        assert_eq!(
            fixture.events(),
            vec![
                expect(InputEvent::KeyDown {
                    key: "escape",
                    modifiers: Modifiers::default(),
                    auto_repeat: true,
                }),
                expect(InputEvent::KeyUp {
                    key: "escape",
                    modifiers: Modifiers::default(),
                }),
                expect(InputEvent::Cancel),
            ]
        );
    }

    /// Tool ids and the shell's reported action names are real `TOOL_ACTION` names,
    /// so they go to the registry. The shell's own chrome reports under a
    /// `kicad.ui.` name precisely so it can be told apart, and asking KiCad for
    /// those would be asking for actions it has never heard of, once per click.
    #[test]
    fn kicad_actions_are_forwarded_and_the_shells_own_chrome_is_not() {
        let mut fixture = fixture();

        fixture
            .sink
            .handle(ShellEvent::ToolActivated(ToolId("eeschema.x.drawWires")));
        fixture
            .sink
            .handle(ShellEvent::ActionInvoked(ActionId::from(
                "common.Control.zoomFitScreen",
            )));
        fixture
            .sink
            .handle(ShellEvent::ActionInvoked(ActionId::from(
                "kicad.ui.toggleTheme",
            )));

        assert_eq!(
            fixture.actions(),
            vec![
                "eeschema.x.drawWires".to_string(),
                "common.Control.zoomFitScreen".to_string()
            ]
        );
        assert!(fixture.events().is_empty());
    }

    /// Undo, redo and save go through the action registry like everything else, rather
    /// than being short-circuited to the ABI calls of the same names.
    ///
    /// That is the whole reason `SCH_HOST` registers `SCH_HOST_CONTROL`: a hotkey is
    /// resolved inside `TOOL_MANAGER`, so ⌘Z arrives here as a key press and never as an
    /// action, and a sink that special-cased the *action* names would make the menu work and
    /// the key not. `Session::undo` and friends stay available for a UI that wants the
    /// answer rather than a fire-and-forget.
    #[test]
    fn undo_redo_and_save_go_through_the_registry_like_everything_else() {
        let mut fixture = fixture();

        for name in [
            "common.Interactive.undo",
            "common.Interactive.redo",
            "common.Control.save",
        ] {
            fixture
                .sink
                .handle(ShellEvent::ActionInvoked(ActionId::from(name)));
        }

        assert_eq!(
            fixture.actions(),
            vec![
                "common.Interactive.undo".to_string(),
                "common.Interactive.redo".to_string(),
                "common.Control.save".to_string(),
            ]
        );
        assert!(
            fixture.commands().is_empty(),
            "the sink should not have bypassed the registry"
        );
    }

    /// The viewport is set by the live document as part of asking for a frame,
    /// which is the only moment it can be right: after the camera has settled and
    /// before the frame is culled to it. Sending it from here as well would point
    /// the session at a camera the canvas is not painting with.
    #[test]
    fn a_viewport_report_is_not_forwarded() {
        let mut fixture = fixture();

        fixture
            .sink
            .handle(ShellEvent::ViewportChanged(input::ViewportState {
                width: 800.0,
                height: 600.0,
                scale: 1.0,
                center: WorldPoint::default(),
            }));

        assert!(fixture.events().is_empty());
        assert!(fixture.actions().is_empty());
    }

    /// Nothing claimed the event, so nothing changed, so the canvas is not asked
    /// to record a frame it already has. This is the property that keeps a free
    /// pointer-move from costing a recording pass.
    #[test]
    fn an_unclaimed_event_does_not_dirty_the_canvas() {
        let mut fixture = fixture();

        fixture.sink.handle(ShellEvent::PointerMove {
            screen: at(1.0, 1.0),
            world: WorldPoint::default(),
            modifiers: ShellModifiers::NONE,
        });

        assert!(!fixture.sink.take_dirty());
        assert_eq!(fixture.sink.selection_count(), 0);
    }

    /// ...and an event the host claimed does, and brings the selection back with
    /// it. This is the path that makes an edit or a selection visible, and it is
    /// the one that cannot be exercised end to end yet, because no tool runs on a
    /// non-frame holder to claim anything.
    #[test]
    fn a_claimed_event_dirties_the_canvas_and_refreshes_the_selection() {
        let mut fixture = Fixture::with(
            InputOutcome {
                handled: true,
                redraw: true,
            },
            EditorState {
                selection_count: 3,
                ..EditorState::default()
            },
        );

        fixture.sink.handle(ShellEvent::PointerDown {
            button: ShellButton::Left,
            screen: at(1.0, 1.0),
            world: WorldPoint::default(),
            modifiers: ShellModifiers::NONE,
            click_count: 1,
        });

        assert_eq!(fixture.sink.selection_count(), 3);
        assert!(fixture.sink.take_dirty());

        // Reading it clears it, so one edit does not re-record for ever.
        assert!(!fixture.sink.take_dirty());
    }
}
