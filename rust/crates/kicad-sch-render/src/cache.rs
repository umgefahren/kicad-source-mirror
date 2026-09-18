// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The tessellation cache.
//!
//! `kgds_group::serial` changes only when the producer re-records a group's
//! body, so `(id, serial)` is a complete statement of "this geometry has not
//! changed". The level of detail joins the key because stroke widths and
//! flattening tolerance are chosen in pixels, so a large zoom change does need
//! new triangles — but only a large one: [`crate::camera::Camera::lod`]
//! quantises zoom to sixteenths of an octave and the residual is applied to the
//! finished vertices.
//!
//! What falls out of that is the property the whole design exists for: a pan
//! re-tessellates nothing, a small zoom re-tessellates nothing, and selecting an
//! item re-tessellates nothing — the stream carries selection as a colour
//! override on the replay, which is substituted at paint time.

use std::collections::HashMap;
use std::rc::Rc;

use kicad_gal::StreamView;

use crate::camera::WorldRect;
use crate::paint::{tessellate, Tessellated};
use crate::translate::translate_group;

/// What identifies a cached tessellation.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct GroupKey {
    /// The group's stable id.
    pub id: u32,
    /// The producer's serial for the body.
    pub serial: u32,
    /// The quantised zoom level it was tessellated at.
    pub lod: i32,
}

/// A tessellated group, ready to be placed.
#[derive(Debug)]
pub struct CachedGroup {
    /// The gpui primitives, in group-local pixel space.
    pub tessellated: Tessellated,
    /// Where group-local pixel space is measured from, in world units.
    pub anchor: [f64; 2],
    /// Pixels per world unit the geometry was flattened at.
    pub scale: f64,
    /// The group's extent in world units.
    pub bounds: WorldRect,
    /// The nearest layer depth in the group.
    pub min_depth: f64,
}

/// Cache counters, for tests and for the frame overlay.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CacheStats {
    /// Lookups served from the cache.
    pub hits: u64,
    /// Lookups that had to tessellate.
    pub misses: u64,
    /// Entries dropped to stay inside the budget.
    pub evictions: u64,
    /// Entries currently held.
    pub entries: usize,
    /// Vertices currently held.
    pub vertices: usize,
}

struct Entry {
    group: Rc<CachedGroup>,
    last_used: u64,
}

/// Default ceiling on cached vertices.
///
/// A `PathVertex<Pixels>` is 32 bytes and lyon emits three per triangle, so
/// four million of them is about 128 MB. A round-capped segment is roughly
/// thirty-odd such vertices, which puts the budget at something over a hundred
/// thousand strokes — more than a screenful at any zoom.
///
/// The product stays bounded for a reason worth stating: flattening tolerance
/// is measured in pixels, so zoomed out a symbol's curves collapse to a few
/// chords while zoomed in there are only a few symbols on screen to tessellate.
/// The budget is the backstop for the middle, not the normal case, and
/// exceeding it costs a re-tessellation of the least recently used group rather
/// than anything worse.
pub const DEFAULT_VERTEX_BUDGET: usize = 4_000_000;

/// Tessellated group geometry, keyed by `(id, serial, lod)`.
pub struct TessellationCache {
    entries: HashMap<GroupKey, Entry>,
    frame: u64,
    vertices: usize,
    budget: usize,
    stats: CacheStats,
}

impl Default for TessellationCache {
    fn default() -> Self {
        TessellationCache::new(DEFAULT_VERTEX_BUDGET)
    }
}

impl std::fmt::Debug for TessellationCache {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("TessellationCache")
            .field("entries", &self.entries.len())
            .field("vertices", &self.vertices)
            .field("budget", &self.budget)
            .finish()
    }
}

impl TessellationCache {
    /// An empty cache holding at most `vertex_budget` vertices.
    pub fn new(vertex_budget: usize) -> TessellationCache {
        TessellationCache {
            entries: HashMap::new(),
            frame: 0,
            vertices: 0,
            budget: vertex_budget,
            stats: CacheStats::default(),
        }
    }

    /// Begin a frame. Entries touched since the last call are protected from
    /// eviction, so a frame can never evict something it is about to draw.
    pub fn begin_frame(&mut self) {
        self.frame = self.frame.wrapping_add(1);
    }

    /// Counters since construction, plus the current occupancy.
    pub fn stats(&self) -> CacheStats {
        CacheStats {
            entries: self.entries.len(),
            vertices: self.vertices,
            ..self.stats
        }
    }

    /// Forget everything. Needed when the stream is replaced by one whose
    /// group ids mean something different.
    pub fn clear(&mut self) {
        self.entries.clear();
        self.vertices = 0;
    }

    /// True when this key is already tessellated.
    pub fn contains(&self, key: &GroupKey) -> bool {
        self.entries.contains_key(key)
    }

    /// Fetch a group's tessellation, building it if this is the first time it
    /// has been asked for at this serial and level of detail.
    ///
    /// Returns `None` only when the stream has no group with that id.
    pub fn get_or_build(
        &mut self,
        view: &StreamView<'_>,
        id: u32,
        lod: i32,
        lod_scale: f64,
    ) -> Option<Rc<CachedGroup>> {
        let serial = view.group(id)?.serial;
        let key = GroupKey { id, serial, lod };

        if let Some(entry) = self.entries.get_mut(&key) {
            entry.last_used = self.frame;
            self.stats.hits += 1;
            return Some(entry.group.clone());
        }

        self.stats.misses += 1;
        let geometry = translate_group(view, id, lod_scale)?;
        let tessellated = tessellate(&geometry.geometry);
        let cost = tessellated.vertices;
        let group = Rc::new(CachedGroup {
            tessellated,
            anchor: geometry.anchor,
            scale: geometry.scale,
            bounds: geometry.bounds,
            min_depth: geometry.min_depth,
        });

        self.entries.insert(
            key,
            Entry {
                group: group.clone(),
                last_used: self.frame,
            },
        );
        self.vertices += cost;
        self.evict_to_budget();
        Some(group)
    }

    /// Drop least-recently-used entries until the vertex budget is met.
    ///
    /// Entries used in the current frame are never dropped: they are about to
    /// be drawn, and evicting them would guarantee a miss on the next frame
    /// too. If everything in the cache is in use the budget is simply exceeded
    /// for that frame, which is the right failure — a frame that genuinely
    /// needs more than the budget should be slow, not wrong.
    fn evict_to_budget(&mut self) {
        if self.vertices <= self.budget {
            return;
        }
        let mut candidates: Vec<(GroupKey, u64, usize)> = self
            .entries
            .iter()
            .filter(|(_, e)| e.last_used != self.frame)
            .map(|(k, e)| (*k, e.last_used, e.group.tessellated.vertices))
            .collect();
        candidates.sort_unstable_by_key(|(_, last, _)| *last);

        for (key, _, cost) in candidates {
            if self.vertices <= self.budget {
                break;
            }
            self.entries.remove(&key);
            self.vertices = self.vertices.saturating_sub(cost);
            self.stats.evictions += 1;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use kicad_gal::{Color, Stream, StreamBuilder};

    fn stream_with(serial: u32) -> Stream {
        let mut b = StreamBuilder::new();
        b.group(0, serial, |g| {
            g.set_stroke_color(Color::new(1.0, 1.0, 1.0, 1.0));
            g.set_line_width(10_000.0);
            for i in 0..20 {
                let x = i as f64 * 100_000.0;
                g.segment([x, 0.0], [x, 500_000.0], 10_000.0);
            }
        });
        b.draw_group(0);
        b.finish().expect("valid")
    }

    #[test]
    fn a_second_lookup_at_the_same_key_is_a_hit() {
        let s = stream_with(1);
        let v = s.view();
        let mut cache = TessellationCache::default();
        cache.begin_frame();
        let a = cache.get_or_build(&v, 0, 0, 1e-4).expect("group 0 exists");
        cache.begin_frame();
        let b = cache.get_or_build(&v, 0, 0, 1e-4).expect("group 0 exists");
        assert!(Rc::ptr_eq(&a, &b), "the tessellation was rebuilt");
        assert_eq!(cache.stats().hits, 1);
        assert_eq!(cache.stats().misses, 1);
    }

    #[test]
    fn a_new_serial_is_a_miss_and_a_new_lod_is_a_miss() {
        let s = stream_with(1);
        let v = s.view();
        let mut cache = TessellationCache::default();
        cache.begin_frame();
        cache.get_or_build(&v, 0, 0, 1e-4);
        cache.get_or_build(&v, 0, 3, 1e-3);
        assert_eq!(cache.stats().misses, 2);

        // Re-recording the body bumps the serial, which is the only thing that
        // may invalidate an entry.
        let s2 = stream_with(2);
        let v2 = s2.view();
        cache.get_or_build(&v2, 0, 0, 1e-4);
        assert_eq!(cache.stats().misses, 3);
        assert!(cache.contains(&GroupKey {
            id: 0,
            serial: 1,
            lod: 0
        }));
    }

    #[test]
    fn an_unknown_group_is_reported_rather_than_invented() {
        let s = stream_with(1);
        let v = s.view();
        let mut cache = TessellationCache::default();
        assert!(cache.get_or_build(&v, 99, 0, 1e-4).is_none());
    }

    #[test]
    fn the_budget_is_enforced_across_frames() {
        let s = stream_with(1);
        let v = s.view();
        // A budget far below one group's cost, so every new key must evict.
        let mut cache = TessellationCache::new(16);
        for lod in 0..8 {
            cache.begin_frame();
            cache.get_or_build(&v, 0, lod, 1e-4);
        }
        let stats = cache.stats();
        assert!(stats.evictions > 0, "nothing was ever evicted");
        // The most recent frame's entry survives whatever the budget says.
        assert!(cache.contains(&GroupKey {
            id: 0,
            serial: 1,
            lod: 7
        }));
    }

    #[test]
    fn entries_used_this_frame_are_never_evicted() {
        let s = stream_with(1);
        let v = s.view();
        let mut cache = TessellationCache::new(1);
        cache.begin_frame();
        // Three lookups inside one frame: all three must survive it, even
        // though each one alone blows the budget.
        for lod in 0..3 {
            cache.get_or_build(&v, 0, lod, 1e-4);
        }
        assert_eq!(cache.stats().entries, 3);
        assert_eq!(cache.stats().evictions, 0);
    }

    #[test]
    fn clearing_drops_everything() {
        let s = stream_with(1);
        let v = s.view();
        let mut cache = TessellationCache::default();
        cache.begin_frame();
        cache.get_or_build(&v, 0, 0, 1e-4);
        assert_eq!(cache.stats().entries, 1);
        cache.clear();
        assert_eq!(cache.stats().entries, 0);
        assert_eq!(cache.stats().vertices, 0);
    }
}
