// SPDX-License-Identifier: GPL-3.0-or-later
//! GPUI document dialogs. Native services validate and commit model changes.
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, Entity, EventEmitter, TestSupportExt, Window, div, px};

pub(crate) const TITLES: [&str; 24] = [
    "Annotate Schematic",
    "Increment Annotations",
    "Page Settings",
    "Export Netlist",
    "Plot Schematic",
    "Print to PDF",
    "Export Bill of Materials",
    "Edit Symbol Fields",
    "Change Symbols",
    "Update Symbols from Library",
    "Remap Symbols",
    "Rescue Symbols",
    "Edit Symbol Library IDs",
    "Bus Aliases",
    "Netclasses",
    "Net Chain Setup",
    "Create Net Chain",
    "Edit Text and Graphics",
    "Import Schematic Settings",
    "Migrate Legacy Buses",
    "Update Schematic from PCB",
    "Resolve Field Name Conflicts",
    "Schematic Data Sources",
    "Import Schematic",
];

#[derive(Clone)]
enum FieldKind {
    Text,
    Number,
    Boolean,
    Choice(Vec<(&'static str, &'static str)>),
    Hidden,
    ReadOnly,
}
fn field_kind(kind: u32, index: usize) -> FieldKind {
    use FieldKind::*;
    match (kind, index) {
        (23, 0) => Choice(vec![
            ("sheet", "Hierarchical sheet"),
            ("append", "Append flat content"),
        ]),
        (23, 3 | 4) => Number,
        (23, 5 | 6) => Boolean,
        (22, 0) => Choice(vec![
            ("refresh", "Refresh"),
            ("install", "Install archive"),
            ("uninstall", "Uninstall"),
        ]),
        (22, 3) => Boolean,
        (22, 4..) => ReadOnly,
        (21, 0) => ReadOnly,
        (21, i) if i >= 2 && (i - 2) % 3 == 0 => Hidden,
        (21, i) if i >= 2 && (i - 2) % 3 == 1 => ReadOnly,
        (21, i) if i >= 2 => Choice(vec![
            ("first", "Keep first field"),
            ("last", "Keep last field"),
            ("join", "Join values"),
        ]),
        (20, 0) => Choice(vec![
            ("preview", "Preview changes"),
            ("apply", "Apply changes"),
        ]),
        (20, 2..=10) => Boolean,
        (20, 11) | (19, 0) => ReadOnly,
        (19, i) if i % 2 == 1 => Hidden,
        (0, 0) | (1, 0) | (14, 2..=7) | (17, 2..=3) => Number,
        (14, 0) => Choice(vec![
            ("save", "Save"),
            ("delete", "Delete class"),
            ("remove-pattern", "Remove pattern"),
        ]),
        (13 | 15, 0) => Choice(vec![("save", "Save"), ("delete", "Delete")]),
        (17, 0) => Choice(vec![
            ("all", "All sheets"),
            ("sheet", "Current sheet"),
            ("selection", "Selection"),
        ]),
        (17, 1) => Choice(vec![
            ("text", "Text and symbol fields"),
            ("graphics", "Lines and shapes"),
            ("both", "Both"),
        ]),
        (17, 4..=5) => Choice(vec![("keep", "Unchanged"), ("yes", "On"), ("no", "Off")]),
        (18, 1..=3) => Boolean,
        (13, 3..) | (14, 9..) | (15, 6..) | (16, 8..) => ReadOnly,
        (0, 4) | (1, 1) => Choice(vec![
            ("all", "All sheets"),
            ("sheet", "Current sheet"),
            ("selection", "Selected symbols"),
        ]),
        (0, 1) => Choice(vec![("x", "Left to right"), ("y", "Top to bottom")]),
        (0, 2) => Choice(vec![
            ("incremental", "First available"),
            ("100", "Sheet × 100"),
            ("1000", "Sheet × 1000"),
        ]),
        (0, 3) | (2, 1 | 15) | (4 | 5, 2..=4) | (6, 1 | 4 | 5) => Boolean,
        (2, 0) => Choice(vec![
            ("A4", "A4"),
            ("A3", "A3"),
            ("A2", "A2"),
            ("A1", "A1"),
            ("A0", "A0"),
            ("A5", "A5"),
            ("A", "ANSI A"),
            ("B", "ANSI B"),
            ("C", "ANSI C"),
            ("D", "ANSI D"),
            ("E", "ANSI E"),
            ("Letter", "Letter"),
            ("Legal", "Legal"),
        ]),
        (6, 2) => Choice(vec![
            ("standard", "Standard"),
            ("manufacturing", "Manufacturing"),
            ("custom", "Custom columns"),
            ("saved", "Saved preset"),
        ]),
        (3, 1) => Choice(vec![
            ("kicad", "KiCad"),
            ("xml", "XML"),
            ("spice", "SPICE"),
            ("cadstar", "CADSTAR"),
            ("orcad", "OrCAD"),
            ("pads", "PADS"),
            ("allegro", "Allegro"),
        ]),
        (4, 1) => Choice(vec![
            ("pdf", "PDF"),
            ("svg", "SVG"),
            ("dxf", "DXF"),
            ("ps", "PostScript"),
            ("hpgl", "HPGL"),
            ("png", "PNG"),
        ]),
        (2, 16) | (5, 1) | (7 | 12, 0) => Hidden,
        (6, 8) | (11, 2..) => ReadOnly,
        _ => Text,
    }
}

pub(crate) enum DocumentRequest {
    Apply(Vec<String>),
    Close,
}
pub(crate) struct DocumentDialog {
    pub(crate) kind: u32,
    fields: Vec<(String, String)>,
    inputs: Vec<Entity<InputState>>,
    status: String,
}
impl EventEmitter<DocumentRequest> for DocumentDialog {}
impl DocumentDialog {
    pub(crate) fn new(
        kind: u32,
        fields: Vec<(String, String)>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let inputs: Vec<Entity<InputState>> = fields
            .iter()
            .map(|(_, value)| cx.new(|cx| InputState::new(window, cx).default_value(value.clone())))
            .collect();
        if let Some(index) = fields
            .iter()
            .enumerate()
            .position(|(i, _)| matches!(field_kind(kind, i), FieldKind::Text | FieldKind::Number))
        {
            inputs[index].update(cx, |input, cx| input.focus(window, cx));
        }
        Self {
            kind,
            fields,
            inputs,
            status: String::new(),
        }
    }
    pub(crate) fn feedback(&mut self, status: String, cx: &mut Context<Self>) {
        self.status = status;
        cx.notify();
    }
    fn apply(&self, cx: &mut Context<Self>) {
        let values = self
            .fields
            .iter()
            .enumerate()
            .map(|(i, (_, value))| match field_kind(self.kind, i) {
                FieldKind::Text | FieldKind::Number => self.inputs[i].read(cx).value().to_string(),
                _ => value.clone(),
            })
            .collect();
        cx.emit(DocumentRequest::Apply(values));
    }
}
impl Render for DocumentDialog {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let rows = self
            .fields
            .iter()
            .enumerate()
            .filter_map(|(i, (name, value))| {
                let kind = field_kind(self.kind, i);
                if matches!(kind, FieldKind::Hidden) {
                    return None;
                }
                let label = name.split(" (").next().unwrap_or(name).to_owned();
                let control = match kind {
                    FieldKind::ReadOnly => div().text_sm().child(value.clone()).into_any_element(),
                    FieldKind::Boolean => Checkbox::new(("document-check", i))
                        .checked(value == "yes")
                        .label(label.clone())
                        .on_click(cx.listener(move |this, checked, _, cx| {
                            this.fields[i].1 = if *checked { "yes" } else { "no" }.into();
                            cx.notify();
                        }))
                        .into_any_element(),
                    FieldKind::Choice(choices) => div()
                        .h_flex()
                        .flex_wrap()
                        .gap_1()
                        .children(choices.into_iter().enumerate().map(|(j, (key, text))| {
                            Button::new(("document-choice", i * 32 + j))
                                .label(text)
                                .small()
                                .when(value == key, |button| button.primary())
                                .on_click(cx.listener(move |this, _, _, cx| {
                                    this.fields[i].1 = key.into();
                                    cx.notify();
                                }))
                        }))
                        .into_any_element(),
                    _ => Input::new(&self.inputs[i]).into_any_element(),
                };
                Some(
                    div()
                        .flex_shrink_0()
                        .v_flex()
                        .gap_1()
                        .when(matches!(self.kind, 7 | 12), |row| {
                            row.h_flex().items_center()
                        })
                        .child(div().text_sm().w(px(210.)).child(label))
                        .child(div().flex_1().child(control)),
                )
            })
            .collect::<Vec<_>>();
        div()
            .id("document-dialog")
            .test_support()
            .v_flex()
            .size_full()
            .min_h(px(0.))
            .overflow_hidden()
            .p_3()
            .gap_2()
            .bg(cx.theme().background)
            .border_b_1()
            .border_color(cx.theme().border)
            .on_key_down(cx.listener(|_, event: &gpui_kit::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" {
                    cx.emit(DocumentRequest::Close);
                    cx.stop_propagation();
                }
            }))
            .child(
                div().child(
                    TITLES
                        .get(self.kind as usize)
                        .copied()
                        .unwrap_or("Document"),
                ),
            )
            .when(self.kind == 5, |row| {
                row.child(
                    div()
                        .text_sm()
                        .child("Create a printable PDF for the selected sheets."),
                )
            })
            .when(self.kind == 7, |row| {
                row.child(
                    div()
                        .text_sm()
                        .child("Edit symbol instance fields. Apply commits one undo step."),
                )
            })
            .child(
                div()
                    .id("document-fields")
                    .v_flex()
                    .flex_1()
                    .min_h(px(0.))
                    .gap_3()
                    .overflow_y_scroll()
                    .children(rows),
            )
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .child(
                        Button::new("document-apply")
                            .label(if (3..=6).contains(&self.kind) {
                                "Export"
                            } else {
                                "Apply"
                            })
                            .primary()
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| this.apply(cx))),
                    )
                    .child(
                        Button::new("document-close")
                            .label("Close")
                            .small()
                            .on_click(cx.listener(|_, _, _, cx| cx.emit(DocumentRequest::Close))),
                    ),
            )
            .child(div().text_sm().child(self.status.clone()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn options_use_controls_and_identity_is_hidden() {
        assert!(matches!(field_kind(0, 1), FieldKind::Choice(_)));
        assert!(matches!(field_kind(0, 3), FieldKind::Boolean));
        assert!(matches!(field_kind(7, 0), FieldKind::Hidden));
        assert!(matches!(field_kind(12, 0), FieldKind::Hidden));
    }
}
