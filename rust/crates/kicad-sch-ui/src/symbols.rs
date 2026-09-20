// SPDX-License-Identifier: GPL-3.0-or-later
//! Browse one installed symbol library at a time, with an offline schematic cache.
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::input::{Input, InputEvent, InputState};
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, Entity, EventEmitter, TestSupportExt, Window, div, px};

use kicad_sch_render::{SchematicCanvas, SchematicRenderer};
use std::{cell::RefCell, rc::Rc};

pub(crate) enum SymbolRequest {
    Place(String, u32, u32),
    Preview(String, u32, u32),
    Browse(String, bool),
    Close,
}
pub(crate) struct SymbolPanel {
    query: Entity<InputState>,
    library_query: Entity<InputState>,
    libraries: Vec<String>,
    symbols: Vec<String>,
    library: String,
    power_only: bool,
    status: String,
    preview_id: String,
    unit: u32,
    body: u32,
    units: u32,
    bodies: u32,
    renderer: Rc<RefCell<SchematicRenderer>>,
}
impl EventEmitter<SymbolRequest> for SymbolPanel {}
impl SymbolPanel {
    pub(crate) fn new(window: &mut Window, cx: &mut Context<Self>) -> Self {
        let query = cx.new(|cx| {
            InputState::new(window, cx).placeholder("Filter symbols or enter Library:Symbol")
        });
        let library_query =
            cx.new(|cx| InputState::new(window, cx).placeholder("Filter installed libraries"));
        cx.subscribe(&query, |this, _, event, cx| {
            if let InputEvent::PressEnter { .. } = event {
                this.place(cx);
            }
            cx.notify();
        })
        .detach();
        cx.subscribe(&library_query, |_, _, _: &InputEvent, cx| cx.notify())
            .detach();
        Self {
            query,
            library_query,
            libraries: Vec::new(),
            symbols: Vec::new(),
            library: String::new(),
            power_only: false,
            status: String::new(),
            preview_id: String::new(),
            unit: 1,
            body: 1,
            units: 1,
            bodies: 1,
            renderer: Rc::new(RefCell::new(SchematicRenderer::new())),
        }
    }
    pub(crate) fn open(
        &mut self,
        symbols: Vec<String>,
        libraries: Vec<String>,
        power_only: bool,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        self.symbols = symbols;
        self.libraries = libraries;
        self.power_only = power_only;
        self.status.clear();
        self.query.update(cx, |input, cx| input.focus(window, cx));
        cx.notify();
    }
    pub(crate) fn set_symbols(&mut self, symbols: Vec<String>, cx: &mut Context<Self>) {
        self.symbols = symbols;
        self.status.clear();
        cx.notify();
    }
    pub(crate) fn set_preview(
        &mut self,
        stream: kicad_gal::Stream,
        units: u32,
        bodies: u32,
        cx: &mut Context<Self>,
    ) {
        self.units = units;
        self.bodies = bodies;
        let mut renderer = self.renderer.borrow_mut();
        renderer.set_stream(stream);
        renderer.set_viewport([470., 150.]);
        renderer.zoom_to_fit(16.);
        self.status.clear();
        cx.notify();
    }
    fn preview(&mut self, id: String, cx: &mut Context<Self>) {
        if self.preview_id != id {
            self.unit = 1;
            self.body = 1;
        }
        self.preview_id = id.clone();
        cx.emit(SymbolRequest::Preview(id, self.unit, self.body));
        cx.notify();
    }
    fn place_id(&self, id: String, cx: &mut Context<Self>) {
        let (unit, body) = if id == self.preview_id {
            (self.unit, self.body)
        } else {
            (1, 1)
        };
        cx.emit(SymbolRequest::Place(id, unit, body));
    }
    pub(crate) fn feedback(&mut self, status: String, cx: &mut Context<Self>) {
        self.status = status;
        cx.notify();
    }
    fn browse(&mut self, library: String, cx: &mut Context<Self>) {
        self.library = library.clone();
        self.symbols.clear();
        cx.emit(SymbolRequest::Browse(library, self.power_only));
        cx.notify();
    }
    fn place(&mut self, cx: &mut Context<Self>) {
        let id = self.query.read(cx).value().to_string();
        let id = id.trim();
        if id.contains(':') {
            self.place_id(id.to_owned(), cx);
        } else if let Some(id) = self
            .symbols
            .iter()
            .find(|s| s.to_lowercase().contains(&id.to_lowercase()))
        {
            self.place_id(id.clone(), cx);
        }
    }
}
impl Render for SymbolPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let query = self.query.read(cx).value().to_lowercase();
        let library_query = self.library_query.read(cx).value().to_lowercase();
        let matches: Vec<_> = self
            .symbols
            .iter()
            .filter(|id| id.to_lowercase().contains(&query))
            .cloned()
            .collect();
        let libraries: Vec<_> = self
            .libraries
            .iter()
            .filter(|id| id.to_lowercase().contains(&library_query))
            .cloned()
            .collect();
        let summary = format!(
            "{}: {} matches",
            if self.library.is_empty() {
                "Schematic cache"
            } else {
                &self.library
            },
            matches.len()
        );
        div()
            .id("symbol-chooser")
            .test_support()
            .on_key_down(cx.listener(|_, event: &gpui_kit::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" {
                    cx.emit(SymbolRequest::Close);
                    cx.stop_propagation();
                }
            }))
            .v_flex()
            .gap_2()
            .p_3()
            .size_full()
            .min_h(px(0.))
            .overflow_hidden()
            .bg(cx.theme().background)
            .border_1()
            .border_color(cx.theme().border)
            .rounded_md()
            .child(div().text_lg().child(if self.power_only {
                "Choose Power Symbol"
            } else {
                "Choose Symbol"
            }))
            .child(Input::new(&self.library_query))
            .child(div().text_sm().child(format!(
                "{} installed libraries — filter by name, then select one",
                self.libraries.len()
            )))
            .child(
                div()
                    .id("symbol-library-list")
                    .h_flex()
                    .flex_wrap()
                    .gap_1()
                    .flex_1()
                    .min_h(px(60.))
                    .overflow_y_scroll()
                    .child(
                        Button::new("cached-symbols")
                            .label("Schematic cache")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| this.browse(String::new(), cx))),
                    )
                    .children(libraries.into_iter().enumerate().map(|(index, name)| {
                        Button::new(("symbol-library", index))
                            .flex_shrink_0()
                            .label(name.clone())
                            .small()
                            .on_click(
                                cx.listener(move |this, _, _, cx| this.browse(name.clone(), cx)),
                            )
                    })),
            )
            .child(Input::new(&self.query))
            .child(div().text_sm().child(summary))
            .child(
                div()
                    .id("symbol-results")
                    .v_flex()
                    .gap_1()
                    .flex_1()
                    .min_h(px(100.))
                    .overflow_y_scroll()
                    .children(matches.into_iter().enumerate().map(|(index, id)| {
                        let preview_id = id.clone();
                        div()
                            .h_flex()
                            .flex_shrink_0()
                            .gap_1()
                            .child(
                                Button::new(("cached-symbol", index))
                                    .label(id.clone())
                                    .small()
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.place_id(id.clone(), cx)
                                    })),
                            )
                            .child(
                                Button::new(("preview-symbol", index))
                                    .label("Preview")
                                    .small()
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.preview(preview_id.clone(), cx)
                                    })),
                            )
                    })),
            )
            .when(!self.preview_id.is_empty(), |panel| {
                panel
                    .child(div().text_sm().child(self.preview_id.clone()))
                    .child(
                        SchematicCanvas::new("symbol-preview-canvas", self.renderer.clone())
                            .w_full()
                            .flex_1()
                            .min_h(px(100.)),
                    )
                    .child(
                        div()
                            .h_flex()
                            .flex_shrink_0()
                            .flex_wrap()
                            .gap_2()
                            .child(
                                Button::new("symbol-unit")
                                    .label(format!("Unit {} / {}", self.unit, self.units))
                                    .small()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.unit = this.unit % this.units.max(1) + 1;
                                        this.preview(this.preview_id.clone(), cx);
                                    })),
                            )
                            .child(
                                Button::new("symbol-body")
                                    .label(format!("Body {} / {}", self.body, self.bodies))
                                    .small()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.body = this.body % this.bodies.max(1) + 1;
                                        this.preview(this.preview_id.clone(), cx);
                                    })),
                            )
                            .child(
                                Button::new("place-preview-symbol")
                                    .label("Place Preview")
                                    .small()
                                    .on_click(cx.listener(|this, _, _, cx| {
                                        this.place_id(this.preview_id.clone(), cx)
                                    })),
                            ),
                    )
            })
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .child(
                        Button::new("place-symbol")
                            .label("Place")
                            .primary()
                            .on_click(cx.listener(|this, _, _, cx| this.place(cx))),
                    )
                    .child(
                        Button::new("close-symbols")
                            .label("Cancel")
                            .on_click(cx.listener(|_, _, _, cx| cx.emit(SymbolRequest::Close))),
                    ),
            )
            .child(div().text_sm().child(self.status.clone()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use gpui_kit::component::Root;
    use gpui_kit::test::TestWindowExt;
    use gpui_kit::{TestAppContext, size};
    use std::{cell::RefCell, rc::Rc};

    #[gpui_kit::test]
    fn cached_and_installed_choices_emit_placement_browse_and_cancel(cx: &mut TestAppContext) {
        cx.update(crate::shell::init);
        let requests = Rc::new(RefCell::new(Vec::new()));
        let received = requests.clone();
        let window = cx.open_window(size(px(1000.), px(900.)), move |window, cx| {
            let panel = cx.new(|cx| {
                let mut panel = SymbolPanel::new(window, cx);
                panel.open(
                    vec!["power:GND".into()],
                    vec!["power".into()],
                    true,
                    window,
                    cx,
                );
                panel
            });
            cx.subscribe(&panel, move |_, panel, request: &SymbolRequest, cx| {
                let record = match request {
                    SymbolRequest::Place(id, _, _) => format!("place:{id}"),
                    SymbolRequest::Preview(id, _, _) => format!("preview:{id}"),
                    SymbolRequest::Browse(library, power) => {
                        panel.update(cx, |panel, cx| {
                            panel.set_symbols(vec!["power:VCC".into()], cx)
                        });
                        format!("browse:{library}:{power}")
                    }
                    SymbolRequest::Close => "close".into(),
                };
                received.borrow_mut().push(record);
            })
            .detach();
            Root::new(panel, window, cx)
        });
        cx.run_until_parked();
        for id in [
            gpui_kit::ElementId::from(("cached-symbol", 0usize)),
            gpui_kit::ElementId::from(("symbol-library", 0usize)),
            gpui_kit::ElementId::from(("cached-symbol", 0usize)),
            gpui_kit::ElementId::from("cached-symbols"),
            gpui_kit::ElementId::from("close-symbols"),
        ] {
            cx.update_window(window.into(), |_, window, cx| {
                window.render_frame(cx);
                window.click(id, cx);
            })
            .unwrap();
            cx.run_until_parked();
        }
        assert_eq!(
            *requests.borrow(),
            [
                "place:power:GND",
                "browse:power:true",
                "place:power:VCC",
                "browse::true",
                "close"
            ]
        );
    }
}
