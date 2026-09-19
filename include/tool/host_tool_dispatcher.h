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

#ifndef KICAD_TOOL_HOST_TOOL_DISPATCHER_H
#define KICAD_TOOL_HOST_TOOL_DISPATCHER_H

#include <array>
#include <chrono>
#include <cstdint>
#include <string_view>

#include <math/vector2d.h>
#include <tool/tool_event.h>

class TOOL_MANAGER;

namespace KIGFX
{
class HOST_VIEW_CONTROLS;
}

/**
 * What kind of input a ::HOST_INPUT_EVENT carries.
 *
 * Deliberately smaller than wx's vocabulary. `TOOL_DISPATCHER` consumes fifteen
 * `wxEVT_*` click types, a motion type, a wheel type, a magnify type and a
 * synthetic refresh type, and spends most of its 827 lines reconciling them with
 * what the OS actually did. A host that delivers ordered, reliable events needs
 * none of that reconciliation, so the vocabulary is the one the tools care about.
 */
enum class HOST_INPUT_TYPE
{
    /// The pointer moved. Carries a position; no button.
    POINTER_MOTION,

    /// A button went down. Carries a position and a button.
    POINTER_DOWN,

    /// A button came up. Carries a position and a button.
    POINTER_UP,

    /// A double click. Carries a position and a button, and is delivered
    /// *instead of* the second POINTER_DOWN of the pair.
    POINTER_DBLCLICK,

    /// The pointer left the canvas. Carries nothing. Ends any drag in progress and
    /// then forgets the button state, so a release that happens where the host can
    /// never see it cannot leave a tool waiting for one. See ::ResetState.
    POINTER_LEAVE,

    /// The wheel turned, or a two-finger scroll. Carries a position and a delta.
    SCROLL,

    /// A key went down. Carries a key code and modifiers.
    KEY_DOWN,

    /// A key came up. Carries a key code and modifiers. Produces no TOOL_EVENT —
    /// KiCad's tools are driven entirely by key *presses* — and exists so the
    /// dispatcher can keep an accurate idea of what is held.
    KEY_UP,

    /// Cancel whatever is running, as Escape does. Carries nothing.
    CANCEL
};


/**
 * One input event from a host that is not wxWidgets.
 *
 * Positions are in **screen pixels**, relative to the top-left of the canvas,
 * because that is what a UI toolkit has and because the world position has to be
 * derived through the same `VIEW_CONTROLS` that a forced or warped cursor goes
 * through. Handing world coordinates in instead would bypass that and the tools
 * would disagree with the view about where the cursor is.
 */
struct HOST_INPUT_EVENT
{
    HOST_INPUT_TYPE type = HOST_INPUT_TYPE::POINTER_MOTION;

    /// One of `TOOL_MOUSE_BUTTONS` — `BUT_LEFT`, `BUT_RIGHT`, `BUT_MIDDLE`,
    /// `BUT_AUX1`, `BUT_AUX2` — or `BUT_NONE` for an event with no button.
    int button = BUT_NONE;

    /// A bitwise-or of `TOOL_MODIFIERS` (`MD_SHIFT`, `MD_CTRL`, `MD_ALT`, ...).
    int modifiers = 0;

    /// A `WXK_*` code, or a character's code point for a printable key. Zero for
    /// an event that is not a key event. ::KeyCodeFromName resolves a host's key
    /// name to one of these.
    int keyCode = 0;

    /// True when the host says this key press is an auto-repeat rather than a
    /// fresh one. KiCad has no use for the distinction today; it is carried
    /// because the host knows and the dispatcher would otherwise have to guess.
    bool isAutoRepeat = false;

    /// Pointer position, in screen pixels relative to the canvas' top-left.
    VECTOR2D position;

    /// Scroll delta in wheel detents: positive y is a scroll away from the user.
    /// Only meaningful for ::HOST_INPUT_TYPE::SCROLL.
    VECTOR2D scrollDelta;
};


/**
 * A `TOOL_MANAGER` input source for a host that is not wxWidgets.
 *
 * `TOOL_DISPATCHER` is `: public wxEvtHandler` and is not abstract, so it cannot
 * be subclassed usefully by a non-wx host; what it *is*, though, is a translator
 * with a small output contract — about ten distinct `TOOL_EVENT` constructions —
 * wrapped in a great deal of wx-quirk reconciliation. This class reproduces the
 * contract and drops the reconciliation, because the quirks are wx's rather than
 * KiCad's:
 *
 * | `TOOL_DISPATCHER` does | and this does not, because |
 * |---|---|
 * | polls `wxGetMouseState()` to notice a button-up it never received | the host delivers every up; `POINTER_LEAVE` and ::ResetState cover a gesture that ended where it cannot be seen, by *finishing* it rather than by discovering it later |
 * | polls `wxGetKeyState()` to drop an auto-repeat that arrived after release | the host says whether a press is a repeat; a burst cannot outlive the release |
 * | remaps Ctrl+letter from ASCII 1..26, and Alt+digit from Apple raw key codes | those are wx key-event artefacts; a host reports the key and the modifier separately |
 * | folds the numpad arrows onto the arrow keys for `wxEVT_CHAR_HOOK` only | the host reports one or the other and means it |
 * | flushes a pending click before Escape, because wx may deliver them out of order | host events are ordered |
 * | asks `wxWindow::FindFocus()` whether a text control has focus | the host decides what its keyboard focus means and simply does not send us the event |
 *
 * What is kept, deliberately:
 *
 * - **Position comes from `VIEW_CONTROLS`, never from the event.** The event's
 *   screen position is pushed into the view controls and the world position is
 *   read back, exactly as `TOOL_DISPATCHER` does
 *   (`common/tool/tool_dispatcher.cpp:615`). That is what lets
 *   `ForceCursorPosition` and a tool-placed cursor reach the tools.
 * - **"Motion" means the *world* position changed**, not the screen position, so
 *   a view that moves under a stationary pointer still produces motion.
 * - **`TOOL_DISPATCHER::IsPastDragThreshold`** is reused rather than
 *   reimplemented. It is `static` and wx-free precisely so it can be.
 * - The drag threshold is measured from the press position **in screen pixels**,
 *   and a double click that arrives after the pointer has travelled past it is
 *   demoted to a fresh press — without which a fast click-drag reads as a double
 *   click.
 * - A release emits `TA_MOUSE_UP` when a drag was running and `TA_MOUSE_CLICK`
 *   when one was not, never both, and a click reports the position of the
 *   *press*.
 * - A wheel event reaches the tools only when **two or more** modifier bits are
 *   set. Zero or one belongs to the view controls for pan and zoom
 *   (`tool_dispatcher.cpp:641`).
 *
 * ## What this class does not do
 *
 * It has no idea what a canvas, a window or a focus is, and it does not repaint
 * anything: `TOOL_MANAGER::ProcessEvent` returning is the whole of its output.
 * A host decides what to redraw, which it must anyway, because it owns the frame
 * clock.
 *
 * ## Threading
 *
 * One thread — the tool framework's. Not thread safe.
 */
class HOST_TOOL_DISPATCHER
{
public:
    /**
     * @param aToolManager the manager to feed; must outlive this.
     * @param aViewControls the same object the manager was given in
     *        `TOOL_MANAGER::SetEnvironment`. It is taken concretely rather than as
     *        a `VIEW_CONTROLS*` because the dispatcher has to *write* the pointer
     *        position into it, which the abstract interface has no method for —
     *        `WX_VIEW_CONTROLS` gets it by polling the OS instead.
     */
    HOST_TOOL_DISPATCHER( TOOL_MANAGER* aToolManager, KIGFX::HOST_VIEW_CONTROLS* aViewControls );

    ~HOST_TOOL_DISPATCHER() = default;

    HOST_TOOL_DISPATCHER( const HOST_TOOL_DISPATCHER& ) = delete;
    HOST_TOOL_DISPATCHER& operator=( const HOST_TOOL_DISPATCHER& ) = delete;

    /**
     * Translate one host event and give the result to the tool framework.
     *
     * @return what `TOOL_MANAGER::ProcessEvent` returned, i.e. whether any tool
     *         or hotkey claimed the event. False for an event that produces no
     *         `TOOL_EVENT` at all — a key release, or a wheel turn with fewer
     *         than two modifiers.
     */
    bool Dispatch( const HOST_INPUT_EVENT& aEvent );

    /**
     * End any gesture in progress and forget which buttons are down.
     *
     * The equivalent of `EDA_DRAW_PANEL_GAL::onLostFocus` calling
     * `TOOL_DISPATCHER::ResetState()`, with one thing added that the wx one gets
     * from polling the OS instead: a button that was *dragging* is given its
     * `TA_MOUSE_UP` before being forgotten. Clearing the state alone leaves the
     * tool waiting for a release that can no longer be matched, which is a hang
     * rather than a lost event. A press that never became a drag gets nothing —
     * see the implementation for why.
     *
     * Like the wx one, this keeps the last known pointer position: a tool asking
     * for it after a focus change should not be told the origin.
     *
     * @return whether a tool claimed one of the releases it sent.
     */
    bool ResetState();

    /**
     * The drag threshold, in screen pixels, per axis.
     *
     * Defaults to `TOOL_DISPATCHER`'s own fallback of 8 px in both axes. A host
     * that can ask the platform for `wxSYS_DRAG_X` / `wxSYS_DRAG_Y` equivalents
     * should pass them on, because the number is a user-perceptible one.
     */
    void SetDragThreshold( int aMinX, int aMinY );

    /// The last pointer position the dispatcher was told about, in world coordinates.
    const VECTOR2D& LastMousePosition() const { return m_lastMousePos; }

    /**
     * Resolve a host's name for a key to the `WXK_*` code KiCad's hotkeys use.
     *
     * **This is the one place the two key vocabularies meet.** KiCad's hotkey
     * registry stores `WXK_*` integers — `TOOL_ACTION::GetDefaultHotKey()`
     * returns one, and so does every binding in the user's configuration — so a
     * UI whose keys are named rather than numbered has to be mapped onto them or
     * every shortcut silently does nothing. Doing it here rather than in the UI
     * means the numbers are read out of `wx/defs.h` by the compiler instead of
     * being transcribed into another language by hand.
     *
     * The names are gpui's spelling, which is lower case and unpunctuated
     * ("escape", "pageup", "f11"). A few obvious synonyms are accepted because
     * they cost nothing and a host that spells `enter` as `return` should not
     * fail silently. A single character maps to its upper-cased code point,
     * which is what wx reports for a letter key and what `actions.cpp` spells
     * its bindings with.
     *
     * @return the key code, or 0 for a name this function does not know — which
     *         a caller should treat as "do not send this key" rather than as
     *         key 0.
     */
    static int KeyCodeFromName( std::string_view aName );

private:
    /// Per-button press/drag state. The wx dispatcher keeps five of these too, but
    /// has to poll the OS to keep them honest; here the host's events are the truth.
    struct BUTTON_STATE
    {
        bool     pressed = false;
        bool     dragging = false;
        VECTOR2D dragOrigin;       ///< Press position, world.
        VECTOR2D dragOriginScreen; ///< Press position, screen — what the threshold measures.
        VECTOR2D downPosition;     ///< Press position, world, reported by a click.

        /// When the press happened, for the macOS slow-drag promotion below.
        std::chrono::steady_clock::time_point downAt{};
    };

    /// The `TOOL_MOUSE_BUTTONS` bit stored at an index of m_buttons.
    static int buttonBit( std::size_t aIndex );

    BUTTON_STATE* stateFor( int aButton );

    /// Latch a press and build its `TA_MOUSE_DOWN`.
    TOOL_EVENT press( BUTTON_STATE& aState, int aArgs );

    /// Push the event's screen position into the view controls and read the world
    /// position back. Returns true if the world position changed, i.e. if this is
    /// motion in the sense the tools mean.
    bool trackPosition( const VECTOR2D& aScreenPosition );

    bool dispatchPointer( const HOST_INPUT_EVENT& aEvent, bool aMotion );
    bool dispatchKey( const HOST_INPUT_EVENT& aEvent );
    bool dispatchScroll( const HOST_INPUT_EVENT& aEvent );
    bool dispatchCancel();

    TOOL_MANAGER*              m_toolManager;
    KIGFX::HOST_VIEW_CONTROLS* m_viewControls;

    /// LEFT, RIGHT, MIDDLE, AUX1, AUX2 — the order `TOOL_DISPATCHER` uses, kept so
    /// that the two read the same way side by side.
    std::array<BUTTON_STATE, 5> m_buttons;

    VECTOR2D m_lastMousePos;       ///< World.
    VECTOR2D m_lastMousePosScreen; ///< Screen.

    int m_dragMinX;
    int m_dragMinY;
};

#endif // KICAD_TOOL_HOST_TOOL_DISPATCHER_H
