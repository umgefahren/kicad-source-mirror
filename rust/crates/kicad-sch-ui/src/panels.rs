// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The two docked panels: the sheet hierarchy on the left, properties on the
//! right.
//!
//! Both are real `DockArea` panels, so they can be dragged, tabbed, zoomed and
//! persisted by the dock machinery rather than being hard-coded columns. What
//! they show is placeholder data held in [`DesignState`]; when the host is
//! attached, that struct is what gets filled from `SCHEMATIC`'s sheet list and
//! the selection, and neither panel changes.

use gpui_kit::component::dock::{Panel, PanelEvent};
use gpui_kit::component::list::ListItem;
use gpui_kit::component::tree::{TreeItem, TreeState, tree};
use gpui_kit::component::{ActiveTheme, Icon, StyledExt};
use gpui_kit::assets::IconName;
use gpui_kit::prelude::*;
use gpui_kit::TestSupportExt;
use gpui_kit::{
    App, Context, Entity, EventEmitter, FocusHandle, Focusable, SharedString, Window, div, px,
};

/// One row in the properties panel.
#[derive(Clone, Debug, PartialEq)]
pub struct Property {
    /// The field name.
    pub name: SharedString,
    /// The field value, already formatted.
    pub value: SharedString,
}

impl Property {
    /// A property row.
    pub fn new(name: impl Into<SharedString>, value: impl Into<SharedString>) -> Self {
        Self {
            name: name.into(),
            value: value.into(),
        }
    }
}

/// What the two panels display.
///
/// Shared between them so that picking a sheet on the left changes what the
/// right shows without either panel knowing about the other.
pub struct DesignState {
    hierarchy: Vec<TreeItem>,
    selected_id: SharedString,
    selected_label: SharedString,
    selected_kind: SharedString,
    properties: Vec<Property>,
}

impl Default for DesignState {
    fn default() -> Self {
        Self::placeholder()
    }
}

impl DesignState {
    /// The placeholder design shown until a real schematic is loaded.
    pub fn placeholder() -> Self {
        Self {
            hierarchy: placeholder_hierarchy(),
            selected_id: "sheet-root".into(),
            selected_label: "Root Sheet".into(),
            selected_kind: "Sheet".into(),
            properties: placeholder_properties("Root Sheet"),
        }
    }

    /// The sheet tree.
    pub fn hierarchy(&self) -> &[TreeItem] {
        &self.hierarchy
    }

    /// The id of the selected hierarchy row.
    pub fn selected_id(&self) -> &SharedString {
        &self.selected_id
    }

    /// The label of the selected hierarchy row.
    pub fn selected_label(&self) -> &SharedString {
        &self.selected_label
    }

    /// What kind of thing is selected, for the properties header.
    pub fn selected_kind(&self) -> &SharedString {
        &self.selected_kind
    }

    /// The properties of the selection.
    pub fn properties(&self) -> &[Property] {
        &self.properties
    }

    /// Select a hierarchy row, refreshing the properties that follow from it.
    pub fn select(&mut self, id: impl Into<SharedString>, label: impl Into<SharedString>) {
        self.selected_id = id.into();
        self.selected_label = label.into();
        self.selected_kind = if self.selected_id.starts_with("sym-") {
            "Symbol".into()
        } else {
            "Sheet".into()
        };
        self.properties = placeholder_properties(&self.selected_label);
    }
}

fn placeholder_hierarchy() -> Vec<TreeItem> {
    vec![
        TreeItem::new("sheet-root", "Root Sheet")
            .expanded(true)
            .children([
                TreeItem::new("sym-u1", "U1  MCU-48"),
                TreeItem::new("sym-r1", "R1  10k"),
                TreeItem::new("sym-c1", "C1  100n"),
                TreeItem::new("sheet-power", "Power")
                    .expanded(true)
                    .children([
                        TreeItem::new("sym-u2", "U2  LDO-3V3"),
                        TreeItem::new("sym-c2", "C2  10u"),
                        TreeItem::new("sym-c3", "C3  10u"),
                    ]),
                TreeItem::new("sheet-analog", "Analog Front End").children([
                    TreeItem::new("sym-u3", "U3  OPA-DUAL"),
                    TreeItem::new("sym-r2", "R2  100k"),
                    TreeItem::new("sym-r3", "R3  100k"),
                ]),
                TreeItem::new("sheet-io", "Connectors").children([
                    TreeItem::new("sym-j1", "J1  USB-C"),
                    TreeItem::new("sym-j2", "J2  HEADER-2x5"),
                ]),
            ]),
    ]
}

fn placeholder_properties(label: &str) -> Vec<Property> {
    vec![
        Property::new("Name", label.to_string()),
        Property::new("Library", "kicad_sch_ui:placeholder"),
        Property::new("Position", "112.5, 68.0 mm"),
        Property::new("Rotation", "0\u{b0}"),
        Property::new("Unit", "1 of 1"),
        Property::new("Exclude from BOM", "No"),
        Property::new("Exclude from board", "No"),
        Property::new("Do not populate", "No"),
    ]
}

/// The sheet hierarchy panel.
pub struct HierarchyPanel {
    focus_handle: FocusHandle,
    tree: Entity<TreeState>,
    design: Entity<DesignState>,
}

impl HierarchyPanel {
    /// Build the panel over a shared [`DesignState`].
    pub fn new(design: Entity<DesignState>, cx: &mut Context<Self>) -> Self {
        let items = design.read(cx).hierarchy().to_vec();
        let tree = cx.new(|cx| TreeState::new(cx).items(items));
        Self {
            focus_handle: cx.focus_handle(),
            tree,
            design,
        }
    }

    /// The tree state, so tests can inspect what is expanded or selected.
    pub fn tree(&self) -> &Entity<TreeState> {
        &self.tree
    }
}

impl Focusable for HierarchyPanel {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl EventEmitter<PanelEvent> for HierarchyPanel {}

impl gpui_kit::component::dock::BasePanel for HierarchyPanel {
    fn panel_name(&self) -> &'static str {
        "SchematicHierarchy"
    }
}

impl Panel for HierarchyPanel {
    fn title(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        "Hierarchy"
    }

    fn tab_name(&self, _cx: &App) -> Option<SharedString> {
        Some("Hierarchy".into())
    }
}

impl Render for HierarchyPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let design = self.design.clone();
        div()
            .id("hierarchy-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .v_flex()
            .child(
                div()
                    .h_flex()
                    .items_center()
                    .gap_2()
                    .px_3()
                    .py_2()
                    .text_xs()
                    .text_color(cx.theme().muted_foreground)
                    .child(Icon::new(IconName::ListTree).size_4())
                    .child("SHEETS AND SYMBOLS"),
            )
            .child(
                div()
                    .id("hierarchy-tree")
                    .test_support()
                    .flex_1()
                    .min_h(px(120.))
                    .child(tree(&self.tree, move |_ix, entry, selected, _window, _cx| {
                        let item = entry.item();
                        let id = item.id.clone();
                        let label = item.label.clone();
                        let design = design.clone();
                        let icon = if item.is_folder() {
                            IconName::Frame
                        } else {
                            IconName::Component
                        };
                        ListItem::new(item.id.clone())
                            .selected(selected)
                            .child(
                                div()
                                    .h_flex()
                                    .gap_2()
                                    .items_center()
                                    .child(Icon::new(icon).size_3p5())
                                    .child(item.label.clone()),
                            )
                            .on_click(move |_event, _window, cx| {
                                design.update(cx, |design, cx| {
                                    design.select(id.clone(), label.clone());
                                    cx.notify();
                                });
                            })
                    })),
            )
    }
}

/// The properties panel.
pub struct PropertiesPanel {
    focus_handle: FocusHandle,
    design: Entity<DesignState>,
}

impl PropertiesPanel {
    /// Build the panel over a shared [`DesignState`].
    pub fn new(design: Entity<DesignState>, cx: &mut Context<Self>) -> Self {
        // Redraw whenever the hierarchy changes the selection.
        cx.observe(&design, |_, _, cx| cx.notify()).detach();
        Self {
            focus_handle: cx.focus_handle(),
            design,
        }
    }
}

impl Focusable for PropertiesPanel {
    fn focus_handle(&self, _cx: &App) -> FocusHandle {
        self.focus_handle.clone()
    }
}

impl EventEmitter<PanelEvent> for PropertiesPanel {}

impl gpui_kit::component::dock::BasePanel for PropertiesPanel {
    fn panel_name(&self) -> &'static str {
        "SchematicProperties"
    }
}

impl Panel for PropertiesPanel {
    fn title(&mut self, _window: &mut Window, _cx: &mut Context<Self>) -> impl IntoElement {
        "Properties"
    }

    fn tab_name(&self, _cx: &App) -> Option<SharedString> {
        Some("Properties".into())
    }
}

impl Render for PropertiesPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let design = self.design.read(cx);
        let kind = design.selected_kind().clone();
        let label = design.selected_label().clone();
        let properties = design.properties().to_vec();
        let theme = cx.theme();
        let muted = theme.muted_foreground;
        let border = theme.border;

        div()
            .id("properties-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .v_flex()
            .child(
                div()
                    .v_flex()
                    .gap_0p5()
                    .px_3()
                    .py_2()
                    .border_b_1()
                    .border_color(border)
                    .child(
                        div()
                            .text_xs()
                            .text_color(muted)
                            .child(kind.to_uppercase()),
                    )
                    .child(div().text_sm().font_semibold().child(label)),
            )
            .child(
                div()
                    .id("properties-list")
                    .test_support()
                    .v_flex()
                    .flex_1()
                    .min_h(px(80.))
                    .py_1()
                    .children(properties.into_iter().map(|property| {
                        div()
                            .h_flex()
                            .items_center()
                            .justify_between()
                            .gap_2()
                            .px_3()
                            .py_1()
                            .text_xs()
                            .child(div().text_color(muted).child(property.name))
                            .child(div().child(property.value))
                    })),
            )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn selecting_a_symbol_changes_the_reported_kind() {
        let mut design = DesignState::placeholder();
        assert_eq!(design.selected_kind().as_ref(), "Sheet");
        design.select("sym-u1", "U1  MCU-48");
        assert_eq!(design.selected_kind().as_ref(), "Symbol");
        assert_eq!(design.selected_label().as_ref(), "U1  MCU-48");
        assert_eq!(design.properties()[0].value.as_ref(), "U1  MCU-48");
    }

    #[test]
    fn the_placeholder_hierarchy_has_nested_sheets() {
        let design = DesignState::placeholder();
        let root = &design.hierarchy()[0];
        assert!(root.is_folder());
        assert!(root.is_expanded());
        assert!(root.ancestors(&"sym-u2".into()).is_some());
    }
}
