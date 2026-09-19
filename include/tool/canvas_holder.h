/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <memory>

#include <eda_units.h>
#include <math/box2.h>
#include <math/vector2d.h>

class APP_SETTINGS_BASE;
class EDA_DRAW_PANEL_GAL;
class GRID_HELPER;
class UNITS_PROVIDER;
struct WINDOW_SETTINGS;

enum class KICURSOR;

namespace KIGFX
{
class GAL_DISPLAY_OPTIONS;
}

/**
 * What a tool that moves or configures the *view* needs from whatever owns the canvas.
 *
 * This is `SCHEMATIC_HOLDER`'s idea one level up. `COMMON_TOOLS` and `ZOOM_TOOL` are the
 * tools that pan, zoom, pick a grid and switch units, and every KiCad program registers
 * them — so unlike eeschema's tools they cannot be given an eeschema-shaped seam. What
 * they asked for instead was `EDA_DRAW_FRAME`, which is a `wxFrame`, and that is the only
 * reason a headless host could not zoom.
 *
 * `EDA_DRAW_FRAME` implements this once, so pcbnew, gerbview, the page-layout editor and
 * the symbol and footprint editors inherit it without changing. `SCH_HOST` implements it
 * too, and has no window at all.
 *
 * The rule for what belongs here is the same rule as `SCHEMATIC_HOLDER`'s: the view, the
 * preferences that decide how it behaves, and notifications a canvas *owner* can act on.
 * Anything inherently a window — the preferences dialog, the status bar, a wx event loop —
 * is either absent or defaulted to doing nothing, and a tool that wants one downcasts to
 * the frame and skips the work when the answer is null.
 */
class CANVAS_HOLDER
{
public:
    virtual ~CANVAS_HOLDER() = default;

    // ------------------------------------------------------------------ the view

    /**
     * The bounding box of everything there is to look at, in internal units.
     *
     * @param aIncludeAllVisible true for everything drawn, false for the "zoom to objects"
     *                           subset each program defines for itself.
     */
    virtual const BOX2I GetDocumentExtents( bool aIncludeAllVisible = true ) const = 0;

    /**
     * What to frame when ::GetDocumentExtents is empty, which is what an empty document
     * gives. A frame answers its canvas's page box; a holder with no canvas may have
     * nothing to say, and an empty box means "use the document extents anyway".
     */
    virtual BOX2I GetDefaultViewBBox() const { return BOX2I(); }

    // ------------------------------------------------------------------- the settings

    /**
     * The application settings, or null where there are none.
     *
     * `EDA_BASE_FRAME` declares an identical virtual, so a frame has to declare one itself
     * to say which it means.
     */
    virtual APP_SETTINGS_BASE* config() const = 0;

    /**
     * The per-window zoom and grid preferences inside \a aCfg.
     *
     * Each program stores its own, which is why this takes the settings object rather than
     * reading a fixed one.
     */
    virtual WINDOW_SETTINGS* GetWindowSettings( APP_SETTINGS_BASE* aCfg ) = 0;

    virtual KIGFX::GAL_DISPLAY_OPTIONS& GetGalDisplayOptions() = 0;

    // ------------------------------------------------------------------- the grid

    virtual const VECTOR2I& GetGridOrigin() const = 0;
    virtual void            SetGridOrigin( const VECTOR2I& aPosition ) = 0;

    /// Not `IsGridVisible()`: `EDA_DRAW_FRAME`'s is non-const and this one is.
    virtual bool GridVisible() const = 0;
    virtual void SetGridVisibility( bool aVisible ) = 0;

    /// Not `IsGridOverridden()`, for the same reason as ::GridVisible.
    virtual bool GridOverridden() const = 0;
    virtual void SetGridOverrides( bool aOverride ) = 0;

    virtual std::unique_ptr<GRID_HELPER> MakeGridHelper() = 0;

    /**
     * The grid the user is on has changed.
     *
     * A frame refreshes the grid picker on its toolbar; a holder with no toolbar has
     * nothing to do, which is why this is not pure.
     */
    virtual void OnGridSelectionChanged() {}

    // ------------------------------------------------------------------- the units

    /**
     * The units and internal scale to read and format with.
     *
     * Handed out whole rather than forwarded one accessor at a time, because
     * `UNITS_PROVIDER::GetUserUnits` and `::GetIuScale` are **not virtual**: declaring
     * pure virtuals of those names here would leave a frame with two unrelated members
     * called `GetUserUnits` in two base subobjects, and every `m_frame->GetUserUnits()`
     * in the tree would become ambiguous. A frame answers `this`.
     */
    virtual UNITS_PROVIDER* GetUnitsProvider() = 0;

    /// Not `ChangeUserUnits`: `EDA_BASE_FRAME`'s is non-virtual, per ::GetUnitsProvider.
    virtual void SwitchUserUnits( EDA_UNITS aUnits ) = 0;

    /// Not `GetShowPolarCoords`/`SetShowPolarCoords`, for the same reason.
    virtual bool PolarCoords() const { return false; }
    virtual void SetPolarCoords( bool aShow ) {}

    // --------------------------------------------------- feedback that is not a window

    /**
     * Repaint now rather than posting a paint event.
     *
     * The synchronous half is load bearing for a tool that changes what is on screen
     * inside its own event loop — a zoom-window rubber band, for one. A holder that does
     * not own the frame clock records the request instead.
     */
    virtual void ForceRefreshCanvas() {}

    /// Set the pointer's shape, to say what a click here would do.
    virtual void SetCanvasCursor( KICURSOR aCursor ) {}

    /**
     * The canvas, where there is one.
     *
     * The escape hatch for the handful of places a view tool genuinely wants the wx
     * widget rather than the view. Null on a holder with no window, and every caller
     * tests it.
     */
    virtual EDA_DRAW_PANEL_GAL* GetCanvasPanel() const { return nullptr; }

    /**
     * Rebuild the GAL and redraw. Called when something went wrong.
     *
     * Same name and signature as `EDA_DRAW_FRAME`'s own virtual, which is deliberate:
     * where both are virtual and agree, one declaration in the frame overrides both and
     * nothing is ambiguous. That is not true of the non-virtual members above, which is
     * why those are renamed and these are not.
     */
    virtual void HardRedraw() {}

    /// The cursor position readout changed. A window has a status bar to put it in.
    virtual void UpdateStatusBar() {}
};
