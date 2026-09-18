// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Frame pacing measurement.
//!
//! The target is 120 Hz, which is an 8.33 ms budget. A target nobody measures
//! is a wish, so the shell keeps a rolling window of inter-frame intervals and
//! can put the worst of them on the status bar. The readout deliberately shows
//! the 99th percentile as well as the mean: a mean of 8 ms with a 40 ms tail is
//! a stutter the user sees and an average hides.

use std::collections::VecDeque;
use std::time::{Duration, Instant};

/// The frame interval that meets the 120 Hz target, in milliseconds.
pub const TARGET_FRAME_MS: f32 = 1000.0 / 120.0;

/// A rolling window of frame intervals.
pub struct FrameStats {
    last: Option<Instant>,
    samples: VecDeque<f32>,
    capacity: usize,
    enabled: bool,
    /// Frames observed since the stats were created, whether or not the
    /// readout is shown. Lets a smoke test assert the app is really animating.
    total: u64,
}

impl Default for FrameStats {
    fn default() -> Self {
        Self::new(180)
    }
}

impl FrameStats {
    /// A window of `capacity` frames — 180 is about 1.5 s at 120 Hz.
    pub fn new(capacity: usize) -> Self {
        Self {
            last: None,
            samples: VecDeque::with_capacity(capacity.max(1)),
            capacity: capacity.max(1),
            enabled: false,
            total: 0,
        }
    }

    /// Whether the readout is shown. Measurement runs either way, so turning
    /// it on never shows an empty box.
    pub fn is_enabled(&self) -> bool {
        self.enabled
    }

    /// Show or hide the readout.
    pub fn set_enabled(&mut self, enabled: bool) {
        self.enabled = enabled;
    }

    /// Flip the readout and report the new state.
    pub fn toggle(&mut self) -> bool {
        self.enabled = !self.enabled;
        self.enabled
    }

    /// Frames counted since construction.
    pub fn frame_count(&self) -> u64 {
        self.total
    }

    /// Record a frame boundary. Call once per `render`.
    pub fn tick(&mut self) {
        self.tick_at(Instant::now());
    }

    /// Record a frame boundary at a given instant, so tests can supply a clock.
    pub fn tick_at(&mut self, now: Instant) {
        self.total += 1;
        if let Some(last) = self.last {
            let delta = now.saturating_duration_since(last);
            self.push(delta);
        }
        self.last = Some(now);
    }

    fn push(&mut self, delta: Duration) {
        let ms = delta.as_secs_f32() * 1000.0;
        // A frame gap of a second or more means the window was occluded or the
        // process was stopped, not that rendering is slow. Counting it would
        // poison the window for the next second and a half.
        if ms > 1000.0 {
            self.samples.clear();
            return;
        }
        if self.samples.len() == self.capacity {
            self.samples.pop_front();
        }
        self.samples.push_back(ms);
    }

    /// How many intervals are in the window.
    pub fn len(&self) -> usize {
        self.samples.len()
    }

    /// Whether anything has been measured yet.
    pub fn is_empty(&self) -> bool {
        self.samples.is_empty()
    }

    /// Mean frame interval in milliseconds.
    pub fn mean_ms(&self) -> Option<f32> {
        if self.samples.is_empty() {
            return None;
        }
        Some(self.samples.iter().sum::<f32>() / self.samples.len() as f32)
    }

    /// The `q`-quantile of the frame interval, `q` in 0..=1.
    pub fn quantile_ms(&self, q: f32) -> Option<f32> {
        if self.samples.is_empty() {
            return None;
        }
        let mut sorted: Vec<f32> = self.samples.iter().copied().collect();
        sorted.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
        let index = ((sorted.len() - 1) as f32 * q.clamp(0., 1.)).round() as usize;
        Some(sorted[index])
    }

    /// Frames per second implied by the mean interval.
    pub fn fps(&self) -> Option<f32> {
        self.mean_ms()
            .filter(|ms| *ms > 0.)
            .map(|ms| 1000.0 / ms)
    }

    /// Whether the window is inside the 120 Hz budget at the 99th percentile.
    pub fn meets_target(&self) -> bool {
        self.quantile_ms(0.99)
            .map(|ms| ms <= TARGET_FRAME_MS * 1.05)
            .unwrap_or(false)
    }

    /// The status-bar text, or `None` when nothing has been measured.
    pub fn readout(&self) -> Option<String> {
        let mean = self.mean_ms()?;
        let p99 = self.quantile_ms(0.99)?;
        let fps = self.fps()?;
        Some(format!("{fps:.0} fps  {mean:.1}/{p99:.1} ms"))
    }

    /// Forget the window, for instance after a resize storm.
    pub fn reset(&mut self) {
        self.samples.clear();
        self.last = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn stats_with(intervals: &[u64]) -> FrameStats {
        let mut stats = FrameStats::new(180);
        let mut now = Instant::now();
        stats.tick_at(now);
        for ms in intervals {
            now += Duration::from_millis(*ms);
            stats.tick_at(now);
        }
        stats
    }

    #[test]
    fn a_fresh_window_reports_nothing() {
        let stats = FrameStats::default();
        assert!(stats.is_empty());
        assert!(stats.readout().is_none());
        assert!(!stats.meets_target());
    }

    #[test]
    fn the_first_tick_is_a_boundary_not_an_interval() {
        let mut stats = FrameStats::default();
        stats.tick();
        assert_eq!(stats.len(), 0);
        assert_eq!(stats.frame_count(), 1);
    }

    #[test]
    fn eight_millisecond_frames_meet_the_target() {
        let stats = stats_with(&[8, 8, 8, 8, 8, 8, 8, 8]);
        assert!(stats.meets_target());
        let fps = stats.fps().expect("measured");
        assert!((fps - 125.0).abs() < 1.0, "{fps}");
    }

    /// Two stalls in a hundred frames, not one: the 99th percentile of a
    /// hundred samples is the second-worst by construction, so a single
    /// outlier is exactly the tail p99 is meant to ignore.
    #[test]
    fn a_long_tail_fails_the_target_even_with_a_good_mean() {
        let mut intervals = vec![4u64; 98];
        intervals.push(60);
        intervals.push(60);
        let stats = stats_with(&intervals);
        assert!(stats.mean_ms().expect("measured") < TARGET_FRAME_MS);
        assert!(
            !stats.meets_target(),
            "a 60 ms stall has to fail, p99 = {:?}",
            stats.quantile_ms(0.99)
        );
    }

    #[test]
    fn the_window_is_bounded() {
        let mut stats = FrameStats::new(8);
        let mut now = Instant::now();
        for _ in 0..50 {
            stats.tick_at(now);
            now += Duration::from_millis(8);
        }
        assert_eq!(stats.len(), 8);
    }

    #[test]
    fn a_suspended_window_does_not_poison_the_measurements() {
        let mut stats = FrameStats::new(16);
        let mut now = Instant::now();
        stats.tick_at(now);
        for _ in 0..4 {
            now += Duration::from_millis(8);
            stats.tick_at(now);
        }
        now += Duration::from_secs(30);
        stats.tick_at(now);
        assert!(stats.is_empty(), "the outlier should clear the window");
    }

    #[test]
    fn the_readout_mentions_fps_and_both_timings() {
        let stats = stats_with(&[8, 9, 8, 10, 8]);
        let text = stats.readout().expect("measured");
        assert!(text.contains("fps"), "{text}");
        assert!(text.contains("ms"), "{text}");
        assert!(text.contains('/'), "{text}");
    }

    #[test]
    fn toggling_reports_the_new_state() {
        let mut stats = FrameStats::default();
        assert!(stats.toggle());
        assert!(stats.is_enabled());
        assert!(!stats.toggle());
    }
}
