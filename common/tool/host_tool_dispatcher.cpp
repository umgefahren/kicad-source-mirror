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
#include <bit>
#include <cctype>
#include <optional>
#include <unordered_map>

#include <math/util.h> // for KiROUND
#include <tool/host_tool_dispatcher.h>
#include <tool/tool_dispatcher.h> // for IsPastDragThreshold
#include <tool/tool_manager.h>
#include <view/host_view_controls.h>

#include <wx/defs.h> // the WXK_* vocabulary this file translates into


/**
 * What wx reports per wheel detent, and therefore the magnitude a `TA_MOUSE_WHEEL`
 * event is expected to carry. Only the sign is read today — eeschema's and
 * pcbnew's selection tools both reduce it to `delta > 0 ? 1 : -1` — but scaling
 * keeps the two dispatchers' events comparable rather than merely compatible.
 */
static constexpr double WHEEL_DETENT = 120.0;


/**
 * How long a button may be held, while moving, before the movement counts as a
 * drag regardless of distance. macOS only, matching `TOOL_DISPATCHER`'s
 * `DragTimeThreshold` and its `#ifdef __WXMAC__` guard: a slow drag on a trackpad
 * never crosses the distance threshold between two motion events, and without
 * this it reads as a click.
 */
static constexpr std::chrono::milliseconds DRAG_TIME_THRESHOLD{ 300 };


HOST_TOOL_DISPATCHER::HOST_TOOL_DISPATCHER( TOOL_MANAGER*              aToolManager,
                                            KIGFX::HOST_VIEW_CONTROLS* aViewControls ) :
        m_toolManager( aToolManager ),
        m_viewControls( aViewControls ),
        // TOOL_DISPATCHER's own fallback when the platform has no opinion
        // (tool_dispatcher.cpp:132). A host that can ask should call
        // SetDragThreshold().
        m_dragMinX( 8 ),
        m_dragMinY( 8 )
{
}


void HOST_TOOL_DISPATCHER::SetDragThreshold( int aMinX, int aMinY )
{
    m_dragMinX = std::max( 0, aMinX );
    m_dragMinY = std::max( 0, aMinY );
}


bool HOST_TOOL_DISPATCHER::ResetState()
{
    // A drag in progress has to be *ended*, not merely forgotten. Clearing the state
    // alone would leave the tool in its Wait() loop with no release coming — which is
    // a hang rather than a lost event — and the release it was owed can never be
    // matched afterwards, because the state it would be matched against is gone.
    // TOOL_DISPATCHER reaches the same place from the other direction: it polls the OS
    // and synthesises the up it never received.
    //
    // A press that had *not* become a drag gets nothing. wx's flushPendingClicks()
    // does emit a click there, but only because the OS has confirmed the button is
    // already up — it knows the user completed the click. Losing the pointer, or the
    // focus, says nothing of the kind, and completing a click the user did not finish
    // is worse than dropping it.
    bool handled = false;

    for( std::size_t ii = 0; ii < m_buttons.size(); ++ii )
    {
        BUTTON_STATE& state = m_buttons[ii];

        if( state.pressed && state.dragging )
        {
            TOOL_EVENT evt( TC_MOUSE, TA_MOUSE_UP, buttonBit( ii ) );

            evt.SetMousePosition( m_lastMousePos );

            handled |= m_toolManager->ProcessEvent( evt );
        }

        state.pressed = false;
        state.dragging = false;
    }

    // Deliberately not m_lastMousePos: TOOL_DISPATCHER::ResetState() leaves it too,
    // and a tool that asks where the cursor is after a focus change should be told
    // where it last was rather than the origin.

    return handled;
}


int HOST_TOOL_DISPATCHER::buttonBit( std::size_t aIndex )
{
    switch( aIndex )
    {
    case 0: return BUT_LEFT;
    case 1: return BUT_RIGHT;
    case 2: return BUT_MIDDLE;
    case 3: return BUT_AUX1;
    case 4: return BUT_AUX2;
    default: return BUT_NONE;
    }
}


HOST_TOOL_DISPATCHER::BUTTON_STATE* HOST_TOOL_DISPATCHER::stateFor( int aButton )
{
    for( std::size_t ii = 0; ii < m_buttons.size(); ++ii )
    {
        if( buttonBit( ii ) == aButton )
            return &m_buttons[ii];
    }

    return nullptr;
}


bool HOST_TOOL_DISPATCHER::trackPosition( const VECTOR2D& aScreenPosition )
{
    m_viewControls->SetPointerPosition( aScreenPosition );

    // Read the position back out of the view controls rather than using the one that
    // came in. It is the same number today, but it is the view controls that clamp it
    // to the coordinate system, and it is the view controls a tool will have moved
    // with ForceCursorPosition() or SetCursorPosition() — so this is the value the
    // tools are going to see. TOOL_DISPATCHER takes the same route for the same
    // reason (tool_dispatcher.cpp:615).
    VECTOR2D world = m_viewControls->GetMousePosition( true );

    m_lastMousePosScreen = m_viewControls->GetMousePosition( false );

    if( world == m_lastMousePos )
        return false;

    m_lastMousePos = world;

    return true;
}


bool HOST_TOOL_DISPATCHER::Dispatch( const HOST_INPUT_EVENT& aEvent )
{
    switch( aEvent.type )
    {
    case HOST_INPUT_TYPE::POINTER_LEAVE:
        m_viewControls->PointerLeft();

        // Which ends any drag rather than abandoning it; see ResetState().
        return ResetState();

    case HOST_INPUT_TYPE::POINTER_MOTION:
    case HOST_INPUT_TYPE::POINTER_DOWN:
    case HOST_INPUT_TYPE::POINTER_UP:
    case HOST_INPUT_TYPE::POINTER_DBLCLICK:
    {
        bool motion = trackPosition( aEvent.position );

        return dispatchPointer( aEvent, motion );
    }

    case HOST_INPUT_TYPE::SCROLL:
    {
        // The wheel goes through the *whole* mouse path in TOOL_DISPATCHER
        // (tool_dispatcher.cpp:602), and the reason matters: a zoom moves the world
        // under a stationary pointer, so a tool tracking a drag has to be told that
        // the cursor is somewhere else now. Dropping trackPosition()'s answer here
        // would leave a rubber band anchored to the pre-zoom world point until the
        // pointer physically moved.
        //
        // One deliberate difference: wx emits the wheel event only when no motion
        // event was made, and this emits both. A tool handles them independently and
        // suppressing one of two true statements has nothing to recommend it.
        bool motion = trackPosition( aEvent.position );
        bool handled = dispatchPointer( aEvent, motion );

        return dispatchScroll( aEvent ) || handled;
    }

    case HOST_INPUT_TYPE::KEY_DOWN:
        return dispatchKey( aEvent );

    case HOST_INPUT_TYPE::KEY_UP:
        // KiCad's tools are driven by presses only. There is no TOOL_EVENT for a
        // release, and inventing one would reach nothing.
        return false;

    case HOST_INPUT_TYPE::CANCEL:
        return dispatchCancel();
    }

    return false;
}


TOOL_EVENT HOST_TOOL_DISPATCHER::press( BUTTON_STATE& aState, int aArgs )
{
    // Only the first press of a gesture sets the origin, so that a double click
    // demoted back to a press still measures its drag from where the gesture began.
    if( !aState.pressed )
    {
        aState.dragOrigin = m_lastMousePos;
        aState.dragOriginScreen = m_lastMousePosScreen;
    }

    aState.downPosition = m_lastMousePos;
    aState.downAt = std::chrono::steady_clock::now();
    aState.pressed = true;

    return TOOL_EVENT( TC_MOUSE, TA_MOUSE_DOWN, aArgs );
}


bool HOST_TOOL_DISPATCHER::dispatchPointer( const HOST_INPUT_EVENT& aEvent, bool aMotion )
{
    const int mods = aEvent.modifiers & MD_MODIFIER_MASK;

    bool handled = false;
    bool buttonEvents = false;

    // 1. The transition the host is telling us about, for the button it names.
    if( BUTTON_STATE* state = stateFor( aEvent.button ) )
    {
        std::optional<TOOL_EVENT> evt;
        bool                      isClick = false;
        const int                 args = aEvent.button | mods;

        switch( aEvent.type )
        {
        case HOST_INPUT_TYPE::POINTER_DOWN:
            evt = press( *state, args );
            break;

        case HOST_INPUT_TYPE::POINTER_DBLCLICK:
        {
            // A double click that arrives after the pointer has travelled past the
            // drag threshold is a fast click-drag, and is demoted to a fresh press —
            // tool_dispatcher.cpp:232. Without it, a quick drag reads as a double
            // click and whatever the double click does happens instead of the drag.
            // Measured from the press that *opened* the gesture, which is still on
            // record: the release between the two clicks clears `pressed` but not
            // `dragOriginScreen`. Requiring `pressed` here — which was the first
            // version of this — makes the branch unreachable, because the documented
            // ordering delivers the release before the double click.
            VECTOR2D offset = m_lastMousePosScreen - state->dragOriginScreen;

            if( TOOL_DISPATCHER::IsPastDragThreshold( offset, m_dragMinX, m_dragMinY ) )
            {
                state->pressed = false;
                state->dragging = false;
                evt = press( *state, args );
            }
            else
            {
                evt = TOOL_EVENT( TC_MOUSE, TA_MOUSE_DBLCLICK, args );
            }

            break;
        }

        case HOST_INPUT_TYPE::POINTER_UP:
            // A release for a button we never saw pressed is dropped rather than
            // turned into a click at a stale position. It happens after
            // POINTER_LEAVE, which clears the press.
            if( state->pressed )
            {
                state->pressed = false;

                if( state->dragging )
                {
                    evt = TOOL_EVENT( TC_MOUSE, TA_MOUSE_UP, args );
                }
                else
                {
                    evt = TOOL_EVENT( TC_MOUSE, TA_MOUSE_CLICK, args );
                    isClick = true;
                }

                state->dragging = false;
            }

            break;

        default:
            break;
        }

        if( evt )
        {
            // A click reports where the button went down, everything else where the
            // pointer is now (tool_dispatcher.cpp:320).
            evt->SetMousePosition( isClick ? state->downPosition : m_lastMousePos );

            handled |= m_toolManager->ProcessEvent( *evt );
            buttonEvents = true;
        }
    }

    // 2. A drag for every button that is held. Checking all five rather than only the
    //    one the event names is what TOOL_DISPATCHER does, and it is right for the
    //    same reason: a motion event names no button, and more than one can be down.
    if( aMotion )
    {
        for( std::size_t ii = 0; ii < m_buttons.size(); ++ii )
        {
            BUTTON_STATE& state = m_buttons[ii];

            if( !state.pressed )
                continue;

            if( !state.dragging )
            {
#ifdef __APPLE__
                if( std::chrono::steady_clock::now() - state.downAt > DRAG_TIME_THRESHOLD )
                    state.dragging = true;
#endif
                VECTOR2D offset = m_lastMousePosScreen - state.dragOriginScreen;

                if( TOOL_DISPATCHER::IsPastDragThreshold( offset, m_dragMinX, m_dragMinY ) )
                    state.dragging = true;
            }

            if( state.dragging )
            {
                TOOL_EVENT evt( TC_MOUSE, TA_MOUSE_DRAG, buttonBit( ii ) | mods );

                evt.setMouseDragOrigin( state.dragOrigin );
                evt.setMouseDelta( m_lastMousePos - state.dragOrigin );
                evt.SetMousePosition( m_lastMousePos );

                handled |= m_toolManager->ProcessEvent( evt );
                buttonEvents = true;
            }
        }
    }

    // 3. Plain motion, but only if nothing above fired: a button event supersedes it
    //    (tool_dispatcher.cpp:628), because a tool tracking a drag does not also want
    //    to be told the pointer moved.
    if( aMotion && !buttonEvents )
    {
        TOOL_EVENT evt( TC_MOUSE, TA_MOUSE_MOTION, mods );

        evt.SetMousePosition( m_lastMousePos );

        handled |= m_toolManager->ProcessEvent( evt );
    }

    return handled;
}


bool HOST_TOOL_DISPATCHER::dispatchKey( const HOST_INPUT_EVENT& aEvent )
{
    if( aEvent.keyCode == WXK_ESCAPE )
        return dispatchCancel();

    // A key the host could not name is not key zero; it is a key we cannot express,
    // and sending it would run whatever is bound to 0.
    if( aEvent.keyCode == 0 )
        return false;

    const int mods = aEvent.modifiers & MD_MODIFIER_MASK;

    TOOL_EVENT evt( TC_KEYBOARD, TA_KEY_PRESSED, aEvent.keyCode | mods );

    // Keyboard events carry the cursor position, and every hotkey-driven placement
    // tool depends on it: TOOL_MANAGER::processEvent snapshots it into m_hotKeyPos
    // for exactly this event shape.
    evt.SetMousePosition( m_lastMousePos );
    evt.SetHasPosition( true );

    return m_toolManager->ProcessEvent( evt );
}


bool HOST_TOOL_DISPATCHER::dispatchScroll( const HOST_INPUT_EVENT& aEvent )
{
    const unsigned mods = static_cast<unsigned>( aEvent.modifiers ) & MD_MODIFIER_MASK;

    // Zero or one modifier belongs to the view controls, for pan and zoom; only two
    // or more reaches a tool. The test is `popcount > 1` in both dispatchers
    // (tool_dispatcher.cpp:645) and it is load-bearing — an unmodified wheel turn
    // that also reached the tools would zoom and increment a field at once.
    if( std::popcount( mods ) <= 1 )
        return false;

    const int rotation = KiROUND( aEvent.scrollDelta.y * WHEEL_DETENT );

    if( rotation == 0 )
        return false;

    TOOL_EVENT evt( TC_MOUSE, TA_MOUSE_WHEEL, static_cast<int>( mods ) );

    evt.SetParameter<int>( rotation );

    // A deliberate divergence, and the only one in this file that changes what a
    // tool would see. TOOL_DISPATCHER does not set a position on a wheel event, and
    // because the event is TC_MOUSE its `m_hasPosition` is true anyway — so
    // `Position()` returns the origin rather than tripping the check that exists to
    // catch exactly this. Nothing reads it today; filling it in costs a line and
    // means that when something does, it gets the truth.
    evt.SetMousePosition( m_lastMousePos );

    return m_toolManager->ProcessEvent( evt );
}


bool HOST_TOOL_DISPATCHER::dispatchCancel()
{
    // TOOL_DISPATCHER flushes any click whose button is physically up before sending
    // the cancel, because wx can deliver the key event before the button-up. A host
    // with ordered delivery has already sent the release, so there is nothing to
    // flush — but the invariant that protects still holds: a click is delivered
    // before the Escape that follows it.
    TOOL_EVENT evt( TC_COMMAND, TA_CANCEL_TOOL, WXK_ESCAPE );

    return m_toolManager->ProcessEvent( evt );
}


int HOST_TOOL_DISPATCHER::KeyCodeFromName( std::string_view aName )
{
    // KiCad's hotkeys are WXK_* integers, and this is the only place a host's name
    // for a key becomes one. The numbers come from the compiler reading wx/defs.h
    // rather than from a table transcribed by hand into another language, which is
    // the whole reason the mapping lives on this side of the boundary.
    static const std::unordered_map<std::string_view, int> named = {
        { "escape", WXK_ESCAPE },
        { "esc", WXK_ESCAPE },
        { "enter", WXK_RETURN },
        { "return", WXK_RETURN },
        { "tab", WXK_TAB },
        { "backspace", WXK_BACK },
        { "delete", WXK_DELETE },
        { "del", WXK_DELETE },
        { "space", WXK_SPACE },
        { "up", WXK_UP },
        { "down", WXK_DOWN },
        { "left", WXK_LEFT },
        { "right", WXK_RIGHT },
        { "pageup", WXK_PAGEUP },
        { "pagedown", WXK_PAGEDOWN },
        { "home", WXK_HOME },
        { "end", WXK_END },
        { "insert", WXK_INSERT },
        { "menu", WXK_MENU },
        { "pause", WXK_PAUSE },
        { "capslock", WXK_CAPITAL },
        { "numlock", WXK_NUMLOCK },
        { "scrolllock", WXK_SCROLL },
        { "printscreen", WXK_SNAPSHOT },
        { "help", WXK_HELP },
        // gpui's names for the two mouse-side navigation buttons, which arrive as
        // keys on some hosts rather than as BUT_AUX1/BUT_AUX2.
        { "back", WXK_BROWSER_BACK },
        { "forward", WXK_BROWSER_FORWARD },
        { "numpadenter", WXK_NUMPAD_ENTER },
        { "numpadadd", WXK_NUMPAD_ADD },
        { "numpadsubtract", WXK_NUMPAD_SUBTRACT },
        { "numpadmultiply", WXK_NUMPAD_MULTIPLY },
        { "numpaddivide", WXK_NUMPAD_DIVIDE },
        { "numpaddecimal", WXK_NUMPAD_DECIMAL },
        { "numpad0", WXK_NUMPAD0 },
        { "numpad1", WXK_NUMPAD1 },
        { "numpad2", WXK_NUMPAD2 },
        { "numpad3", WXK_NUMPAD3 },
        { "numpad4", WXK_NUMPAD4 },
        { "numpad5", WXK_NUMPAD5 },
        { "numpad6", WXK_NUMPAD6 },
        { "numpad7", WXK_NUMPAD7 },
        { "numpad8", WXK_NUMPAD8 },
        { "numpad9", WXK_NUMPAD9 },
    };

    if( aName.empty() )
        return 0;

    if( auto found = named.find( aName ); found != named.end() )
        return found->second;

    // "f1" through "f24". WXK_F1..WXK_F24 are consecutive, which is what makes this
    // arithmetic rather than another two dozen table rows.
    if( ( aName[0] == 'f' || aName[0] == 'F' ) && aName.size() >= 2 && aName.size() <= 3 )
    {
        int index = 0;

        for( std::size_t ii = 1; ii < aName.size(); ++ii )
        {
            if( aName[ii] < '0' || aName[ii] > '9' )
                return 0;

            index = index * 10 + ( aName[ii] - '0' );
        }

        if( index >= 1 && index <= 24 )
            return WXK_F1 + index - 1;

        return 0;
    }

    // A single ASCII character is its own key code, upper-cased, because that is what
    // wx reports for a letter key and what `.DefaultHotkey( 'W' )` in actions.cpp
    // means. Anything above ASCII is refused rather than truncated: KiCad packs the
    // modifiers into the same integer from bit 12 upwards (MD_SHIFT is 0x1000), so a
    // code point at or beyond that would collide with them.
    if( aName.size() == 1 )
    {
        unsigned char ch = static_cast<unsigned char>( aName[0] );

        if( ch < 0x80 )
            return std::toupper( ch );
    }

    return 0;
}
