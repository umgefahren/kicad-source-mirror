// SPDX-License-Identifier: GPL-3.0-or-later
//! Independent GPUI windows for schematic workflows.
use crate::commands::{CancelTool, OpenCommandPalette, Quit, RunAction};
use crate::shell::{CloseWindow, SchematicShell};
use gpui_kit::component::{ActiveTheme, Root, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{
    AnyView, AnyWindowHandle, App, Context, Entity, FocusHandle, SharedString, Subscription,
    TestSupportExt, WeakEntity, Window, WindowBounds, WindowKind, WindowOptions, div, px, size,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum DialogKind {
    Erc,
    Properties,
    Symbols,
    Search,
    Document(u32),
    LibraryTable,
    Simulation,
    LibrarySymbol(u32),
    SheetPins,
    ImageImport,
    GraphicsImport,
    Setup,
    Preferences,
}
impl DialogKind {
    pub(crate) fn simulation(kind: u32) -> Self {
        match kind {
            3..=12 | 16 | 18 | 22 => Self::LibrarySymbol(kind),
            _ => Self::Simulation,
        }
    }
}

pub(crate) struct DialogEntry {
    pub window: AnyWindowHandle,
    pub view: Entity<DialogWindow>,
}

pub(crate) struct DialogWindow {
    content: AnyView,
    focus: FocusHandle,
    initial_focus: Option<FocusHandle>,
    owner: WeakEntity<SchematicShell>,
    owner_window: AnyWindowHandle,
    kind: DialogKind,
    _owner_release: Subscription,
}

impl DialogWindow {
    pub(crate) fn open(
        kind: DialogKind,
        title: SharedString,
        content: AnyView,
        initial_focus: Option<FocusHandle>,
        owner: Entity<SchematicShell>,
        parent: &Window,
        cx: &mut App,
    ) -> gpui_kit::Result<DialogEntry> {
        let owner_window = parent.window_handle();
        let weak_owner = owner.downgrade();
        let bounds = gpui_kit::Bounds::centered(None, size(px(920.), px(680.)), cx);
        let mut view = None;
        let window = cx.open_window(
            WindowOptions {
                window_bounds: Some(WindowBounds::Windowed(bounds)),
                titlebar: Some(gpui_kit::TitlebarOptions {
                    title: Some(title),
                    ..Default::default()
                }),
                kind: WindowKind::Normal,
                inactive_frame_interval: None,
                window_min_size: Some(size(px(480.), px(320.))),
                app_id: Some("org.kicad.eeschema-gpui".into()),
                ..Default::default()
            },
            |window, cx| {
                let handle = window.window_handle();
                let weak = weak_owner.clone();
                window.on_window_should_close(cx, move |_, cx| {
                    close_owner_dialog(&weak, owner_window, kind, cx);
                    true
                });
                let entity = cx.new(|cx| {
                    let release = cx.observe_release(&owner, move |_, _, cx| {
                        cx.defer(move |cx| {
                            let _ = handle.update(cx, |_, window, _| window.remove_window());
                        });
                    });
                    let focus = cx.focus_handle();
                    Self {
                        content,
                        focus,
                        initial_focus,
                        owner: weak_owner,
                        owner_window,
                        kind,
                        _owner_release: release,
                    }
                });
                view = Some(entity.clone());
                cx.new(|cx| Root::new(entity, window, cx))
            },
        )?;
        Ok(DialogEntry {
            window: window.into(),
            view: view.expect("window view created"),
        })
    }

    pub(crate) fn replace(
        &mut self,
        content: AnyView,
        initial_focus: Option<FocusHandle>,
        cx: &mut Context<Self>,
    ) {
        self.content = content;
        self.initial_focus = Some(initial_focus.unwrap_or_else(|| self.focus.clone()));
        cx.notify();
    }

    fn close(&mut self, window: &mut Window, cx: &mut Context<Self>) {
        close_owner_dialog(&self.owner, self.owner_window, self.kind, cx);
        window.remove_window();
    }

    fn forward(&self, action: Box<dyn gpui_kit::Action>, cx: &mut Context<Self>) {
        let owner_window = self.owner_window;
        cx.defer(move |cx| {
            let _ = owner_window.update(cx, |_, window, cx| {
                window.activate_window();
                window.dispatch_action(action, cx);
            });
        });
    }
}

fn close_owner_dialog(
    owner: &WeakEntity<SchematicShell>,
    owner_window: AnyWindowHandle,
    kind: DialogKind,
    cx: &mut App,
) {
    let owner = owner.clone();
    cx.defer(move |cx| {
        let _ = owner_window.update(cx, |_, window, cx| {
            let _ = owner.update(cx, |shell, cx| shell.dialog_closed(kind, window, cx));
        });
    });
}

impl Render for DialogWindow {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        if let Some(focus) = self.initial_focus.take() {
            focus.focus(window, cx);
        } else if window.focused(cx).is_none() {
            self.focus.focus(window, cx);
        }
        div()
            .id("workflow-window")
            .test_support()
            .size_full()
            .v_flex()
            .track_focus(&self.focus)
            .key_context("SchematicDialog")
            .bg(cx.theme().background)
            .text_color(cx.theme().foreground)
            .font_family(cx.theme().font_family.clone())
            .on_action(cx.listener(|this, _: &CloseWindow, window, cx| this.close(window, cx)))
            .on_action(cx.listener(|this, _: &CancelTool, window, cx| this.close(window, cx)))
            .on_action(
                cx.listener(|this, action: &Quit, _, cx| {
                    this.forward(Box::new(action.clone()), cx)
                }),
            )
            .on_action(cx.listener(|this, action: &RunAction, window, cx| {
                if !crate::app_menu::route_input_edit(action.id.as_ref(), window, cx) {
                    this.forward(Box::new(action.clone()), cx)
                }
            }))
            .on_action(cx.listener(|this, action: &OpenCommandPalette, _, cx| {
                this.forward(Box::new(action.clone()), cx)
            }))
            .child(self.content.clone())
    }
}
