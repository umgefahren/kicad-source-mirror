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

/**
 * @file
 * The input seam for a host that is not wxWidgets: HOST_VIEW_CONTROLS and
 * HOST_TOOL_DISPATCHER.
 *
 * Testing the wx dispatcher means synthesising `wxEvent`s and owning a window.
 * Testing this one means calling a method with a struct, which is most of the
 * point of writing it: the whole contract — which `TOOL_EVENT` comes out of which
 * input, carrying which position — is asserted here with no GUI, no display and
 * no event loop.
 *
 * The events are checked through a tool that records everything it is offered,
 * rather than by inspecting the dispatcher, so what is asserted is what a real
 * tool would actually see.
 */

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>
#include <vector>

#include <gal/gal_display_options.h>
#include <gal/recording/recording_gal.h>
#include <tool/host_tool_dispatcher.h>
#include <tool/tool_interactive.h>
#include <tool/tool_manager.h>
#include <tool/tools_holder.h>
#include <view/host_view_controls.h>
#include <view/view.h>

#include <wx/defs.h>

using namespace KIGFX;


namespace
{

/**
 * The minimum a non-wx host has to be. `GetToolCanvas()` is `TOOLS_HOLDER`'s only
 * pure virtual and returning null from it is already a production state —
 * `SIMULATOR_FRAME` and `MERGETOOL_FRAME` both do, and `TOOL_DISPATCHER`
 * null-checks it. Same double as `qa/tests/eeschema/test_non_frame_tools_holder.cpp`.
 */
class NON_FRAME_HOLDER : public TOOLS_HOLDER
{
public:
    wxWindow* GetToolCanvas() const override { return nullptr; }
};


/**
 * A tool that records every event offered to it.
 *
 * It uses a transition rather than an `Activate()`/`Wait()` loop so that it sees
 * events without first being activated: the question here is what the dispatcher
 * produces, not how a tool's coroutine consumes it.
 */
class EVENT_RECORDER_TOOL : public TOOL_INTERACTIVE
{
public:
    EVENT_RECORDER_TOOL() : TOOL_INTERACTIVE( "test.hostInputRecorder" ) {}

    void Reset( RESET_REASON aReason ) override {}

    int record( const TOOL_EVENT& aEvent )
    {
        m_events.push_back( aEvent );
        return 0;
    }

    const TOOL_EVENT& last() const { return m_events.back(); }

    std::vector<TOOL_EVENT> m_events;

private:
    void setTransitions() override
    {
        Go( &EVENT_RECORDER_TOOL::record, TOOL_EVENT( TC_ANY, TA_ANY ) );
    }
};


/**
 * A GAL, a VIEW, host view controls, a tool manager and a recording tool, wired
 * up the way a non-wx host wires them.
 *
 * The GAL is the recording backend because it is the one that needs no window.
 * The screen is 800x600 and the view is left at its default scale and centre, so
 * that a screen coordinate maps to a world coordinate through exactly the same
 * code a real canvas uses.
 */
struct HOST_INPUT_FIXTURE
{
    HOST_INPUT_FIXTURE() :
            gal( options ),
            viewControls( &view ),
            dispatcher( &toolManager, &viewControls )
    {
        gal.ResizeScreen( 800, 600 );
        view.SetGAL( &gal );

        tool = new EVENT_RECORDER_TOOL();

        toolManager.SetEnvironment( nullptr, &view, &viewControls, nullptr, &holder );
        toolManager.RegisterTool( tool );
        toolManager.ResetTools( TOOL_BASE::RUN );
    }

    /// The world point the view maps a screen point to, so a test can state its
    /// expectations in the same space the tools receive.
    VECTOR2D world( double aX, double aY ) const
    {
        return view.ToWorld( VECTOR2D( aX, aY ) );
    }

    HOST_INPUT_EVENT motion( double aX, double aY, int aMods = 0 ) const
    {
        HOST_INPUT_EVENT event;
        event.type = HOST_INPUT_TYPE::POINTER_MOTION;
        event.modifiers = aMods;
        event.position = VECTOR2D( aX, aY );
        return event;
    }

    HOST_INPUT_EVENT button( HOST_INPUT_TYPE aType, int aButton, double aX, double aY,
                             int aMods = 0 ) const
    {
        HOST_INPUT_EVENT event;
        event.type = aType;
        event.button = aButton;
        event.modifiers = aMods;
        event.position = VECTOR2D( aX, aY );
        return event;
    }

    HOST_INPUT_EVENT key( int aKeyCode, int aMods = 0 ) const
    {
        HOST_INPUT_EVENT event;
        event.type = HOST_INPUT_TYPE::KEY_DOWN;
        event.keyCode = aKeyCode;
        event.modifiers = aMods;
        return event;
    }

    HOST_INPUT_EVENT scroll( double aDetents, int aMods ) const
    {
        HOST_INPUT_EVENT event;
        event.type = HOST_INPUT_TYPE::SCROLL;
        event.modifiers = aMods;
        event.position = VECTOR2D( 400, 300 );
        event.scrollDelta = VECTOR2D( 0, aDetents );
        return event;
    }

    /// The actions of every event recorded so far, for a one-line assertion on a
    /// whole gesture.
    std::vector<int> actions() const
    {
        std::vector<int> out;

        for( const TOOL_EVENT& event : tool->m_events )
            out.push_back( event.Action() );

        return out;
    }

    GAL_DISPLAY_OPTIONS  options;
    RECORDING_GAL        gal;
    VIEW                 view;
    HOST_VIEW_CONTROLS   viewControls;
    NON_FRAME_HOLDER     holder;
    TOOL_MANAGER         toolManager;
    EVENT_RECORDER_TOOL* tool;
    HOST_TOOL_DISPATCHER dispatcher;
};

} // namespace


BOOST_FIXTURE_TEST_SUITE( HostViewControls, HOST_INPUT_FIXTURE )


/**
 * WX_VIEW_CONTROLS answers this by polling the operating system. The whole reason
 * a second implementation exists is that a host which is not a wxWindow has to be
 * able to say where its pointer is instead.
 */
BOOST_AUTO_TEST_CASE( ThePointerPositionIsToldRatherThanPolled )
{
    viewControls.SetPointerPosition( VECTOR2D( 100, 50 ) );

    BOOST_CHECK( viewControls.PointerIsOverCanvas() );
    BOOST_CHECK_EQUAL( viewControls.GetMousePosition( false ).x, 100 );
    BOOST_CHECK_EQUAL( viewControls.GetMousePosition( false ).y, 50 );

    const VECTOR2D expected = world( 100, 50 );

    BOOST_CHECK_CLOSE( viewControls.GetMousePosition( true ).x, expected.x, 1e-9 );
    BOOST_CHECK_CLOSE( viewControls.GetMousePosition( true ).y, expected.y, 1e-9 );
}


BOOST_AUTO_TEST_CASE( LosingThePointerKeepsItsLastPositionButSaysItIsGone )
{
    viewControls.SetPointerPosition( VECTOR2D( 100, 50 ) );
    viewControls.PointerLeft();

    // A tool asking mid-gesture should be told where the cursor last was, not the
    // origin; only the "is it over the canvas" answer changes.
    BOOST_CHECK( !viewControls.PointerIsOverCanvas() );
    BOOST_CHECK_EQUAL( viewControls.GetMousePosition( false ).x, 100 );
}


BOOST_AUTO_TEST_CASE( AForcedPositionOutranksThePointer )
{
    viewControls.SetPointerPosition( VECTOR2D( 100, 50 ) );

    const VECTOR2D forced( 123456, -654321 );

    viewControls.ForceCursorPosition( true, forced );

    BOOST_CHECK_EQUAL( viewControls.GetCursorPosition( false ).x, forced.x );
    BOOST_CHECK_EQUAL( viewControls.GetCursorPosition( false ).y, forced.y );

    // ...and the pointer moving does not dislodge it, which is the property the
    // placement tools depend on.
    viewControls.SetPointerPosition( VECTOR2D( 400, 300 ) );

    BOOST_CHECK_EQUAL( viewControls.GetCursorPosition( false ).x, forced.x );

    viewControls.ForceCursorPosition( false );

    BOOST_CHECK_CLOSE( viewControls.GetCursorPosition( false ).x, world( 400, 300 ).x, 1e-9 );
}


/**
 * A forced position is clamped like every other coordinate this class hands out.
 *
 * The base class stores it raw, so inheriting `ForceCursorPosition` unchanged is the
 * one path by which a tool could give the tools a coordinate that overflows on its
 * first conversion to a `VECTOR2I`. `WX_VIEW_CONTROLS` overrides it for the same
 * reason.
 */
BOOST_AUTO_TEST_CASE( AForcedPositionIsClampedToTheCoordinateRange )
{
    const double beyond = 4.0e9; // well past INT32_MAX

    viewControls.ForceCursorPosition( true, VECTOR2D( beyond, -beyond ) );

    const VECTOR2D forced = viewControls.GetCursorPosition( false );

    BOOST_CHECK_LT( forced.x, static_cast<double>( std::numeric_limits<int32_t>::max() ) );
    BOOST_CHECK_GT( forced.y, static_cast<double>( std::numeric_limits<int32_t>::min() ) );
}


/**
 * `SetCursorPosition()` is specified as "place the cursor and let the next mouse
 * motion override it". WX_VIEW_CONTROLS delivers the first half by warping the
 * pointer, which a host without a pointer API cannot do — so the requirement here
 * is that the cursor reads back as placed, and that the next pointer report wins.
 */
BOOST_AUTO_TEST_CASE( AToolCanPlaceTheCursorAndThePointerTakesItBack )
{
    viewControls.SetPointerPosition( VECTOR2D( 100, 50 ) );

    const VECTOR2D placed = world( 700, 500 );

    viewControls.SetCursorPosition( placed, false, false, 0 );

    BOOST_CHECK_CLOSE( viewControls.GetCursorPosition( false ).x, placed.x, 1e-9 );
    BOOST_CHECK_CLOSE( viewControls.GetCursorPosition( false ).y, placed.y, 1e-9 );

    viewControls.SetPointerPosition( VECTOR2D( 100, 50 ) );

    BOOST_CHECK_CLOSE( viewControls.GetCursorPosition( false ).x, world( 100, 50 ).x, 1e-9 );
}


/**
 * The one capability a non-window host genuinely lacks. Recording the request is
 * what lets a host that *can* move its pointer honour it, and what makes the gap
 * visible rather than silent.
 */
BOOST_AUTO_TEST_CASE( AWarpRequestIsRecordedForAHostThatCanHonourIt )
{
    BOOST_CHECK( !viewControls.WarpRequestPending() );

    const VECTOR2D target = world( 300, 200 );

    viewControls.WarpMouseCursor( target, true, false );

    BOOST_CHECK( viewControls.WarpRequestPending() );

    // Reading it clears it: a host consumes each request once.
    BOOST_CHECK( !viewControls.WarpRequestPending() );

    BOOST_CHECK_CLOSE( viewControls.WarpRequestPosition().x, target.x, 1e-9 );
    BOOST_CHECK_CLOSE( viewControls.GetCursorPosition( false ).x, target.x, 1e-9 );
}


BOOST_AUTO_TEST_CASE( CenteringOnTheCursorMovesTheView )
{
    viewControls.SetPointerPosition( VECTOR2D( 700, 500 ) );

    const VECTOR2D cursor = viewControls.GetCursorPosition( false );

    viewControls.CenterOnCursor();

    BOOST_CHECK_CLOSE( view.GetCenter().x, cursor.x, 1e-9 );
    BOOST_CHECK_CLOSE( view.GetCenter().y, cursor.y, 1e-9 );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_FIXTURE_TEST_SUITE( HostToolDispatcher, HOST_INPUT_FIXTURE )


BOOST_AUTO_TEST_CASE( PointerMotionBecomesAPositionedMotionEvent )
{
    dispatcher.Dispatch( motion( 100, 50 ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 1 );
    BOOST_CHECK_EQUAL( tool->last().Action(), TA_MOUSE_MOTION );
    BOOST_CHECK( tool->last().HasPosition() );
    BOOST_CHECK_CLOSE( tool->last().Position().x, world( 100, 50 ).x, 1e-9 );
}


/**
 * "Motion" means the *world* position changed, which is the definition
 * `TOOL_DISPATCHER` uses. Two reports of the same pointer position are not motion,
 * and a tool that redraws a preview per motion event should not be woken for them.
 */
BOOST_AUTO_TEST_CASE( AStationaryPointerIsNotMotion )
{
    dispatcher.Dispatch( motion( 100, 50 ) );
    dispatcher.Dispatch( motion( 100, 50 ) );

    BOOST_CHECK_EQUAL( tool->m_events.size(), 1 );
}


/**
 * A press and a release with no travel in between is a click, and the click
 * reports the position of the *press* rather than of the release — so a tool
 * acting on a click acts where the user aimed.
 */
BOOST_AUTO_TEST_CASE( APressAndAReleaseInPlaceIsADownAndAClick )
{
    dispatcher.Dispatch( motion( 100, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 101, 50 ) );

    const std::vector<int> expected{ TA_MOUSE_MOTION, TA_MOUSE_DOWN, TA_MOUSE_CLICK };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );

    BOOST_CHECK_CLOSE( tool->last().Position().x, world( 100, 50 ).x, 1e-9 );
}


/**
 * Past the threshold the gesture is a drag, and a release then ends the drag
 * rather than being reported as a click. Emitting both would make every drag also
 * act on whatever the click under its start point does.
 */
BOOST_AUTO_TEST_CASE( TravelPastTheThresholdMakesADragAndTheReleaseIsNotAClick )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( motion( 140, 50 ) );
    dispatcher.Dispatch( motion( 180, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 180, 50 ) );

    const std::vector<int> expected{ TA_MOUSE_DOWN, TA_MOUSE_DRAG, TA_MOUSE_DRAG, TA_MOUSE_UP };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );
}


/**
 * A drag event carries where the gesture started and how far it has come, and
 * those are the only two things a move tool needs. They have no public setter on
 * `TOOL_EVENT`, which is why both dispatchers are friends of it.
 */
BOOST_AUTO_TEST_CASE( ADragCarriesItsOriginAndItsDelta )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( motion( 200, 150 ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 2 );
    BOOST_REQUIRE_EQUAL( tool->last().Action(), TA_MOUSE_DRAG );

    const VECTOR2D origin = world( 100, 50 );
    const VECTOR2D now = world( 200, 150 );

    BOOST_CHECK_CLOSE( tool->last().DragOrigin().x, origin.x, 1e-9 );
    BOOST_CHECK_CLOSE( tool->last().Delta().x, now.x - origin.x, 1e-9 );
    BOOST_CHECK_CLOSE( tool->last().Delta().y, now.y - origin.y, 1e-9 );
}


/**
 * Below the threshold the pointer has not really moved, and the gesture is still
 * a click. Without this every click with a shaky hand would be a one-pixel drag.
 */
BOOST_AUTO_TEST_CASE( TravelBelowTheThresholdIsStillAClick )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( motion( 102, 51 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 102, 51 ) );

    const std::vector<int> expected{ TA_MOUSE_DOWN, TA_MOUSE_MOTION, TA_MOUSE_CLICK };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );
}


BOOST_AUTO_TEST_CASE( TheDragThresholdIsTheHostsToSet )
{
    dispatcher.SetDragThreshold( 100, 100 );

    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( motion( 150, 50 ) );

    // 50 px would have been a drag at the default threshold of 8.
    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 2 );
    BOOST_CHECK_EQUAL( tool->last().Action(), TA_MOUSE_MOTION );
}


/**
 * A double click delivered after the pointer has travelled is a fast click-drag,
 * and `TOOL_DISPATCHER` demotes it to a fresh press for exactly that reason. A
 * host that reports a click count rather than reasoning about it hands us the
 * same ambiguity, so the same rule applies.
 *
 * Note the ordering: the first click's *release* arrives before the double click,
 * which is what the ABI specifies and what the shell produces. An earlier version
 * of the demotion also tested `pressed`, which is false by then — so the branch
 * was unreachable and this whole gesture lost its drag. That is why the release is
 * in the sequence below rather than omitted for brevity.
 */
BOOST_AUTO_TEST_CASE( ADoubleClickAfterTravelIsDemotedToAPressAndCanStillDrag )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DBLCLICK, BUT_LEFT, 400, 300 ) );
    dispatcher.Dispatch( motion( 500, 300 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 500, 300 ) );

    const std::vector<int> expected{ TA_MOUSE_DOWN, TA_MOUSE_CLICK, TA_MOUSE_DOWN, TA_MOUSE_DRAG,
                                     TA_MOUSE_UP };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );

    // ...and the drag measures from where the demoted press was, not from the first
    // click of the pair.
    BOOST_CHECK_CLOSE( tool->m_events[3].DragOrigin().x, world( 400, 300 ).x, 1e-9 );
}


BOOST_AUTO_TEST_CASE( ADoubleClickInPlaceIsADoubleClick )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DBLCLICK, BUT_LEFT, 100, 50 ) );

    BOOST_CHECK_EQUAL( tool->last().Action(), TA_MOUSE_DBLCLICK );
}


BOOST_AUTO_TEST_CASE( TheButtonAndTheModifiersReachTheEvent )
{
    dispatcher.Dispatch(
            button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_RIGHT, 100, 50, MD_CTRL | MD_SHIFT ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 1 );
    BOOST_CHECK( tool->last().IsMouseDown( BUT_RIGHT ) );
    BOOST_CHECK_EQUAL( tool->last().Modifier(), MD_CTRL | MD_SHIFT );
}


/**
 * Losing the pointer has to *end* a drag, not merely forget it.
 *
 * Forgetting leaves the tool in its `Wait()` loop with no release coming, and the
 * release it was owed can never be matched because the state to match it against is
 * gone — which is a hang rather than a lost event. `TOOL_DISPATCHER` reaches the
 * same place from the other side: it polls the OS and synthesises the up it never
 * received.
 */
BOOST_AUTO_TEST_CASE( LosingThePointerEndsADragRatherThanForgettingIt )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( motion( 200, 50 ) );

    HOST_INPUT_EVENT leave;
    leave.type = HOST_INPUT_TYPE::POINTER_LEAVE;

    dispatcher.Dispatch( leave );

    // The release that follows is for a button we no longer believe is down, so it
    // produces nothing rather than a second end to the same drag.
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 200, 50 ) );

    const std::vector<int> expected{ TA_MOUSE_DOWN, TA_MOUSE_DRAG, TA_MOUSE_UP };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );
}


/**
 * A press that never became a drag gets nothing, though.
 *
 * `flushPendingClicks()` does emit a click in the wx dispatcher, but only because
 * the operating system has confirmed the button is already up — it knows the user
 * finished the click. Losing the pointer says nothing of the kind, and completing a
 * click the user did not finish is worse than dropping it.
 */
BOOST_AUTO_TEST_CASE( LosingThePointerDoesNotCompleteAClickTheUserDidNotFinish )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );

    HOST_INPUT_EVENT leave;
    leave.type = HOST_INPUT_TYPE::POINTER_LEAVE;

    dispatcher.Dispatch( leave );

    const std::vector<int> expected{ TA_MOUSE_DOWN };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );
}


BOOST_AUTO_TEST_CASE( ResettingTheStateEndsAnUnfinishedGestureToo )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.ResetState();
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_UP, BUT_LEFT, 100, 50 ) );

    BOOST_CHECK_EQUAL( tool->m_events.size(), 1 );

    // But the last position survives, as it does in TOOL_DISPATCHER::ResetState():
    // a tool asking after a focus change should not be told the origin.
    BOOST_CHECK_CLOSE( dispatcher.LastMousePosition().x, world( 100, 50 ).x, 1e-9 );
}


/**
 * A key press carries the cursor position, and every hotkey-driven placement tool
 * depends on it — `TOOL_MANAGER` snapshots it into `m_hotKeyPos` for this event
 * shape and nothing else.
 */
BOOST_AUTO_TEST_CASE( AKeyPressCarriesTheCursorPosition )
{
    dispatcher.Dispatch( motion( 100, 50 ) );
    dispatcher.Dispatch( key( 'W' ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 2 );
    BOOST_CHECK_EQUAL( tool->last().Action(), TA_KEY_PRESSED );
    BOOST_CHECK_EQUAL( tool->last().KeyCode(), 'W' );
    BOOST_CHECK( tool->last().HasPosition() );
    BOOST_CHECK_CLOSE( tool->last().Position().x, world( 100, 50 ).x, 1e-9 );
}


BOOST_AUTO_TEST_CASE( EscapeIsACancelRatherThanAKeyPress )
{
    dispatcher.Dispatch( key( WXK_ESCAPE ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 1 );
    BOOST_CHECK( tool->last().IsCancel() );
}


BOOST_AUTO_TEST_CASE( AnExplicitCancelIsTheSameEventAsEscape )
{
    HOST_INPUT_EVENT cancel;
    cancel.type = HOST_INPUT_TYPE::CANCEL;

    dispatcher.Dispatch( cancel );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 1 );
    BOOST_CHECK( tool->last().IsCancel() );
}


/**
 * A key the host could not name is not key zero. Sending it would run whatever is
 * bound to zero, which is a wrong action rather than no action.
 */
BOOST_AUTO_TEST_CASE( AnUnresolvedKeyIsDroppedRatherThanSentAsKeyZero )
{
    dispatcher.Dispatch( key( 0 ) );

    BOOST_CHECK( tool->m_events.empty() );
}


BOOST_AUTO_TEST_CASE( AKeyReleaseReachesNoTool )
{
    HOST_INPUT_EVENT release;
    release.type = HOST_INPUT_TYPE::KEY_UP;
    release.keyCode = 'W';

    dispatcher.Dispatch( release );

    BOOST_CHECK( tool->m_events.empty() );
}


/**
 * Zero or one modifier on the wheel belongs to the view controls, for pan and
 * zoom. Only two or more reaches a tool. Getting this wrong means an unmodified
 * wheel turn both zooms and edits.
 */
BOOST_AUTO_TEST_CASE( AWheelTurnReachesAToolOnlyWithTwoModifiers )
{
    dispatcher.Dispatch( scroll( 1.0, 0 ) );
    dispatcher.Dispatch( scroll( 1.0, MD_CTRL ) );

    BOOST_CHECK( tool->m_events.empty() );

    dispatcher.Dispatch( scroll( 1.0, MD_CTRL | MD_ALT ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 1 );
    BOOST_CHECK_EQUAL( tool->last().Action(), TA_MOUSE_WHEEL );
    BOOST_CHECK( tool->last().Parameter<int>() > 0 );

    dispatcher.Dispatch( scroll( -1.0, MD_CTRL | MD_ALT ) );

    BOOST_CHECK( tool->last().Parameter<int>() < 0 );
}


/**
 * A wheel turn is a mouse event, and in `TOOL_DISPATCHER` it goes through the whole
 * mouse path. That is load-bearing rather than incidental: zooming moves the world
 * under a stationary pointer, so a tool tracking a drag has to be told the cursor
 * is somewhere else now, or its rubber band stays anchored to the pre-zoom point
 * until the pointer physically moves.
 */
BOOST_AUTO_TEST_CASE( AWheelTurnThatMovedTheWorldStillReportsMotion )
{
    dispatcher.Dispatch( button( HOST_INPUT_TYPE::POINTER_DOWN, BUT_LEFT, 100, 50 ) );
    dispatcher.Dispatch( motion( 200, 50 ) );

    // The scroll handler has moved the shell's camera, and the session's view follows;
    // the pointer has not moved but the world under it has.
    view.SetCenter( view.GetCenter() + VECTOR2D( 1000000, 0 ) );

    dispatcher.Dispatch( scroll( 1.0, 0 ) );

    const std::vector<int> expected{ TA_MOUSE_DOWN, TA_MOUSE_DRAG, TA_MOUSE_DRAG };

    const std::vector<int> seen = actions();

    BOOST_CHECK_EQUAL_COLLECTIONS( seen.begin(), seen.end(), expected.begin(), expected.end() );
}


/**
 * The dispatcher takes positions in screen pixels and hands the tools world
 * coordinates, and it gets the conversion by asking the view controls rather than
 * doing it itself. That is what lets a tool that has forced the cursor somewhere
 * see its own position in the events that follow, instead of the pointer's.
 */
BOOST_AUTO_TEST_CASE( PositionsComeFromTheViewControlsNotFromTheEvent )
{
    view.SetScale( 4.0 );
    view.SetCenter( VECTOR2D( 1000, 2000 ) );

    dispatcher.Dispatch( motion( 100, 50 ) );

    BOOST_REQUIRE_EQUAL( tool->m_events.size(), 1 );
    BOOST_CHECK_CLOSE( tool->last().Position().x, world( 100, 50 ).x, 1e-9 );
    BOOST_CHECK_CLOSE( tool->last().Position().y, world( 100, 50 ).y, 1e-9 );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( HostKeyNames )


/**
 * KiCad's hotkeys are `WXK_*` integers and a host's keys are names, so somewhere
 * the two have to meet. Doing it in C++ is what keeps the numbers coming from
 * `wx/defs.h` through the compiler rather than from a table transcribed by hand
 * into another language — where a single wrong entry is a shortcut that silently
 * does nothing.
 */
BOOST_AUTO_TEST_CASE( TheNamedKeysResolveToTheirWxCodes )
{
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "escape" ), WXK_ESCAPE );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "delete" ), WXK_DELETE );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "backspace" ), WXK_BACK );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "tab" ), WXK_TAB );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "enter" ), WXK_RETURN );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "space" ), WXK_SPACE );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "up" ), WXK_UP );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "pagedown" ), WXK_PAGEDOWN );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "home" ), WXK_HOME );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "insert" ), WXK_INSERT );
}


BOOST_AUTO_TEST_CASE( TheFunctionKeysAreArithmeticRatherThanTwoDozenTableRows )
{
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "f1" ), WXK_F1 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "f9" ), WXK_F9 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "f12" ), WXK_F12 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "f24" ), WXK_F24 );

    // Out of range, and not a function key at all: both are "no such key".
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "f0" ), 0 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "f25" ), 0 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "fnord" ), 0 );
}


/**
 * `.DefaultHotkey( 'W' )` in `sch_actions.cpp` is an upper-case character, and wx
 * reports an upper-case character for a letter key, so a host's "w" has to
 * become 'W' or the wire tool's shortcut misses.
 */
BOOST_AUTO_TEST_CASE( ASingleCharacterIsItsOwnUpperCasedCode )
{
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "w" ), 'W' );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "W" ), 'W' );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "5" ), '5' );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "/" ), '/' );
}


/**
 * KiCad packs the modifier bits into the same integer as the key code from bit 12
 * up (`MD_SHIFT` is 0x1000), so a code point at or above that would be
 * indistinguishable from a modified key. Refusing is the only honest answer.
 */
BOOST_AUTO_TEST_CASE( AKeyThatCannotBeEncodedIsRefusedRatherThanTruncated )
{
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "é" ), 0 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "" ), 0 );
    BOOST_CHECK_EQUAL( HOST_TOOL_DISPATCHER::KeyCodeFromName( "no such key" ), 0 );
}


BOOST_AUTO_TEST_SUITE_END()

