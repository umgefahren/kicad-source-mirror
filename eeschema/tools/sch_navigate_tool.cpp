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

#include <tools/sch_selection_tool.h>
#include <sch_edit_frame.h>
#include <tool/tool_manager.h>
#include <schematic.h>
#include <schematic_holder.h>
#include <sch_screen.h>
#include <eeschema_id.h>
#include <tools/sch_actions.h>
#include <tools/sch_navigate_tool.h>
#include <view/view.h>
#include <common.h>
#include "eda_doc.h"


wxString SCH_NAVIGATE_TOOL::g_BackLink = wxT( "HYPERTEXT_BACK" );


void SCH_NAVIGATE_TOOL::ResetHistory()
{
    m_navHistory.clear();

    // The current sheet is the document's, not the editor's, so this works with or
    // without a frame. With nothing loaded there is no sheet to seed the history with.
    if( SCHEMATIC* schematic = m_editor->GetSchematic() )
        m_navHistory.push_back( schematic->CurrentSheet() );

    m_navIndex = m_navHistory.begin();
}


void SCH_NAVIGATE_TOOL::CleanHistory()
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    wxCHECK( schematic, /* void */ );

    SCH_SHEET_LIST sheets = schematic->Hierarchy();

    wxCHECK( !sheets.empty(), /* void */ );

    // Search through our history, and removing any entries
    // that the no longer point to a sheet on the schematic
    auto entry = m_navHistory.begin();

    while( entry != m_navHistory.end() )
    {
        if( std::find( sheets.begin(), sheets.end(), *entry ) != sheets.end() )
        {
            // Don't allow multiple consecutive instances of the same history.
            if( ( entry != m_navHistory.begin() ) && ( *entry == *std::prev( entry ) ) )
                entry = m_navHistory.erase( entry );
            else
                ++entry;
        }
        else
        {
            entry = m_navHistory.erase( entry );
        }
    }
    if( m_navHistory.size() <= 1 )
        m_navIndex = m_navHistory.begin();
    else
        m_navIndex = --m_navHistory.end();
}


void SCH_NAVIGATE_TOOL::HypertextCommand( const wxString& aHref )
{
    // Hypertext navigation is a window's affordance from end to end: the href arrives
    // from a click on text drawn in a window, the environment-variable substitution
    // below is the frame's project, and the two branches that are not a page jump are a
    // popup menu and an info bar. Without a window nothing can ask for this, so there is
    // nothing to do rather than something to degrade.
    if( !m_frame )
        return;

    wxString destPage;
    wxString href = ResolveUriByEnvVars( aHref, &m_frame->Prj() );

    if( href == SCH_NAVIGATE_TOOL::g_BackLink )
    {
        TOOL_EVENT dummy;
        Back( dummy );
    }
    else if( EDA_TEXT::IsGotoPageHref( href, &destPage ) && !destPage.IsEmpty() )
    {
        for( const SCH_SHEET_PATH& sheet : m_frame->Schematic().Hierarchy() )
        {
            if( sheet.GetPageNumber() == destPage )
            {
                changeSheet( sheet );
                return;
            }
        }

        m_frame->ShowInfoBarError( wxString::Format( _( "Page '%s' not found." ), destPage ) );
    }
    else
    {
        wxMenu menu;

        menu.Append( 1, wxString::Format( _( "Open %s" ), href ) );

        if( m_frame->GetPopupMenuSelectionFromUser( menu ) == 1 )
            GetAssociatedDocument( m_frame, href, &m_frame->Prj(), nullptr, { &m_frame->Schematic() } );
    }
}


int SCH_NAVIGATE_TOOL::Up( const TOOL_EVENT& aEvent )
{
    // Checks for CanGoUp()
    LeaveSheet( aEvent );
    return 0;
}


int SCH_NAVIGATE_TOOL::Forward( const TOOL_EVENT& aEvent )
{
    if( CanGoForward() )
    {
        m_navIndex++;

        m_toolMgr->RunAction( ACTIONS::cancelInteractive );
        m_toolMgr->RunAction( ACTIONS::selectionClear );

        m_frame->SetCurrentSheet( *m_navIndex );
        m_frame->DisplayCurrentSheet();
    }
    else
    {
        wxBell();
    }

    return 0;
}


int SCH_NAVIGATE_TOOL::Back( const TOOL_EVENT& aEvent )
{
    if( CanGoBack() )
    {
        m_navIndex--;

        m_toolMgr->RunAction( ACTIONS::cancelInteractive );
        m_toolMgr->RunAction( ACTIONS::selectionClear );

        m_frame->SetCurrentSheet( *m_navIndex );
        m_frame->DisplayCurrentSheet();
    }
    else
    {
        wxBell();
    }

    return 0;
}


int SCH_NAVIGATE_TOOL::Previous( const TOOL_EVENT& aEvent )
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    wxCHECK( schematic, 0 );

    if( CanGoPrevious() )
    {
        int targetSheet = schematic->CurrentSheet().GetVirtualPageNumber() - 1;
        changeSheet( schematic->Hierarchy().at( targetSheet - 1 ) );
    }
    else
    {
        wxBell();
    }

    return 0;
}


int SCH_NAVIGATE_TOOL::Next( const TOOL_EVENT& aEvent )
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    wxCHECK( schematic, 0 );

    if( CanGoNext() )
    {
        int targetSheet = schematic->CurrentSheet().GetVirtualPageNumber() + 1;
        changeSheet( schematic->Hierarchy().at( targetSheet - 1 ) );
    }
    else
    {
        wxBell();
    }

    return 0;
}


bool SCH_NAVIGATE_TOOL::CanGoBack()
{
    return m_navHistory.size() > 0 && m_navIndex != m_navHistory.begin();
}


bool SCH_NAVIGATE_TOOL::CanGoForward()
{
    return m_navHistory.size() > 0 && m_navIndex != --m_navHistory.end();
}


bool SCH_NAVIGATE_TOOL::CanGoUp()
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    if( !schematic )
        return false;

    std::vector<SCH_SHEET*> topLevelSheets = schematic->GetTopLevelSheets();

    for( SCH_SHEET* top_sheet : topLevelSheets )
    {
        if( schematic->CurrentSheet().Last() == top_sheet )
            return false;
    }

    return true;
}


bool SCH_NAVIGATE_TOOL::CanGoPrevious()
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    if( !schematic )
        return false;

    return schematic->CurrentSheet().GetVirtualPageNumber() > 1;
}


bool SCH_NAVIGATE_TOOL::CanGoNext()
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    if( !schematic || !schematic->IsValid() )
        return false;

    return schematic->CurrentSheet().GetVirtualPageNumber()
           < (int) schematic->Hierarchy().size();
}


int SCH_NAVIGATE_TOOL::ChangeSheet( const TOOL_EVENT& aEvent )
{
    SCH_SHEET_PATH* path = aEvent.Parameter<SCH_SHEET_PATH*>();
    wxCHECK( path, 0 );

    changeSheet( *path );

    return 0;
}


int SCH_NAVIGATE_TOOL::EnterSheet( const TOOL_EVENT& aEvent )
{
    SCHEMATIC*          schematic = m_editor->GetSchematic();
    SCH_SELECTION_TOOL* selTool = m_toolMgr->GetTool<SCH_SELECTION_TOOL>();

    wxCHECK( schematic && selTool, 0 );

    const SCH_SELECTION& selection = selTool->RequestSelection( { SCH_SHEET_T } );

    if( selection.GetSize() == 1 )
    {
        SCH_SHEET_PATH pushed = schematic->CurrentSheet();
        pushed.push_back( (SCH_SHEET*) selection.Front() );

        changeSheet( pushed );
    }

    return 0;
}


int SCH_NAVIGATE_TOOL::LeaveSheet( const TOOL_EVENT& aEvent )
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    wxCHECK( schematic, 0 );

    if( CanGoUp() )
    {
        SCH_SHEET_PATH popped = schematic->CurrentSheet();
        popped.pop_back();

        changeSheet( popped );
    }
    else
    {
        wxBell();
    }

    return 0;
}


void SCH_NAVIGATE_TOOL::setTransitions()
{
    Go( &SCH_NAVIGATE_TOOL::ChangeSheet,           SCH_ACTIONS::changeSheet.MakeEvent() );
    Go( &SCH_NAVIGATE_TOOL::EnterSheet,            SCH_ACTIONS::enterSheet.MakeEvent() );
    Go( &SCH_NAVIGATE_TOOL::LeaveSheet,            SCH_ACTIONS::leaveSheet.MakeEvent() );

    Go( &SCH_NAVIGATE_TOOL::Up,                    SCH_ACTIONS::navigateUp.MakeEvent() );
    Go( &SCH_NAVIGATE_TOOL::Forward,               SCH_ACTIONS::navigateForward.MakeEvent() );
    Go( &SCH_NAVIGATE_TOOL::Back,                  SCH_ACTIONS::navigateBack.MakeEvent() );

    Go( &SCH_NAVIGATE_TOOL::Previous,              SCH_ACTIONS::navigatePrevious.MakeEvent() );
    Go( &SCH_NAVIGATE_TOOL::Next,                  SCH_ACTIONS::navigateNext.MakeEvent() );
}


void SCH_NAVIGATE_TOOL::pushToHistory( const SCH_SHEET_PATH& aPath )
{
    if( CanGoForward() )
        m_navHistory.erase( std::next( m_navIndex ), m_navHistory.end() );

    if( m_navHistory.empty() || ( *(--m_navHistory.end()) != aPath ) )
        m_navHistory.push_back( aPath );

    m_navIndex = --m_navHistory.end();
}


void SCH_NAVIGATE_TOOL::changeSheet( const SCH_SHEET_PATH& aPath )
{
    SCHEMATIC* schematic = m_editor->GetSchematic();

    wxCHECK( schematic, /* void */ );

    m_toolMgr->RunAction( ACTIONS::cancelInteractive );
    m_toolMgr->RunAction( ACTIONS::selectionClear );

    // Store the current zoom level into the current screen before switching
    if( SCH_SCREEN* screen = m_editor->GetScreen() )
        screen->m_LastZoomLevel = getView()->GetScale();

    pushToHistory( aPath );

    // A window: keyboard focus is one's, and there is nothing to clear without it.
    if( m_frame )
        m_frame->ClearFocus();

    // Moves the document's current sheet *and* swaps the editor over to that sheet's
    // screen. It is one call because doing only the first would leave the editor showing
    // the old sheet while the history pointed at the new one.
    m_editor->DisplaySheet( aPath );
}
