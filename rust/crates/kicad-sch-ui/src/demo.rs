// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! A synthetic draw stream, so the shell has a schematic to show before
//! `sch_dump` produces real ones.
//!
//! This is not a schematic model and nothing should grow into one here. It
//! builds a [`kicad_gal::Stream`] with the same builder the C++ producer's
//! output is tested against, so what the shell renders goes through the real
//! decoder, the real translator and the real tessellation cache. The only thing
//! standing in for KiCad is the source of the commands.
//!
//! Each drawable is recorded as its own group at absolute coordinates, which is
//! what `KIGFX::VIEW` does, so the renderer's per-group cache is exercised the
//! way it will be in earnest.
//!
//! Text is drawn as line segments rather than glyphs because the draw stream
//! has no text command: `KIFONT` lowers text to polylines on the C++ side. The
//! stroke font below is a stand-in for that, not a second font system.

use std::path::Path;

use kicad_gal::{Color, Stream, StreamBuilder};

/// Internal units in one millimetre. One eeschema internal unit is 100 nm.
const MM: f64 = crate::grid::IU_PER_MM;

/// Read a recorded draw stream from disk.
///
/// Golden streams will live in `qa/data/draw_streams/`; until then this is how
/// the binary would load one if pointed at a file.
pub fn load_stream(path: &Path) -> std::io::Result<Stream> {
    let file = std::fs::File::open(path)?;
    Stream::read(std::io::BufReader::new(file))
        .map_err(|error| std::io::Error::new(std::io::ErrorKind::InvalidData, error.to_string()))
}

// KiCad's classic schematic colours, which the demo uses so that the shell's
// two canvas palettes can be judged against something recognisable.
const WIRE: Color = Color::new(0.09, 0.72, 0.42, 1.0);
const BUS: Color = Color::new(0.24, 0.53, 0.94, 1.0);
const OUTLINE: Color = Color::new(0.80, 0.42, 0.33, 1.0);
const PIN: Color = Color::new(0.87, 0.36, 0.33, 1.0);
const FIELD: Color = Color::new(0.35, 0.80, 0.76, 1.0);
const LABEL: Color = Color::new(0.58, 0.76, 1.00, 1.0);
const FRAME: Color = Color::new(0.35, 0.40, 0.47, 1.0);
const NOTE: Color = Color::new(0.72, 0.77, 0.84, 1.0);

/// A small schematic: a sheet frame, a regulator, a microcontroller, some
/// passives, a bus and enough labelling to judge the rendering by.
pub fn demo_stream() -> Stream {
    let mut b = StreamBuilder::new();
    let mut next_id = 0u32;
    let mut id = || {
        next_id += 1;
        next_id
    };
    let mut drawn: Vec<u32> = Vec::new();

    // --- sheet frame and title block -------------------------------------
    let frame = id();
    b.group(frame, 1, |g| {
        g.set_is_stroke(true);
        g.set_is_fill(false);
        g.set_stroke_color(FRAME);
        g.set_line_width(0.25 * MM);
        g.polyline_closed(&[
            [5.0 * MM, 5.0 * MM],
            [292.0 * MM, 5.0 * MM],
            [292.0 * MM, 205.0 * MM],
            [5.0 * MM, 205.0 * MM],
        ]);
        g.polyline_closed(&[
            [180.0 * MM, 172.0 * MM],
            [292.0 * MM, 172.0 * MM],
            [292.0 * MM, 205.0 * MM],
            [180.0 * MM, 205.0 * MM],
        ]);
        g.line([180.0 * MM, 184.0 * MM], [292.0 * MM, 184.0 * MM]);
        g.line([180.0 * MM, 194.0 * MM], [292.0 * MM, 194.0 * MM]);
    });
    drawn.push(frame);

    let title = id();
    b.group(title, 1, |g| {
        stroke_text(g, [184.0 * MM, 176.0 * MM], 4.2 * MM, "SENSOR FRONT END", NOTE);
        stroke_text(g, [184.0 * MM, 187.0 * MM], 2.8 * MM, "SHEET 1/1   REV A", FIELD);
        stroke_text(g, [184.0 * MM, 197.0 * MM], 2.8 * MM, "KICAD GPUI SHELL", FIELD);
    });
    drawn.push(title);

    // --- U1: the microcontroller ------------------------------------------
    let u1_body = [[118.0 * MM, 58.0 * MM], [158.0 * MM, 108.0 * MM]];
    let u1 = id();
    b.group(u1, 1, |g| {
        g.set_stroke_color(OUTLINE);
        g.set_line_width(0.3 * MM);
        g.polyline_closed(&[
            u1_body[0],
            [u1_body[1][0], u1_body[0][1]],
            u1_body[1],
            [u1_body[0][0], u1_body[1][1]],
        ]);
        // The pin-1 notch, which is what makes a rectangle read as a part.
        g.arc(
            [(118.0 + 20.0) * MM, 58.0 * MM],
            3.0 * MM,
            0.0,
            std::f64::consts::PI,
        );
        stroke_text(g, [118.0 * MM, 52.0 * MM], 3.6 * MM, "U1", FIELD);
        stroke_text(g, [131.0 * MM, 52.0 * MM], 3.6 * MM, "MCU48", FIELD);
    });
    drawn.push(u1);

    for (index, name) in ["VDD", "RST", "SDA", "SCL"].iter().enumerate() {
        let y = (66.0 + index as f64 * 10.0) * MM;
        let pin = id();
        let name = *name;
        b.group(pin, 1, |g| {
            g.set_stroke_color(PIN);
            g.set_line_width(0.25 * MM);
            g.segment([110.5 * MM, y], [118.0 * MM, y], 0.25 * MM);
            g.circle([110.5 * MM, y], 0.5 * MM);
            stroke_text(g, [120.0 * MM, y - 1.4 * MM], 2.6 * MM, name, PIN);
        });
        drawn.push(pin);
    }
    for (index, name) in ["D0", "D1", "D2", "GND"].iter().enumerate() {
        let y = (66.0 + index as f64 * 10.0) * MM;
        let pin = id();
        let name = *name;
        b.group(pin, 1, |g| {
            g.set_stroke_color(PIN);
            g.set_line_width(0.25 * MM);
            g.segment([158.0 * MM, y], [165.5 * MM, y], 0.25 * MM);
            g.circle([165.5 * MM, y], 0.5 * MM);
            stroke_text(g, [(158.0 - 9.0) * MM, y - 1.4 * MM], 2.6 * MM, name, PIN);
        });
        drawn.push(pin);
    }

    // --- R1 and R2: the pull-ups -------------------------------------------
    for (index, (x, reference, value)) in [
        (78.0_f64, "R1", "4k7"),
        (90.0_f64, "R2", "4k7"),
    ]
    .iter()
    .enumerate()
    {
        let _ = index;
        let g_id = id();
        let (x, reference, value) = (*x, *reference, *value);
        b.group(g_id, 1, |g| {
            g.set_stroke_color(OUTLINE);
            g.set_line_width(0.3 * MM);
            g.polyline_closed(&[
                [(x - 2.0) * MM, 56.0 * MM],
                [(x + 2.0) * MM, 56.0 * MM],
                [(x + 2.0) * MM, 68.0 * MM],
                [(x - 2.0) * MM, 68.0 * MM],
            ]);
            g.set_stroke_color(PIN);
            g.segment([x * MM, 48.0 * MM], [x * MM, 56.0 * MM], 0.25 * MM);
            g.segment([x * MM, 68.0 * MM], [x * MM, 76.0 * MM], 0.25 * MM);
            stroke_text(g, [(x + 4.0) * MM, 57.0 * MM], 3.0 * MM, reference, FIELD);
            stroke_text(g, [(x + 4.0) * MM, 62.0 * MM], 3.0 * MM, value, FIELD);
        });
        drawn.push(g_id);
    }

    // --- C1: the decoupling capacitor --------------------------------------
    let c1 = id();
    b.group(c1, 1, |g| {
        g.set_stroke_color(OUTLINE);
        g.set_line_width(0.45 * MM);
        g.segment([26.0 * MM, 60.0 * MM], [38.0 * MM, 60.0 * MM], 0.45 * MM);
        g.segment([26.0 * MM, 64.0 * MM], [38.0 * MM, 64.0 * MM], 0.45 * MM);
        g.set_stroke_color(PIN);
        g.segment([32.0 * MM, 52.0 * MM], [32.0 * MM, 60.0 * MM], 0.25 * MM);
        g.segment([32.0 * MM, 64.0 * MM], [32.0 * MM, 72.0 * MM], 0.25 * MM);
        stroke_text(g, [40.0 * MM, 58.0 * MM], 3.0 * MM, "C1", FIELD);
        stroke_text(g, [40.0 * MM, 63.0 * MM], 3.0 * MM, "100N", FIELD);
    });
    drawn.push(c1);

    // --- the +3V3 rail ------------------------------------------------------
    let rail = id();
    b.group(rail, 1, |g| {
        g.set_stroke_color(WIRE);
        g.set_line_width(0.3 * MM);
        g.segment_chain(
            &[
                [30.0 * MM, 48.0 * MM],
                [110.5 * MM, 48.0 * MM],
                [110.5 * MM, 66.0 * MM],
            ],
            0.3 * MM,
        );
        g.segment([32.0 * MM, 48.0 * MM], [32.0 * MM, 52.0 * MM], 0.3 * MM);
        g.set_is_fill(true);
        g.set_fill_color(WIRE);
        for x in [32.0_f64, 78.0, 90.0] {
            g.circle([x * MM, 48.0 * MM], 0.65 * MM);
        }
        g.set_is_fill(false);
        // The power-port arrow.
        g.set_stroke_color(LABEL);
        g.segment_chain(
            &[
                [27.0 * MM, 51.0 * MM],
                [30.0 * MM, 45.0 * MM],
                [33.0 * MM, 51.0 * MM],
            ],
            0.3 * MM,
        );
        g.segment([30.0 * MM, 45.0 * MM], [30.0 * MM, 48.0 * MM], 0.3 * MM);
        stroke_text(g, [24.0 * MM, 38.0 * MM], 3.2 * MM, "+3V3", LABEL);
    });
    drawn.push(rail);

    // --- ground symbols ------------------------------------------------------
    for (x, y) in [(32.0_f64, 72.0_f64), (165.5, 96.0), (78.0, 76.0), (90.0, 76.0)] {
        let gnd = id();
        b.group(gnd, 1, |g| {
            g.set_stroke_color(WIRE);
            g.set_line_width(0.35 * MM);
            g.segment([(x - 3.5) * MM, y * MM], [(x + 3.5) * MM, y * MM], 0.35 * MM);
            g.segment(
                [(x - 2.0) * MM, (y + 1.6) * MM],
                [(x + 2.0) * MM, (y + 1.6) * MM],
                0.35 * MM,
            );
            g.segment(
                [(x - 0.8) * MM, (y + 3.2) * MM],
                [(x + 0.8) * MM, (y + 3.2) * MM],
                0.35 * MM,
            );
        });
        drawn.push(gnd);
    }
    let gnd_wire = id();
    b.group(gnd_wire, 1, |g| {
        g.set_stroke_color(WIRE);
        g.set_line_width(0.3 * MM);
        g.segment([158.0 * MM, 96.0 * MM], [165.5 * MM, 96.0 * MM], 0.3 * MM);
    });
    drawn.push(gnd_wire);

    // --- the I2C pair, with hierarchical-label flags --------------------------
    for (index, name) in ["SDA", "SCL"].iter().enumerate() {
        let y = (86.0 + index as f64 * 10.0) * MM;
        let x = 78.0 + index as f64 * 12.0;
        let net = id();
        let name = *name;
        b.group(net, 1, |g| {
            g.set_stroke_color(WIRE);
            g.set_line_width(0.3 * MM);
            g.segment_chain(&[[70.0 * MM, y], [110.5 * MM, y]], 0.3 * MM);
            g.segment([x * MM, 76.0 * MM], [x * MM, y], 0.3 * MM);
            g.set_is_fill(true);
            g.set_fill_color(WIRE);
            g.circle([x * MM, y], 0.65 * MM);
            g.set_is_fill(false);
            g.set_stroke_color(LABEL);
            g.set_line_width(0.2 * MM);
            g.polyline_closed(&[
                [70.0 * MM, y],
                [67.0 * MM, y - 2.6 * MM],
                [56.0 * MM, y - 2.6 * MM],
                [56.0 * MM, y + 2.6 * MM],
                [67.0 * MM, y + 2.6 * MM],
            ]);
            stroke_text(g, [57.5 * MM, y - 1.6 * MM], 2.8 * MM, name, LABEL);
        });
        drawn.push(net);
    }

    // --- the data bus ---------------------------------------------------------
    let bus = id();
    b.group(bus, 1, |g| {
        g.set_stroke_color(BUS);
        g.set_line_width(0.7 * MM);
        g.segment_chain(
            &[
                [188.0 * MM, 52.0 * MM],
                [188.0 * MM, 120.0 * MM],
                [250.0 * MM, 120.0 * MM],
            ],
            0.7 * MM,
        );
        stroke_text(g, [190.0 * MM, 44.0 * MM], 3.2 * MM, "D[0..2]", BUS);
    });
    drawn.push(bus);

    for index in 0..3 {
        let y = (66.0 + index as f64 * 10.0) * MM;
        let entry = id();
        b.group(entry, 1, |g| {
            g.set_stroke_color(WIRE);
            g.set_line_width(0.3 * MM);
            g.segment_chain(
                &[[165.5 * MM, y], [182.0 * MM, y], [188.0 * MM, y + 6.0 * MM]],
                0.3 * MM,
            );
        });
        drawn.push(entry);
    }

    // --- a no-connect flag ------------------------------------------------------
    let nc = id();
    b.group(nc, 1, |g| {
        g.set_stroke_color(PIN);
        g.set_line_width(0.3 * MM);
        g.segment([200.0 * MM, 60.0 * MM], [206.0 * MM, 66.0 * MM], 0.3 * MM);
        g.segment([206.0 * MM, 60.0 * MM], [200.0 * MM, 66.0 * MM], 0.3 * MM);
        stroke_text(g, [210.0 * MM, 61.0 * MM], 2.8 * MM, "NC", PIN);
    });
    drawn.push(nc);

    // --- a note --------------------------------------------------------------
    let note = id();
    b.group(note, 1, |g| {
        stroke_text(
            g,
            [30.0 * MM, 140.0 * MM],
            3.6 * MM,
            "DEMONSTRATION DRAW STREAM",
            NOTE,
        );
        stroke_text(
            g,
            [30.0 * MM, 148.0 * MM],
            3.0 * MM,
            "BUILT WITH KICAD-GAL STREAMBUILDER",
            FIELD,
        );
        stroke_text(
            g,
            [30.0 * MM, 155.0 * MM],
            3.0 * MM,
            "RENDERED BY KICAD-SCH-RENDER",
            FIELD,
        );
    });
    drawn.push(note);

    b.begin_frame(1920, 1080);
    for group in &drawn {
        b.draw_group(*group);
    }
    b.end_frame();

    b.finish()
        .expect("the demonstration stream is built by this module and is well formed")
}

/// Draw a string as line segments, the way `KIFONT` lowers text on the C++ side.
///
/// Uppercase only: the stroke font below is a demonstration prop, and a full
/// case-sensitive font would be a second font system in a crate that should not
/// have one.
fn stroke_text(b: &mut StreamBuilder, origin: [f64; 2], height: f64, text: &str, color: Color) {
    let unit = height / 6.0;
    let advance = 5.0 * unit;
    let width = (height / 12.0).max(0.12 * MM);
    b.set_stroke_color(color);
    b.set_is_fill(false);
    b.set_is_stroke(true);
    b.set_line_width(width);
    let mut pen = origin[0];
    for ch in text.chars() {
        for stroke in glyph(ch) {
            let points: Vec<[f64; 2]> = stroke
                .iter()
                .map(|(x, y)| [pen + *x as f64 * unit, origin[1] + *y as f64 * unit])
                .collect();
            if points.len() >= 2 {
                b.segment_chain(&points, width);
            }
        }
        pen += advance;
    }
}

/// A 4x6 stroke font. Each glyph is a list of polylines on a grid whose origin
/// is the character's top left and whose y grows downwards.
fn glyph(ch: char) -> &'static [&'static [(u8, u8)]] {
    match ch.to_ascii_uppercase() {
        'A' => &[&[(0, 6), (0, 2), (2, 0), (4, 2), (4, 6)], &[(0, 4), (4, 4)]],
        'B' => &[
            &[(0, 0), (0, 6)],
            &[(0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)],
            &[(0, 3), (3, 3), (4, 4), (4, 5), (3, 6), (0, 6)],
        ],
        'C' => &[&[(4, 1), (3, 0), (1, 0), (0, 1), (0, 5), (1, 6), (3, 6), (4, 5)]],
        'D' => &[
            &[(0, 0), (0, 6)],
            &[(0, 0), (3, 0), (4, 1), (4, 5), (3, 6), (0, 6)],
        ],
        'E' => &[&[(4, 0), (0, 0), (0, 6), (4, 6)], &[(0, 3), (3, 3)]],
        'F' => &[&[(4, 0), (0, 0), (0, 6)], &[(0, 3), (3, 3)]],
        'G' => &[&[
            (4, 1),
            (3, 0),
            (1, 0),
            (0, 1),
            (0, 5),
            (1, 6),
            (3, 6),
            (4, 5),
            (4, 3),
            (2, 3),
        ]],
        'H' => &[&[(0, 0), (0, 6)], &[(4, 0), (4, 6)], &[(0, 3), (4, 3)]],
        'I' => &[&[(1, 0), (3, 0)], &[(2, 0), (2, 6)], &[(1, 6), (3, 6)]],
        'J' => &[&[(4, 0), (4, 5), (3, 6), (1, 6), (0, 5)]],
        'K' => &[&[(0, 0), (0, 6)], &[(4, 0), (0, 3), (4, 6)]],
        'L' => &[&[(0, 0), (0, 6), (4, 6)]],
        'M' => &[&[(0, 6), (0, 0), (2, 3), (4, 0), (4, 6)]],
        'N' => &[&[(0, 6), (0, 0), (4, 6), (4, 0)]],
        'O' => &[&[
            (1, 0),
            (3, 0),
            (4, 1),
            (4, 5),
            (3, 6),
            (1, 6),
            (0, 5),
            (0, 1),
            (1, 0),
        ]],
        'P' => &[&[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)]],
        'Q' => &[
            &[
                (1, 0),
                (3, 0),
                (4, 1),
                (4, 5),
                (3, 6),
                (1, 6),
                (0, 5),
                (0, 1),
                (1, 0),
            ],
            &[(2, 4), (4, 6)],
        ],
        'R' => &[
            &[(0, 6), (0, 0), (3, 0), (4, 1), (4, 2), (3, 3), (0, 3)],
            &[(2, 3), (4, 6)],
        ],
        'S' => &[&[
            (4, 1),
            (3, 0),
            (1, 0),
            (0, 1),
            (0, 2),
            (1, 3),
            (3, 3),
            (4, 4),
            (4, 5),
            (3, 6),
            (1, 6),
            (0, 5),
        ]],
        'T' => &[&[(0, 0), (4, 0)], &[(2, 0), (2, 6)]],
        'U' => &[&[(0, 0), (0, 5), (1, 6), (3, 6), (4, 5), (4, 0)]],
        'V' => &[&[(0, 0), (2, 6), (4, 0)]],
        'W' => &[&[(0, 0), (1, 6), (2, 3), (3, 6), (4, 0)]],
        'X' => &[&[(0, 0), (4, 6)], &[(4, 0), (0, 6)]],
        'Y' => &[&[(0, 0), (2, 3), (4, 0)], &[(2, 3), (2, 6)]],
        'Z' => &[&[(0, 0), (4, 0), (0, 6), (4, 6)]],
        '0' => &[
            &[
                (1, 0),
                (3, 0),
                (4, 1),
                (4, 5),
                (3, 6),
                (1, 6),
                (0, 5),
                (0, 1),
                (1, 0),
            ],
            &[(0, 5), (4, 1)],
        ],
        '1' => &[&[(1, 1), (2, 0), (2, 6)], &[(1, 6), (3, 6)]],
        '2' => &[&[(0, 1), (1, 0), (3, 0), (4, 1), (4, 2), (0, 6), (4, 6)]],
        '3' => &[&[(0, 0), (4, 0), (2, 3), (4, 4), (4, 5), (3, 6), (1, 6), (0, 5)]],
        '4' => &[&[(3, 6), (3, 0), (0, 4), (4, 4)]],
        '5' => &[&[(4, 0), (0, 0), (0, 3), (3, 3), (4, 4), (4, 5), (3, 6), (1, 6), (0, 5)]],
        '6' => &[&[
            (4, 0),
            (1, 0),
            (0, 1),
            (0, 5),
            (1, 6),
            (3, 6),
            (4, 5),
            (4, 4),
            (3, 3),
            (0, 3),
        ]],
        '7' => &[&[(0, 0), (4, 0), (1, 6)]],
        '8' => &[
            &[(1, 3), (0, 2), (0, 1), (1, 0), (3, 0), (4, 1), (4, 2), (3, 3), (1, 3)],
            &[(1, 3), (0, 4), (0, 5), (1, 6), (3, 6), (4, 5), (4, 4), (3, 3)],
        ],
        '9' => &[&[
            (0, 6),
            (3, 6),
            (4, 5),
            (4, 1),
            (3, 0),
            (1, 0),
            (0, 1),
            (0, 2),
            (1, 3),
            (4, 3),
        ]],
        '+' => &[&[(2, 1), (2, 5)], &[(0, 3), (4, 3)]],
        '-' => &[&[(0, 3), (4, 3)]],
        '/' => &[&[(4, 0), (0, 6)]],
        '[' => &[&[(3, 0), (1, 0), (1, 6), (3, 6)]],
        ']' => &[&[(1, 0), (3, 0), (3, 6), (1, 6)]],
        '.' => &[&[(2, 5), (2, 6)]],
        ':' => &[&[(2, 1), (2, 2)], &[(2, 4), (2, 5)]],
        '_' => &[&[(0, 6), (4, 6)]],
        _ => &[],
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kicad_sch_render::SchematicRenderer;

    #[test]
    fn the_demo_stream_is_well_formed_and_not_empty() {
        let stream = demo_stream();
        assert!(stream.frame_cmds().len() > 20, "too few frame commands");
        assert!(stream.groups().len() > 20, "too few groups");
    }

    /// The point of the demo is that it goes through the real renderer, so the
    /// test that matters is that the renderer finds something to draw in it.
    #[test]
    fn the_renderer_draws_every_group_of_it() {
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(demo_stream());
        renderer.set_viewport([1440.0, 900.0]);
        renderer.zoom_to_fit(24.0);

        let frame = renderer.prepare([0.0, 0.0]);
        assert!(
            frame.stats.groups_drawn > 20,
            "only {} groups drawn",
            frame.stats.groups_drawn
        );
        assert_eq!(frame.stats.groups_culled, 0, "a fitted view culls nothing");
        assert!(frame.stats.vertices > 1000, "{:?}", frame.stats);

        // Panning must not re-tessellate: that is the property the whole cache
        // exists for, and the demo is big enough to notice if it stops holding.
        renderer.pan(17.0, -9.0);
        let frame = renderer.prepare([0.0, 0.0]);
        assert_eq!(frame.stats.cache_misses, 0, "{:?}", frame.stats);
    }

    #[test]
    fn the_document_covers_roughly_an_a4_sheet() {
        let mut renderer = SchematicRenderer::new();
        renderer.set_stream(demo_stream());
        let bounds = renderer.document_bounds();
        let [w, h] = bounds.size();
        assert!(w > 250.0 * MM && w < 320.0 * MM, "width {w}");
        assert!(h > 180.0 * MM && h < 230.0 * MM, "height {h}");
    }

    #[test]
    fn every_letter_and_digit_has_a_glyph() {
        for ch in ('A'..='Z').chain('0'..='9') {
            assert!(!glyph(ch).is_empty(), "{ch} has no strokes");
        }
        assert!(glyph(' ').is_empty(), "a space draws nothing");
    }
}
