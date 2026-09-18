// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! A static R-tree over group bounds, for viewport culling.
//!
//! gpui culls primitives against the content mask, but only after they have
//! been tessellated, and tessellation is the expensive half. Culling has to
//! happen before that, which means answering "which groups touch this
//! rectangle" thousands of times a second over tens of thousands of groups.
//!
//! The tree is packed and immutable: items are sorted along a Hilbert curve,
//! grouped into fixed-size nodes, and the levels stacked bottom-up into two
//! flat arrays. Building is a sort; querying is a walk with no pointer chasing
//! and no allocation beyond the result. There is no insert or remove, because
//! the group table is rebuilt wholesale whenever it changes anyway.

use crate::camera::WorldRect;

/// Children per node.
///
/// Sixteen is the usual choice for a packed R-tree: large enough that the tree
/// is shallow (a million items is five levels), small enough that scanning a
/// node's boxes stays in one or two cache lines' worth of work.
const NODE_SIZE: usize = 16;

/// A bulk-loaded R-tree over world-space rectangles.
#[derive(Clone, Debug, Default)]
pub struct BoundsIndex {
    /// Bounding boxes: the leaves first, then each level above them.
    boxes: Vec<WorldRect>,
    /// For a leaf, the caller's payload; for an internal node, the index of
    /// its first child in `boxes`.
    refs: Vec<u32>,
    /// End offset in `boxes` of each level, leaves first.
    level_ends: Vec<usize>,
    /// Number of leaves.
    len: usize,
}

impl BoundsIndex {
    /// Build an index over `(payload, bounds)` pairs.
    ///
    /// Empty rectangles are kept: a group that drew nothing still has an id,
    /// and dropping it here would make the index disagree with the group table
    /// about what exists.
    pub fn build(items: impl IntoIterator<Item = (u32, WorldRect)>) -> BoundsIndex {
        let items: Vec<(u32, WorldRect)> = items.into_iter().collect();
        let len = items.len();
        if len == 0 {
            return BoundsIndex::default();
        }

        let mut total = WorldRect::EMPTY;
        for (_, b) in &items {
            total.union(b);
        }

        // Sort along a Hilbert curve through the centres, so that nodes group
        // items that are actually near each other. Any space-filling order
        // works; Hilbert has the best locality of the cheap ones.
        let mut ordered = items;
        if len > NODE_SIZE {
            let [w, h] = total.size();
            let sx = if w > 0.0 { 65535.0 / w } else { 0.0 };
            let sy = if h > 0.0 { 65535.0 / h } else { 0.0 };
            let mut keyed: Vec<(u32, (u32, WorldRect))> = ordered
                .into_iter()
                .map(|(payload, b)| {
                    let c = if b.is_empty() {
                        total.center()
                    } else {
                        b.center()
                    };
                    let x = (((c[0] - total.min[0]) * sx).clamp(0.0, 65535.0)) as u32;
                    let y = (((c[1] - total.min[1]) * sy).clamp(0.0, 65535.0)) as u32;
                    (hilbert(x, y), (payload, b))
                })
                .collect();
            keyed.sort_unstable_by_key(|(k, _)| *k);
            ordered = keyed.into_iter().map(|(_, v)| v).collect();
        }

        let mut boxes: Vec<WorldRect> = Vec::with_capacity(len * 2);
        let mut refs: Vec<u32> = Vec::with_capacity(len * 2);
        for (payload, b) in &ordered {
            boxes.push(*b);
            refs.push(*payload);
        }

        let mut level_ends = vec![len];
        let mut level_start = 0usize;
        let mut level_len = len;
        while level_len > 1 {
            let mut i = level_start;
            let level_end = level_start + level_len;
            while i < level_end {
                let stop = (i + NODE_SIZE).min(level_end);
                let mut node = WorldRect::EMPTY;
                for b in &boxes[i..stop] {
                    node.union(b);
                }
                boxes.push(node);
                refs.push(i as u32);
                i = stop;
            }
            level_start = level_end;
            level_len = boxes.len() - level_end;
            level_ends.push(boxes.len());
        }

        BoundsIndex {
            boxes,
            refs,
            level_ends,
            len,
        }
    }

    /// Number of indexed items.
    pub fn len(&self) -> usize {
        self.len
    }

    /// True when nothing is indexed.
    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// The union of every indexed rectangle.
    pub fn bounds(&self) -> WorldRect {
        match self.boxes.last() {
            Some(b) if !self.boxes.is_empty() => *b,
            _ => WorldRect::EMPTY,
        }
    }

    /// Payloads whose rectangle intersects `query`, in no particular order.
    pub fn query(&self, query: &WorldRect) -> Vec<u32> {
        let mut out = Vec::new();
        self.query_into(query, &mut out);
        out
    }

    /// As [`BoundsIndex::query`], appending into a caller-owned buffer so that
    /// a per-frame cull does not allocate.
    pub fn query_into(&self, query: &WorldRect, out: &mut Vec<u32>) {
        if self.len == 0 || query.is_empty() {
            return;
        }
        if self.len <= NODE_SIZE && self.level_ends.len() == 1 {
            // Too small to have built a level above the leaves.
            for i in 0..self.len {
                if self.boxes[i].intersects(query) {
                    out.push(self.refs[i]);
                }
            }
            return;
        }

        // Start at the single root, which is the last box written.
        let mut stack = vec![self.boxes.len() - 1];
        while let Some(node) = stack.pop() {
            let level = self.level_of(node);
            if level == 0 {
                if self.boxes[node].intersects(query) {
                    out.push(self.refs[node]);
                }
                continue;
            }
            if !self.boxes[node].intersects(query) {
                continue;
            }
            let child_level_end = self.level_ends[level - 1];
            let first = self.refs[node] as usize;
            let last = (first + NODE_SIZE).min(child_level_end);
            for child in first..last {
                if self.boxes[child].intersects(query) {
                    stack.push(child);
                }
            }
        }
    }

    /// Which level a box index belongs to; leaves are level 0.
    fn level_of(&self, index: usize) -> usize {
        // At most a handful of levels, so a scan beats a binary search.
        for (level, end) in self.level_ends.iter().enumerate() {
            if index < *end {
                return level;
            }
        }
        self.level_ends.len() - 1
    }
}

/// Hilbert curve index of a point on a 16-bit grid.
///
/// The usual bit-interleaving formulation; the rotations are what give the
/// curve its locality compared with a plain Z-order.
fn hilbert(mut x: u32, mut y: u32) -> u32 {
    let mut rx: u32;
    let mut ry: u32;
    let mut d: u32 = 0;
    let mut s: u32 = 1 << 15;
    while s > 0 {
        rx = u32::from((x & s) > 0);
        ry = u32::from((y & s) > 0);
        d = d.wrapping_add(s.wrapping_mul(s).wrapping_mul((3 * rx) ^ ry));
        // Rotate the quadrant.
        if ry == 0 {
            if rx == 1 {
                x = s.wrapping_sub(1).wrapping_sub(x);
                y = s.wrapping_sub(1).wrapping_sub(y);
            }
            std::mem::swap(&mut x, &mut y);
        }
        s /= 2;
    }
    d
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rect(x: f64, y: f64, w: f64, h: f64) -> WorldRect {
        WorldRect::from_corners([x, y], [x + w, y + h])
    }

    /// The reference implementation the tree has to agree with.
    fn brute_force(items: &[(u32, WorldRect)], q: &WorldRect) -> Vec<u32> {
        let mut v: Vec<u32> = items
            .iter()
            .filter(|(_, b)| b.intersects(q))
            .map(|(id, _)| *id)
            .collect();
        v.sort_unstable();
        v
    }

    /// A deterministic spread of rectangles, at nanometre magnitudes so that
    /// the index is exercised on the numbers it will really see.
    fn sample(n: u32) -> Vec<(u32, WorldRect)> {
        let mut state = 0x2545_F491_4F6C_DD1Du64;
        let mut next = || {
            state ^= state >> 12;
            state ^= state << 25;
            state ^= state >> 27;
            state.wrapping_mul(0x2545_F491_4F6C_DD1D)
        };
        (0..n)
            .map(|i| {
                let x = 1.2e9 + (next() % 2_000_000) as f64;
                let y = -9.0e8 + (next() % 2_000_000) as f64;
                let w = (next() % 50_000) as f64;
                let h = (next() % 50_000) as f64;
                (i, rect(x, y, w, h))
            })
            .collect()
    }

    #[test]
    fn an_empty_index_answers_nothing() {
        let idx = BoundsIndex::build(std::iter::empty());
        assert!(idx.is_empty());
        assert!(idx.query(&rect(0.0, 0.0, 1.0, 1.0)).is_empty());
    }

    #[test]
    fn small_indexes_agree_with_brute_force() {
        // Below the node size the tree has no internal level at all, which is
        // its own code path.
        for n in [1u32, 2, 5, 16, 17] {
            let items = sample(n);
            let idx = BoundsIndex::build(items.clone());
            assert_eq!(idx.len(), n as usize);
            let q = rect(1.2e9, -9.0e8, 1_000_000.0, 1_000_000.0);
            let mut got = idx.query(&q);
            got.sort_unstable();
            assert_eq!(got, brute_force(&items, &q), "n = {n}");
        }
    }

    #[test]
    fn large_indexes_agree_with_brute_force_on_every_query() {
        let items = sample(5_000);
        let idx = BoundsIndex::build(items.clone());

        let queries = [
            rect(1.2e9, -9.0e8, 100_000.0, 100_000.0),
            rect(1.2e9 + 500_000.0, -9.0e8 + 500_000.0, 10_000.0, 10_000.0),
            rect(0.0, 0.0, 1.0, 1.0),
            rect(1.2e9 - 1e7, -9.0e8 - 1e7, 1e8, 1e8),
            rect(1.2e9 + 1_999_999.0, -9.0e8 + 1_999_999.0, 1.0, 1.0),
        ];
        for q in &queries {
            let mut got = idx.query(q);
            got.sort_unstable();
            assert_eq!(got, brute_force(&items, q), "query {q:?}");
        }
    }

    #[test]
    fn a_query_covering_everything_returns_everything() {
        let items = sample(1_000);
        let idx = BoundsIndex::build(items.clone());
        let mut got = idx.query(&idx.bounds());
        got.sort_unstable();
        assert_eq!(got.len(), items.len());
    }

    #[test]
    fn query_into_appends_without_clearing() {
        let items = sample(100);
        let idx = BoundsIndex::build(items);
        let mut out = vec![u32::MAX];
        idx.query_into(&idx.bounds(), &mut out);
        assert_eq!(out[0], u32::MAX);
        assert!(out.len() > 1);
    }

    #[test]
    fn empty_rectangles_stay_indexed_but_never_match() {
        let items = vec![(0u32, WorldRect::EMPTY), (1u32, rect(0.0, 0.0, 10.0, 10.0))];
        let idx = BoundsIndex::build(items);
        assert_eq!(idx.len(), 2);
        assert_eq!(idx.query(&rect(-100.0, -100.0, 1000.0, 1000.0)), vec![1]);
    }

    #[test]
    fn identical_rectangles_do_not_confuse_the_hilbert_sort() {
        let items: Vec<(u32, WorldRect)> =
            (0..100).map(|i| (i, rect(5.0, 5.0, 1.0, 1.0))).collect();
        let idx = BoundsIndex::build(items);
        let mut got = idx.query(&rect(5.0, 5.0, 0.1, 0.1));
        got.sort_unstable();
        assert_eq!(got.len(), 100);
    }

    #[test]
    fn hilbert_is_a_bijection_on_a_small_grid() {
        // A wrong rotation shows up immediately as two points sharing an index.
        let mut seen = std::collections::HashSet::new();
        for y in 0..64u32 {
            for x in 0..64u32 {
                // Scale up so the top bits are the ones being exercised.
                assert!(seen.insert(hilbert(x << 10, y << 10)));
            }
        }
    }
}
