// SPDX-License-Identifier: GPL-3.0-or-later

//! GPUI Find/Replace controls and the presentation-independent host search seam.

use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, Entity, EventEmitter, TestSupportExt, Window, div, px};

/// Owned search settings passed to the schematic host.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct SearchData {
    /// Text to locate.
    pub find: String,
    /// Replacement text; may be empty.
    pub replace: String,
    /// Match upper/lower case exactly.
    pub match_case: bool,
    /// Require a whole-word match.
    pub whole_word: bool,
    /// Restrict search to the current sheet.
    pub current_sheet_only: bool,
    /// Restrict search to the selection.
    pub selected_only: bool,
    /// Allow editing symbol references.
    pub replace_references: bool,
    /// Include all symbol fields.
    pub search_all_fields: bool,
    /// Include pin names and numbers.
    pub search_all_pins: bool,
    /// Find only items that can be replaced.
    pub replace_mode: bool,
    /// Whether the search interface is active.
    pub active: bool,
}

/// A search operation performed by the host.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SearchOperation {
    /// Find the next match.
    Next,
    /// Find the preceding match.
    Previous,
    /// Replace the current match, then find the next.
    Replace,
    /// Replace every matching item in the scope.
    ReplaceAll,
    /// Clear search highlighting and deactivate search.
    Close,
}

impl SearchOperation {
    /// The existing KiCad action for this operation, if any.
    pub fn action(self) -> Option<&'static str> {
        match self {
            Self::Next => Some("common.Interactive.findNext"),
            Self::Previous => Some("common.Interactive.findPrevious"),
            Self::Replace => Some("common.Interactive.replaceAndFindNext"),
            Self::ReplaceAll => Some("common.Interactive.replaceAll"),
            Self::Close => None,
        }
    }
}

/// Search feedback and the new view center, in schematic internal units.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct SearchResult {
    /// A current match exists.
    pub found: bool,
    /// Search passed the end of its scope.
    pub wrapped: bool,
    /// Horizontal coordinate of the match.
    pub center_x: f64,
    /// Vertical coordinate of the match.
    pub center_y: f64,
    /// Number of items changed by this operation.
    pub replaced: u32,
}

pub(crate) struct SearchRequest(pub SearchData, pub SearchOperation);

pub(crate) struct SearchPanel {
    find: Entity<InputState>,
    replace: Entity<InputState>,
    options: SearchData,
    status: String,
}

impl EventEmitter<SearchRequest> for SearchPanel {}

impl SearchPanel {
    pub(crate) fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let find = cx.new(|cx| InputState::new(window, cx).placeholder("Text to find"));
        let replace = cx.new(|cx| {
            InputState::new(window, cx).placeholder("Replacement text (empty to delete)")
        });
        for input in [&find, &replace] {
            cx.subscribe(input, |this, _, event, cx| {
                if let InputEvent::PressEnter { shift, .. } = event {
                    this.execute(
                        if *shift {
                            SearchOperation::Previous
                        } else {
                            SearchOperation::Next
                        },
                        cx,
                    );
                }
            })
            .detach();
        }
        Self {
            find,
            replace,
            options: SearchData::default(),
            status: String::new(),
        }
    }

    pub(crate) fn open(&mut self, replace: bool, window: &mut Window, cx: &mut Context<Self>) {
        self.options.replace_mode = replace;
        self.options.active = true;
        self.find.update(cx, |input, cx| input.focus(window, cx));
        cx.notify();
    }

    pub(crate) fn execute(&mut self, operation: SearchOperation, cx: &mut Context<Self>) {
        let mut data = self.options.clone();
        data.find = self.find.read(cx).value().to_string();
        data.replace = self.replace.read(cx).value().to_string();
        data.active = operation != SearchOperation::Close;
        if data.find.is_empty() && data.active {
            self.feedback("Enter text to find".into(), cx);
            return;
        }
        cx.emit(SearchRequest(data, operation));
    }

    pub(crate) fn feedback(&mut self, status: String, cx: &mut Context<Self>) {
        self.status = status;
        cx.notify();
    }

    fn option(
        &self,
        id: &'static str,
        label: &'static str,
        get: fn(&SearchData) -> bool,
        set: fn(&mut SearchData, bool),
        cx: &Context<Self>,
    ) -> impl IntoElement {
        Checkbox::new(id)
            .label(label)
            .checked(get(&self.options))
            .small()
            .on_click(cx.listener(move |this, checked, _, cx| {
                set(&mut this.options, *checked);
                cx.notify();
            }))
    }
}

impl Render for SearchPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let theme = cx.theme();
        div()
            .id("find-replace-panel")
            .test_support()
            .v_flex()
            .gap_2()
            .p_3()
            .bg(theme.background)
            .border_b_1()
            .border_color(theme.border)
            .on_key_down(cx.listener(|this, event: &gpui_kit::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" {
                    this.execute(SearchOperation::Close, cx);
                    cx.stop_propagation();
                }
            }))
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .items_center()
                    .child(div().w(px(70.)).child("Find"))
                    .child(div().flex_1().child(Input::new(&self.find)))
                    .child(
                        Button::new("find-previous")
                            .label("Previous")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.execute(SearchOperation::Previous, cx)
                            })),
                    )
                    .child(
                        Button::new("find-next")
                            .label("Next")
                            .primary()
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.execute(SearchOperation::Next, cx)
                            })),
                    )
                    .child(Button::new("find-close").label("Close").small().on_click(
                        cx.listener(|this, _, _, cx| this.execute(SearchOperation::Close, cx)),
                    )),
            )
            .when(self.options.replace_mode, |panel| {
                panel.child(
                    div()
                        .h_flex()
                        .gap_2()
                        .items_center()
                        .child(div().w(px(70.)).child("Replace"))
                        .child(div().flex_1().child(Input::new(&self.replace)))
                        .child(
                            Button::new("find-replace")
                                .label("Replace & Next")
                                .small()
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.execute(SearchOperation::Replace, cx)
                                })),
                        )
                        .child(
                            Button::new("find-replace-all")
                                .label("Replace All")
                                .small()
                                .on_click(cx.listener(|this, _, _, cx| {
                                    this.execute(SearchOperation::ReplaceAll, cx)
                                })),
                        ),
                )
            })
            .child(
                div()
                    .h_flex()
                    .flex_wrap()
                    .gap_3()
                    .child(self.option(
                        "find-case",
                        "Match case",
                        |d| d.match_case,
                        |d, v| d.match_case = v,
                        cx,
                    ))
                    .child(self.option(
                        "find-word",
                        "Whole word",
                        |d| d.whole_word,
                        |d, v| d.whole_word = v,
                        cx,
                    ))
                    .child(self.option(
                        "find-sheet",
                        "Current sheet only",
                        |d| d.current_sheet_only,
                        |d, v| d.current_sheet_only = v,
                        cx,
                    ))
                    .child(self.option(
                        "find-selected",
                        "Selected only",
                        |d| d.selected_only,
                        |d, v| d.selected_only = v,
                        cx,
                    ))
                    .child(self.option(
                        "find-fields",
                        "All fields",
                        |d| d.search_all_fields,
                        |d, v| d.search_all_fields = v,
                        cx,
                    ))
                    .child(self.option(
                        "find-pins",
                        "Pin names and numbers",
                        |d| d.search_all_pins,
                        |d, v| d.search_all_pins = v,
                        cx,
                    ))
                    .when(self.options.replace_mode, |row| {
                        row.child(self.option(
                            "find-references",
                            "Replace references",
                            |d| d.replace_references,
                            |d, v| d.replace_references = v,
                            cx,
                        ))
                    }),
            )
            .child(div().text_xs().child(self.status.clone()))
    }
}
