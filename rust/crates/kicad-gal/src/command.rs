// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The typed form of a recorded command.
//!
//! [`Command`] is what consumers work with: every coordinate-arena index has
//! already been resolved into a borrowed slice, every packed colour into a
//! [`Color`], and every enumeration into a Rust enum. The renderer therefore
//! never handles a raw index, which is the whole point — an index it got wrong
//! would be an out-of-bounds read across an FFI boundary.

use crate::abi::{flags, kgds_cmd, Color, GridStyle, Op, Target};
use crate::error::DecodeError;

/// A point run borrowed from the coordinate arena.
///
/// The backing slice always holds an even number of `f64`s, interleaved `x, y`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Points<'a> {
    flat: &'a [f64],
}

impl<'a> Points<'a> {
    /// Wrap an interleaved slice. Returns `None` if its length is odd.
    pub fn new(flat: &'a [f64]) -> Option<Points<'a>> {
        if flat.len() % 2 == 0 {
            Some(Points { flat })
        } else {
            None
        }
    }

    /// Number of points, i.e. half the length of the backing slice.
    pub fn len(self) -> usize {
        self.flat.len() / 2
    }

    /// True when the run holds no points at all.
    pub fn is_empty(self) -> bool {
        self.flat.is_empty()
    }

    /// The `i`th point, or `None` past the end.
    pub fn get(self, i: usize) -> Option<[f64; 2]> {
        let base = i.checked_mul(2)?;
        let x = *self.flat.get(base)?;
        let y = *self.flat.get(base + 1)?;
        Some([x, y])
    }

    /// Iterate the points.
    pub fn iter(self) -> impl ExactSizeIterator<Item = [f64; 2]> + 'a {
        self.flat.chunks_exact(2).map(|c| [c[0], c[1]])
    }

    /// The underlying interleaved slice, for bulk work such as feeding a
    /// tessellator without going through the per-point accessor.
    pub fn as_flat(self) -> &'a [f64] {
        self.flat
    }
}

/// A 2-D affine transform, stored as the `a b c d e f` row-major triple the
/// ABI uses: `x' = a·x + b·y + c`, `y' = d·x + e·y + f`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Affine(pub [f64; 6]);

impl Affine {
    /// The identity transform.
    pub const IDENTITY: Affine = Affine([1.0, 0.0, 0.0, 0.0, 1.0, 0.0]);

    /// Pure translation.
    pub fn translation(dx: f64, dy: f64) -> Affine {
        Affine([1.0, 0.0, dx, 0.0, 1.0, dy])
    }

    /// Pure (possibly anisotropic) scale about the origin.
    pub fn scale(sx: f64, sy: f64) -> Affine {
        Affine([sx, 0.0, 0.0, 0.0, sy, 0.0])
    }

    /// Counter-clockwise rotation about the origin, in radians.
    pub fn rotation(radians: f64) -> Affine {
        let (s, c) = radians.sin_cos();
        Affine([c, -s, 0.0, s, c, 0.0])
    }

    /// Apply to a point.
    pub fn apply(self, p: [f64; 2]) -> [f64; 2] {
        let [a, b, c, d, e, f] = self.0;
        [a * p[0] + b * p[1] + c, d * p[0] + e * p[1] + f]
    }

    /// Apply to a direction, ignoring the translation part.
    pub fn apply_vector(self, v: [f64; 2]) -> [f64; 2] {
        let [a, b, _, d, e, _] = self.0;
        [a * v[0] + b * v[1], d * v[0] + e * v[1]]
    }

    /// `self` followed by `other`, i.e. `other ∘ self`.
    pub fn then(self, other: Affine) -> Affine {
        let [a, b, c, d, e, f] = self.0;
        let [g, h, i, j, k, l] = other.0;
        Affine([
            g * a + h * d,
            g * b + h * e,
            g * c + h * f + i,
            j * a + k * d,
            j * b + k * e,
            j * c + k * f + l,
        ])
    }

    /// The uniform scale factor this transform applies, taken as the square
    /// root of the absolute determinant.
    ///
    /// Stroke widths are scalars, so an anisotropic transform cannot be
    /// applied to them exactly; this is the usual isotropic approximation and
    /// is what the GAL backends do too.
    pub fn uniform_scale(self) -> f64 {
        let [a, b, _, d, e, _] = self.0;
        (a * e - b * d).abs().sqrt()
    }
}

impl Default for Affine {
    fn default() -> Self {
        Affine::IDENTITY
    }
}

/// One decoded command, with its arguments resolved.
#[derive(Clone, Copy, Debug, PartialEq)]
#[non_exhaustive]
// Field names are the ABI header's own argument names; the variant docs say
// what each command means.
#[allow(missing_docs)]
pub enum Command<'a> {
    // -- structural --------------------------------------------------------
    /// Do nothing. Recorded so that a producer can blank a command in place.
    Nop,
    /// Begin a frame with the given pixel viewport.
    BeginFrame { width: u32, height: u32 },
    /// End a frame.
    EndFrame,
    /// Clear the screen to a colour.
    ClearScreen { color: Color },
    /// Replay the cached group with this id.
    ///
    /// The two overrides carry a selection or highlight without touching the
    /// group's recorded body, so a renderer caching tessellation by
    /// `(id, serial)` keeps its entry across a selection change.
    DrawGroup {
        id: u32,
        /// Replaces every stroke and fill colour inside the group for this
        /// replay only.
        color_override: Option<Color>,
        /// Replaces the layer depth for this replay only.
        depth_override: Option<f64>,
    },
    /// Direct subsequent commands at a render target.
    SetTarget { target: Target },
    /// Clear a render target.
    ClearTarget { target: Target },
    /// Start a difference-blended layer.
    StartDiffLayer,
    /// End a difference-blended layer.
    EndDiffLayer,
    /// Start a negatives layer.
    StartNegativesLayer,
    /// End a negatives layer.
    EndNegativesLayer,

    // -- render state ------------------------------------------------------
    /// Enable or disable filling.
    SetIsFill { enabled: bool },
    /// Enable or disable stroking.
    SetIsStroke { enabled: bool },
    /// Set the fill colour.
    SetFillColor { color: Color },
    /// Set the stroke colour.
    SetStrokeColor { color: Color },
    /// Set the hover highlight colour.
    SetHoverColor { color: Color },
    /// Set the stroke width, in internal units.
    SetLineWidth { width: f64 },
    /// Set the floor applied to stroke widths after scaling, in pixels.
    SetMinLineWidth { width: f64 },
    /// Set the painter's-algorithm depth for subsequent geometry.
    SetLayerDepth { depth: f64 },
    /// Enable or disable negative draw mode.
    SetNegativeDrawMode { enabled: bool },
    /// Enable or disable depth testing.
    EnableDepthTest { enabled: bool },

    // -- transforms --------------------------------------------------------
    /// Replace the current transform.
    Transform { matrix: Affine },
    /// Post-multiply a rotation, in radians.
    Rotate { radians: f64 },
    /// Post-multiply a translation.
    Translate { offset: [f64; 2] },
    /// Post-multiply a scale.
    Scale { factor: [f64; 2] },
    /// Push the transform stack.
    Save,
    /// Pop the transform stack.
    Restore,

    // -- geometry ----------------------------------------------------------
    /// A line stroked with the current line width.
    Line { p0: [f64; 2], p1: [f64; 2] },
    /// A round-capped segment of an explicit width.
    Segment {
        p0: [f64; 2],
        p1: [f64; 2],
        width: f64,
    },
    /// A chain of round-capped, round-joined segments of an explicit width.
    SegmentChain { points: Points<'a>, width: f64 },
    /// A polyline stroked with the current line width.
    Polyline { points: Points<'a>, closed: bool },
    /// A filled contour. `hole` marks it as a hole in the preceding outline,
    /// which is how `SHAPE_POLY_SET` contours are flattened.
    Polygon { points: Points<'a>, hole: bool },
    /// A circle, filled and/or stroked per the current state.
    Circle { center: [f64; 2], radius: f64 },
    /// An arc, filled and/or stroked per the current state.
    Arc {
        center: [f64; 2],
        radius: f64,
        start_angle: f64,
        end_angle: f64,
    },
    /// An arc stroked with an explicit width.
    ArcSegment {
        center: [f64; 2],
        radius: f64,
        start_angle: f64,
        end_angle: f64,
        width: f64,
    },
    /// An axis-aligned rectangle.
    Rectangle { p0: [f64; 2], p1: [f64; 2] },
    /// A cubic Bezier.
    Curve {
        start: [f64; 2],
        control_a: [f64; 2],
        control_b: [f64; 2],
        end: [f64; 2],
    },
    /// An ellipse.
    Ellipse {
        center: [f64; 2],
        major_radius: f64,
        minor_radius: f64,
        rotation: f64,
    },
    /// An elliptical arc stroked with an explicit width.
    EllipseArc {
        center: [f64; 2],
        major_radius: f64,
        minor_radius: f64,
        rotation: f64,
        start_angle: f64,
        end_angle: f64,
        width: f64,
    },
    /// A plated hole wall. pcbnew only; recorded for completeness.
    HoleWall {
        center: [f64; 2],
        radius: f64,
        width: f64,
    },
    /// An embedded bitmap placed by an affine transform.
    Bitmap {
        image: u32,
        placement: Affine,
        alpha: f64,
    },

    // -- overlays ----------------------------------------------------------
    /// The construction grid, passed as parameters rather than as the
    /// thousands of lines it would expand to.
    Grid {
        origin: [f64; 2],
        size: [f64; 2],
        line_width_px: f64,
        style: GridStyle,
        color: Color,
    },
    /// The cursor crosshair.
    Cursor { position: [f64; 2], color: Color },
}

/// A decoded command together with where it came from.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Instruction<'a> {
    /// Index of this command in the stream's command array. The renderer uses
    /// it to give co-located primitives a stable sub-layer ordering.
    pub index: usize,
    /// The raw flag bits, for the ones [`Command`] does not fold in.
    pub flags: u16,
    /// The decoded command.
    pub command: Command<'a>,
}

impl Instruction<'_> {
    /// True when the geometry came from a glyph outline. It is still ordinary
    /// geometry — the flag only exists so a renderer may batch it separately.
    pub fn is_glyph(&self) -> bool {
        self.flags & flags::GLYPH != 0
    }
}

/// Fetch `len` coordinates starting at `first`, or fail with the indices that
/// went wrong. Every arithmetic step is checked because `first` and `len` come
/// straight off the wire.
fn coords_at(index: usize, coords: &[f64], first: u32, len: u64) -> Result<&[f64], DecodeError> {
    let first = first as u64;
    let end = first.checked_add(len).ok_or(DecodeError::CoordOutOfRange {
        index,
        first,
        len,
        arena_len: coords.len(),
    })?;

    let (Ok(f), Ok(e)) = (usize::try_from(first), usize::try_from(end)) else {
        return Err(DecodeError::CoordOutOfRange {
            index,
            first,
            len,
            arena_len: coords.len(),
        });
    };

    coords.get(f..e).ok_or(DecodeError::CoordOutOfRange {
        index,
        first,
        len,
        arena_len: coords.len(),
    })
}

fn pt(s: &[f64], i: usize) -> [f64; 2] {
    // Callers only reach this after `coords_at` proved the slice long enough.
    [s[i], s[i + 1]]
}

fn affine(s: &[f64]) -> Affine {
    Affine([s[0], s[1], s[2], s[3], s[4], s[5]])
}

/// Decode one command against the coordinate arena.
///
/// `index` is only used to make errors locatable. `image_count` is needed to
/// range-check [`Op::Bitmap`]; pass the stream's image table length.
pub(crate) fn decode_in<'a>(
    index: usize,
    cmd: &kgds_cmd,
    coords: &'a [f64],
    image_count: usize,
) -> Result<Command<'a>, DecodeError> {
    let op = Op::from_raw(cmd.op).ok_or(DecodeError::UnknownOpcode { index, raw: cmd.op })?;

    if cmd.flags & !flags::KNOWN != 0 {
        return Err(DecodeError::UnknownFlags {
            index,
            raw: cmd.flags,
        });
    }

    // A point run's coordinate count is 2n, and n is a u32 straight off the
    // wire, so the multiply is done in u64 and range-checked by `coords_at`.
    let run = |n: u32| -> u64 { (n as u64) * 2 };

    let command = match op {
        Op::Nop => Command::Nop,
        Op::BeginFrame => Command::BeginFrame {
            width: cmd.arg0,
            height: cmd.arg1,
        },
        Op::EndFrame => Command::EndFrame,
        Op::ClearScreen => Command::ClearScreen {
            color: Color::from_packed(cmd.arg0),
        },
        Op::DrawGroup => Command::DrawGroup {
            id: cmd.arg0,
            color_override: (cmd.flags & flags::GROUP_COLOR != 0)
                .then(|| Color::from_packed(cmd.arg1)),
            depth_override: match cmd.flags & flags::GROUP_DEPTH != 0 {
                true => Some(coords_at(index, coords, cmd.arg2, 1)?[0]),
                false => None,
            },
        },
        Op::SetTarget => Command::SetTarget {
            target: Target::from_raw(cmd.arg0).ok_or(DecodeError::UnknownTarget {
                index,
                raw: cmd.arg0,
            })?,
        },
        Op::ClearTarget => Command::ClearTarget {
            target: Target::from_raw(cmd.arg0).ok_or(DecodeError::UnknownTarget {
                index,
                raw: cmd.arg0,
            })?,
        },
        Op::StartDiffLayer => Command::StartDiffLayer,
        Op::EndDiffLayer => Command::EndDiffLayer,
        Op::StartNegativesLayer => Command::StartNegativesLayer,
        Op::EndNegativesLayer => Command::EndNegativesLayer,

        Op::SetIsFill => Command::SetIsFill {
            enabled: cmd.arg0 != 0,
        },
        Op::SetIsStroke => Command::SetIsStroke {
            enabled: cmd.arg0 != 0,
        },
        Op::SetFillColor => Command::SetFillColor {
            color: Color::from_packed(cmd.arg0),
        },
        Op::SetStrokeColor => Command::SetStrokeColor {
            color: Color::from_packed(cmd.arg0),
        },
        Op::SetHoverColor => Command::SetHoverColor {
            color: Color::from_packed(cmd.arg0),
        },
        Op::SetLineWidth => Command::SetLineWidth {
            width: coords_at(index, coords, cmd.arg0, 1)?[0],
        },
        Op::SetMinLineWidth => Command::SetMinLineWidth {
            width: coords_at(index, coords, cmd.arg0, 1)?[0],
        },
        Op::SetLayerDepth => Command::SetLayerDepth {
            depth: coords_at(index, coords, cmd.arg0, 1)?[0],
        },
        Op::SetNegativeDrawMode => Command::SetNegativeDrawMode {
            enabled: cmd.arg0 != 0,
        },
        Op::EnableDepthTest => Command::EnableDepthTest {
            enabled: cmd.arg0 != 0,
        },

        Op::Transform => Command::Transform {
            matrix: affine(coords_at(index, coords, cmd.arg0, 6)?),
        },
        Op::Rotate => Command::Rotate {
            radians: coords_at(index, coords, cmd.arg0, 1)?[0],
        },
        Op::Translate => Command::Translate {
            offset: pt(coords_at(index, coords, cmd.arg0, 2)?, 0),
        },
        Op::Scale => Command::Scale {
            factor: pt(coords_at(index, coords, cmd.arg0, 2)?, 0),
        },
        Op::Save => Command::Save,
        Op::Restore => Command::Restore,

        Op::Line => {
            let c = coords_at(index, coords, cmd.arg0, 4)?;
            Command::Line {
                p0: pt(c, 0),
                p1: pt(c, 2),
            }
        }
        Op::Segment => {
            let c = coords_at(index, coords, cmd.arg0, 4)?;
            Command::Segment {
                p0: pt(c, 0),
                p1: pt(c, 2),
                width: coords_at(index, coords, cmd.arg1, 1)?[0],
            }
        }
        Op::SegmentChain => {
            let c = coords_at(index, coords, cmd.arg0, run(cmd.arg1))?;
            Command::SegmentChain {
                points: Points::new(c).ok_or(DecodeError::PointCountOverflow {
                    index,
                    points: cmd.arg1,
                })?,
                width: coords_at(index, coords, cmd.arg2, 1)?[0],
            }
        }
        Op::Polyline => {
            let c = coords_at(index, coords, cmd.arg0, run(cmd.arg1))?;
            Command::Polyline {
                points: Points::new(c).ok_or(DecodeError::PointCountOverflow {
                    index,
                    points: cmd.arg1,
                })?,
                closed: cmd.flags & flags::CLOSED != 0,
            }
        }
        Op::Polygon => {
            let c = coords_at(index, coords, cmd.arg0, run(cmd.arg1))?;
            Command::Polygon {
                points: Points::new(c).ok_or(DecodeError::PointCountOverflow {
                    index,
                    points: cmd.arg1,
                })?,
                hole: cmd.flags & flags::HOLE != 0,
            }
        }
        Op::Circle => {
            let c = coords_at(index, coords, cmd.arg0, 3)?;
            Command::Circle {
                center: pt(c, 0),
                radius: c[2],
            }
        }
        Op::Arc => {
            let c = coords_at(index, coords, cmd.arg0, 5)?;
            Command::Arc {
                center: pt(c, 0),
                radius: c[2],
                start_angle: c[3],
                end_angle: c[4],
            }
        }
        Op::ArcSegment => {
            let c = coords_at(index, coords, cmd.arg0, 5)?;
            Command::ArcSegment {
                center: pt(c, 0),
                radius: c[2],
                start_angle: c[3],
                end_angle: c[4],
                width: coords_at(index, coords, cmd.arg1, 1)?[0],
            }
        }
        Op::Rectangle => {
            let c = coords_at(index, coords, cmd.arg0, 4)?;
            Command::Rectangle {
                p0: pt(c, 0),
                p1: pt(c, 2),
            }
        }
        Op::Curve => {
            let c = coords_at(index, coords, cmd.arg0, 8)?;
            Command::Curve {
                start: pt(c, 0),
                control_a: pt(c, 2),
                control_b: pt(c, 4),
                end: pt(c, 6),
            }
        }
        Op::Ellipse => {
            let c = coords_at(index, coords, cmd.arg0, 4)?;
            Command::Ellipse {
                center: pt(c, 0),
                major_radius: c[2],
                minor_radius: c[3],
                rotation: coords_at(index, coords, cmd.arg1, 1)?[0],
            }
        }
        Op::EllipseArc => {
            let c = coords_at(index, coords, cmd.arg0, 4)?;
            let a = coords_at(index, coords, cmd.arg1, 3)?;
            Command::EllipseArc {
                center: pt(c, 0),
                major_radius: c[2],
                minor_radius: c[3],
                rotation: a[0],
                start_angle: a[1],
                end_angle: a[2],
                width: coords_at(index, coords, cmd.arg2, 1)?[0],
            }
        }
        Op::HoleWall => {
            let c = coords_at(index, coords, cmd.arg0, 3)?;
            Command::HoleWall {
                center: pt(c, 0),
                radius: c[2],
                width: coords_at(index, coords, cmd.arg1, 1)?[0],
            }
        }
        Op::Bitmap => {
            if cmd.arg0 as usize >= image_count {
                return Err(DecodeError::ImageOutOfRange {
                    index,
                    image: cmd.arg0,
                    image_count,
                });
            }
            Command::Bitmap {
                image: cmd.arg0,
                placement: affine(coords_at(index, coords, cmd.arg1, 6)?),
                alpha: coords_at(index, coords, cmd.arg2, 1)?[0],
            }
        }

        Op::Grid => {
            let c = coords_at(index, coords, cmd.arg0, 6)?;
            // The style rides in a double because the ABI has no enum slot for
            // it; reject anything that is not a KIGFX::GRID_STYLE rather than
            // guessing at a default.
            let raw = c[5];
            let style = if raw >= 0.0 && raw <= u32::MAX as f64 && raw.fract() == 0.0 {
                GridStyle::from_raw(raw as u32)
            } else {
                None
            };
            Command::Grid {
                origin: pt(c, 0),
                size: pt(c, 2),
                line_width_px: c[4],
                style: style.ok_or(DecodeError::UnknownGridStyle { index, raw })?,
                color: Color::from_packed(cmd.arg1),
            }
        }
        Op::Cursor => Command::Cursor {
            position: pt(coords_at(index, coords, cmd.arg0, 2)?, 0),
            color: Color::from_packed(cmd.arg1),
        },
    };

    Ok(command)
}

/// Iterator over a validated range of the command array.
///
/// Infallible by construction: [`crate::StreamView`] proved every command in
/// the stream decodable before this iterator could exist.
#[derive(Clone, Debug)]
pub struct CommandIter<'a> {
    pub(crate) cmds: &'a [kgds_cmd],
    pub(crate) coords: &'a [f64],
    pub(crate) image_count: usize,
    /// Index of `cmds[0]` within the stream's command array, so that
    /// [`Instruction::index`] is a stream-wide index.
    pub(crate) base: usize,
    pub(crate) pos: usize,
}

impl<'a> Iterator for CommandIter<'a> {
    type Item = Instruction<'a>;

    fn next(&mut self) -> Option<Instruction<'a>> {
        while self.pos < self.cmds.len() {
            let i = self.pos;
            self.pos += 1;
            let raw = &self.cmds[i];
            match decode_in(self.base + i, raw, self.coords, self.image_count) {
                Ok(command) => {
                    return Some(Instruction {
                        index: self.base + i,
                        flags: raw.flags,
                        command,
                    })
                }
                Err(_e) => {
                    // Unreachable: StreamView construction decoded every
                    // command successfully. Skipping rather than panicking
                    // keeps a decoder bug from taking down a render loop.
                    debug_assert!(false, "validated stream failed to decode: {_e}");
                }
            }
        }
        None
    }

    fn size_hint(&self) -> (usize, Option<usize>) {
        (0, Some(self.cmds.len() - self.pos))
    }
}

impl std::iter::FusedIterator for CommandIter<'_> {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn affine_composition_matches_sequential_application() {
        let t = Affine::translation(3.0, -4.0);
        let r = Affine::rotation(std::f64::consts::FRAC_PI_2);
        let p = [1.0, 0.0];
        let a = r.apply(t.apply(p));
        let b = t.then(r).apply(p);
        assert!((a[0] - b[0]).abs() < 1e-12 && (a[1] - b[1]).abs() < 1e-12);
    }

    #[test]
    fn uniform_scale_reads_the_determinant() {
        assert!((Affine::scale(3.0, 3.0).uniform_scale() - 3.0).abs() < 1e-12);
        assert!((Affine::rotation(0.7).uniform_scale() - 1.0).abs() < 1e-12);
    }

    #[test]
    fn points_rejects_an_odd_backing_slice() {
        assert!(Points::new(&[1.0, 2.0, 3.0]).is_none());
        assert_eq!(Points::new(&[1.0, 2.0]).map(|p| p.len()), Some(1));
    }
}
