// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Grid spacings and display units.
//!
//! Everything here is in KiCad internal units, as `f64`, because that is what
//! the draw stream and the renderer's camera use, and a second world unit in
//! the shell would be a conversion waiting to be got wrong.
//!
//! One internal unit is **100 nm in eeschema** — not 1 nm, which is pcbnew's.
//! The recorded fixtures say so plainly: `ecc83_pp_v2.txt` reports an A4 page
//! as `2970022 x 2100072 IU`, and 297 mm at 100 nm per unit is 2 970 000.
//! Millimetres and mils exist only in [`Units`], which formats a number for the
//! status bar and is the one place a display unit is allowed to appear.
//!
//! Schematic work is done on a 50 mil grid because that is the pin pitch
//! KiCad's symbol libraries are drawn on.

use crate::input::WorldPoint;

/// Internal units in one millimetre. One unit is 100 nm.
pub const IU_PER_MM: f64 = 1.0e4;

/// Internal units in one mil (thousandth of an inch). Exact.
pub const IU_PER_MIL: f64 = 254.0;

/// Internal units in one inch. Exact.
pub const IU_PER_INCH: f64 = 254_000.0;

/// How coordinates are displayed.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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

    /// Convert internal units into this display unit.
    pub fn from_iu(self, iu: f64) -> f64 {
        match self {
            Units::Millimetres => iu / IU_PER_MM,
            Units::Mils => iu / IU_PER_MIL,
            Units::Inches => iu / IU_PER_INCH,
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

    /// Format an internal-unit value for the status bar.
    ///
    /// The only place a display unit is allowed to exist: the number goes
    /// straight into a string and is never stored.
    pub fn format(self, iu: f64) -> String {
        format!("{:.*}", self.decimals(), self.from_iu(iu))
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
    /// Spacing in internal units.
    pub iu: f64,
    /// How it is written in the status bar and the grid menu.
    pub label: &'static str,
}

/// The schematic grid ladder, coarsest first, matching eeschema's own list.
pub static GRID_SIZES: &[GridSize] = &[
    GridSize {
        iu: 100.0 * IU_PER_MIL,
        label: "100 mil",
    },
    GridSize {
        iu: 50.0 * IU_PER_MIL,
        label: "50 mil",
    },
    GridSize {
        iu: 25.0 * IU_PER_MIL,
        label: "25 mil",
    },
    GridSize {
        iu: 10.0 * IU_PER_MIL,
        label: "10 mil",
    },
    GridSize {
        iu: IU_PER_MM,
        label: "1.0 mm",
    },
    GridSize {
        iu: 0.5 * IU_PER_MM,
        label: "0.5 mm",
    },
    GridSize {
        iu: 0.25 * IU_PER_MM,
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

    /// Spacing in internal units.
    pub fn spacing_iu(&self) -> f64 {
        self.size().iu
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
        let spacing = self.spacing_iu();
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
        assert_eq!(Units::Mils.from_iu(25_400.0), 100.0);
        assert_eq!(Units::Inches.from_iu(254_000.0), 1.0);
        assert_eq!(Units::Millimetres.from_iu(35_000.0), 3.5);
    }

    #[test]
    fn formatting_uses_a_sensible_number_of_digits() {
        assert_eq!(Units::Millimetres.format(12_700.0), "1.270");
        assert_eq!(Units::Mils.format(12_700.0), "50.0");
        assert_eq!(Units::Inches.format(254_000.0), "1.0000");
    }

    /// A large board coordinate runs into the hundreds of millions of internal
    /// units, past where an `f32` can tell neighbouring units apart. The
    /// formatter has to survive that, because the status bar is where it would
    /// show first.
    #[test]
    fn a_coordinate_far_from_the_origin_still_formats_exactly() {
        let far = 1_234_567_891.0;
        assert_eq!(Units::Millimetres.format(far), "123456.789");
        assert_eq!(Units::Mils.format(far), "4860503.5");
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
        assert_eq!(grid.spacing_iu(), 12_700.0);
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
        let snapped = grid.snap(WorldPoint::new(13_000.0, -26_000.0));
        assert_eq!(snapped.x, 12_700.0);
        assert_eq!(snapped.y, -25_400.0);
    }

    #[test]
    fn hiding_the_grid_is_reversible() {
        let mut grid = GridState::default();
        assert!(!grid.toggle_visible());
        assert!(grid.toggle_visible());
    }
}
