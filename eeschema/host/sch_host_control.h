/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#ifndef KICAD_EESCHEMA_HOST_SCH_HOST_CONTROL_H
#define KICAD_EESCHEMA_HOST_SCH_HOST_CONTROL_H

#include <tool/tool_interactive.h>

class SCH_HOST;

/**
 * Undo, redo and save for a session with no `wxFrame`.
 *
 * `SCH_EDITOR_CONTROL` registers these three actions in the schematic editor, and it still
 * declines an editing context that is not a frame — it is four thousand lines with about a
 * hundred `m_frame->` sites, most of them dialogs, so converting it is not the way to get
 * ⌘Z working. This tool handles the three that need no window, and nothing else.
 *
 * It exists as a *tool* rather than as three C ABI calls because a hotkey is resolved inside
 * `TOOL_MANAGER`: a UI on the far side of the boundary cannot intercept ⌘Z, it can only
 * forward the key. Registering a handler is therefore the only way the key, a menu item and
 * a command-palette entry all end up doing the same thing.
 *
 * The bodies are three lines each and delegate to `SCH_UNDO_REDO` and `SCH_HOST::Save`, so
 * this duplicates the *dispatch* and not the work. ::Init declines any holder that is not a
 * `SCH_HOST`, so it is inert in the wx editor even if it is ever registered there.
 */
class SCH_HOST_CONTROL : public TOOL_INTERACTIVE
{
public:
    SCH_HOST_CONTROL();

    /// Declines every holder but a SCH_HOST.
    bool Init() override;

    void Reset( RESET_REASON aReason ) override;

    int Undo( const TOOL_EVENT& aEvent );
    int Redo( const TOOL_EVENT& aEvent );
    int Save( const TOOL_EVENT& aEvent );

private:
    void setTransitions() override;

    SCH_HOST* m_host;
};

#endif // KICAD_EESCHEMA_HOST_SCH_HOST_CONTROL_H
