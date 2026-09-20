/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef KICAD_VIEW_HOST_VIEW_CONTROLS_H
#define KICAD_VIEW_HOST_VIEW_CONTROLS_H

#include <view/view_controls.h>

namespace KIGFX
{

/**
 * A KIGFX::VIEW_CONTROLS for a host that owns its own event loop and pointer.
 *
 * WX_VIEW_CONTROLS answers "where is the pointer?" by polling the operating
 * system (`KIPLATFORM::UI::GetMousePosition()` plus `ScreenToClient`) and
 * answers "put the pointer there" by moving it (`WarpPointer`). Neither is
 * available to a host that is not a wxWindow — a Rust UI hands us input events
 * and has no pointer API we can call back into — so this implementation is
 * *told* where the pointer is and keeps it.
 *
 * Everything else the interface promises is honoured exactly as
 * WX_VIEW_CONTROLS honours it: snapping goes through `GAL::GetGridPoint`, a
 * forced position wins over the pointer, and every world coordinate handed out
 * is clamped by `GetClampedCoords` so a tool cannot be given a position outside
 * the coordinate system.
 *
 * ## The one capability that is genuinely missing
 *
 * `WarpMouseCursor()` moves the operating system's pointer. A host without that
 * capability — gpui has no pointer-warping API — cannot, so this class does the
 * half it can: it adopts the requested position as the cursor position, moves
 * the view if asked to, and records that a warp was wanted
 * (::WarpRequestPending). A host that *can* move its pointer should consume
 * that and do so; a host that cannot loses only the pointer-follows-cursor
 * behaviour, and the survey's prediction holds — with the crosshair drawn at
 * `GetCursorPosition()` rather than at the pointer, the tools that warp still
 * read back the position they asked for.
 *
 * That is why `SetCursorPosition()` works at all here: it is specified as
 * "place the cursor and let the next mouse motion override it", and the next
 * ::SetPointerPosition from the host is exactly that override.
 *
 * ## Threading
 *
 * Not thread safe, like everything else the view owns.
 */
class GAL_API HOST_VIEW_CONTROLS : public VIEW_CONTROLS
{
public:
    explicit HOST_VIEW_CONTROLS( VIEW* aView );

    ~HOST_VIEW_CONTROLS() override = default;

    // ------------------------------------------------------ the host's input

    /**
     * Report where the host's pointer is, in screen (pixel) coordinates
     * relative to the top-left of the canvas.
     *
     * This is the only way position enters, and it replaces WX_VIEW_CONTROLS'
     * poll of the OS pointer. Call it before dispatching an input event that
     * carries a position, because HOST_TOOL_DISPATCHER reads the position back
     * out of the view controls rather than out of the event — which is what
     * TOOL_DISPATCHER does too, and is what lets a forced or warped position
     * reach the tools.
     */
    void SetPointerPosition( const VECTOR2D& aScreenPosition );

    /**
     * Forget where the pointer is, because it left the canvas.
     *
     * The last known position is kept — a tool asking mid-gesture should not
     * get the origin — but ::PointerIsOverCanvas reports false, so a host can
     * stop drawing a crosshair.
     */
    void PointerLeft();

    /** True while the host has reported a pointer position and not withdrawn it. */
    bool PointerIsOverCanvas() const { return m_pointerOverCanvas; }

    /**
     * Whether a tool asked for the pointer to be moved and nothing did it.
     *
     * Reading it clears it. A host with no pointer-warping capability can
     * ignore this entirely; the view and the cursor position have already been
     * updated either way.
     */
    bool WarpRequestPending();

    /** Where the last unhonoured warp wanted the pointer, in world coordinates. */
    VECTOR2D WarpRequestPosition() const { return m_warpRequest; }

    // ------------------------------------------------- VIEW_CONTROLS overrides

    VECTOR2D GetMousePosition( bool aWorldCoordinates = true ) const override;
    VECTOR2D GetRawCursorPosition( bool aSnappingEnabled = true ) const override;
    VECTOR2D GetCursorPosition( bool aEnableSnapping ) const override;

    void SetCursorPosition( const VECTOR2D& aPosition, bool aWarpView = true,
                            bool aTriggeredByArrows = false, long aArrowCommand = 0 ) override;

    void SetCrossHairCursorPosition( const VECTOR2D& aPosition, bool aWarpView = true ) override;

    void WarpMouseCursor( const VECTOR2D& aPosition, bool aWorldCoordinates = false,
                          bool aWarpView = false ) override;

    void CenterOnCursor() override;

    void PinCursorInsideNonAutoscrollArea( bool aWarpMouseCursor ) override;

    void ForceCursorPosition( bool                aEnabled,
                              const VECTOR2D& aPosition = VECTOR2D( 0, 0 ) ) override;

    void Reset() override;

private:
    /// Move the view so that a world point is visible, as WX_VIEW_CONTROLS does:
    /// only when it is off screen, and only when the caller asked for it.
    void warpViewTo( const VECTOR2D& aWorldPosition, bool aWarpView );

    /// The pointer, in screen pixels. The authoritative input; everything else derives.
    VECTOR2D m_pointerScreen;

    /// The cursor, in world coordinates, unsnapped. Follows the pointer unless a
    /// tool has placed it somewhere with SetCursorPosition().
    VECTOR2D m_cursorPos;

    bool m_pointerOverCanvas;

    bool     m_warpPending;
    VECTOR2D m_warpRequest;
};

} // namespace KIGFX

#endif // KICAD_VIEW_HOST_VIEW_CONTROLS_H
