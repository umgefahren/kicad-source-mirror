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
//! persisted by the dock machinery rather than being hard-coded columns.
//!
//! # What they can honestly show today
//!
//! A sheet hierarchy and per-item properties come from `SCHEMATIC` and the
//! selection, which live in C++ behind a link that is not wired yet. Rather
//! than invent a plausible-looking tree — placeholder data sitting beside real
//! rendering is worse than none, because a reader cannot tell which is which —
//! [`DesignState`] reports what the loaded draw stream actually contains, and
//! says plainly that the document model is not connected.
//!
//! When the host arrives, [`DesignState::set_document_tree`] replaces the
//! contents and neither panel changes.

use gpui_kit::TestSupportExt;
use gpui_kit::assets::IconName;
use gpui_kit::component::dock::{Panel, PanelEvent};
use gpui_kit::component::list::ListItem;
use gpui_kit::component::tree::{TreeItem, TreeState, tree};
use gpui_kit::component::{ActiveTheme, Icon, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{
    App, Context, Entity, EventEmitter, FocusHandle, Focusable, SharedString, Window, div, px,
};

/// Where what the panels show came from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DocumentSource {
    /// The built-in demonstration stream, synthesised by [`crate::demo`].
    Demonstration,
    /// A draw stream recorded from a real schematic by `kicad-sch-dump`.
    RecordedStream {
        /// The file it was read from, as shown in the title and the panels.
        file: SharedString,
    },
    /// A `.kicad_sch` opened through the C++ host, rendered in this process.
    ///
    /// Distinct from [`DocumentSource::RecordedStream`] on purpose: the two look
    /// identical on the canvas, and a window that cannot say which one it is
    /// showing makes it impossible to tell a live render from a replay of one.
    Schematic {
        /// The file the session loaded, as shown in the title and the panels.
        file: SharedString,
    },
    /// Nothing loaded.
    Empty,
}

impl DocumentSource {
    /// The name to put in the canvas tab and the panel headers.
    pub fn title(&self) -> SharedString {
        match self {
            DocumentSource::Demonstration => "demonstration stream".into(),
            DocumentSource::RecordedStream { file } | DocumentSource::Schematic { file } => {
                file.clone()
            }
            DocumentSource::Empty => "no document".into(),
        }
    }

    /// One line describing where the geometry came from.
    pub fn description(&self) -> SharedString {
        match self {
            DocumentSource::Demonstration => "Synthesised draw stream".into(),
            DocumentSource::RecordedStream { .. } => "Recorded draw stream".into(),
            DocumentSource::Schematic { .. } => "Live schematic session".into(),
            DocumentSource::Empty => "Nothing loaded".into(),
        }
    }
}

/// Facts about the loaded draw stream, which is all the shell can know about
/// the document without the C++ host.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct StreamFacts {
    /// Retained groups — one per cached `KIGFX::VIEW` item.
    pub groups: usize,
    /// Commands across all group bodies.
    pub group_commands: usize,
    /// Commands in the opening frame's body, mostly `DRAW_GROUP` references.
    ///
    /// The opening frame specifically, not the current one. With a live document
    /// the frame body is re-recorded per view change and `KIGFX::VIEW` culls it to
    /// the viewport, so this number moves with the camera and is not a property of
    /// the document. What the current frame costs is in the status bar, where it
    /// is updated every paint.
    pub frame_commands: usize,
    /// Embedded images.
    pub images: usize,
}

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
/// Shared between them so that picking a row on the left changes what the right
/// shows without either panel knowing about the other.
pub struct DesignState {
    source: DocumentSource,
    facts: StreamFacts,
    /// Document extent in internal units, as the renderer reports it.
    extent: [f64; 2],
    origin: [f64; 2],
    hierarchy: Vec<TreeItem>,
    selected_id: SharedString,
    selected_label: SharedString,
    selected_kind: SharedString,
    properties: Vec<Property>,
    /// Whether the tree is the real document hierarchy or the stream summary.
    connected: bool,
}

impl Default for DesignState {
    fn default() -> Self {
        Self::from_stream(
            DocumentSource::Empty,
            StreamFacts::default(),
            [0., 0.],
            [0., 0.],
        )
    }
}

impl DesignState {
    /// Describe a loaded draw stream.
    pub fn from_stream(
        source: DocumentSource,
        facts: StreamFacts,
        origin: [f64; 2],
        extent: [f64; 2],
    ) -> Self {
        let mut this = Self {
            source,
            facts,
            extent,
            origin,
            hierarchy: Vec::new(),
            selected_id: "document".into(),
            selected_label: SharedString::default(),
            selected_kind: "Document".into(),
            properties: Vec::new(),
            connected: false,
        };
        this.selected_label = this.source.title();
        this.hierarchy = this.stream_tree();
        this.properties = this.document_properties();
        this
    }

    /// Replace the summary with the host's real sheet tree.
    ///
    /// The single call the C++ side will make once `SCHEMATIC` is reachable;
    /// everything else in both panels already works against it.
    pub fn set_document_tree(&mut self, items: Vec<TreeItem>) {
        self.hierarchy = items;
        self.connected = true;
    }

    /// Whether the panels are showing the real document model.
    pub fn is_connected(&self) -> bool {
        self.connected
    }

    /// Where the geometry came from.
    pub fn source(&self) -> &DocumentSource {
        &self.source
    }

    /// What the loaded stream contains.
    pub fn facts(&self) -> StreamFacts {
        self.facts
    }

    /// The tree.
    pub fn hierarchy(&self) -> &[TreeItem] {
        &self.hierarchy
    }

    /// The id of the selected row.
    pub fn selected_id(&self) -> &SharedString {
        &self.selected_id
    }

    /// The label of the selected row.
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

    /// The line the hierarchy panel shows under its header.
    pub fn connection_note(&self) -> SharedString {
        if self.connected {
            return self.source.description();
        }

        match self.source {
            // A live session drew this, so "showing draw stream contents" would
            // undersell it — and claiming a connection would oversell it. The
            // geometry is the document's; the tree below is still the stream's.
            DocumentSource::Schematic { .. } => {
                "Rendered from the schematic \u{2014} the tree below describes the \
                 draw stream, and the document model is not connected yet"
                    .into()
            }
            _ => "Document model not connected \u{2014} showing draw stream contents".into(),
        }
    }

    /// Select a row.
    pub fn select(&mut self, id: impl Into<SharedString>, label: impl Into<SharedString>) {
        self.selected_id = id.into();
        self.selected_label = label.into();
        self.selected_kind = if self.connected {
            if self.selected_id.starts_with("sym-") {
                "Symbol".into()
            } else {
                "Sheet".into()
            }
        } else {
            "Document".into()
        };
        self.properties = self.document_properties();
    }

    fn stream_tree(&self) -> Vec<TreeItem> {
        let mm = crate::grid::IU_PER_MM;
        vec![
            TreeItem::new("document", self.source.title())
                .expanded(true)
                .children([
                    TreeItem::new("stream", "Draw stream")
                        .expanded(true)
                        .children([
                            TreeItem::new(
                                "stream-groups",
                                format!("{} retained groups", self.facts.groups),
                            ),
                            TreeItem::new(
                                "stream-gcmds",
                                format!("{} group commands", self.facts.group_commands),
                            ),
                            TreeItem::new(
                                "stream-fcmds",
                                format!(
                                    "{} commands in the opening frame",
                                    self.facts.frame_commands
                                ),
                            ),
                            TreeItem::new("stream-images", format!("{} images", self.facts.images)),
                        ]),
                    TreeItem::new("extent", "Extent").expanded(true).children([
                        TreeItem::new(
                            "extent-size",
                            format!(
                                "{:.1} \u{00d7} {:.1} mm",
                                self.extent[0] / mm,
                                self.extent[1] / mm
                            ),
                        ),
                        TreeItem::new(
                            "extent-origin",
                            format!(
                                "origin {:.1}, {:.1} mm",
                                self.origin[0] / mm,
                                self.origin[1] / mm
                            ),
                        ),
                    ]),
                ]),
        ]
    }

    fn document_properties(&self) -> Vec<Property> {
        let mm = crate::grid::IU_PER_MM;
        vec![
            Property::new("Document", self.source.title()),
            Property::new("Source", self.source.description()),
            Property::new("Retained groups", self.facts.groups.to_string()),
            Property::new("Group commands", self.facts.group_commands.to_string()),
            Property::new(
                "Opening frame commands",
                self.facts.frame_commands.to_string(),
            ),
            Property::new("Images", self.facts.images.to_string()),
            Property::new("Width", format!("{:.3} mm", self.extent[0] / mm)),
            Property::new("Height", format!("{:.3} mm", self.extent[1] / mm)),
            Property::new(
                "Origin",
                format!("{:.3}, {:.3} mm", self.origin[0] / mm, self.origin[1] / mm),
            ),
        ]
    }
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
        let (heading, note, connected) = {
            let state = self.design.read(cx);
            (
                if state.is_connected() {
                    SharedString::from("SHEETS AND SYMBOLS")
                } else {
                    SharedString::from("DRAW STREAM")
                },
                state.connection_note(),
                state.is_connected(),
            )
        };
        let theme = cx.theme();
        let muted = theme.muted_foreground;
        // An unconnected panel is a caveat, not an error: the warning colour
        // says "read this" without claiming something is broken.
        let warning = if connected { muted } else { theme.warning };
        div()
            .id("hierarchy-panel")
            .test_support()
            .track_focus(&self.focus_handle)
            .size_full()
            .v_flex()
            .child(
                div()
                    .v_flex()
                    .gap_1()
                    .px_3()
                    .py_2()
                    .child(
                        div()
                            .h_flex()
                            .items_center()
                            .gap_2()
                            .text_xs()
                            .text_color(muted)
                            .child(Icon::new(IconName::ListTree).size_4())
                            .child(heading),
                    )
                    .child(
                        div()
                            .id("hierarchy-note")
                            .test_support()
                            .text_xs()
                            .text_color(warning)
                            .child(note),
                    ),
            )
            .child(
                div()
                    .id("hierarchy-tree")
                    .test_support()
                    .flex_1()
                    .min_h(px(120.))
                    .child(tree(
                        &self.tree,
                        move |_ix, entry, selected, _window, _cx| {
                            let item = entry.item();
                            let id = item.id.clone();
                            let label = item.label.clone();
                            let design = design.clone();
                            let icon = if item.is_folder() {
                                IconName::Folder
                            } else if connected {
                                IconName::Component
                            } else {
                                IconName::Dash
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
                        },
                    )),
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
        let connected = design.is_connected();
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
                    .child(div().text_xs().text_color(muted).child(kind.to_uppercase()))
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
                            .items_start()
                            .justify_between()
                            .gap_2()
                            .px_3()
                            .py_1()
                            .text_xs()
                            .child(div().flex_shrink_0().text_color(muted).child(property.name))
                            .child(
                                div()
                                    .min_w_0()
                                    .text_right()
                                    .truncate()
                                    .child(property.value),
                            )
                    })),
            )
            .when(!connected, |this| {
                this.child(
                    div()
                        .id("properties-note")
                        .test_support()
                        .px_3()
                        .py_2()
                        .border_t_1()
                        .border_color(border)
                        .text_xs()
                        .text_color(muted)
                        .child(
                            "Per-item properties need the C++ document model, \
                             which this shell is not connected to yet.",
                        ),
                )
            })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn facts() -> StreamFacts {
        StreamFacts {
            groups: 336,
            group_commands: 2971,
            frame_commands: 795,
            images: 0,
        }
    }

    fn all_labels(items: &[TreeItem]) -> Vec<String> {
        fn walk(items: &[TreeItem], out: &mut Vec<String>) {
            for item in items {
                out.push(item.label.to_string());
                walk(&item.children, out);
            }
        }
        let mut out = Vec::new();
        walk(items, &mut out);
        out
    }

    fn state() -> DesignState {
        DesignState::from_stream(
            DocumentSource::RecordedStream {
                file: "ecc83-pp_v2.kicad_sch".into(),
            },
            facts(),
            [649_073.0, 367_249.0],
            [1_660_271.0, 1_393_524.0],
        )
    }

    /// The panels must not imply they are showing a document model they are
    /// not connected to. This is the assertion that keeps that honest.
    #[test]
    fn an_unconnected_panel_says_so() {
        let design = state();
        assert!(!design.is_connected());
        assert!(
            design.connection_note().contains("not connected"),
            "{}",
            design.connection_note()
        );
    }

    /// Including when the geometry did come from a live session, which is the
    /// case where the claim is most tempting to overstate.
    #[test]
    fn a_live_session_still_admits_the_model_is_not_connected() {
        let design = DesignState::from_stream(
            DocumentSource::Schematic {
                file: "video.kicad_sch".into(),
            },
            facts(),
            [0.0, 0.0],
            [1_000.0, 1_000.0],
        );

        assert!(!design.is_connected());

        let note = design.connection_note();
        assert!(note.contains("not connected yet"), "{note}");
        assert!(
            note.contains("Rendered from the schematic"),
            "and it should say where the geometry came from: {note}"
        );
    }

    #[test]
    fn the_tree_reports_the_streams_real_numbers() {
        let design = state();
        let root = &design.hierarchy()[0];
        assert_eq!(root.label.as_ref(), "ecc83-pp_v2.kicad_sch");
        let labels = all_labels(design.hierarchy());
        assert!(
            labels.iter().any(|l| l == "336 retained groups"),
            "{labels:?}"
        );
        assert!(
            labels
                .iter()
                .any(|l| l == "795 commands in the opening frame"),
            "{labels:?}"
        );
        // 1 660 271 internal units at 100 nm each is 166.0 mm.
        assert!(labels.iter().any(|l| l.contains("166.0")), "{labels:?}");
    }

    #[test]
    fn the_properties_are_the_documents_own_and_are_labelled_as_such() {
        let design = state();
        let names: Vec<&str> = design
            .properties()
            .iter()
            .map(|property| property.name.as_ref())
            .collect();
        assert!(names.contains(&"Retained groups"));
        assert!(names.contains(&"Width"));
        assert_eq!(design.selected_kind().as_ref(), "Document");
        let width = design
            .properties()
            .iter()
            .find(|property| property.name == "Width")
            .expect("width is reported");
        assert_eq!(width.value.as_ref(), "166.027 mm");
    }

    #[test]
    fn the_host_can_replace_the_summary_with_a_real_tree() {
        let mut design = state();
        design.set_document_tree(vec![
            TreeItem::new("sheet-root", "Root Sheet").child(TreeItem::new("sym-u1", "U1")),
        ]);
        assert!(design.is_connected());
        assert_eq!(design.hierarchy()[0].label.as_ref(), "Root Sheet");
        design.select("sym-u1", "U1");
        assert_eq!(design.selected_kind().as_ref(), "Symbol");
    }

    #[test]
    fn an_empty_document_still_describes_itself() {
        let design = DesignState::default();
        assert_eq!(design.source(), &DocumentSource::Empty);
        assert_eq!(design.source().title().as_ref(), "no document");
    }
}
