// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Colours: the application chrome, and the schematic canvas.
//!
//! Two palettes, because they answer to different authorities. The chrome
//! palette tunes gpui-component's theme — the same tokens every widget in the
//! library reads — so buttons, docks and popovers stay coherent for free. The
//! [`CanvasPalette`] is separate because those colours belong to KiCad's own
//! colour-theme system, which the host will eventually supply; keeping them in
//! one struct means adopting the host's theme is a matter of filling this in
//! from it rather than hunting for literals.

use gpui_kit::component::theme::{Theme, ThemeMode};
use gpui_kit::{App, Hsla, Rgba, Window, px, rgb};

/// Turn a hex literal into the `Hsla` gpui works in.
fn color(hex: u32) -> Hsla {
    let rgba: Rgba = rgb(hex);
    rgba.into()
}

/// Apply the schematic editor's look on top of gpui-component's theme.
///
/// The library ships a perfectly good neutral theme; this leans it a little
/// cooler, tightens the corner radius (a CAD tool full of small controls reads
/// as cluttered with large radii) and moves the accent onto KiCad's blue so
/// that focus rings, primary buttons and the active tool all agree.
pub fn apply(mode: ThemeMode, window: Option<&mut Window>, cx: &mut App) {
    // `change` reloads the whole token set from the registry's config, so
    // every override has to happen after it.
    Theme::change(mode, None, cx);

    let dark = mode.is_dark();
    {
        let theme = Theme::global_mut(cx);
        theme.radius = px(5.);
        theme.radius_lg = px(9.);
        theme.font_size = px(14.);
        theme.shadow = true;

        let accent = if dark {
            color(0x3f9ae5)
        } else {
            color(0x1d6fb8)
        };
        theme.colors.primary = accent;
        theme.colors.primary_hover = if dark {
            color(0x55a8ea)
        } else {
            color(0x2f80ca)
        };
        theme.colors.primary_active = if dark {
            color(0x2f86cf)
        } else {
            color(0x175c9b)
        };
        theme.colors.primary_foreground = color(0xffffff);
        theme.colors.button_primary = accent;
        theme.colors.button_primary_hover = theme.colors.primary_hover;
        theme.colors.button_primary_active = theme.colors.primary_active;
        theme.colors.button_primary_foreground = color(0xffffff);
        theme.colors.ring = accent;
        theme.colors.link = accent;
        theme.colors.selection = accent.opacity(0.28);

        if dark {
            theme.colors.background = color(0x14181e);
            theme.colors.foreground = color(0xdfe4ec);
            theme.colors.border = color(0x252c36);
            theme.colors.muted = color(0x1b2027);
            theme.colors.muted_foreground = color(0x8b95a4);
            theme.colors.popover = color(0x1a1f26);
            theme.colors.title_bar = color(0x11151a);
            theme.colors.title_bar_border = color(0x252c36);
            theme.colors.status_bar = color(0x11151a);
            theme.colors.status_bar_border = color(0x252c36);
            theme.colors.sidebar = color(0x171c23);
            theme.colors.sidebar_border = color(0x252c36);
            theme.colors.sidebar_foreground = color(0xc8d0da);
            theme.colors.tab_bar = color(0x11151a);
            theme.colors.tab = color(0x11151a);
            theme.colors.tab_active = color(0x1a1f26);
            theme.colors.list = color(0x14181e);
            theme.colors.list_hover = color(0x1e242c);
            theme.colors.list_active = accent.opacity(0.22);
            theme.colors.accent = color(0x1e242c);
            theme.colors.accent_foreground = color(0xdfe4ec);
            theme.colors.secondary = color(0x1e242c);
            theme.colors.secondary_hover = color(0x262d37);
            theme.colors.secondary_active = color(0x2e3641);
            theme.colors.secondary_foreground = color(0xdfe4ec);
            theme.colors.button = color(0x1e242c);
            theme.colors.button_hover = color(0x262d37);
            theme.colors.button_active = color(0x2e3641);
            theme.colors.button_foreground = color(0xdfe4ec);
            theme.colors.input = color(0x0e1116);
        } else {
            theme.colors.background = color(0xfbfcfd);
            theme.colors.foreground = color(0x1c2530);
            theme.colors.border = color(0xdbe1e8);
            theme.colors.muted = color(0xf1f4f7);
            theme.colors.muted_foreground = color(0x687380);
            theme.colors.popover = color(0xffffff);
            theme.colors.title_bar = color(0xf2f5f8);
            theme.colors.title_bar_border = color(0xdbe1e8);
            theme.colors.status_bar = color(0xf2f5f8);
            theme.colors.status_bar_border = color(0xdbe1e8);
            theme.colors.sidebar = color(0xf4f6f9);
            theme.colors.sidebar_border = color(0xdbe1e8);
            theme.colors.sidebar_foreground = color(0x2b3440);
            theme.colors.tab_bar = color(0xf2f5f8);
            theme.colors.tab = color(0xf2f5f8);
            theme.colors.tab_active = color(0xffffff);
            theme.colors.list = color(0xfbfcfd);
            theme.colors.list_hover = color(0xeef2f6);
            theme.colors.list_active = accent.opacity(0.16);
            theme.colors.accent = color(0xeef2f6);
            theme.colors.accent_foreground = color(0x1c2530);
            theme.colors.secondary = color(0xeef2f6);
            theme.colors.secondary_hover = color(0xe4eaf1);
            theme.colors.secondary_active = color(0xd9e1ea);
            theme.colors.secondary_foreground = color(0x1c2530);
            theme.colors.button = color(0xffffff);
            theme.colors.button_hover = color(0xeef2f6);
            theme.colors.button_active = color(0xe4eaf1);
            theme.colors.button_foreground = color(0x1c2530);
            theme.colors.input = color(0xffffff);
        }
    }

    // The scrollbar and the resize handles live in gpui-base and read a
    // projection of the theme, which only refreshes here.
    Theme::sync_base(cx);
    if let Some(window) = window {
        window.refresh();
    }
}

/// The colours the schematic canvas draws with.
///
/// Deliberately a plain value with no gpui-component dependency: this is the
/// struct `kicad-sch-render` will be handed, and eventually the one KiCad's own
/// `COLOR_SETTINGS` will be converted into.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CanvasPalette {
    /// Behind everything.
    pub background: Hsla,
    /// Ordinary grid points.
    pub grid: Hsla,
    /// Every tenth grid point, so the eye can count.
    pub grid_major: Hsla,
    /// The sheet border and its title block rules.
    pub sheet_border: Hsla,
    /// Inside the sheet border, when it differs from the background.
    pub sheet_fill: Hsla,
    /// Wires.
    pub wire: Hsla,
    /// Buses.
    pub bus: Hsla,
    /// Junction dots.
    pub junction: Hsla,
    /// Symbol body outlines.
    pub symbol_outline: Hsla,
    /// Symbol body fill.
    pub symbol_fill: Hsla,
    /// Pins and pin numbers.
    pub pin: Hsla,
    /// Net labels and hierarchical labels.
    pub label: Hsla,
    /// Reference designators and values.
    pub field: Hsla,
    /// Free text and notes.
    pub text: Hsla,
    /// No-connect markers and error flags.
    pub no_connect: Hsla,
    /// Selection outline.
    pub selection: Hsla,
    /// The full-window crosshair.
    pub crosshair: Hsla,
}

impl CanvasPalette {
    /// The dark canvas.
    pub fn dark() -> Self {
        Self {
            background: color(0x0f1318),
            grid: color(0x232a33),
            grid_major: color(0x313a46),
            sheet_border: color(0x46505d),
            sheet_fill: color(0x121720),
            wire: color(0x2fc98a),
            bus: color(0x4b90f7),
            junction: color(0x2fc98a),
            symbol_outline: color(0xd3785c),
            symbol_fill: color(0x1d1a1c),
            pin: color(0xe0685f),
            label: color(0x9ecbff),
            field: color(0x6fd3c8),
            text: color(0xc9d2de),
            no_connect: color(0xe0685f),
            selection: color(0x59b0ff),
            crosshair: color(0x4d5866),
        }
    }

    /// The light canvas, close to eeschema's traditional colours.
    pub fn light() -> Self {
        Self {
            background: color(0xffffff),
            grid: color(0xdfe4ea),
            grid_major: color(0xc7cfd8),
            sheet_border: color(0x7a838f),
            sheet_fill: color(0xfdfdfd),
            wire: color(0x008e4a),
            bus: color(0x0b4fc0),
            junction: color(0x008e4a),
            symbol_outline: color(0x9b2d1c),
            symbol_fill: color(0xfff8f5),
            pin: color(0xb33a2c),
            label: color(0x14508f),
            field: color(0x0f7d78),
            text: color(0x222b34),
            no_connect: color(0xb33a2c),
            selection: color(0x1d6fb8),
            crosshair: color(0xa8b2bd),
        }
    }

    /// The palette matching a theme mode.
    pub fn for_mode(mode: ThemeMode) -> Self {
        if mode.is_dark() {
            Self::dark()
        } else {
            Self::light()
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_conversion_preserves_the_channels() {
        let white = color(0xffffff);
        assert!(white.l > 0.99);
        let black = color(0x000000);
        assert!(black.l < 0.01);
    }

    #[test]
    fn the_two_canvas_palettes_differ_in_lightness_where_it_matters() {
        let dark = CanvasPalette::dark();
        let light = CanvasPalette::light();
        assert!(dark.background.l < 0.2);
        assert!(light.background.l > 0.9);
        // Text has to be legible against its own background in both.
        assert!((dark.text.l - dark.background.l).abs() > 0.4);
        assert!((light.text.l - light.background.l).abs() > 0.4);
    }

    #[test]
    fn palette_follows_the_mode() {
        assert_eq!(
            CanvasPalette::for_mode(ThemeMode::Dark),
            CanvasPalette::dark()
        );
        assert_eq!(
            CanvasPalette::for_mode(ThemeMode::Light),
            CanvasPalette::light()
        );
    }
}
