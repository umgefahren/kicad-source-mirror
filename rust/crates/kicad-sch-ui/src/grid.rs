// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Grid spacings and display units.
//!
//! Schematic work is done on a 50 mil grid because that is the pin pitch KiCad's
//! symbol libraries are drawn on; everything else here exists so a user can say
//! so in whichever unit they think in.

use crate::input::WorldPoint;

/// Millimetres in one mil, exactly.
pub const MM_PER_MIL: f64 = 0.0254;

/// How coordinates are displayed.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Units {
    /// Millimetres.
    Millimetres,
    /// Thousandths of an inch. The unit the symbol libraries are drawn in.
    Mils,
    /// Inches.
    Inches,
}

impl Units {
    /// The suffix shown after a number.
    pub fn suffix(self) -> &'static str {
        match self {
            Units::Millimetres => "mm",
            Units::Mils => "mil",
            Units::Inches => "in",
        }
    }

    /// Convert a millimetre value into this unit.
    pub fn from_mm(self, mm: f64) -> f64 {
        match self {
            Units::Millimetres => mm,
            Units::Mils => mm / MM_PER_MIL,
            Units::Inches => mm / 25.4,
        }
    }

    /// How many digits after the point read sensibly in this unit.
    pub fn decimals(self) -> usize {
        match self {
            Units::Millimetres => 3,
            Units::Mils => 1,
            Units::Inches => 4,
        }
    }

    /// Format a millimetre value for the status bar.
    pub fn format(self, mm: f64) -> String {
        format!("{:.*}", self.decimals(), self.from_mm(mm))
    }

    /// The next unit in the cycle the `Switch Units` command walks.
    pub fn next(self) -> Self {
        match self {
            Units::Millimetres => Units::Mils,
            Units::Mils => Units::Inches,
            Units::Inches => Units::Millimetres,
        }
    }
}

/// One selectable grid spacing.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GridSize {
    /// Spacing in millimetres.
    pub mm: f64,
    /// How it is written in the status bar and the grid menu.
    pub label: &'static str,
}

/// The schematic grid ladder, coarsest first, matching eeschema's own list.
pub static GRID_SIZES: &[GridSize] = &[
    GridSize {
        mm: 100.0 * MM_PER_MIL,
        label: "100 mil",
    },
    GridSize {
        mm: 50.0 * MM_PER_MIL,
        label: "50 mil",
    },
    GridSize {
        mm: 25.0 * MM_PER_MIL,
        label: "25 mil",
    },
    GridSize {
        mm: 10.0 * MM_PER_MIL,
        label: "10 mil",
    },
    GridSize {
        mm: 1.0,
        label: "1.0 mm",
    },
    GridSize {
        mm: 0.5,
        label: "0.5 mm",
    },
    GridSize {
        mm: 0.25,
        label: "0.25 mm",
    },
];

/// Index of the default 50 mil grid within [`GRID_SIZES`].
pub const DEFAULT_GRID: usize = 1;

/// The grid as the shell holds it: which spacing, and whether it is drawn.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct GridState {
    index: usize,
    visible: bool,
}

impl Default for GridState {
    fn default() -> Self {
        Self {
            index: DEFAULT_GRID,
            visible: true,
        }
    }
}

impl GridState {
    /// The current spacing.
    pub fn size(&self) -> GridSize {
        GRID_SIZES[self.index.min(GRID_SIZES.len() - 1)]
    }

    /// Spacing in millimetres.
    pub fn spacing_mm(&self) -> f64 {
        self.size().mm
    }

    /// Whether the grid is drawn. Snapping is the host's business and is not
    /// affected by this.
    pub fn is_visible(&self) -> bool {
        self.visible
    }

    /// Show or hide the grid.
    pub fn set_visible(&mut self, visible: bool) {
        self.visible = visible;
    }

    /// Flip the grid's visibility and report the new state.
    pub fn toggle_visible(&mut self) -> bool {
        self.visible = !self.visible;
        self.visible
    }

    /// Step to the next spacing, wrapping.
    pub fn cycle(&mut self) {
        self.index = (self.index + 1) % GRID_SIZES.len();
    }

    /// Snap a world point to the nearest grid intersection.
    pub fn snap(&self, point: WorldPoint) -> WorldPoint {
        let spacing = self.spacing_mm();
        if spacing <= 0. {
            return point;
        }
        WorldPoint::new(
            (point.x / spacing).round() * spacing,
            (point.y / spacing).round() * spacing,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_mil_is_25_4_micrometres() {
        assert!((Units::Mils.from_mm(2.54) - 100.0).abs() < 1e-9);
        assert!((Units::Inches.from_mm(25.4) - 1.0).abs() < 1e-9);
        assert_eq!(Units::Millimetres.from_mm(3.5), 3.5);
    }

    #[test]
    fn formatting_uses_a_sensible_number_of_digits() {
        assert_eq!(Units::Millimetres.format(1.27), "1.270");
        assert_eq!(Units::Mils.format(1.27), "50.0");
        assert_eq!(Units::Inches.format(25.4), "1.0000");
    }

    #[test]
    fn units_cycle_back_to_the_start() {
        let mut unit = Units::Millimetres;
        for _ in 0..3 {
            unit = unit.next();
        }
        assert_eq!(unit, Units::Millimetres);
    }

    #[test]
    fn the_default_grid_is_fifty_mil() {
        let grid = GridState::default();
        assert_eq!(grid.size().label, "50 mil");
        assert!((grid.spacing_mm() - 1.27).abs() < 1e-12);
        assert!(grid.is_visible());
    }

    #[test]
    fn cycling_the_grid_wraps() {
        let mut grid = GridState::default();
        for _ in 0..GRID_SIZES.len() {
            grid.cycle();
        }
        assert_eq!(grid.size().label, "50 mil");
    }

    #[test]
    fn snapping_lands_on_the_grid() {
        let grid = GridState::default();
        let snapped = grid.snap(WorldPoint::new(1.3, -2.6));
        assert!((snapped.x - 1.27).abs() < 1e-9, "{snapped:?}");
        assert!((snapped.y + 2.54).abs() < 1e-9, "{snapped:?}");
    }

    #[test]
    fn hiding_the_grid_is_reversible() {
        let mut grid = GridState::default();
        assert!(!grid.toggle_visible());
        assert!(grid.toggle_visible());
    }
}
