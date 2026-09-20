// SPDX-License-Identifier: GPL-3.0-or-later
//! Platform menu presentation, sharing the editor's action registry and layout.
use gpui_kit::{App, Menu};

use crate::commands::{self, ActionRegistry};

/// Install platform-native menus on macOS and the kit menu model elsewhere.
pub(crate) fn install(cx: &mut App) {
    let menus = if let Some(registry) = cx.try_global::<ActionRegistry>() {
        commands::app_menus_with_registry(registry)
    } else {
        commands::app_menus()
    };
    #[cfg(target_os = "macos")]
    {
        cx.on_action(|_: &HideApplication, cx| cx.hide());
        cx.on_action(|_: &HideOtherApplications, cx| cx.hide_other_apps());
        cx.on_action(|_: &ShowAllApplications, cx| cx.unhide_other_apps());
        cx.bind_keys([
            gpui_kit::KeyBinding::new("cmd-h", HideApplication, None),
            gpui_kit::KeyBinding::new("cmd-alt-h", HideOtherApplications, None),
            gpui_kit::KeyBinding::new("cmd-q", commands::Quit, None),
            gpui_kit::KeyBinding::new(
                "cmd-,",
                commands::RunAction::new("common.SuiteControl.openPreferences"),
                None,
            ),
        ]);
        cx.set_menus(macos_menus(menus));
    }
    #[cfg(not(target_os = "macos"))]
    {
        use gpui_kit::base::GlobalState;
        GlobalState::global_mut(cx).set_app_menus(menus.into_iter().map(Menu::owned).collect());
    }
}

/// Route native editing commands to the focused kit input before canvas dispatch.
/// Returns true only when an input consumed a recognized editing command.
pub(crate) fn route_input_edit(id: &str, window: &mut gpui_kit::Window, cx: &mut App) -> bool {
    if !window
        .context_stack()
        .iter()
        .any(|context| context.contains("Input"))
    {
        return false;
    }
    let Some(action) = input_edit_action(id) else {
        return false;
    };
    window.dispatch_action(action, cx);
    cx.stop_propagation();
    true
}

fn input_edit_action(id: &str) -> Option<Box<dyn gpui_kit::Action>> {
    use gpui_kit::component::input;
    Some(match id {
        "common.Interactive.cut" => Box::new(input::Cut),
        "common.Interactive.copy" => Box::new(input::Copy),
        "common.Interactive.paste" => Box::new(input::Paste),
        "common.Interactive.selectAll" => Box::new(input::SelectAll),
        "common.Interactive.undo" => Box::new(input::Undo),
        "common.Interactive.redo" => Box::new(input::Redo),
        _ => return None,
    })
}

#[cfg(any(target_os = "macos", test))]
gpui_kit::actions!(
    kicad_native_menu,
    [HideApplication, HideOtherApplications, ShowAllApplications]
);

/// Adapt the shared layout without replacing the actions or their shortcuts.
#[cfg(any(target_os = "macos", test))]
fn macos_menus(mut menus: Vec<Menu>) -> Vec<Menu> {
    use crate::commands::{Quit, RunAction};
    use gpui_kit::{MenuItem, OsAction, SystemMenuType};

    let mut about = None;
    let mut preferences = None;
    for menu in &mut menus {
        menu.items = std::mem::take(&mut menu.items)
            .into_iter()
            .filter_map(|mut item| {
                if let MenuItem::Action {
                    action, os_action, ..
                } = &mut item
                {
                    if action.as_any().is::<Quit>() {
                        return None;
                    }
                    if let Some(command) = action.as_any().downcast_ref::<RunAction>() {
                        match command.id.as_ref() {
                            "common.SuiteControl.about" => {
                                about = Some(item);
                                return None;
                            }
                            "common.SuiteControl.openPreferences" => {
                                preferences = Some(item);
                                return None;
                            }
                            "common.Interactive.cut" => *os_action = Some(OsAction::Cut),
                            "common.Interactive.copy" => *os_action = Some(OsAction::Copy),
                            "common.Interactive.paste" => *os_action = Some(OsAction::Paste),
                            "common.Interactive.selectAll" => {
                                *os_action = Some(OsAction::SelectAll)
                            }
                            "common.Interactive.undo" => *os_action = Some(OsAction::Undo),
                            "common.Interactive.redo" => *os_action = Some(OsAction::Redo),
                            _ => {}
                        }
                    }
                }
                Some(item)
            })
            .collect();
        while matches!(menu.items.last(), Some(MenuItem::Separator)) {
            menu.items.pop();
        }
    }
    let mut application = Vec::new();
    if let Some(item) = about {
        application.push(item);
        application.push(MenuItem::separator());
    }
    if let Some(item) = preferences {
        application.push(item);
        application.push(MenuItem::separator());
    }
    application.extend([
        MenuItem::os_submenu("Services", SystemMenuType::Services),
        MenuItem::separator(),
        MenuItem::action("Hide KiCad", HideApplication),
        MenuItem::action("Hide Others", HideOtherApplications),
        MenuItem::action("Show All", ShowAllApplications),
        MenuItem::separator(),
        // Keep the shell's save-before-close flow; never call App::quit here.
        MenuItem::action("Quit KiCad", Quit),
    ]);
    if let Some(file) = menus.iter_mut().find(|menu| menu.name.as_ref() == "File") {
        file.items.push(MenuItem::separator());
        file.items
            .push(MenuItem::action("Close Window", crate::shell::CloseWindow));
    }
    menus.retain(|menu| !menu.items.is_empty());
    menus.insert(0, Menu::new("KiCad").items(application));
    menus
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::commands::{Quit, RunAction};
    use gpui_kit::{MenuItem, OsAction, SystemMenuType};

    #[test]
    fn native_edit_commands_map_to_kit_inputs_not_schematic_actions() {
        use gpui_kit::component::input;
        assert!(
            input_edit_action("common.Interactive.copy")
                .unwrap()
                .as_any()
                .is::<input::Copy>()
        );
        assert!(
            input_edit_action("common.Interactive.cut")
                .unwrap()
                .as_any()
                .is::<input::Cut>()
        );
        assert!(
            input_edit_action("common.Interactive.paste")
                .unwrap()
                .as_any()
                .is::<input::Paste>()
        );
        assert!(
            input_edit_action("common.Interactive.selectAll")
                .unwrap()
                .as_any()
                .is::<input::SelectAll>()
        );
        assert!(
            input_edit_action("common.Interactive.undo")
                .unwrap()
                .as_any()
                .is::<input::Undo>()
        );
        assert!(
            input_edit_action("common.Interactive.redo")
                .unwrap()
                .as_any()
                .is::<input::Redo>()
        );
        assert!(input_edit_action("eeschema.EditorControl.annotate").is_none());
    }

    #[test]
    fn macos_uses_application_menu_and_preserves_editor_dispatch() {
        let menus = macos_menus(commands::app_menus());
        assert_eq!(menus[0].name.as_ref(), "KiCad");
        let app = &menus[0].items;
        assert!(app.iter().any(|item| matches!(item, MenuItem::SystemMenu(menu) if menu.menu_type == SystemMenuType::Services)));
        assert!(app.iter().any(
            |item| matches!(item, MenuItem::Action { action, .. } if action.as_any().is::<Quit>())
        ));
        for id in [
            "common.SuiteControl.about",
            "common.SuiteControl.openPreferences",
        ] {
            assert!(app.iter().any(|item| matches!(item, MenuItem::Action { action, .. } if action.as_any().downcast_ref::<RunAction>().is_some_and(|command| command.id.as_ref() == id))));
        }
        let file = menus
            .iter()
            .find(|menu| menu.name.as_ref() == "File")
            .unwrap();
        assert!(!file.items.iter().any(
            |item| matches!(item, MenuItem::Action { action, .. } if action.as_any().is::<Quit>())
        ));
        let edit = menus
            .iter()
            .find(|menu| menu.name.as_ref() == "Edit")
            .unwrap();
        assert!(edit.items.iter().any(|item| matches!(item, MenuItem::Action { action, os_action: Some(OsAction::Copy), .. } if action.as_any().downcast_ref::<RunAction>().is_some_and(|command| command.id.as_ref() == "common.Interactive.copy"))));
        assert!(menus.iter().any(|menu| menu.name.as_ref() == "Place"));
    }

    #[test]
    fn macos_keeps_registry_filtering_and_localized_action_labels() {
        let registry = ActionRegistry::new(vec![commands::ActionInfo {
            name: "common.Control.save".into(),
            label: "Save localized".into(),
            hotkey: "Cmd+S".into(),
            ..Default::default()
        }]);
        let menus = macos_menus(commands::app_menus_with_registry(&registry));
        assert_eq!(menus[1].name.as_ref(), "File");
        assert_eq!(menus[1].items.len(), 3);
        assert!(
            matches!(&menus[1].items[2], MenuItem::Action { action, .. } if action.as_any().is::<crate::shell::CloseWindow>())
        );
        assert!(
            matches!(&menus[1].items[0], MenuItem::Action { name, action, .. } if name.as_ref() == "Save localized" && action.as_any().downcast_ref::<RunAction>().unwrap().id.as_ref() == "common.Control.save")
        );
    }

    struct NativeInputHarness {
        input: gpui_kit::Entity<gpui_kit::component::input::InputState>,
        canvas_calls: std::rc::Rc<std::cell::Cell<usize>>,
    }
    impl gpui_kit::Render for NativeInputHarness {
        fn render(
            &mut self,
            _: &mut gpui_kit::Window,
            cx: &mut gpui_kit::Context<Self>,
        ) -> impl gpui_kit::IntoElement {
            use gpui_kit::prelude::*;
            gpui_kit::div()
                .on_action(cx.listener(|this, action: &RunAction, window, cx| {
                    if !route_input_edit(action.id.as_ref(), window, cx) {
                        this.canvas_calls.set(this.canvas_calls.get() + 1);
                    }
                }))
                .child(gpui_kit::component::input::Input::new(&self.input))
        }
    }

    #[gpui_kit::test]
    fn native_menu_select_all_copy_and_cut_target_focused_input(cx: &mut gpui_kit::TestAppContext) {
        use gpui_kit::test::TestWindowExt as _;
        use gpui_kit::{AppContext as _, Focusable as _, px, size};
        cx.update(gpui_kit::init);
        let calls = std::rc::Rc::new(std::cell::Cell::new(0));
        let mut input = None;
        let handle = cx.open_window(size(px(480.), px(240.)), |window, cx| {
            let state = cx.new(|cx| {
                gpui_kit::component::input::InputState::new(window, cx).default_value("47k")
            });
            state.read(cx).focus_handle(cx).focus(window, cx);
            input = Some(state.clone());
            let view = cx.new(|_| NativeInputHarness {
                input: state,
                canvas_calls: calls.clone(),
            });
            gpui_kit::component::Root::new(view, window, cx)
        });
        let handle = handle.into();
        let input = input.unwrap();
        for id in [
            "common.Interactive.selectAll",
            "common.Interactive.copy",
            "common.Interactive.cut",
        ] {
            cx.update_window(handle, |_, window, cx| {
                window.render_frame(cx);
                window.dispatch_action(Box::new(RunAction::new(id)), cx);
            })
            .unwrap();
            cx.run_until_parked();
        }
        cx.update(|cx| assert_eq!(input.read(cx).value().as_ref(), ""));
        assert_eq!(
            calls.get(),
            0,
            "native edit actions must never reach the schematic"
        );
    }
}
