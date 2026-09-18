// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The intermediate representation between a draw stream and gpui.
//!
//! Everything in this module is plain data: no gpui types, no GPU, no window.
//! That is deliberate and is what makes the renderer testable at all — gpui
//! supplies no headless renderer on Linux, so golden images are not available,
//! and the useful thing to assert on is the geometry itself. A test builds a
//! stream, runs it through [`crate::translate`], and checks the [`Batch`]es
//! that come out.
//!
//! A [`Batch`] is one gpui primitive batch: a set of polylines sharing a colour
//! and stroke width, a set of filled contours sharing a colour, or a run of
//! quads. Batching is what keeps a schematic affordable — gpui tessellates each
//! `Path` on the CPU, so one path per wire segment would be ruinous — but it
//! must not reorder anything a viewer could see, so batches only ever absorb
//! primitives that are adjacent in paint order.

/// A colour packed as RGBA8, exactly as the draw stream carries it.
pub type PackedColor = u32;

/// A run of connected points in pixel space.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct Polyline {
    /// The points, in order.
    pub points: Vec<[f32; 2]>,
    /// Whether the last point joins back to the first.
    pub closed: bool,
}

/// One closed contour of a filled shape, in pixel space.
///
/// Holes are stored wound opposite to their outline, so that a non-zero fill
/// rule subtracts them whatever winding the producer used. That matters because
/// `SHAPE_POLY_SET` contours reach the stream flattened, with no promise about
/// orientation.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct Contour {
    /// The points, in order; the contour is implicitly closed.
    pub points: Vec<[f32; 2]>,
}

/// An axis-aligned rectangle in pixel space.
#[derive(Clone, Copy, Debug, PartialEq, Default)]
pub struct PixelRect {
    /// Minimum corner.
    pub min: [f32; 2],
    /// Maximum corner.
    pub max: [f32; 2],
}

impl PixelRect {
    /// A rectangle from two opposite corners, in any order.
    pub fn from_corners(a: [f32; 2], b: [f32; 2]) -> PixelRect {
        PixelRect {
            min: [a[0].min(b[0]), a[1].min(b[1])],
            max: [a[0].max(b[0]), a[1].max(b[1])],
        }
    }

    /// Width and height.
    pub fn size(&self) -> [f32; 2] {
        [self.max[0] - self.min[0], self.max[1] - self.min[1]]
    }
}

/// One gpui primitive batch.
#[derive(Clone, Debug, PartialEq)]
pub enum Batch {
    /// Stroked polylines, all of one colour and width. Round caps and joins,
    /// matching what the GAL backends draw.
    Stroke {
        /// Packed RGBA8.
        color: PackedColor,
        /// Stroke width in logical pixels, already floored by the stream's
        /// minimum line width.
        width_px: f32,
        /// The polylines.
        polylines: Vec<Polyline>,
    },
    /// Filled contours, all of one colour, resolved with a non-zero fill rule.
    Fill {
        /// Packed RGBA8.
        color: PackedColor,
        /// Outlines and holes, holes wound opposite their outline.
        contours: Vec<Contour>,
    },
    /// Axis-aligned filled rectangles. gpui draws quads as one instanced call,
    /// so anything that is genuinely a rectangle is cheaper here than as a path.
    Quads {
        /// Packed RGBA8.
        color: PackedColor,
        /// The rectangles.
        rects: Vec<PixelRect>,
    },
}

impl Batch {
    /// Number of points across the batch, which is what tessellation cost
    /// scales with.
    pub fn point_count(&self) -> usize {
        match self {
            Batch::Stroke { polylines, .. } => polylines.iter().map(|p| p.points.len()).sum(),
            Batch::Fill { contours, .. } => contours.iter().map(|c| c.points.len()).sum(),
            Batch::Quads { rects, .. } => rects.len() * 4,
        }
    }

    /// Number of separate shapes in the batch.
    pub fn shape_count(&self) -> usize {
        match self {
            Batch::Stroke { polylines, .. } => polylines.len(),
            Batch::Fill { contours, .. } => contours.len(),
            Batch::Quads { rects, .. } => rects.len(),
        }
    }
}

/// What makes two primitives mergeable into one batch.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
enum BatchKey {
    /// Width is quantised to a 64th of a pixel: finer than anything visible,
    /// coarse enough that floating-point noise does not split a batch.
    Stroke {
        color: PackedColor,
        width_q: i64,
    },
    Fill {
        color: PackedColor,
    },
    Quads {
        color: PackedColor,
    },
}

fn quantise_width(width_px: f32) -> i64 {
    (width_px as f64 * 64.0).round() as i64
}

/// Geometry for one frame, or for one cached group, in paint order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Geometry {
    /// The batches, in the order they must be painted.
    pub batches: Vec<Batch>,
}

impl Geometry {
    /// Total points across every batch.
    pub fn point_count(&self) -> usize {
        self.batches.iter().map(Batch::point_count).sum()
    }

    /// Total shapes across every batch.
    pub fn shape_count(&self) -> usize {
        self.batches.iter().map(Batch::shape_count).sum()
    }

    /// True when nothing would be painted.
    pub fn is_empty(&self) -> bool {
        self.batches.is_empty()
    }
}

/// Accumulates primitives into [`Batch`]es without reordering them.
///
/// A primitive joins the open batch when it matches its key. Anything else
/// closes the open batch and opens a new one, so paint order is preserved
/// exactly — which is the whole safety property, since gpui has no depth buffer
/// and draws strictly in the order it is told to.
///
/// Reordering that *is* safe happens one level up, in
/// [`crate::translate`]: within a single group body, primitives are stably
/// sorted by layer depth before they get here, so that a symbol's fills and
/// strokes batch together instead of alternating.
#[derive(Debug, Default)]
pub struct BatchBuilder {
    batches: Vec<Batch>,
    open: Option<BatchKey>,
}

impl BatchBuilder {
    /// An empty builder.
    pub fn new() -> BatchBuilder {
        BatchBuilder::default()
    }

    /// Add a stroked polyline.
    pub fn stroke(&mut self, color: PackedColor, width_px: f32, polyline: Polyline) {
        if polyline.points.len() < 2 {
            // A degenerate stroke tessellates to nothing but still costs a
            // lyon pass, so drop it here.
            return;
        }
        let key = BatchKey::Stroke {
            color,
            width_q: quantise_width(width_px),
        };
        if self.open != Some(key) {
            self.batches.push(Batch::Stroke {
                color,
                width_px,
                polylines: Vec::new(),
            });
            self.open = Some(key);
        }
        if let Some(Batch::Stroke { polylines, .. }) = self.batches.last_mut() {
            polylines.push(polyline);
        }
    }

    /// Add a filled shape: an outline followed by however many holes it has.
    pub fn fill(&mut self, color: PackedColor, contours: impl IntoIterator<Item = Contour>) {
        let key = BatchKey::Fill { color };
        let mut iter = contours
            .into_iter()
            .filter(|c| c.points.len() >= 3)
            .peekable();
        if iter.peek().is_none() {
            return;
        }
        if self.open != Some(key) {
            self.batches.push(Batch::Fill {
                color,
                contours: Vec::new(),
            });
            self.open = Some(key);
        }
        if let Some(Batch::Fill { contours: out, .. }) = self.batches.last_mut() {
            out.extend(iter);
        }
    }

    /// Add a filled rectangle.
    pub fn quad(&mut self, color: PackedColor, rect: PixelRect) {
        let key = BatchKey::Quads { color };
        if self.open != Some(key) {
            self.batches.push(Batch::Quads {
                color,
                rects: Vec::new(),
            });
            self.open = Some(key);
        }
        if let Some(Batch::Quads { rects, .. }) = self.batches.last_mut() {
            rects.push(rect);
        }
    }

    /// Finish, yielding the batches in paint order.
    pub fn finish(self) -> Geometry {
        Geometry {
            batches: self.batches,
        }
    }
}

/// Twice the signed area of a closed contour. Positive and negative correspond
/// to the two windings; which is which depends on the axis convention, and
/// nothing here needs to know.
pub fn signed_area2(points: &[[f32; 2]]) -> f64 {
    let n = points.len();
    if n < 3 {
        return 0.0;
    }
    let mut acc = 0.0f64;
    for i in 0..n {
        let a = points[i];
        let b = points[(i + 1) % n];
        acc += a[0] as f64 * b[1] as f64 - b[0] as f64 * a[1] as f64;
    }
    acc
}

#[cfg(test)]
mod tests {
    use super::*;

    fn line(a: [f32; 2], b: [f32; 2]) -> Polyline {
        Polyline {
            points: vec![a, b],
            closed: false,
        }
    }

    #[test]
    fn matching_primitives_merge_into_one_batch() {
        let mut b = BatchBuilder::new();
        b.stroke(0xFF00_00FF, 2.0, line([0.0, 0.0], [1.0, 0.0]));
        b.stroke(0xFF00_00FF, 2.0, line([1.0, 0.0], [2.0, 0.0]));
        b.stroke(0xFF00_00FF, 2.0, line([2.0, 0.0], [3.0, 0.0]));
        let g = b.finish();
        assert_eq!(g.batches.len(), 1);
        assert_eq!(g.shape_count(), 3);
        assert_eq!(g.point_count(), 6);
    }

    #[test]
    fn a_different_colour_or_width_starts_a_new_batch() {
        let mut b = BatchBuilder::new();
        b.stroke(0xFF00_00FF, 2.0, line([0.0, 0.0], [1.0, 0.0]));
        b.stroke(0xFF00_FF00, 2.0, line([0.0, 0.0], [1.0, 0.0]));
        b.stroke(0xFF00_FF00, 3.0, line([0.0, 0.0], [1.0, 0.0]));
        assert_eq!(b.finish().batches.len(), 3);
    }

    #[test]
    fn widths_that_differ_below_a_64th_of_a_pixel_still_merge() {
        let mut b = BatchBuilder::new();
        b.stroke(1, 2.000_0, line([0.0, 0.0], [1.0, 0.0]));
        b.stroke(1, 2.000_1, line([0.0, 0.0], [1.0, 0.0]));
        assert_eq!(b.finish().batches.len(), 1);
    }

    #[test]
    fn interleaving_reopens_rather_than_reordering() {
        // The property that keeps painting correct: a batch is never reopened
        // once something else has been painted over it.
        let mut b = BatchBuilder::new();
        b.stroke(1, 1.0, line([0.0, 0.0], [1.0, 0.0]));
        b.quad(2, PixelRect::from_corners([0.0, 0.0], [1.0, 1.0]));
        b.stroke(1, 1.0, line([2.0, 0.0], [3.0, 0.0]));
        let g = b.finish();
        assert_eq!(g.batches.len(), 3);
        assert!(matches!(g.batches[0], Batch::Stroke { .. }));
        assert!(matches!(g.batches[1], Batch::Quads { .. }));
        assert!(matches!(g.batches[2], Batch::Stroke { .. }));
    }

    #[test]
    fn degenerate_shapes_are_dropped_before_they_cost_a_tessellation() {
        let mut b = BatchBuilder::new();
        b.stroke(1, 1.0, Polyline::default());
        b.stroke(
            1,
            1.0,
            Polyline {
                points: vec![[0.0, 0.0]],
                closed: false,
            },
        );
        b.fill(
            1,
            [Contour {
                points: vec![[0.0, 0.0], [1.0, 0.0]],
            }],
        );
        assert!(b.finish().is_empty());
    }

    #[test]
    fn signed_area_reports_opposite_signs_for_opposite_windings() {
        let square = [[0.0, 0.0], [1.0, 0.0], [1.0, 1.0], [0.0, 1.0]];
        let mut reversed = square;
        reversed.reverse();
        let a = signed_area2(&square);
        let b = signed_area2(&reversed);
        assert!(a.abs() > 0.0);
        assert_eq!(a, -b);
        assert_eq!(signed_area2(&[[0.0, 0.0], [1.0, 1.0]]), 0.0);
    }
}
