// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The command catalogue: one table from which the menu bar, the key map and
//! the command palette are all generated.
//!
//! # Why there are two kinds of action
//!
//! gpui dispatches *typed* actions, and the obvious thing is one type per
//! command. That is right for the fifteen commands the shell answers itself —
//! they need a handler each anyway, so a type each costs nothing and reads
//! well. It is wrong for the hundred-odd commands that only exist to be handed
//! to KiCad: they would be a hundred empty structs whose only content is a
//! string, and the string is the part that has to match `sch_actions.cpp`.
//!
//! So: [`ShellCommand`] covers what the shell does (declared with
//! `gpui_kit::actions!`, one unit struct each), and [`RunAction`] carries a
//! KiCad `TOOL_ACTION` name for everything else. [`MENUS`] supplies layout;
//! [`ActionRegistry`] supplies live KiCad labels and configured shortcuts.
//! The static metadata is retained for standalone replay without a C++ host.

use std::rc::Rc;

use gpui_kit::assets::IconName;
use gpui_kit::{Action, KeyBinding, KeyBindingContextPredicate, Menu, MenuItem, SharedString};

use crate::tools::{TOOLS, Tool};

// The commands the shell answers without involving the document: view
// transforms, chrome, and quitting. One unit struct each, because each one
// also needs an `on_action` handler.
gpui_kit::actions!(
    kicad,
    [
        /// Close the editor.
        Quit,
        /// Zoom in one step about the canvas centre.
        ZoomIn,
        /// Zoom out one step about the canvas centre.
        ZoomOut,
        /// Frame the whole sheet.
        ZoomToFit,
        /// Frame the drawn items rather than the sheet.
        ZoomToObjects,
        /// Return to 1:1.
        ZoomActualSize,
        /// Show or hide the grid.
        ToggleGrid,
        /// Step to the next grid spacing.
        CycleGrid,
        /// Switch between millimetres and mils.
        ToggleUnits,
        /// Switch between the dark and light themes.
        ToggleTheme,
        /// Show or hide the hierarchy dock.
        ToggleLeftPanel,
        /// Show or hide the properties dock.
        ToggleRightPanel,
        /// Show or hide the frame-time readout.
        ToggleFrameStats,
        /// Open the command palette.
        OpenCommandPalette,
        /// Abandon the active tool and go back to Select.
        CancelTool,
    ]
);

/// Invoke a KiCad `TOOL_ACTION` by name.
///
/// `no_json` because these are never built from a keymap file in this
/// process — the shell constructs them from [`MENUS`] — and it spares the
/// crate a `serde` and `schemars` dependency.
#[derive(Clone, Debug, Default, gpui_kit::Action)]
#[action(namespace = kicad, no_json)]
pub struct RunAction {
    /// The KiCad action name, for example `eeschema.EditorControl.save`.
    pub id: SharedString,
    /// Tool shortcut, when invoked at the canvas cursor rather than from chrome.
    pub hotkey: Option<SharedString>,
}

// Keyboard invocation carries cursor-dispatch metadata, but it is still the
// same command as a menu or palette invocation. GPUI uses action equality to
// discover shortcut hints; including that metadata hid every tool shortcut.
impl PartialEq for RunAction {
    fn eq(&self, other: &Self) -> bool {
        self.id == other.id
    }
}

impl RunAction {
    /// An action that invokes `id`.
    pub fn new(id: impl Into<SharedString>) -> Self {
        Self {
            id: id.into(),
            hotkey: None,
        }
    }
}

/// The shell-owned commands, as one enum so tables can name them.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ShellCommand {
    /// Resolve custom field name case conflicts.
    FieldCaseConflicts,
    /// Manage schematic data source packages.
    SchematicDataSources,
    /// Migrate legacy bus labels.
    MigrateBuses,
    /// Edit bus aliases.
    BusAliases,
    /// Edit net classes.
    Netclasses,
    /// Edit net chains.
    NetChains,
    /// Import schematic settings.
    ImportSettings,
    /// Configure remote symbol providers.
    RemoteSymbolSettings,
    /// Close the editor.
    Quit,
    /// Zoom in one step.
    ZoomIn,
    /// Zoom out one step.
    ZoomOut,
    /// Frame the whole sheet.
    ZoomToFit,
    /// Frame the drawn items.
    ZoomToObjects,
    /// Return to 1:1.
    ZoomActualSize,
    /// Show or hide the grid.
    ToggleGrid,
    /// Step to the next grid spacing.
    CycleGrid,
    /// Move to the next display unit.
    ToggleUnits,
    /// Swap the dark and light themes.
    ToggleTheme,
    /// Show or hide the hierarchy dock.
    ToggleLeftPanel,
    /// Show or hide the properties dock.
    ToggleRightPanel,
    /// Show or hide the frame-time readout.
    ToggleFrameStats,
    /// Open the command palette.
    OpenCommandPalette,
    /// Abandon the active tool.
    CancelTool,
}

impl ShellCommand {
    /// The gpui action this command dispatches.
    pub fn action(self) -> Box<dyn Action> {
        match self {
            ShellCommand::BusAliases
            | ShellCommand::Netclasses
            | ShellCommand::NetChains
            | ShellCommand::ImportSettings => Box::new(RunAction::new(self.reported_id())),
            ShellCommand::FieldCaseConflicts | ShellCommand::SchematicDataSources => {
                Box::new(RunAction::new(self.reported_id()))
            }
            ShellCommand::MigrateBuses => Box::new(RunAction::new(self.reported_id())),
            ShellCommand::RemoteSymbolSettings => Box::new(RunAction::new(self.reported_id())),
            ShellCommand::Quit => Box::new(Quit),
            ShellCommand::ZoomIn => Box::new(ZoomIn),
            ShellCommand::ZoomOut => Box::new(ZoomOut),
            ShellCommand::ZoomToFit => Box::new(ZoomToFit),
            ShellCommand::ZoomToObjects => Box::new(ZoomToObjects),
            ShellCommand::ZoomActualSize => Box::new(ZoomActualSize),
            ShellCommand::ToggleGrid => Box::new(ToggleGrid),
            ShellCommand::CycleGrid => Box::new(CycleGrid),
            ShellCommand::ToggleUnits => Box::new(ToggleUnits),
            ShellCommand::ToggleTheme => Box::new(ToggleTheme),
            ShellCommand::ToggleLeftPanel => Box::new(ToggleLeftPanel),
            ShellCommand::ToggleRightPanel => Box::new(ToggleRightPanel),
            ShellCommand::ToggleFrameStats => Box::new(ToggleFrameStats),
            ShellCommand::OpenCommandPalette => Box::new(OpenCommandPalette),
            ShellCommand::CancelTool => Box::new(CancelTool),
        }
    }

    /// The KiCad action name reported to the host when this command runs, for
    /// the commands that have a KiCad counterpart. Pure chrome — panels,
    /// themes, the frame readout — reports under a `kicad.ui.` name instead,
    /// so the host can tell the two populations apart and ignore ours.
    pub fn reported_id(self) -> &'static str {
        match self {
            ShellCommand::BusAliases => "gpui.Setup.buses",
            ShellCommand::Netclasses => "gpui.Setup.netclasses",
            ShellCommand::NetChains => "gpui.Setup.netchains",
            ShellCommand::ImportSettings => "gpui.Setup.import",
            ShellCommand::FieldCaseConflicts => "gpui.FieldCaseConflicts",
            ShellCommand::SchematicDataSources => "gpui.SchematicDataSources",
            ShellCommand::MigrateBuses => "gpui.MigrateBuses",
            ShellCommand::RemoteSymbolSettings => "gpui.RemoteSymbols.settings",
            ShellCommand::Quit => "common.Control.quit",
            ShellCommand::ZoomIn => "common.Control.zoomIn",
            ShellCommand::ZoomOut => "common.Control.zoomOut",
            ShellCommand::ZoomToFit => "common.Control.zoomFitScreen",
            ShellCommand::ZoomToObjects => "common.Control.zoomFitObjects",
            ShellCommand::ZoomActualSize => "common.Control.zoomPreset",
            ShellCommand::ToggleGrid => "common.Control.toggleGrid",
            ShellCommand::CycleGrid => "common.Control.gridNext",
            ShellCommand::ToggleUnits => "common.Control.toggleUnits",
            ShellCommand::CancelTool => "common.Interactive.cancel",
            ShellCommand::ToggleTheme => "kicad.ui.toggleTheme",
            ShellCommand::ToggleLeftPanel => "kicad.ui.toggleLeftPanel",
            ShellCommand::ToggleRightPanel => "kicad.ui.toggleRightPanel",
            ShellCommand::ToggleFrameStats => "kicad.ui.toggleFrameStats",
            ShellCommand::OpenCommandPalette => "kicad.ui.openCommandPalette",
        }
    }
}

/// What a menu entry or palette row does.
#[derive(Clone, Copy, Debug)]
pub enum CommandKind {
    /// Hand a KiCad `TOOL_ACTION` name to the host.
    Kicad(&'static str),
    /// Make a tool active. Reported as a tool activation, not an action.
    Tool(Tool),
    /// Something the shell does itself.
    Shell(ShellCommand),
}

/// One invocable command.
#[derive(Clone, Copy, Debug)]
pub struct CommandSpec {
    /// Menu and palette label.
    pub label: &'static str,
    /// What invoking it does.
    pub kind: CommandKind,
    /// Default key binding, as a gpui keystroke string.
    pub key: Option<&'static str>,
    /// Icon for the palette row, where one helps.
    pub icon: Option<IconName>,
}

impl CommandSpec {
    /// The gpui action this command dispatches.
    pub fn action(&self) -> Box<dyn Action> {
        match self.kind {
            CommandKind::Kicad(id) => Box::new(RunAction::new(id)),
            CommandKind::Tool(tool) => Box::new(RunAction::new(tool.id().as_str())),
            CommandKind::Shell(command) => command.action(),
        }
    }

    /// The name this command reports to the host.
    pub fn reported_id(&self) -> &'static str {
        match self.kind {
            CommandKind::Kicad(id) => id,
            CommandKind::Tool(tool) => tool.id().as_str(),
            CommandKind::Shell(command) => command.reported_id(),
        }
    }
}

/// Owned metadata copied from the host's action registry.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ActionInfo {
    /// Stable TOOL_ACTION name.
    pub name: String,
    /// Localized menu label, without wx mnemonic markup.
    pub label: String,
    /// Localized help text.
    pub description: String,
    /// Configured shortcut; normalized to gpui notation by `ActionRegistry::new`.
    pub hotkey: String,
    /// Alternate configured shortcut.
    pub hotkey_alt: String,
}

/// Snapshot of KiCad's registered actions for this editor.
#[derive(Clone, Debug, Default)]
pub struct ActionRegistry {
    actions: std::collections::BTreeMap<String, ActionInfo>,
}

impl gpui_kit::Global for ActionRegistry {}

impl ActionRegistry {
    /// Copy a host snapshot and translate KiCad's key names once.
    pub fn new(actions: Vec<ActionInfo>) -> Self {
        Self {
            actions: actions
                .into_iter()
                .map(|mut info| {
                    info.hotkey = normalize_hotkey(&info.hotkey);
                    info.hotkey_alt = normalize_hotkey(&info.hotkey_alt);
                    info.label = strip_mnemonics(&info.label);
                    (info.name.clone(), info)
                })
                .collect(),
        }
    }

    /// Look up a registered action by its stable name.
    pub fn get(&self, name: &str) -> Option<&ActionInfo> {
        self.actions.get(name)
    }

    /// Resolve a layout slot against the host, omitting unavailable actions.
    pub fn resolve(&self, spec: &CommandSpec) -> Option<ResolvedCommand> {
        let info = self.get(spec.reported_id());
        if info.is_none() && !matches!(spec.kind, CommandKind::Shell(_)) {
            return None;
        }
        let label = info
            .map(|i| {
                if i.label.is_empty() {
                    i.name.clone()
                } else {
                    i.label.clone()
                }
            })
            .unwrap_or_else(|| label_of(spec).to_string());
        let key = match info {
            Some(info) => (!info.hotkey.is_empty()).then(|| info.hotkey.clone()),
            None => key_of(spec).map(platform_default_key),
        };
        Some(ResolvedCommand {
            label,
            description: info.map(|i| i.description.clone()).unwrap_or_default(),
            key,
            kind: spec.kind,
            icon: spec.icon.or_else(|| match spec.kind {
                CommandKind::Tool(tool) => Some(tool.spec().icon),
                _ => None,
            }),
        })
    }
}

/// A layout command enriched with owned registry metadata.
#[derive(Clone, Debug)]
pub struct ResolvedCommand {
    /// Localized presentation label.
    pub label: String,
    /// Localized help text.
    pub description: String,
    /// Configured gpui shortcut.
    pub key: Option<String>,
    /// Dispatch target.
    pub kind: CommandKind,
    /// UI-owned icon for this layout slot.
    pub icon: Option<IconName>,
}

impl ResolvedCommand {
    /// Construct the typed gpui action.
    pub fn action(&self) -> Box<dyn Action> {
        match self.kind {
            CommandKind::Shell(command) => command.action(),
            _ => Box::new(RunAction::new(self.reported_id())),
        }
    }

    /// Stable action name reported to the host.
    pub fn reported_id(&self) -> &'static str {
        match self.kind {
            CommandKind::Kicad(id) => id,
            CommandKind::Tool(tool) => tool.id().as_str(),
            CommandKind::Shell(command) => command.reported_id(),
        }
    }
}

fn strip_mnemonics(label: &str) -> String {
    let mut chars = label
        .split('\t')
        .next()
        .unwrap_or_default()
        .chars()
        .peekable();
    let mut result = String::new();
    while let Some(ch) = chars.next() {
        if ch != '&' {
            result.push(ch);
        } else if chars.peek() == Some(&'&') {
            result.push('&');
            chars.next();
        }
    }
    result
}

/// Convert KiCad's `KeyNameFromKeyCode` output into gpui keystroke notation.
fn normalize_hotkey(value: &str) -> String {
    let mut rest = value.trim();
    let mut modifiers = String::new();
    loop {
        let modifier = [
            ("Ctrl+", "ctrl-"),
            ("Cmd+", "cmd-"),
            ("RawCtrl+", "ctrl-"),
            ("Alt+", "alt-"),
            ("Option+", "alt-"),
            ("Meta+", "cmd-"),
            ("Win+", "super-"),
            ("Super+", "super-"),
            ("Shift+", "shift-"),
        ]
        .into_iter()
        .find(|(prefix, _)| rest.starts_with(prefix));
        let Some((prefix, translated)) = modifier else {
            break;
        };
        modifiers.push_str(translated);
        rest = &rest[prefix.len()..];
    }
    let key = match rest {
        "" | "<unassigned>" => return String::new(),
        "Esc" => "escape",
        "Del" => "delete",
        "Back" => "backspace",
        "Ins" => "insert",
        "PgUp" => "pageup",
        "PgDn" => "pagedown",
        "Return" => "enter",
        key => key,
    };
    modifiers.push_str(&key.to_lowercase());
    modifiers
}

/// Use the native modifier for standalone defaults and shell-owned shortcuts.
/// Host shortcuts have already been translated by KiCad and are left intact.
pub fn platform_default_key(key: &str) -> String {
    if cfg!(target_os = "macos") {
        if key == "ctrl-y" {
            return "cmd-shift-z".to_string();
        }
        key.replacen("ctrl-", "cmd-", 1)
    } else {
        key.to_string()
    }
}

/// Build native menus from layout slots present in the live registry.
pub fn app_menus_with_registry(registry: &ActionRegistry) -> Vec<Menu> {
    fn items(entries: &[MenuEntry], registry: &ActionRegistry) -> Vec<MenuItem> {
        let mut result = Vec::new();
        for entry in entries {
            let item = match entry {
                MenuEntry::Separator => {
                    if result.is_empty() || matches!(result.last(), Some(MenuItem::Separator)) {
                        continue;
                    }
                    MenuItem::Separator
                }
                MenuEntry::Submenu {
                    label,
                    items: children,
                } => {
                    let children = items(children, registry);
                    if children.is_empty() {
                        continue;
                    }
                    MenuItem::Submenu(Menu {
                        name: (*label).into(),
                        items: children,
                        disabled: false,
                    })
                }
                MenuEntry::Command(spec) => {
                    let Some(command) = registry.resolve(spec) else {
                        continue;
                    };
                    MenuItem::Action {
                        name: command.label.clone().into(),
                        action: command.action(),
                        os_action: None,
                        checked: false,
                        disabled: false,
                    }
                }
            };
            result.push(item);
        }
        if matches!(result.last(), Some(MenuItem::Separator)) {
            result.pop();
        }
        result
    }
    MENUS
        .iter()
        .filter_map(|menu| {
            let items = items(menu.items, registry);
            (!items.is_empty()).then(|| Menu {
                name: menu.name.into(),
                items,
                disabled: false,
            })
        })
        .collect()
}

/// Palette commands for the live editor, using registry labels and shortcuts.
pub fn all_commands_with_registry(registry: &ActionRegistry) -> Vec<(String, ResolvedCommand)> {
    all_commands()
        .into_iter()
        .filter_map(|(path, spec)| registry.resolve(&spec).map(|command| (path, command)))
        .collect()
}

/// Bind configured host shortcuts, retaining the shell's Escape cancellation.
pub fn key_bindings_with_registry(registry: &ActionRegistry) -> (Vec<KeyBinding>, Vec<String>) {
    let mut bindings = Vec::new();
    let mut rejected = Vec::new();
    let mut bound = std::collections::HashSet::from(["escape".to_string()]);
    let commands = all_commands_with_registry(registry);
    let mut requested = Vec::new();
    for (_, command) in &commands {
        let alternate = registry
            .get(command.reported_id())
            .map(|info| info.hotkey_alt.as_str());
        for keys in [command.key.as_deref(), alternate]
            .into_iter()
            .flatten()
            .filter(|key| !key.is_empty())
        {
            requested.push((keys.to_string(), keys, command));
        }
    }
    // Configured shortcuts take precedence over compatibility aliases.
    #[cfg(target_os = "macos")]
    {
        let aliases: Vec<_> = requested
            .iter()
            .filter_map(|(keys, original, command)| {
                let alias = if keys.starts_with("ctrl-") {
                    Some(if keys == "ctrl-y" {
                        "cmd-shift-z".to_string()
                    } else {
                        keys.replacen("ctrl-", "cmd-", 1)
                    })
                } else if keys.starts_with("cmd-") {
                    Some(
                        if keys == "cmd-shift-z"
                            && command.reported_id() == "common.Interactive.redo"
                        {
                            "ctrl-y".to_string()
                        } else {
                            keys.replacen("cmd-", "ctrl-", 1)
                        },
                    )
                } else {
                    None
                };
                alias.map(|alias| (alias, *original, *command))
            })
            .collect();
        requested.extend(aliases);
    }
    for (keys, original, command) in requested {
        if !bound.insert(keys.clone()) {
            continue;
        }
        let action = match command.kind {
            CommandKind::Tool(_) => Box::new(RunAction {
                id: command.reported_id().into(),
                hotkey: Some(original.to_string().into()),
            }) as Box<dyn Action>,
            _ => command.action(),
        };
        match KeyBinding::load(
            &keys,
            action,
            context_for(&keys, command.kind),
            false,
            None,
            &gpui_kit::DummyKeyboardMapper,
        ) {
            Ok(binding) => bindings.push(binding),
            Err(_) => rejected.push(keys),
        }
    }
    match KeyBinding::load(
        "escape",
        ShellCommand::CancelTool.action(),
        context_for("escape", CommandKind::Shell(ShellCommand::CancelTool)),
        false,
        None,
        &gpui_kit::DummyKeyboardMapper,
    ) {
        Ok(binding) => bindings.push(binding),
        Err(_) => rejected.push("escape".to_string()),
    }
    prefer_primary_hints(&mut bindings);
    (bindings, rejected)
}

// Native macOS menus use the first matching binding; GPUI components use the
// last. Repeat the primary after alternate/compatibility bindings so both show
// the same hint. The duplicate dispatches exactly the same action and payload.
fn prefer_primary_hints(bindings: &mut Vec<KeyBinding>) {
    let mut primaries: Vec<KeyBinding> = Vec::new();
    for binding in bindings.iter() {
        if !primaries
            .iter()
            .any(|primary| primary.action().partial_eq(binding.action()))
        {
            primaries.push(binding.clone());
        }
    }
    for primary in primaries {
        if bindings
            .iter()
            .filter(|binding| binding.action().partial_eq(primary.action()))
            .count()
            > 1
        {
            bindings.push(primary);
        }
    }
}

/// An entry in a menu.
#[derive(Clone, Copy, Debug)]
pub enum MenuEntry {
    /// A horizontal rule.
    Separator,
    /// A nested menu.
    Submenu {
        /// The submenu's title.
        label: &'static str,
        /// Its entries.
        items: &'static [MenuEntry],
    },
    /// An invocable command.
    Command(CommandSpec),
}

/// A top-level menu.
#[derive(Clone, Copy, Debug)]
pub struct MenuDef {
    /// The title on the menu bar.
    pub name: &'static str,
    /// Its entries.
    pub items: &'static [MenuEntry],
}

/// Shorthand for a command that forwards a KiCad action name.
const fn kicad(label: &'static str, id: &'static str, key: Option<&'static str>) -> MenuEntry {
    MenuEntry::Command(CommandSpec {
        label,
        kind: CommandKind::Kicad(id),
        key,
        icon: None,
    })
}

/// Shorthand for a command the shell answers itself.
const fn shell(label: &'static str, command: ShellCommand, key: Option<&'static str>) -> MenuEntry {
    MenuEntry::Command(CommandSpec {
        label,
        kind: CommandKind::Shell(command),
        key,
        icon: None,
    })
}

/// Shorthand for a command that activates a tool. The label, key and icon all
/// come from the tool table, so the two can never drift.
const fn tool(tool: Tool) -> MenuEntry {
    MenuEntry::Command(CommandSpec {
        label: "",
        kind: CommandKind::Tool(tool),
        key: None,
        icon: None,
    })
}

const SEP: MenuEntry = MenuEntry::Separator;

static FILE_EXPORT: &[MenuEntry] = &[
    kicad("Netlist...", "eeschema.EditorControl.exportNetlist", None),
    kicad(
        "Bill of Materials...",
        "eeschema.EditorControl.generateBOM",
        None,
    ),
    kicad(
        "Symbols to Library...",
        "eeschema.EditorControl.exportSymbolsToLibrary",
        None,
    ),
];

static FILE_ITEMS: &[MenuEntry] = &[
    kicad("New Schematic", "common.Control.new", Some("ctrl-n")),
    kicad("Open...", "common.Control.open", Some("ctrl-o")),
    SEP,
    kicad("Save", "common.Control.save", Some("ctrl-s")),
    kicad("Save As...", "common.Control.saveAs", Some("ctrl-shift-s")),
    kicad("Save All", "common.Control.saveAll", None),
    kicad("Revert", "common.Control.revert", None),
    SEP,
    kicad("Page Settings...", "common.Control.pageSettings", None),
    kicad("Print...", "common.Control.print", Some("ctrl-p")),
    kicad("Plot...", "common.Control.plot", None),
    SEP,
    MenuEntry::Submenu {
        label: "Export",
        items: FILE_EXPORT,
    },
    SEP,
    shell("Quit", ShellCommand::Quit, Some("ctrl-q")),
];

static EDIT_ITEMS: &[MenuEntry] = &[
    kicad("Undo", "common.Interactive.undo", Some("ctrl-z")),
    kicad("Redo", "common.Interactive.redo", Some("ctrl-y")),
    SEP,
    kicad("Cut", "common.Interactive.cut", Some("ctrl-x")),
    kicad("Copy", "common.Interactive.copy", Some("ctrl-c")),
    kicad("Paste", "common.Interactive.paste", Some("ctrl-v")),
    kicad("Duplicate", "common.Interactive.duplicate", Some("ctrl-d")),
    kicad("Delete", "common.Interactive.delete", Some("delete")),
    kicad(
        "Properties...",
        "eeschema.InteractiveEdit.properties",
        Some("e"),
    ),
    SEP,
    kicad("Select All", "common.Interactive.selectAll", Some("ctrl-a")),
    kicad(
        "Unselect All",
        "common.Interactive.unselectAll",
        Some("ctrl-shift-a"),
    ),
    SEP,
    kicad("Find...", "common.Interactive.find", Some("ctrl-f")),
    kicad(
        "Find and Replace...",
        "common.Interactive.findAndReplace",
        Some("ctrl-alt-f"),
    ),
    kicad("Find Next", "common.Interactive.findNext", Some("f3")),
];

static VIEW_GRID: &[MenuEntry] = &[
    shell("Show Grid", ShellCommand::ToggleGrid, None),
    shell("Next Grid Size", ShellCommand::CycleGrid, Some("alt-g")),
    SEP,
    kicad("Grid Settings...", "common.Control.editGrids", None),
];

static VIEW_ITEMS: &[MenuEntry] = &[
    shell("Zoom In", ShellCommand::ZoomIn, Some("ctrl-=")),
    shell("Zoom Out", ShellCommand::ZoomOut, Some("ctrl--")),
    shell("Zoom to Fit Sheet", ShellCommand::ZoomToFit, Some("home")),
    shell(
        "Zoom to Objects",
        ShellCommand::ZoomToObjects,
        Some("ctrl-home"),
    ),
    shell("Actual Size", ShellCommand::ZoomActualSize, Some("ctrl-0")),
    SEP,
    MenuEntry::Submenu {
        label: "Grid",
        items: VIEW_GRID,
    },
    shell("Switch Units", ShellCommand::ToggleUnits, Some("ctrl-u")),
    SEP,
    kicad(
        "Show Hidden Pins",
        "eeschema.EditorControl.showHiddenPins",
        None,
    ),
    kicad(
        "Show ERC Warnings",
        "eeschema.EditorControl.showERCWarnings",
        None,
    ),
    kicad(
        "Show Directive Labels",
        "eeschema.EditorControl.showDirectiveLabels",
        None,
    ),
    SEP,
    shell(
        "Hierarchy Panel",
        ShellCommand::ToggleLeftPanel,
        Some("ctrl-b"),
    ),
    shell(
        "Properties Panel",
        ShellCommand::ToggleRightPanel,
        Some("ctrl-alt-b"),
    ),
    shell(
        "Frame Time Readout",
        ShellCommand::ToggleFrameStats,
        Some("ctrl-shift-f"),
    ),
    SEP,
    shell("Dark / Light Theme", ShellCommand::ToggleTheme, None),
    shell(
        "Command Palette...",
        ShellCommand::OpenCommandPalette,
        Some("ctrl-shift-p"),
    ),
];

static PLACE_GRAPHICS: &[MenuEntry] = &[
    tool(Tool::DrawRectangle),
    tool(Tool::DrawCircle),
    tool(Tool::DrawEllipse),
    tool(Tool::DrawArc),
    tool(Tool::DrawEllipseArc),
    tool(Tool::DrawBezier),
    tool(Tool::DrawPolygon),
    tool(Tool::DrawLine),
    SEP,
    tool(Tool::PlaceImage),
];

static PLACE_ITEMS: &[MenuEntry] = &[
    tool(Tool::PlaceSymbol),
    tool(Tool::PlacePower),
    SEP,
    tool(Tool::DrawWire),
    tool(Tool::DrawBus),
    tool(Tool::PlaceBusEntry),
    tool(Tool::PlaceJunction),
    tool(Tool::PlaceNoConnect),
    SEP,
    tool(Tool::PlaceLabel),
    tool(Tool::PlaceClassLabel),
    tool(Tool::PlaceGlobalLabel),
    tool(Tool::PlaceHierLabel),
    SEP,
    tool(Tool::DrawRuleArea),
    tool(Tool::DrawSheet),
    tool(Tool::PlaceSheetPin),
    kicad(
        "Sync All Sheet Pins...",
        "eeschema.InteractiveDrawing.syncAllSheetsPins",
        None,
    ),
    SEP,
    tool(Tool::PlaceText),
    tool(Tool::DrawTextBox),
    tool(Tool::DrawTable),
    MenuEntry::Submenu {
        label: "Graphics",
        items: PLACE_GRAPHICS,
    },
];

static INSPECT_ITEMS: &[MenuEntry] = &[
    kicad(
        "Electrical Rules Checker",
        "eeschema.InspectionTool.runERC",
        None,
    ),
    kicad(
        "Bus Syntax Help",
        "eeschema.InspectionTool.showBusSyntaxHelp",
        None,
    ),
    SEP,
    kicad(
        "Net Navigator",
        "eeschema.EditorControl.showNetNavigator",
        None,
    ),
    tool(Tool::HighlightNet),
    // The one-shot sibling of the tool above, and the action backtick is really
    // bound to. Named apart from the tool so two rows reading "Highlight Net"
    // do not sit next to each other.
    kicad(
        "Highlight Net Under Cursor",
        "eeschema.EditorControl.highlightNet",
        Some("`"),
    ),
    kicad(
        "Clear Net Highlighting",
        "eeschema.EditorControl.clearHighlight",
        Some("escape"),
    ),
    SEP,
    kicad("Simulator", "eeschema.EditorControl.showSimulator", None),
    tool(Tool::Measure),
];

static TOOLS_ITEMS: &[MenuEntry] = &[
    kicad(
        "Update PCB from Schematic...",
        "common.Control.updatePcbFromSchematic",
        Some("f8"),
    ),
    kicad(
        "Update Schematic from PCB...",
        "common.Control.updateSchematicFromPCB",
        None,
    ),
    SEP,
    kicad(
        "Annotate Schematic...",
        "eeschema.EditorControl.annotate",
        None,
    ),
    kicad(
        "Increment Annotations...",
        "eeschema.EditorControl.incrementAnnotations",
        None,
    ),
    kicad(
        "Assign Footprints...",
        "eeschema.EditorControl.assignFootprints",
        None,
    ),
    kicad(
        "Edit Symbol Fields...",
        "eeschema.EditorControl.editSymbolFields",
        None,
    ),
    kicad(
        "Edit Symbol Library Links...",
        "eeschema.EditorControl.editSymbolLibraryLinks",
        None,
    ),
    SEP,
    MenuEntry::Submenu {
        label: "Symbol Libraries",
        items: &[
            kicad(
                "Symbol Pin Maps...",
                "eeschema.InteractiveEdit.editSymbolPinMaps",
                None,
            ),
            kicad("Symbol Editor", "common.Control.showSymbolEditor", None),
            kicad("Symbol Browser", "common.Control.showSymbolBrowser", None),
            kicad(
                "Library Fields Table...",
                "eeschema.SymbolLibraryControl.showLibraryFieldsTable",
                None,
            ),
            kicad(
                "Related Library Fields...",
                "eeschema.SymbolLibraryControl.showRelatedLibraryFieldsTable",
                None,
            ),
            kicad(
                "New Library Symbol...",
                "eeschema.SymbolLibraryControl.newSymbol",
                None,
            ),
            kicad(
                "Library Symbol Properties...",
                "eeschema.InteractiveEdit.symbolProperties",
                None,
            ),
            kicad(
                "Library Pin Table...",
                "eeschema.InteractiveEdit.pinTable",
                None,
            ),
            kicad(
                "Import Library Symbol...",
                "eeschema.SymbolLibraryControl.importSymbol",
                None,
            ),
        ],
    },
    SEP,
    MenuEntry::Submenu {
        label: "Symbol Maintenance",
        items: &[
            kicad(
                "Change Symbols...",
                "eeschema.InteractiveEdit.changeSymbols",
                None,
            ),
            kicad(
                "Update Symbols...",
                "eeschema.InteractiveEdit.updateSymbols",
                None,
            ),
            kicad(
                "Remap Symbols...",
                "eeschema.EditorControl.remapSymbols",
                None,
            ),
            kicad(
                "Rescue Symbols...",
                "eeschema.EditorControl.rescueSymbols",
                None,
            ),
        ],
    },
    SEP,
    kicad(
        "Generate Bill of Materials...",
        "eeschema.EditorControl.generateBOM",
        None,
    ),
    kicad(
        "Export Netlist...",
        "eeschema.EditorControl.exportNetlist",
        None,
    ),
];

static PREFERENCES_ITEMS: &[MenuEntry] = &[
    MenuEntry::Submenu {
        label: "Schematic Tools and Settings",
        items: &[
            shell(
                "Resolve Field Name Case Conflicts...",
                ShellCommand::FieldCaseConflicts,
                None,
            ),
            shell(
                "Schematic Data Sources...",
                ShellCommand::SchematicDataSources,
                None,
            ),
            shell("Migrate Legacy Buses...", ShellCommand::MigrateBuses, None),
            kicad(
                "Update Inherited Symbol Fields...",
                "eeschema.SymbolLibraryControl.updateSymbolFields",
                None,
            ),
            shell("Bus Aliases...", ShellCommand::BusAliases, None),
            shell("Net Classes...", ShellCommand::Netclasses, None),
            shell("Net Chains...", ShellCommand::NetChains, None),
            shell("Import Settings...", ShellCommand::ImportSettings, None),
            kicad(
                "Create Net Chain...",
                "eeschema.EditorControl.createNetChain",
                None,
            ),
            kicad(
                "Edit Text and Graphics...",
                "eeschema.InteractiveEdit.editTextAndGraphics",
                None,
            ),
            kicad(
                "Synchronize Sheet Pins...",
                "eeschema.InteractiveDrawing.syncSheetPins",
                None,
            ),
            kicad(
                "Synchronize All Sheet Pins...",
                "eeschema.InteractiveDrawing.syncAllSheetsPins",
                None,
            ),
        ],
    },
    shell(
        "Remote Symbol Providers...",
        ShellCommand::RemoteSymbolSettings,
        None,
    ),
    kicad(
        "Schematic Setup...",
        "eeschema.EditorControl.schematicSetup",
        None,
    ),
    kicad(
        "Preferences...",
        "common.SuiteControl.openPreferences",
        Some("ctrl-,"),
    ),
    SEP,
    shell("Dark / Light Theme", ShellCommand::ToggleTheme, None),
    SEP,
    kicad(
        "Manage Symbol Libraries...",
        "common.SuiteControl.showSymbolLibTable",
        None,
    ),
    kicad(
        "Configure Paths...",
        "common.SuiteControl.configurePaths",
        None,
    ),
    SEP,
    kicad("Hotkeys...", "common.SuiteControl.listHotKeys", None),
];

static HELP_ITEMS: &[MenuEntry] = &[
    kicad("KiCad Manual", "common.SuiteControl.help", Some("f1")),
    kicad(
        "Getting Started",
        "common.SuiteControl.gettingStarted",
        None,
    ),
    kicad("Hotkey Reference", "common.SuiteControl.listHotKeys", None),
    SEP,
    kicad("Report a Bug", "common.SuiteControl.reportBug", None),
    kicad("Get Involved", "common.SuiteControl.getInvolved", None),
    SEP,
    kicad("About KiCad", "common.SuiteControl.about", None),
];

/// The menu bar, top level first.
pub static MENUS: &[MenuDef] = &[
    MenuDef {
        name: "File",
        items: FILE_ITEMS,
    },
    MenuDef {
        name: "Edit",
        items: EDIT_ITEMS,
    },
    MenuDef {
        name: "View",
        items: VIEW_ITEMS,
    },
    MenuDef {
        name: "Place",
        items: PLACE_ITEMS,
    },
    MenuDef {
        name: "Inspect",
        items: INSPECT_ITEMS,
    },
    MenuDef {
        name: "Tools",
        items: TOOLS_ITEMS,
    },
    MenuDef {
        name: "Preferences",
        items: PREFERENCES_ITEMS,
    },
    MenuDef {
        name: "Help",
        items: HELP_ITEMS,
    },
];

/// The label a command presents, resolving tool entries against the tool table.
pub fn label_of(spec: &CommandSpec) -> &'static str {
    match spec.kind {
        CommandKind::Tool(tool) => tool.label(),
        _ => spec.label,
    }
}

/// The key binding a command presents, resolving tool entries against the tool
/// table.
pub fn key_of(spec: &CommandSpec) -> Option<&'static str> {
    match spec.kind {
        CommandKind::Tool(tool) => tool.spec().shortcut,
        _ => spec.key,
    }
}

/// Build the gpui menu bar from [`MENUS`].
pub fn app_menus() -> Vec<Menu> {
    MENUS
        .iter()
        .map(|menu| Menu {
            name: menu.name.into(),
            items: menu.items.iter().map(menu_item).collect(),
            disabled: false,
        })
        .collect()
}

fn menu_item(entry: &MenuEntry) -> MenuItem {
    match entry {
        MenuEntry::Separator => MenuItem::Separator,
        MenuEntry::Submenu { label, items } => MenuItem::Submenu(Menu {
            name: (*label).into(),
            items: items.iter().map(menu_item).collect(),
            disabled: false,
        }),
        MenuEntry::Command(spec) => MenuItem::Action {
            name: label_of(spec).into(),
            action: spec.action(),
            os_action: None,
            checked: false,
            disabled: false,
        },
    }
}

/// Every command in the catalogue, flattened, with the menu path it sits
/// under. The command palette and the key map are both built from this.
pub fn all_commands() -> Vec<(String, CommandSpec)> {
    let mut out = Vec::new();
    for menu in MENUS {
        collect(menu.name, menu.items, &mut out);
    }
    // Tools that no menu lists still deserve a palette row and a binding.
    for spec in TOOLS {
        let id = spec.id.as_str();
        if !out.iter().any(|(_, command)| command.reported_id() == id) {
            out.push((
                "Tools".to_string(),
                CommandSpec {
                    label: spec.label,
                    kind: CommandKind::Tool(spec.tool),
                    key: spec.shortcut,
                    icon: Some(spec.icon),
                },
            ));
        }
    }
    out
}

fn collect(path: &str, items: &'static [MenuEntry], out: &mut Vec<(String, CommandSpec)>) {
    for entry in items {
        match entry {
            MenuEntry::Separator => {}
            MenuEntry::Submenu { label, items } => {
                collect(&format!("{path} \u{203a} {label}"), items, out);
            }
            MenuEntry::Command(spec) => out.push((path.to_string(), *spec)),
        }
    }
}

/// The default key map.
///
/// Returns the bindings it could build and the keystroke strings it could not
/// parse; a caller that wants to be strict can assert the second is empty, and
/// the unit test below does exactly that. Nothing here silently disappears.
pub fn key_bindings() -> (Vec<KeyBinding>, Vec<String>) {
    // Replay uses the same registration and hint ordering as a live session.
    let registry = ActionRegistry::new(
        all_commands()
            .into_iter()
            .map(|(_, spec)| ActionInfo {
                name: spec.reported_id().to_string(),
                label: label_of(&spec).to_string(),
                hotkey: key_of(&spec).map(platform_default_key).unwrap_or_default(),
                ..Default::default()
            })
            .collect(),
    );
    key_bindings_with_registry(&registry)
}

/// Host shortcuts must never override a focused text input, including modified
/// editing shortcuts (select all, clipboard, undo and redo). Only explicitly
/// global shell commands may run from an input, and only with a modifier.
fn context_for(keys: &str, kind: CommandKind) -> Option<Rc<KeyBindingContextPredicate>> {
    let modified = keys.contains("ctrl-")
        || keys.contains("alt-")
        || keys.contains("cmd-")
        || keys.contains("super-");
    let global = matches!(
        kind,
        CommandKind::Shell(
            ShellCommand::Quit
                | ShellCommand::OpenCommandPalette
                | ShellCommand::ToggleTheme
                | ShellCommand::ToggleLeftPanel
                | ShellCommand::ToggleRightPanel
                | ShellCommand::ToggleFrameStats
        )
    );
    if modified && global {
        return None;
    }
    KeyBindingContextPredicate::parse("!Input")
        .ok()
        .map(Rc::new)
}

/// The tool a KiCad action name activates, if it names a tool.
pub fn tool_for_action(id: &str) -> Option<Tool> {
    TOOLS
        .iter()
        .find(|spec| spec.id.as_str() == id)
        .map(|spec| spec.tool)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn tool_hint_matches_menu_action_without_losing_cursor_payload() {
        let registry = ActionRegistry::new(vec![ActionInfo {
            name: Tool::DrawWire.id().as_str().into(),
            label: "Draw Wires".into(),
            hotkey: "W".into(),
            ..Default::default()
        }]);
        let (bindings, rejected) = key_bindings_with_registry(&registry);
        assert!(rejected.is_empty());
        let menu_action = RunAction::new(Tool::DrawWire.id().as_str());
        let binding = bindings
            .iter()
            .find(|binding| binding.action().partial_eq(&menu_action))
            .unwrap();
        assert_eq!(binding.keystrokes()[0].inner().key, "w");
        assert_eq!(
            binding
                .action()
                .as_any()
                .downcast_ref::<RunAction>()
                .unwrap()
                .hotkey
                .as_deref(),
            Some("w")
        );
        assert_ne!(menu_action, RunAction::new(Tool::DrawBus.id().as_str()));
    }

    #[test]
    fn primary_hint_wins_for_both_first_and_last_binding_consumers() {
        let registry = ActionRegistry::new(vec![ActionInfo {
            name: "common.Control.save".into(),
            hotkey: if cfg!(target_os = "macos") {
                "Cmd+S"
            } else {
                "Ctrl+S"
            }
            .into(),
            hotkey_alt: "F2".into(),
            ..Default::default()
        }]);
        let (bindings, rejected) = key_bindings_with_registry(&registry);
        assert!(rejected.is_empty());
        let action = RunAction::new("common.Control.save");
        let matching: Vec<_> = bindings
            .iter()
            .filter(|binding| binding.action().partial_eq(&action))
            .collect();
        assert_eq!(
            matching.first().unwrap().keystrokes(),
            matching.last().unwrap().keystrokes()
        );
        assert_eq!(matching.first().unwrap().keystrokes()[0].inner().key, "s");
        if cfg!(target_os = "macos") {
            assert!(
                matching.first().unwrap().keystrokes()[0]
                    .inner()
                    .modifiers
                    .platform
            );
            assert!(
                matching
                    .iter()
                    .any(|binding| binding.keystrokes()[0].inner().modifiers.control)
            );
        }
    }

    #[test]
    fn registry_metadata_controls_labels_keys_and_availability() {
        let registry = ActionRegistry::new(vec![ActionInfo {
            name: "common.Control.save".into(),
            label: "&Save && Close\tCtrl+S".into(),
            description: "Configured save".into(),
            hotkey: "Ctrl+Shift+S".into(),
            hotkey_alt: "F2".into(),
        }]);
        let commands = all_commands_with_registry(&registry);
        let save = commands
            .iter()
            .find(|(_, c)| c.reported_id() == "common.Control.save")
            .unwrap();
        assert_eq!(save.1.label, "Save & Close");
        assert_eq!(save.1.key.as_deref(), Some("ctrl-shift-s"));
        assert_eq!(save.1.description, "Configured save");
        assert!(
            !commands
                .iter()
                .any(|(_, c)| c.reported_id() == "common.Control.open")
        );
        assert!(
            commands
                .iter()
                .any(|(_, c)| c.reported_id() == "kicad.ui.toggleTheme")
        );
        let (bindings, rejected) = key_bindings_with_registry(&registry);
        assert!(rejected.is_empty(), "{rejected:?}");
        assert!(
            bindings
                .iter()
                .any(|b| b.keystrokes()[0].inner().key == "f2")
        );
        let menus = app_menus_with_registry(&registry);
        let file = menus.iter().find(|m| m.name == "File").unwrap();
        assert!(matches!(&file.items[0], MenuItem::Action { name, .. } if name == "Save & Close"));
        assert!(!matches!(file.items.last(), Some(MenuItem::Separator)));
    }

    #[test]
    fn registry_tool_shortcut_uses_configured_cursor_hotkey() {
        let registry = ActionRegistry::new(vec![ActionInfo {
            name: Tool::DrawWire.id().as_str().into(),
            label: "Wire from registry".into(),
            hotkey: "Shift+W".into(),
            hotkey_alt: "F4".into(),
            ..Default::default()
        }]);
        let (bindings, rejected) = key_bindings_with_registry(&registry);
        assert!(rejected.is_empty());
        let wire: Vec<_> = bindings
            .iter()
            .filter_map(|binding| {
                binding
                    .action()
                    .as_any()
                    .downcast_ref::<RunAction>()
                    .filter(|action| action.id.as_ref() == Tool::DrawWire.id().as_str())
            })
            .collect();
        assert_eq!(wire.len(), 3);
        assert!(
            wire.iter()
                .any(|action| action.hotkey.as_deref() == Some("shift-w"))
        );
        assert!(
            wire.iter()
                .any(|action| action.hotkey.as_deref() == Some("f4"))
        );
        let unassigned = ActionRegistry::new(vec![ActionInfo {
            name: Tool::DrawWire.id().as_str().into(),
            label: "Wire".into(),
            ..Default::default()
        }]);
        assert!(
            all_commands_with_registry(&unassigned)
                .iter()
                .find(|(_, command)| command.reported_id() == Tool::DrawWire.id().as_str())
                .unwrap()
                .1
                .key
                .is_none()
        );
    }

    #[test]
    fn registry_hotkeys_translate_named_keys_and_literal_plus() {
        for (host, gpui) in [
            ("Cmd+Shift+Z", "cmd-shift-z"),
            ("Option+Shift+X", "alt-shift-x"),
            ("Ctrl++", "ctrl-+"),
            ("Shift+Space", "shift-space"),
            ("Esc", "escape"),
            ("Del", "delete"),
            ("PgUp", "pageup"),
            ("", ""),
        ] {
            assert_eq!(normalize_hotkey(host), gpui);
        }
    }

    /// Modified host shortcuts must also stay out of focused text inputs.
    #[test]
    fn host_shortcuts_are_scoped_away_from_text_inputs() {
        for key in [
            "w",
            "escape",
            "delete",
            "shift-t",
            "ctrl-a",
            "cmd-a",
            "ctrl-c",
            "cmd-x",
            "ctrl-v",
            "cmd-z",
            "cmd-shift-z",
            "ctrl-s",
        ] {
            assert!(
                context_for(key, CommandKind::Kicad("common.Interactive.edit")).is_some(),
                "{key}"
            );
        }
        assert!(
            context_for(
                "cmd-shift-p",
                CommandKind::Shell(ShellCommand::OpenCommandPalette)
            )
            .is_none()
        );
        assert!(context_for("cmd-q", CommandKind::Shell(ShellCommand::Quit)).is_none());
        assert!(context_for("p", CommandKind::Shell(ShellCommand::OpenCommandPalette)).is_some());
        assert!(context_for("cmd-a", CommandKind::Tool(Tool::PlaceSymbol)).is_some());

        let (bindings, _) = key_bindings();
        for binding in &bindings {
            let keys: Vec<String> = binding
                .keystrokes()
                .iter()
                .map(|k| k.inner().key.clone())
                .collect();
            let modifiers = binding.keystrokes().iter().any(|k| {
                k.inner().modifiers.control
                    || k.inner().modifiers.alt
                    || k.inner().modifiers.platform
            });
            if !modifiers {
                assert!(
                    binding.predicate().is_some(),
                    "{keys:?} is bound globally and would steal typed characters"
                );
            }
        }
    }

    #[test]
    fn every_keystroke_in_the_catalogue_parses() {
        let (bindings, rejected) = key_bindings();
        assert!(rejected.is_empty(), "unparsable keystrokes: {rejected:?}");
        assert!(bindings.len() > 20, "only {} bindings", bindings.len());
    }

    #[test]
    fn escape_cancels_rather_than_clearing_highlighting() {
        // Both are in the catalogue; the cancel binding has to be the one that
        // survives, or Escape stops being an escape.
        let (bindings, _) = key_bindings();
        let escape: Vec<_> = bindings
            .iter()
            .filter(|binding| {
                binding.keystrokes().len() == 1 && binding.keystrokes()[0].inner().key == "escape"
            })
            .collect();
        assert_eq!(escape.len(), 1, "escape bound {} times", escape.len());
        assert_eq!(
            escape[0].action().name(),
            ShellCommand::CancelTool.action().name(),
            "escape has to cancel the tool, not clear net highlighting"
        );
    }

    #[test]
    fn the_menu_bar_has_the_eight_expected_menus() {
        let names: Vec<_> = MENUS.iter().map(|menu| menu.name).collect();
        assert_eq!(
            names,
            [
                "File",
                "Edit",
                "View",
                "Place",
                "Inspect",
                "Tools",
                "Preferences",
                "Help"
            ]
        );
    }

    #[test]
    fn menu_items_all_carry_an_action_and_a_label() {
        for menu in app_menus() {
            assert!(!menu.name.is_empty());
            check_items(&menu.items);
        }

        fn check_items(items: &[MenuItem]) {
            for item in items {
                match item {
                    MenuItem::Separator => {}
                    MenuItem::Submenu(menu) => {
                        assert!(!menu.name.is_empty());
                        check_items(&menu.items);
                    }
                    MenuItem::Action { name, action, .. } => {
                        assert!(!name.is_empty(), "unlabelled menu item");
                        assert!(!action.name().is_empty());
                    }
                    MenuItem::SystemMenu(_) => panic!("no OS menus on this platform"),
                }
            }
        }
    }

    #[test]
    fn every_tool_is_reachable_from_the_palette() {
        let commands = all_commands();
        for spec in TOOLS {
            assert!(
                commands
                    .iter()
                    .any(|(_, command)| command.reported_id() == spec.id.as_str()),
                "{} is not in the catalogue",
                spec.label
            );
        }
    }

    #[test]
    fn shell_commands_all_report_a_distinct_name() {
        let commands = [
            ShellCommand::Quit,
            ShellCommand::ZoomIn,
            ShellCommand::ZoomOut,
            ShellCommand::ZoomToFit,
            ShellCommand::ZoomToObjects,
            ShellCommand::ZoomActualSize,
            ShellCommand::ToggleGrid,
            ShellCommand::CycleGrid,
            ShellCommand::ToggleUnits,
            ShellCommand::ToggleTheme,
            ShellCommand::ToggleLeftPanel,
            ShellCommand::ToggleRightPanel,
            ShellCommand::ToggleFrameStats,
            ShellCommand::OpenCommandPalette,
            ShellCommand::CancelTool,
        ];
        let names: HashSet<_> = commands.iter().map(|c| c.reported_id()).collect();
        assert_eq!(names.len(), commands.len());
        // Distinct gpui action types too, or the keymap would collapse them.
        let actions: HashSet<_> = commands.iter().map(|c| c.action().name()).collect();
        assert_eq!(actions.len(), commands.len());
    }

    #[test]
    fn tool_actions_round_trip_to_their_tool() {
        assert_eq!(
            tool_for_action("eeschema.InteractiveDrawingLineWireBus.drawWires"),
            Some(Tool::DrawWire)
        );
        assert_eq!(tool_for_action("common.Control.save"), None);
    }

    #[test]
    fn the_palette_labels_tools_from_the_tool_table() {
        let commands = all_commands();
        let wire = commands
            .iter()
            .find(|(_, spec)| spec.reported_id() == Tool::DrawWire.id().as_str())
            .expect("wire tool is listed");
        assert_eq!(label_of(&wire.1), "Draw Wire");
        assert_eq!(key_of(&wire.1), Some("w"));
    }
}
