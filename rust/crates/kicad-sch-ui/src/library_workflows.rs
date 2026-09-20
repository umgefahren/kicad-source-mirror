// SPDX-License-Identifier: GPL-3.0-or-later
//! GPUI library table editor.
/// One editable symbol library table row.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LibraryRow {
    /// Unique nickname.
    pub name: String,
    /// KiCad plugin type (KiCad, Legacy, Database, etc.).
    pub kind: String,
    /// File or remote resource location, including environment substitutions.
    pub uri: String,
    /// Plugin options.
    pub options: String,
    /// User description.
    pub description: String,
    /// Whether the library is enabled.
    pub enabled: bool,
    /// Whether it appears in choosers.
    pub visible: bool,
}

use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, Entity, EventEmitter, Window, div, px};

pub(crate) enum LibraryRequest {
    Load(bool),
    Configure(bool, String),
    Apply(bool, Vec<LibraryRow>),
    Close,
}
struct EditableRow {
    inputs: Vec<Entity<InputState>>,
    enabled: bool,
    visible: bool,
}
impl EditableRow {
    fn new(row: LibraryRow, window: &mut Window, cx: &mut Context<LibraryPanel>) -> Self {
        let inputs = [row.name, row.kind, row.uri, row.options, row.description]
            .into_iter()
            .map(|value| cx.new(|cx| InputState::new(window, cx).default_value(value)))
            .collect();
        Self {
            inputs,
            enabled: row.enabled,
            visible: row.visible,
        }
    }
    fn value(&self, cx: &Context<LibraryPanel>) -> LibraryRow {
        let get = |i: usize| self.inputs[i].read(cx).value().to_string();
        LibraryRow {
            name: get(0),
            kind: get(1),
            uri: get(2),
            options: get(3),
            description: get(4),
            enabled: self.enabled,
            visible: self.visible,
        }
    }
}
pub(crate) struct LibraryPanel {
    global: bool,
    rows: Vec<EditableRow>,
    status: String,
}
impl EventEmitter<LibraryRequest> for LibraryPanel {}
impl LibraryPanel {
    pub(crate) fn new(
        global: bool,
        rows: Vec<LibraryRow>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        Self {
            global,
            rows: rows
                .into_iter()
                .map(|r| EditableRow::new(r, window, cx))
                .collect(),
            status: String::new(),
        }
    }
    pub(crate) fn feedback(&mut self, status: String, cx: &mut Context<Self>) {
        self.status = status;
        cx.notify();
    }
}
impl Render for LibraryPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().id("library-table-panel").v_flex().gap_2().p_3().size_full().min_h(px(0.)).overflow_hidden()
            .bg(cx.theme().background).border_b_1().border_color(cx.theme().border)
            .on_key_down(cx.listener(|_, event: &gpui_kit::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" { cx.emit(LibraryRequest::Close); cx.stop_propagation(); }
            }))
            .child(div().h_flex().flex_wrap().flex_shrink_0().gap_2().items_center()
                .child(div().flex_1().child(if self.global { "Global Symbol Libraries" } else { "Project Symbol Libraries" }))
                .child(Button::new("library-switch").label(if self.global { "Open Project Table" } else { "Open Global Table" }).small()
                    .on_click(cx.listener(|this, _, _, cx| cx.emit(LibraryRequest::Load(!this.global)))))
                .child(Button::new("library-close").label("Close").small().on_click(cx.listener(|_, _, _, cx| cx.emit(LibraryRequest::Close)))))
            .child(div().text_xs().child("Types: KiCad, Legacy, Database, HTTP. URI and options are passed to the selected KiCad library plugin. Save applies the entire table; Close discards edits."))
            .child(div().id("library-rows").v_flex().flex_1().min_h(px(0.)).gap_2().overflow_y_scroll()
            .children(self.rows.iter().enumerate().map(|(index, row)| {
                div().v_flex().flex_shrink_0().gap_1().p_2().border_1().border_color(cx.theme().border)
                    .children(row.inputs.iter().enumerate().map(|(column, input)| div().h_flex().flex_wrap().flex_shrink_0().gap_2().items_center()
                        .child(div().w(px(100.)).child(["Name", "Type", "URI", "Options", "Description"][column]))
                        .child(div().flex_1().child(Input::new(input)))))
                    .child(div().h_flex().flex_wrap().flex_shrink_0().gap_2()
                        .child(Button::new(("library-enabled", index)).label(if row.enabled { "Enabled" } else { "Disabled" }).small()
                            .on_click(cx.listener(move |this, _, _, cx| { this.rows[index].enabled = !this.rows[index].enabled; cx.notify(); })))
                        .child(Button::new(("library-visible", index)).label(if row.visible { "Visible" } else { "Hidden" }).small()
                            .on_click(cx.listener(move |this, _, _, cx| { this.rows[index].visible = !this.rows[index].visible; cx.notify(); })))
                        .child(Button::new(("library-configure", index)).label("Connection Settings").small()
                            .on_click(cx.listener(move |this, _, _, cx| { let name=this.rows[index].value(cx).name; cx.emit(LibraryRequest::Configure(this.global,name)); })))
                        .child(Button::new(("library-remove", index)).label("Remove").small()
                            .on_click(cx.listener(move |this, _, _, cx| { this.rows.remove(index); cx.notify(); }))))
            }))
            )
            .child(div().h_flex().flex_wrap().flex_shrink_0().gap_2()
                .child(Button::new("library-add").label("Add Library").small().on_click(cx.listener(|this, _, window, cx| {
                    this.rows.push(EditableRow::new(LibraryRow { kind: "KiCad".into(), enabled: true, visible: true, ..Default::default() }, window, cx)); cx.notify();
                })))
                .child(Button::new("library-save").label("Save Table").primary().small().on_click(cx.listener(|this, _, _, cx| {
                    let rows = this.rows.iter().map(|row| row.value(cx)).collect();
                    cx.emit(LibraryRequest::Apply(this.global, rows));
                }))))
            .child(div().text_xs().child(self.status.clone()))
    }
}
