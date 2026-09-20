// SPDX-License-Identifier: GPL-3.0-or-later
//! Typed model property editors, owned by GPUI and committed by the host.
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState, Textarea, TextareaState};
use gpui_kit::component::select::{Select, SelectState};
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, Entity, EventEmitter, TestSupportExt, Window, div, px};

/// Typed editable properties of a selected schematic item or settings scope.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ItemProperties {
    /// Model capabilities: bit 0 custom fields; bit 1 hierarchical sheet relinking.
    pub capabilities: u32,
    /// Stable identity, checked again on apply.
    pub item_id: String,
    /// Fields in host order.
    pub entries: Vec<PropertyEntry>,
}
/// A named property with an explicit editor kind and optional choices.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PropertyEntry {
    /// Display name.
    pub name: String,
    /// Editable value.
    pub value: String,
    /// Explicit host editor kind (text, boolean, integer, number, mm, degrees, color, choice, multiline).
    pub kind: u32,
    /// Allowed values for choice editors.
    pub choices: Vec<String>,
}

pub(crate) enum PropertiesRequest {
    Apply(ItemProperties),
    SheetFile {
        item_id: String,
        path: String,
    },
    CustomField {
        item_id: String,
        name: String,
        value: Option<String>,
    },
    Close,
}
type PropertyChoice = Entity<SelectState<Vec<String>>>;

pub(crate) struct PropertiesPanel {
    data: ItemProperties,
    inputs: Vec<Entity<InputState>>,
    textareas: Vec<Option<Entity<TextareaState>>>,
    choices: Vec<Option<PropertyChoice>>,
    title: String,
    filter: Entity<InputState>,
    sheet_path: Entity<InputState>,
    field_name: Entity<InputState>,
    field_value: Entity<InputState>,
    status: String,
}
impl EventEmitter<PropertiesRequest> for PropertiesPanel {}
impl PropertiesPanel {
    pub(crate) fn new(data: ItemProperties, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let inputs: Vec<Entity<InputState>> = data
            .entries
            .iter()
            .map(|entry| {
                cx.new(|cx| InputState::new(window, cx).default_value(entry.value.clone()))
            })
            .collect();
        for input in &inputs {
            cx.subscribe(input, |this, _, event, cx| {
                if matches!(event, InputEvent::PressEnter { .. }) {
                    this.apply(cx);
                }
            })
            .detach();
        }
        if let Some(input) = inputs.first() {
            input.update(cx, |state, cx| state.focus(window, cx));
        }
        let choices = data
            .entries
            .iter()
            .map(|entry| {
                (entry.kind == 7).then(|| {
                    cx.new(|cx| {
                        let mut state = SelectState::new(entry.choices.clone(), None, window, cx);
                        state.set_selected_value(&entry.value, window, cx);
                        state
                    })
                })
            })
            .collect();
        let textareas: Vec<Option<Entity<TextareaState>>> = data
            .entries
            .iter()
            .map(|entry| {
                (entry.kind == 8).then(|| {
                    cx.new(|cx| TextareaState::new(window, cx).default_value(entry.value.clone()))
                })
            })
            .collect();
        if let Some(Some(textarea)) = textareas.first() {
            textarea.update(cx, |state, cx| state.focus(window, cx));
        }
        let filter = cx.new(|cx| InputState::new(window, cx).placeholder("Filter settings…"));
        cx.subscribe(&filter, |_, _, _: &InputEvent, cx| cx.notify())
            .detach();
        Self {
            data,
            inputs,
            textareas,
            choices,
            title: "Item Properties".into(),
            filter,
            sheet_path: cx.new(|cx| {
                InputState::new(window, cx).placeholder("Hierarchical sheet filename (.kicad_sch)")
            }),
            field_name: cx.new(|cx| InputState::new(window, cx).placeholder("Custom field name")),
            field_value: cx.new(|cx| InputState::new(window, cx).placeholder("Custom field value")),
            status: String::new(),
        }
    }
    pub(crate) fn new_with_title(
        data: ItemProperties,
        title: impl Into<String>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut panel = Self::new(data, window, cx);
        panel.title = title.into();
        panel
    }

    fn apply(&self, cx: &mut Context<Self>) {
        let mut data = self.data.clone();
        for (index, (entry, input)) in data.entries.iter_mut().zip(&self.inputs).enumerate() {
            if let Some(choice) = &self.choices[index] {
                if let Some(value) = choice.read(cx).selected_value() {
                    entry.value = value.clone();
                }
            } else if let Some(textarea) = &self.textareas[index] {
                entry.value = textarea.read(cx).value().to_string();
            } else if entry.kind != 1 {
                entry.value = input.read(cx).value().to_string();
            }
        }
        cx.emit(PropertiesRequest::Apply(data));
    }

    pub(crate) fn feedback(&mut self, status: String, cx: &mut Context<Self>) {
        self.status = status;
        cx.notify();
    }
}
impl Render for PropertiesPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let filter = self.filter.read(cx).value().to_lowercase();
        div()
            .id("item-properties-panel")
            .test_support()
            .size_full()
            .min_h(px(0.))
            .overflow_hidden()
            .v_flex()
            .gap_2()
            .p_3()
            .bg(cx.theme().background)
            .border_b_1()
            .border_color(cx.theme().border)
            .on_key_down(cx.listener(|_, event: &gpui_kit::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" {
                    cx.emit(PropertiesRequest::Close);
                    cx.stop_propagation();
                }
            }))
            .child(div().child(self.title.clone()))
            .child(
                div()
                    .id("properties-filter")
                    .child(Input::new(&self.filter)),
            )
            .child(
                div()
                    .id("properties-body")
                    .v_flex()
                    .flex_1()
                    .min_h(px(0.))
                    .overflow_y_scroll()
                    .gap_2()
                    .children(
                        self.data
                            .entries
                            .iter()
                            .zip(&self.inputs)
                            .enumerate()
                            .filter(|(_, (entry, _))| entry.name.to_lowercase().contains(&filter))
                            .map(|(index, (entry, input))| {
                                let editor = if entry.kind == 1 {
                                    Checkbox::new(("property-boolean", index))
                                        .checked(entry.value == "true")
                                        .on_click(cx.listener(
                                            move |this, checked: &bool, _, cx| {
                                                this.data.entries[index].value =
                                                    checked.to_string();
                                                cx.notify();
                                            },
                                        ))
                                        .into_any_element()
                                } else if let Some(choice) = &self.choices[index] {
                                    Select::new(choice).into_any_element()
                                } else if let Some(textarea) = &self.textareas[index] {
                                    Textarea::new(textarea).h(px(100.)).into_any_element()
                                } else {
                                    Input::new(input).into_any_element()
                                };
                                let unit = match entry.kind {
                                    4 if !entry.name.trim_end().ends_with("(mm)") => " (mm)",
                                    5 if !entry.name.trim_end().ends_with("(°)") => " (°)",
                                    _ => "",
                                };
                                div()
                                    .h_flex()
                                    .flex_shrink_0()
                                    .gap_2()
                                    .items_center()
                                    .child(
                                        div().w(px(220.)).child(format!("{}{}", entry.name, unit)),
                                    )
                                    .child(div().flex_1().child(editor))
                            }),
                    )
                    .when(self.data.capabilities & 1 != 0, |panel| {
                        panel.child(
                            div()
                                .v_flex()
                                .gap_2()
                                .child(div().text_sm().child("Custom fields"))
                                .child(Input::new(&self.field_name))
                                .child(Input::new(&self.field_value))
                                .child(
                                    div()
                                        .h_flex()
                                        .gap_2()
                                        .child(
                                            Button::new("properties-add-field")
                                                .label("Add Field")
                                                .small()
                                                .on_click(cx.listener(|this, _, _, cx| {
                                                    cx.emit(PropertiesRequest::CustomField {
                                                        item_id: this.data.item_id.clone(),
                                                        name: this
                                                            .field_name
                                                            .read(cx)
                                                            .value()
                                                            .to_string(),
                                                        value: Some(
                                                            this.field_value
                                                                .read(cx)
                                                                .value()
                                                                .to_string(),
                                                        ),
                                                    });
                                                })),
                                        )
                                        .child(
                                            Button::new("properties-remove-field")
                                                .label("Remove Named Field")
                                                .small()
                                                .on_click(cx.listener(|this, _, _, cx| {
                                                    cx.emit(PropertiesRequest::CustomField {
                                                        item_id: this.data.item_id.clone(),
                                                        name: this
                                                            .field_name
                                                            .read(cx)
                                                            .value()
                                                            .to_string(),
                                                        value: None,
                                                    });
                                                })),
                                        ),
                                ),
                        )
                    })
                    .when(self.data.capabilities & 2 != 0, |panel| {
                        panel.child(
                            div()
                                .v_flex()
                                .gap_2()
                                .child(Input::new(&self.sheet_path))
                                .child(
                                    Button::new("properties-relink-sheet")
                                        .label(if self.data.capabilities & 4 != 0 {
                                            "Set Sheet File"
                                        } else {
                                            "Relink Sheet (clears undo history)"
                                        })
                                        .small()
                                        .on_click(cx.listener(|this, _, _, cx| {
                                            cx.emit(PropertiesRequest::SheetFile {
                                                item_id: this.data.item_id.clone(),
                                                path: this.sheet_path.read(cx).value().to_string(),
                                            });
                                        })),
                                ),
                        )
                    }),
            )
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .child(
                        Button::new("properties-apply")
                            .label("Apply")
                            .primary()
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.apply(cx);
                            })),
                    )
                    .child(
                        Button::new("properties-close")
                            .label("Close")
                            .small()
                            .on_click(cx.listener(|_, _, _, cx| cx.emit(PropertiesRequest::Close))),
                    ),
            )
            .child(div().text_xs().child(self.status.clone()))
    }
}
