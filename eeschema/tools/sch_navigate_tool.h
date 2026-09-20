/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2019 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */


#ifndef SCH_NAVIGATE_TOOL_H
#define SCH_NAVIGATE_TOOL_H

#include <sch_base_frame.h>
#include <tools/sch_tool_base.h>
#include <status_popup.h>

class SCH_EDIT_FRAME;

/**
 * Handle actions specific to the schematic editor.
 */
class SCH_NAVIGATE_TOOL : public wxEvtHandler, public SCH_TOOL_BASE<SCH_EDIT_FRAME>
{
public:
    SCH_NAVIGATE_TOOL() : SCH_TOOL_BASE<SCH_EDIT_FRAME>( "eeschema.NavigateTool" ) {}

    ~SCH_NAVIGATE_TOOL() { }

    /**
     * Runs on an editing context that is not a wxFrame.
     *
     * Everything it asks the editor for is SCHEMATIC_HOLDER's: the document and its sheet
     * hierarchy to decide where it could go, and ::DisplaySheet to go there. That last one
     * was added for this tool, because changing sheet is the one thing it does and doing
     * only the document half would leave the editor showing the sheet it had left.
     *
     * What it loses without a frame is hypertext navigation, which is a window from end
     * to end — the click that raises it, the project its hrefs resolve against, its popup
     * menu and its info bar — and clearing keyboard focus, which there is none of.
     */
    bool runsWithoutAFrame() const override { return true; }

    ///< Reset navigation history. Must be done when schematic changes
    void ResetHistory();
    ///< Remove deleted pages from history. Must be done when schematic
    // hierarchy changes.
    void CleanHistory();

    ///< Enter sheet provided in aEvent
    int ChangeSheet( const TOOL_EVENT& aEvent );
    ///< Enter selected sheet
    int EnterSheet( const TOOL_EVENT& aEvent );
    ///< Return to parent sheet. Synonymous with Up
    int LeaveSheet( const TOOL_EVENT& aEvent );
    ///< Navigate up in sheet hierarchy
    int Up( const TOOL_EVENT& aEvent );
    ///< Navigate forward in sheet history
    int Forward( const TOOL_EVENT& aEvent );
    ///< Navigate back in sheet history
    int Back( const TOOL_EVENT& aEvent );
    ///< Navigate to previous sheet by numeric sheet number
    int Previous( const TOOL_EVENT& aEvent );
    ///< Navigate to next sheet by numeric sheet number
    int Next( const TOOL_EVENT& aEvent );

    void HypertextCommand( const wxString& aHref );

    bool CanGoBack();
    bool CanGoForward();
    bool CanGoUp();
    bool CanGoPrevious();
    bool CanGoNext();

public:
    static wxString g_BackLink;

private:
    ///< Set up handlers for various events.
    void setTransitions() override;
    ///< Clear history after this nav index and pushes aPath to history
    void pushToHistory( const SCH_SHEET_PATH& aPath );
    ///< Change current sheet to aPath and handle history, zooming, etc.
    void changeSheet( const SCH_SHEET_PATH& aPath );

private:
    std::list<SCH_SHEET_PATH>           m_navHistory;
    std::list<SCH_SHEET_PATH>::iterator m_navIndex;
};


#endif // SCH_NAVIGATE_TOOL_H
