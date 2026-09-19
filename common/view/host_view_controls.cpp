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

#include <algorithm>

#include <geometry/geometry_utils.h>
#include <gal/graphics_abstraction_layer.h>
#include <math/util.h> // for KiROUND
#include <view/host_view_controls.h>
#include <view/view.h>

using namespace KIGFX;


HOST_VIEW_CONTROLS::HOST_VIEW_CONTROLS( VIEW* aView ) :
        VIEW_CONTROLS( aView ),
        m_pointerOverCanvas( false ),
        m_warpPending( false )
{
}


void HOST_VIEW_CONTROLS::SetPointerPosition( const VECTOR2D& aScreenPosition )
{
    m_pointerScreen = aScreenPosition;
    m_pointerOverCanvas = true;

    // The cursor follows the pointer. A forced position still wins, because
    // GetCursorPosition() consults m_settings.m_forcedPosition before this, which is
    // the same order WX_VIEW_CONTROLS uses — the force outranks motion rather than
    // being overwritten by it.
    m_cursorPos = GetClampedCoords( m_view->ToWorld( m_pointerScreen ) );

    // The pointer moving is the "next mouse motion" that SetCursorPosition()'s
    // contract says overrides a placed cursor, so a warp is no longer outstanding.
    m_cursorWarped = false;
}


void HOST_VIEW_CONTROLS::PointerLeft()
{
    m_pointerOverCanvas = false;
}


bool HOST_VIEW_CONTROLS::WarpRequestPending()
{
    bool pending = m_warpPending;

    m_warpPending = false;

    return pending;
}


VECTOR2D HOST_VIEW_CONTROLS::GetMousePosition( bool aWorldCoordinates ) const
{
    if( aWorldCoordinates )
        return GetClampedCoords( m_view->ToWorld( m_pointerScreen ) );

    return m_pointerScreen;
}


VECTOR2D HOST_VIEW_CONTROLS::GetRawCursorPosition( bool aEnableSnapping ) const
{
    GAL* gal = m_view->GetGAL();

    if( aEnableSnapping && gal->GetGridSnapping() )
        return gal->GetGridPoint( m_cursorPos );

    return m_cursorPos;
}


VECTOR2D HOST_VIEW_CONTROLS::GetCursorPosition( bool aEnableSnapping ) const
{
    if( m_settings.m_forceCursorPosition )
        return m_settings.m_forcedPosition;

    return GetClampedCoords( GetRawCursorPosition( aEnableSnapping ) );
}


void HOST_VIEW_CONTROLS::SetCursorPosition( const VECTOR2D& aPosition, bool aWarpView,
                                            bool aTriggeredByArrows, long aArrowCommand )
{
    VECTOR2D clamped = GetClampedCoords( aPosition );

    // Arrow-key movement is remembered so that COMMON_TOOLS can accumulate a run of
    // keystrokes from where the last one landed rather than from the pointer, which
    // has not moved. Anything else invalidates that memory. Same bookkeeping as
    // WX_VIEW_CONTROLS::SetCursorPosition (common/view/wx_view_controls.cpp:925).
    if( aTriggeredByArrows )
    {
        m_settings.m_lastKeyboardCursorPositionValid = true;
        m_settings.m_lastKeyboardCursorPosition = clamped;
        m_settings.m_lastKeyboardCursorCommand = aArrowCommand;
        m_cursorWarped = false;
    }
    else
    {
        m_settings.m_lastKeyboardCursorPositionValid = false;
        m_settings.m_lastKeyboardCursorPosition = { 0.0, 0.0 };
        m_settings.m_lastKeyboardCursorCommand = 0;
        m_cursorWarped = true;
    }

    WarpMouseCursor( clamped, true, aWarpView );

    m_cursorPos = clamped;
}


void HOST_VIEW_CONTROLS::SetCrossHairCursorPosition( const VECTOR2D& aPosition, bool aWarpView )
{
    VECTOR2D clamped = GetClampedCoords( aPosition );

    warpViewTo( clamped, aWarpView );

    m_cursorPos = clamped;
}


void HOST_VIEW_CONTROLS::WarpMouseCursor( const VECTOR2D& aPosition, bool aWorldCoordinates,
                                          bool aWarpView )
{
    // This is the one method of the interface that needs a capability a non-window
    // host may not have: moving the operating system's pointer. Doing the half that
    // is possible — the view, and our idea of where the cursor is — keeps every
    // caller's postcondition except "the pointer is now under the cursor", and
    // records the request so a host that can move its pointer may honour it.
    VECTOR2D world = aWorldCoordinates ? GetClampedCoords( aPosition )
                                       : GetClampedCoords( m_view->ToWorld( aPosition ) );

    warpViewTo( world, aWarpView );

    m_cursorPos = world;
    m_warpRequest = world;
    m_warpPending = true;
}


void HOST_VIEW_CONTROLS::CenterOnCursor()
{
    // Qualified because declaring the one-argument override hides the base's
    // no-argument overload, which is the one that reads the snapping setting.
    VECTOR2D cursor = VIEW_CONTROLS::GetCursorPosition();

    m_view->SetCenter( cursor );

    // WX_VIEW_CONTROLS also warps the pointer to the middle of the canvas, which is
    // where the cursor now is. Requesting it keeps the two in step for a host that
    // can; for one that cannot, the cursor is already correct and only the pointer
    // is in the wrong place, which the next motion event fixes.
    m_warpRequest = cursor;
    m_warpPending = true;
}


void HOST_VIEW_CONTROLS::PinCursorInsideNonAutoscrollArea( bool aWarpMouseCursor )
{
    // Verbatim from WX_VIEW_CONTROLS: the autopan margin, plus two pixels, in from
    // each edge. The wx version exists so that an autopan that has just scrolled the
    // view does not immediately re-trigger because the pointer is still in the
    // margin. Autopan is not implemented here, so nothing calls this for that
    // reason — but VIEW_CONTROLS' contract is "clamp the cursor into the safe
    // area", which is well defined without a pointer, so it is implemented rather
    // than stubbed.
    const VECTOR2I& screenSize = m_view->GetScreenPixelSize();

    double border = std::min( m_settings.m_autoPanMargin * screenSize.x,
                              m_settings.m_autoPanMargin * screenSize.y )
                    + 2.0;

    VECTOR2D topLeft = m_view->ToWorld( VECTOR2D( border, border ) );
    VECTOR2D botRight = m_view->ToWorld( VECTOR2D( screenSize.x - border, screenSize.y - border ) );

    VECTOR2D pos = GetMousePosition( true );

    pos.x = std::clamp( pos.x, std::min( topLeft.x, botRight.x ), std::max( topLeft.x, botRight.x ) );
    pos.y = std::clamp( pos.y, std::min( topLeft.y, botRight.y ), std::max( topLeft.y, botRight.y ) );

    SetCursorPosition( pos, false, false, 0 );

    if( aWarpMouseCursor )
        WarpMouseCursor( pos, true );
}


void HOST_VIEW_CONTROLS::ForceCursorPosition( bool aEnabled, const VECTOR2D& aPosition )
{
    // Overridden only to clamp, as WX_VIEW_CONTROLS does
    // (common/view/wx_view_controls.cpp:1227). The base implementation stores the
    // position raw, and GetCursorPosition() then hands a forced position back
    // *without* the clamp every other path in this class goes through — so a tool
    // forcing a position outside the coordinate range would give the tools a value
    // that overflows on its first conversion to a VECTOR2I. The interface's own
    // contract says the position is clamped.
    m_settings.m_forceCursorPosition = aEnabled;
    m_settings.m_forcedPosition = GetClampedCoords( aPosition );
}


void HOST_VIEW_CONTROLS::Reset()
{
    VIEW_CONTROLS::Reset();

    m_warpPending = false;
}


void HOST_VIEW_CONTROLS::warpViewTo( const VECTOR2D& aWorldPosition, bool aWarpView )
{
    if( !aWarpView )
        return;

    // Only when the point is not already on screen, matching WX_VIEW_CONTROLS. A view
    // that recentred on every request would jump under the user for no reason.
    BOX2I    screen( VECTOR2I( 0, 0 ), m_view->GetScreenPixelSize() );
    VECTOR2D screenPos = m_view->ToScreen( aWorldPosition );

    if( !screen.Contains( VECTOR2I( KiROUND( screenPos.x ), KiROUND( screenPos.y ) ) ) )
        m_view->SetCenter( aWorldPosition );
}
