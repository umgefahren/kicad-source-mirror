// SPDX-License-Identifier: GPL-3.0-or-later
//! Electrical rules results presented by GPUI.
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, EventEmitter, Window, div, px};

/// Owned native ERC result and navigation target.
#[derive(Clone, Debug, PartialEq)]
pub struct ErcViolation {
    /// Stable identity of the live ERC marker.
    pub marker_id: String,
    /// Description from the rules engine.
    pub message: String,
    /// KiCad severity bitmask.
    pub severity: u32,
    /// Sheet hierarchy index.
    pub sheet_index: u32,
    /// Horizontal schematic coordinate.
    pub x: f64,
    /// Vertical schematic coordinate.
    pub y: f64,
}
impl ErcViolation {
    fn severity_label(&self) -> &'static str {
        match self.severity {
            0x20 => "Error",
            0x10 => "Warning",
            0x04 => "Excluded",
            _ => "Info",
        }
    }
}
pub(crate) enum ErcRequest {
    Run,
    Settings,
    Exclude(String, bool),
    Navigate(ErcViolation),
    Close,
}
pub(crate) struct ErcPanel {
    results: Vec<ErcViolation>,
    status: String,
    show_errors: bool,
    show_warnings: bool,
    show_excluded: bool,
}
impl EventEmitter<ErcRequest> for ErcPanel {}
impl ErcPanel {
    pub(crate) fn new() -> Self {
        Self {
            results: Vec::new(),
            show_errors: true,
            show_warnings: true,
            show_excluded: true,
            status: "Run electrical rules checks for this schematic.".into(),
        }
    }
    pub(crate) fn set_results(
        &mut self,
        results: Result<Vec<ErcViolation>, String>,
        cx: &mut Context<Self>,
    ) {
        match results {
            Ok(results) => {
                let errors = results.iter().filter(|v| v.severity == 0x20).count();
                let warnings = results.iter().filter(|v| v.severity == 0x10).count();
                self.status = format!(
                    "{} violations · {errors} errors · {warnings} warnings",
                    results.len()
                );
                self.results = results;
            }
            Err(error) => {
                self.results.clear();
                self.status = error;
            }
        }
        cx.notify();
    }
}
impl Render for ErcPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div()
            .id("erc-panel")
            .size_full()
            .min_h(px(0.))
            .overflow_hidden()
            .v_flex()
            .gap_2()
            .p_3()
            .bg(cx.theme().background)
            .border_b_1()
            .border_color(cx.theme().border)
            .child(
                div()
                    .h_flex()
                    .flex_wrap()
                    .flex_shrink_0()
                    .gap_2()
                    .items_center()
                    .child(div().flex_1().child("Electrical Rules Checker"))
                    .child(
                        Button::new("erc-run")
                            .label("Run ERC")
                            .primary()
                            .small()
                            .on_click(cx.listener(|_, _, _, cx| cx.emit(ErcRequest::Run))),
                    )
                    .child(
                        Button::new("erc-settings")
                            .label("Settings")
                            .small()
                            .on_click(cx.listener(|_, _, _, cx| cx.emit(ErcRequest::Settings))),
                    )
                    .child(
                        Button::new("erc-copy-report")
                            .label("Copy Report")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| {
                                let report = this
                                    .results
                                    .iter()
                                    .map(|v| {
                                        format!(
                                            "{} · Sheet {} · {}",
                                            v.severity_label(),
                                            v.sheet_index + 1,
                                            v.message
                                        )
                                    })
                                    .collect::<Vec<_>>()
                                    .join("\n");
                                cx.write_to_clipboard(gpui_kit::ClipboardItem::new_string(report));
                                this.status = "ERC report copied".into();
                                cx.notify();
                            })),
                    )
                    .child(
                        Button::new("erc-close")
                            .label("Close")
                            .small()
                            .on_click(cx.listener(|_, _, _, cx| cx.emit(ErcRequest::Close))),
                    ),
            )
            .child(
                div()
                    .h_flex()
                    .gap_3()
                    .child(
                        Checkbox::new("erc-errors")
                            .label("Errors")
                            .checked(self.show_errors)
                            .on_click(cx.listener(|this, checked, _, cx| {
                                this.show_errors = *checked;
                                cx.notify();
                            })),
                    )
                    .child(
                        Checkbox::new("erc-warnings")
                            .label("Warnings")
                            .checked(self.show_warnings)
                            .on_click(cx.listener(|this, checked, _, cx| {
                                this.show_warnings = *checked;
                                cx.notify();
                            })),
                    )
                    .child(
                        Checkbox::new("erc-excluded")
                            .label("Excluded")
                            .checked(self.show_excluded)
                            .on_click(cx.listener(|this, checked, _, cx| {
                                this.show_excluded = *checked;
                                cx.notify();
                            })),
                    ),
            )
            .child(div().text_xs().child(self.status.clone()))
            .child(
                div()
                    .id("erc-results")
                    .v_flex()
                    .flex_1()
                    .min_h(px(0.))
                    .overflow_y_scroll()
                    .children(
                        self.results
                            .iter()
                            .enumerate()
                            .filter(|(_, result)| match result.severity {
                                0x20 => self.show_errors,
                                0x10 => self.show_warnings,
                                0x04 => self.show_excluded,
                                _ => true,
                            })
                            .map(|(index, result)| {
                                let target = result.clone();
                                let label = format!(
                                    "{} · Sheet {} · {}",
                                    result.severity_label(),
                                    result.sheet_index + 1,
                                    result.message
                                );
                                let marker_id = result.marker_id.clone();
                                let excluded = result.severity == 0x04;
                                div()
                                    .h_flex()
                                    .flex_shrink_0()
                                    .gap_1()
                                    .child(
                                        Button::new(("erc-result", index))
                                            .ghost()
                                            .small()
                                            .flex_1()
                                            .min_w(px(0.))
                                            .h_auto()
                                            .min_h(px(30.))
                                            .py_2()
                                            .accessibility_label(label.clone())
                                            .tooltip(label.clone())
                                            .child(
                                                div()
                                                    .w_full()
                                                    .whitespace_normal()
                                                    .text_left()
                                                    .child(label),
                                            )
                                            .on_click(cx.listener(move |_, _, _, cx| {
                                                cx.emit(ErcRequest::Navigate(target.clone()))
                                            })),
                                    )
                                    .child(
                                        Button::new(("erc-exclude", index))
                                            .small()
                                            .label(if excluded { "Restore" } else { "Exclude" })
                                            .on_click(cx.listener(move |_, _, _, cx| {
                                                cx.emit(ErcRequest::Exclude(
                                                    marker_id.clone(),
                                                    !excluded,
                                                ))
                                            })),
                                    )
                            }),
                    ),
            )
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
    fn results_emit_owned_navigation_and_rerun_requests(cx: &mut TestAppContext) {
        cx.update(crate::shell::init);
        let requests = Rc::new(RefCell::new(Vec::new()));
        let received = requests.clone();
        let result = ErcViolation {
            marker_id: "marker".into(),
            message: "Input pin not driven".into(),
            severity: 0x20,
            sheet_index: 2,
            x: 123.,
            y: 456.,
        };
        let target = result.clone();
        let window = cx.open_window(size(px(1000.), px(400.)), move |window, cx| {
            let panel = cx.new(|cx| {
                let mut panel = ErcPanel::new();
                panel.set_results(Ok(vec![target]), cx);
                panel
            });
            cx.subscribe(&panel, move |_, _, request: &ErcRequest, _| {
                received.borrow_mut().push(match request {
                    ErcRequest::Run => ("run", None),
                    ErcRequest::Settings => ("settings", None),
                    ErcRequest::Exclude(_, _) => ("exclude", None),
                    ErcRequest::Navigate(target) => ("navigate", Some(target.clone())),
                    ErcRequest::Close => ("close", None),
                });
            })
            .detach();
            Root::new(panel, window, cx)
        });
        cx.run_until_parked();
        cx.update_window(window.into(), |_, window, cx| {
            window.render_frame(cx);
            window.click(("erc-result", 0usize), cx);
        })
        .unwrap();
        cx.run_until_parked();
        assert_eq!(requests.borrow()[0], ("navigate", Some(result)));
        cx.update_window(window.into(), |_, window, cx| window.click("erc-run", cx))
            .unwrap();
        cx.run_until_parked();
        assert_eq!(requests.borrow()[1], ("run", None));
        cx.update_window(window.into(), |_, window, cx| window.click("erc-close", cx))
            .unwrap();
        cx.run_until_parked();
        assert_eq!(requests.borrow()[2], ("close", None));
    }
}
