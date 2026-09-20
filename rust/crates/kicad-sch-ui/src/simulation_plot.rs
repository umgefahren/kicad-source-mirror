// SPDX-License-Identifier: GPL-3.0-or-later
//! Numeric simulation traces painted directly by GPUI.
use gpui_kit::component::button::Button;
use gpui_kit::component::{Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, PathBuilder, Window, canvas, div, point, px, rgb};

pub(crate) struct SimulationPlot {
    samples: Vec<[f64; 3]>,
    zoom: f64,
    offset: f64,
    zoom_y: f64,
    offset_y: f64,
    wheel: Vec<u32>,
}
impl SimulationPlot {
    pub(crate) fn new(samples: Vec<[f64; 3]>, wheel: Vec<u32>) -> Self {
        Self {
            samples,
            zoom: 1.,
            offset: 0.,
            zoom_y: 1.,
            offset_y: 0.,
            wheel,
        }
    }
}
impl Render for SimulationPlot {
    fn render(&mut self, _: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let samples = self.samples.clone();
        let (mut xmin, mut xmax, mut ymin, mut ymax) = (
            f64::INFINITY,
            f64::NEG_INFINITY,
            f64::INFINITY,
            f64::NEG_INFINITY,
        );
        for [x, re, im] in &samples {
            if x.is_finite() {
                xmin = xmin.min(*x);
                xmax = xmax.max(*x);
            }
            for y in [re, im] {
                if y.is_finite() {
                    ymin = ymin.min(*y);
                    ymax = ymax.max(*y);
                }
            }
        }
        if !xmin.is_finite() {
            xmin = 0.;
            xmax = 1.;
        }
        if !ymin.is_finite() {
            ymin = 0.;
            ymax = 1.;
        }
        let span = (xmax - xmin).max(f64::EPSILON * xmax.abs().max(1.));
        let center = (xmin + xmax) / 2. + self.offset * span;
        xmin = center - span / self.zoom / 2.;
        xmax = center + span / self.zoom / 2.;
        let yspan = (ymax - ymin).max(f64::EPSILON * ymax.abs().max(1.));
        let ycenter = (ymax + ymin) / 2. + self.offset_y * yspan;
        ymin = ycenter - yspan * 0.55 / self.zoom_y;
        ymax = ycenter + yspan * 0.55 / self.zoom_y;
        div()
            .size_full()
            .v_flex()
            .min_h(px(0.))
            .id("simulation-plot-traces")
            .v_flex()
            .gap_1()
            .on_scroll_wheel(
                cx.listener(|this, event: &gpui_kit::ScrollWheelEvent, _, cx| {
                    let delta = event.delta.pixel_delta(px(20.));
                    let dx = f32::from(delta.x) as f64;
                    let dy = f32::from(delta.y) as f64;
                    let index = if dx.abs() > dy.abs() {
                        4
                    } else if event.modifiers.control {
                        1
                    } else if event.modifiers.shift {
                        2
                    } else if event.modifiers.alt {
                        3
                    } else {
                        0
                    };
                    let amount = if index == 4 { dx } else { dy };
                    let scale = (amount * 0.01).exp().clamp(0.5, 2.);
                    match this.wheel.get(index).copied().unwrap_or(4) {
                        1 => this.offset += amount * 0.002 / this.zoom,
                        2 => this.offset -= amount * 0.002 / this.zoom,
                        3 => this.offset_y += amount * 0.002 / this.zoom_y,
                        4 => {
                            this.zoom = (this.zoom * scale).clamp(1., 1e6);
                            this.zoom_y = (this.zoom_y * scale).clamp(1., 1e6);
                        }
                        5 => this.zoom = (this.zoom * scale).clamp(1., 1e6),
                        6 => this.zoom_y = (this.zoom_y * scale).clamp(1., 1e6),
                        _ => {}
                    }
                    cx.stop_propagation();
                    cx.notify();
                }),
            )
            .child(
                div()
                    .h_flex()
                    .gap_2()
                    .child("Real (blue) · Imaginary (orange)")
                    .children(
                        [
                            ("zoom-in", "Zoom In", 1.5, 0.),
                            ("zoom-out", "Zoom Out", 1. / 1.5, 0.),
                            ("pan-left", "←", 1., -0.2),
                            ("pan-right", "→", 1., 0.2),
                        ]
                        .into_iter()
                        .map(|(id, label, scale, pan)| {
                            Button::new(id).label(label).small().on_click(cx.listener(
                                move |this, _, _, cx| {
                                    this.zoom = (this.zoom * scale).clamp(1., 1e6);
                                    this.offset += pan / this.zoom;
                                    cx.notify();
                                },
                            ))
                        }),
                    )
                    .child(
                        Button::new("plot-fit")
                            .label("Fit")
                            .small()
                            .on_click(cx.listener(|this, _, _, cx| {
                                this.zoom = 1.;
                                this.offset = 0.;
                                this.offset_y = 0.;
                                this.zoom_y = 1.;
                                cx.notify();
                            })),
                    ),
            )
            .child(div().text_xs().child(format!(
                "X: {xmin:.5e} … {xmax:.5e}    Y: {ymin:.5e} … {ymax:.5e}"
            )))
            .child(
                canvas(
                    |_, _, _| (),
                    move |bounds, _, window, _| {
                        let width = f32::from(bounds.size.width);
                        let height = f32::from(bounds.size.height);
                        let mut grid = PathBuilder::stroke(px(0.5));
                        for i in 0..=10 {
                            let f = i as f32 / 10.;
                            grid.move_to(bounds.origin + point(px(f * width), px(0.)));
                            grid.line_to(bounds.origin + point(px(f * width), px(height)));
                            grid.move_to(bounds.origin + point(px(0.), px(f * height)));
                            grid.line_to(bounds.origin + point(px(width), px(f * height)));
                        }
                        if let Ok(grid) = grid.build() {
                            window.paint_path(grid, rgb(0x858585));
                        }
                        for (column, color) in [(1, rgb(0x3694ed)), (2, rgb(0xef973e))] {
                            let mut path = PathBuilder::stroke(px(1.5));
                            let mut connected = false;
                            for sample in &samples {
                                let x = (sample[0] - xmin) / (xmax - xmin);
                                let y = (sample[column] - ymin) / (ymax - ymin);
                                if !x.is_finite()
                                    || !y.is_finite()
                                    || (!(0.0..=1.0).contains(&x) || !(0.0..=1.0).contains(&y))
                                {
                                    connected = false;
                                    continue;
                                }
                                let p = bounds.origin
                                    + point(px(x as f32 * width), px((1. - y) as f32 * height));
                                if samples.len() == 1 {
                                    path.move_to(p - point(px(3.), px(0.)));
                                    path.line_to(p + point(px(3.), px(0.)));
                                    path.move_to(p - point(px(0.), px(3.)));
                                    path.line_to(p + point(px(0.), px(3.)));
                                }
                                if connected {
                                    path.line_to(p);
                                } else {
                                    path.move_to(p);
                                    connected = true;
                                }
                            }
                            if let Ok(path) = path.build() {
                                window.paint_path(path, color);
                            }
                        }
                    },
                )
                .w_full()
                .flex_1()
                .min_h(px(0.)),
            )
    }
}
