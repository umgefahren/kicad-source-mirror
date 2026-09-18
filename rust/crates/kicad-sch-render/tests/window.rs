// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! End-to-end tests through a real gpui window, with no GPU.
//!
//! These cover what the pure translation tests cannot: that the element's
//! layout, prepaint and paint phases are legal — gpui asserts on phase in debug
//! builds, so `insert_hitbox` in the wrong place is a panic, not a warning —
//! and that the primitives actually reach a `Scene`.
//!
//! Only quads can be read back: `Window::painted_quads` exists but there is no
//! `painted_paths`, and `render_to_image` needs a `PlatformHeadlessRenderer`,
//! which gpui only supplies on macOS. The grid is therefore the thing asserted
//! on here, since it is the part of a schematic that becomes quads.

use std::cell::RefCell;
use std::rc::Rc;

use gpui::{
    div, AnyWindowHandle, AppContext, Context, IntoElement, ParentElement, Render, Styled,
    TestAppContext, Window,
};
use kicad_gal::{Color, GridStyle, Stream, StreamBuilder};
use kicad_sch_render::{SchematicCanvas, SchematicRenderer};

/// One millimetre in KiCad internal units.
const MM: f64 = 1_000_000.0;

struct Host {
    renderer: Rc<RefCell<SchematicRenderer>>,
}

impl Render for Host {
    fn render(&mut self, _: &mut Window, _: &mut Context<Self>) -> impl IntoElement {
        div()
            .size_full()
            .child(SchematicCanvas::new("schematic", self.renderer.clone()).size_full())
    }
}

/// A sheet with a dotted grid, a background and a handful of symbol groups.
fn demo_stream() -> Stream {
    let mut b = StreamBuilder::new();
    for i in 0..4u32 {
        let x = i as f64 * 20.0 * MM;
        b.group(i, 1, |g| {
            g.set_stroke_color(Color::new(0.5, 0.0, 0.0, 1.0));
            g.set_line_width(0.15 * MM);
            g.rectangle([x, 0.0], [x + 10.0 * MM, 6.0 * MM]);
            g.segment([x - 2.54 * MM, 2.0 * MM], [x, 2.0 * MM], 0.15 * MM);
        });
    }
    b.begin_frame(1024, 768);
    b.clear_screen(Color::new(1.0, 1.0, 1.0, 1.0));
    b.grid(
        [0.0, 0.0],
        [2.54 * MM, 2.54 * MM],
        1.0,
        GridStyle::Dots,
        Color::new(0.75, 0.75, 0.75, 1.0),
    );
    for i in 0..4u32 {
        b.draw_group(i);
    }
    b.end_frame();
    b.finish().expect("the demo stream is valid")
}

fn host(cx: &mut TestAppContext, stream: Stream) -> (Rc<RefCell<SchematicRenderer>>, AnyWindowHandle) {
    let renderer = Rc::new(RefCell::new(SchematicRenderer::new()));
    renderer.borrow_mut().set_stream(stream);
    let handle = cx.add_window({
        let renderer = renderer.clone();
        move |_, _| Host { renderer }
    });
    (renderer, handle.into())
}

#[gpui::test]
fn the_canvas_paints_a_frame_without_tripping_a_phase_assertion(cx: &mut TestAppContext) {
    // gpui's `insert_hitbox` is prepaint-only and its `paint_*` calls are
    // paint-only, with debug assertions on both. Drawing a frame at all is the
    // assertion.
    let (renderer, handle) = host(cx, demo_stream());
    cx.update_window(handle, |_, window, cx| {
        renderer.borrow_mut().zoom_to_fit(20.0);
        let _ = window.draw(cx);
    })
    .expect("the window draws");

    let stats = renderer.borrow().last_stats();
    assert_eq!(stats.groups_drawn, 4, "not every group reached the scene");
    assert!(stats.paths > 0, "no paths were produced");
    assert!(stats.vertices > 0, "no geometry was tessellated");
}

#[gpui::test]
fn the_grid_and_background_reach_the_scene_as_quads(cx: &mut TestAppContext) {
    let (renderer, handle) = host(cx, demo_stream());
    let quads = cx
        .update_window(handle, |_, window, cx| {
            renderer.borrow_mut().zoom_to_fit(20.0);
            let _ = window.draw(cx);
            window.painted_quads()
        })
        .expect("the window draws");

    // The clear-screen quad plus one per visible grid intersection.
    assert!(quads.len() > 10, "only {} quads were painted", quads.len());

    // The grid colour survives the crossing into gpui's own colour type.
    let grid: gpui::Hsla = gpui::Rgba {
        r: 0.75,
        g: 0.75,
        b: 0.75,
        a: 1.0,
    }
    .into();
    let grid_quads = quads
        .iter()
        .filter_map(|q| q.background.as_solid())
        .filter(|c| (c.l - grid.l).abs() < 0.02 && c.a > 0.99)
        .count();
    assert!(grid_quads > 5, "only {grid_quads} grid marks were painted");

    // Quad bounds reach the scene in `ScaledPixels`, already multiplied by the
    // window's scale factor. Nothing should have gone non-finite on the way.
    for q in quads.iter() {
        let w = q.bounds.size.width.as_f32();
        let h = q.bounds.size.height.as_f32();
        assert!(w.is_finite() && w >= 0.0, "a quad had a bad width: {w}");
        assert!(h.is_finite() && h >= 0.0, "a quad had a bad height: {h}");
    }
}

#[gpui::test]
fn a_second_frame_over_an_unchanged_document_tessellates_nothing(cx: &mut TestAppContext) {
    // The interactive case: gpui redraws on any invalidation, and a redraw that
    // re-tessellated the sheet would not hold a frame budget.
    let (renderer, handle) = host(cx, demo_stream());
    cx.update_window(handle, |_, window, cx| {
        renderer.borrow_mut().zoom_to_fit(20.0);
        let _ = window.draw(cx);
    })
    .expect("the window draws");
    assert!(renderer.borrow().last_stats().cache_misses > 0);

    cx.update_window(handle, |_, window, cx| {
        renderer.borrow_mut().pan(5.0, 3.0);
        window.refresh();
        let _ = window.draw(cx);
    })
    .expect("the window draws again");

    let stats = renderer.borrow().last_stats();
    assert_eq!(stats.cache_misses, 0, "a redraw re-tessellated the sheet");
    assert_eq!(stats.cache_hits, 4);
}

#[gpui::test]
fn an_empty_renderer_still_draws(cx: &mut TestAppContext) {
    let renderer = Rc::new(RefCell::new(SchematicRenderer::new()));
    let handle = cx.add_window({
        let renderer = renderer.clone();
        move |_, _| Host { renderer }
    });
    cx.update_window(handle.into(), |_, window, cx| {
        let _ = window.draw(cx);
    })
    .expect("an empty canvas draws");
    assert_eq!(renderer.borrow().last_stats().groups_drawn, 0);
}

#[gpui::test]
fn the_canvas_takes_its_viewport_from_the_element_bounds(cx: &mut TestAppContext) {
    let (renderer, handle) = host(cx, demo_stream());
    cx.update_window(handle, |_, window, cx| {
        let _ = window.draw(cx);
    })
    .expect("the window draws");

    let viewport = renderer.borrow().camera().viewport();
    let window_size = cx
        .update_window(handle, |_, window, _| window.viewport_size())
        .expect("the window has a size");
    assert!(
        (viewport[0] - window_size.width.to_f64()).abs() < 1.0,
        "canvas viewport {viewport:?} does not match the window {window_size:?}"
    );
    assert!(viewport[1] > 0.0);
}
