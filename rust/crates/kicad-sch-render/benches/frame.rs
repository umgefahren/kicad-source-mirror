// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Frame-preparation benchmarks over a synthetic large schematic.
//!
//! What is measured here is the CPU half of a frame: decoding the stream,
//! culling, tessellating with lyon, and placing the finished vertices. That is
//! the part this crate owns. Rasterisation belongs to gpui's wgpu renderer and
//! is not measured, because the only GPU available here is a software one and a
//! frame time from it would say nothing about real hardware.
//!
//! The two numbers that matter are the cold pass, which is what a document load
//! or a large zoom costs, and the warm pan, which is what every frame of an
//! interactive session costs. The warm pan is the one that has to fit inside
//! the 8.3 ms budget, and it is the one the cache exists to make cheap.

use criterion::{criterion_group, criterion_main, Criterion};
use kicad_gal::{Color, Stream, StreamBuilder};
use kicad_sch_render::{tessellate, translate_group, SchematicRenderer};

/// One millimetre in KiCad internal units.
const MM: f64 = 1_000_000.0;

/// Primitives recorded per symbol, matching the body below.
const PRIMS_PER_SYMBOL: usize = 20;

/// Build a sheet of `symbols` symbol-like groups laid out in a grid, plus the
/// wires between them.
///
/// Each group holds a body rectangle, a fill, eight pin lines with their
/// junction dots, and a two-segment label outline — about twenty primitives,
/// which is the right order for a real schematic symbol.
fn synthetic_sheet(symbols: u32) -> Stream {
    let mut b = StreamBuilder::new();
    let cols = (symbols as f64).sqrt().ceil() as u32;
    let pitch = 30.0 * MM;

    for i in 0..symbols {
        let ox = (i % cols) as f64 * pitch;
        let oy = (i / cols) as f64 * pitch;
        b.group(i, 1, |g| {
            g.set_line_width(0.15 * MM);
            g.set_is_fill(true);
            g.set_is_stroke(true);
            g.set_fill_color(Color::new(1.0, 1.0, 0.76, 1.0));
            g.set_stroke_color(Color::new(0.5, 0.0, 0.0, 1.0));
            g.set_layer_depth(10.0);
            g.rectangle([ox, oy], [ox + 12.0 * MM, oy + 10.0 * MM]);

            g.set_is_fill(false);
            g.set_layer_depth(0.0);
            for p in 0..4 {
                let y = oy + 2.0 * MM + p as f64 * 2.0 * MM;
                g.segment([ox - 2.54 * MM, y], [ox, y], 0.15 * MM);
                g.segment([ox + 12.0 * MM, y], [ox + 14.54 * MM, y], 0.15 * MM);
            }

            g.set_is_fill(true);
            g.set_fill_color(Color::new(0.0, 0.5, 0.0, 1.0));
            for p in 0..4 {
                let y = oy + 2.0 * MM + p as f64 * 2.0 * MM;
                g.circle([ox - 2.54 * MM, y], 0.4 * MM);
            }

            // A reference designator, lowered to geometry by the recorder the
            // way a real glyph run reaches the stream.
            g.set_glyph(true);
            g.set_is_fill(true);
            g.set_is_stroke(false);
            g.set_fill_color(Color::new(0.0, 0.0, 0.5, 1.0));
            g.polygon(&[
                [ox + 1.0 * MM, oy + 11.0 * MM],
                [ox + 2.0 * MM, oy + 11.0 * MM],
                [ox + 2.0 * MM, oy + 12.5 * MM],
                [ox + 1.0 * MM, oy + 12.5 * MM],
            ]);
            g.polygon(&[
                [ox + 2.5 * MM, oy + 11.0 * MM],
                [ox + 3.5 * MM, oy + 11.0 * MM],
                [ox + 3.5 * MM, oy + 12.5 * MM],
                [ox + 2.5 * MM, oy + 12.5 * MM],
            ]);
            g.set_glyph(false);

            // The wire leaving this symbol, as its own retained geometry.
            g.set_is_fill(false);
            g.set_is_stroke(true);
            g.set_stroke_color(Color::new(0.0, 0.5, 0.0, 1.0));
            g.segment_chain(
                &[
                    [ox + 14.54 * MM, oy + 2.0 * MM],
                    [ox + 20.0 * MM, oy + 2.0 * MM],
                    [ox + 20.0 * MM, oy + 20.0 * MM],
                ],
                0.15 * MM,
            );
        });
    }

    b.begin_frame(1920, 1080);
    b.clear_screen(Color::new(1.0, 1.0, 1.0, 1.0));
    b.grid(
        [0.0, 0.0],
        [1.27 * MM, 1.27 * MM],
        1.0,
        kicad_gal::GridStyle::Dots,
        Color::new(0.8, 0.8, 0.8, 1.0),
    );
    for i in 0..symbols {
        b.draw_group(i);
    }
    b.end_frame();

    b.finish().expect("the synthetic sheet is a valid stream")
}

fn renderer_for(stream: Stream, viewport: [f64; 2]) -> SchematicRenderer {
    let mut r = SchematicRenderer::new();
    r.set_stream(stream);
    r.set_viewport(viewport);
    r.zoom_to_fit(20.0);
    r
}

fn bench(c: &mut Criterion) {
    // 5 000 symbols x ~20 primitives is about 100 000 primitives, the size the
    // renderer is meant to hold 120 Hz on.
    const SYMBOLS: u32 = 5_000;
    let viewport = [1920.0, 1080.0];
    let stream = synthetic_sheet(SYMBOLS);

    // Report the shape of the workload before timing it, so that a frame time
    // is never read without the counts that explain it.
    {
        let mut r = renderer_for(stream.clone(), viewport);
        let f = r.prepare([0.0, 0.0]);
        eprintln!(
            "synthetic sheet: {SYMBOLS} groups, ~{} primitives recorded",
            SYMBOLS as usize * PRIMS_PER_SYMBOL
        );
        eprintln!(
            "zoomed to fit: {} groups drawn, {} culled, {} gpui paths, {} quads, {} vertices",
            f.stats.groups_drawn,
            f.stats.groups_culled,
            f.stats.paths,
            f.stats.quads,
            f.stats.vertices
        );
        eprintln!(
            "frame body: {} commands, {} batches, {} points",
            f.stats.frame_translation.commands,
            f.stats.frame_translation.batches,
            f.stats.frame_translation.points
        );
        let view = stream.view();
        let g = translate_group(&view, 0, r.camera().lod_scale()).expect("group 0 exists");
        eprintln!(
            "one symbol: {} primitives, {} batches, {} points",
            g.stats.primitives, g.stats.batches, g.stats.points
        );
    }

    let mut group = c.benchmark_group("frame");
    // A full-sheet cold pass is slow enough that criterion's default sample
    // count would take minutes on a shared machine.
    group.sample_size(10);

    // Cold: every group tessellated from scratch. This is what a document load
    // or a zoom across several levels of detail costs.
    group.bench_function("prepare_cold_5k_symbols", |b| {
        b.iter_batched(
            || renderer_for(stream.clone(), viewport),
            |mut r| std::hint::black_box(r.prepare([0.0, 0.0]).stats),
            criterion::BatchSize::LargeInput,
        )
    });

    // Warm pan: the steady state. Nothing is re-tessellated; the cost is
    // walking the frame body, culling, and placing cached vertices. This is the
    // number that has to fit in 8.3 ms.
    {
        let mut warm = renderer_for(stream.clone(), viewport);
        warm.prepare([0.0, 0.0]);
        group.bench_function("prepare_warm_pan_5k_symbols", |b| {
            b.iter(|| {
                warm.pan(1.0, 0.0);
                std::hint::black_box(warm.prepare([0.0, 0.0]).stats)
            })
        });
    }

    // Zoomed in, which is how a schematic is actually edited: most of the sheet
    // is culled and only a screenful is drawn.
    {
        let mut zoomed = renderer_for(stream.clone(), viewport);
        zoomed.zoom_to_point(20.0, [960.0, 540.0]);
        zoomed.prepare([0.0, 0.0]);
        let stats = zoomed.last_stats();
        eprintln!(
            "zoomed in: {} groups drawn, {} culled",
            stats.groups_drawn, stats.groups_culled
        );
        group.bench_function("prepare_warm_pan_zoomed_in", |b| {
            b.iter(|| {
                zoomed.pan(1.0, 0.0);
                std::hint::black_box(zoomed.prepare([0.0, 0.0]).stats)
            })
        });
    }

    group.finish();

    // The two halves of a cold pass, separately, so that a regression can be
    // attributed rather than guessed at.
    let mut parts = c.benchmark_group("symbol");
    let view = stream.view();
    let scale = 1.0e-4;
    parts.bench_function("translate_one_symbol", |b| {
        b.iter(|| std::hint::black_box(translate_group(&view, 0, scale)))
    });
    let geometry = translate_group(&view, 0, scale)
        .expect("group 0 exists")
        .geometry;
    parts.bench_function("tessellate_one_symbol", |b| {
        b.iter(|| std::hint::black_box(tessellate(&geometry)))
    });
    parts.finish();
}

criterion_group!(benches, bench);
criterion_main!(benches);
