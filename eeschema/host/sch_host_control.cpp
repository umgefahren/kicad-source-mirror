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

#include <tool/actions.h>
#include <tool/tool_manager.h>

#include "sch_host.h"
#include "sch_host_control.h"


SCH_HOST_CONTROL::SCH_HOST_CONTROL() :
        TOOL_INTERACTIVE( "eeschema.SchHostControl" ),
        m_host( nullptr )
{
}


bool SCH_HOST_CONTROL::Init()
{
    m_host = dynamic_cast<SCH_HOST*>( m_toolMgr->GetToolHolder() );

    return m_host != nullptr;
}


void SCH_HOST_CONTROL::Reset( RESET_REASON aReason )
{
    m_host = dynamic_cast<SCH_HOST*>( m_toolMgr->GetToolHolder() );
}


int SCH_HOST_CONTROL::Undo( const TOOL_EVENT& aEvent )
{
    if( m_host )
        m_host->Undo();

    return 0;
}


int SCH_HOST_CONTROL::Redo( const TOOL_EVENT& aEvent )
{
    if( m_host )
        m_host->Redo();

    return 0;
}


int SCH_HOST_CONTROL::Save( const TOOL_EVENT& aEvent )
{
    // A failure lands on the session's error string, which the ABI reports; there is no
    // message box to put it in. ksch_session_save() is the entry point for a UI that wants
    // the status code rather than a fire-and-forget action.
    if( m_host )
        m_host->Save();

    return 0;
}


void SCH_HOST_CONTROL::setTransitions()
{
    Go( &SCH_HOST_CONTROL::Undo, ACTIONS::undo.MakeEvent() );
    Go( &SCH_HOST_CONTROL::Redo, ACTIONS::redo.MakeEvent() );
    Go( &SCH_HOST_CONTROL::Save, ACTIONS::save.MakeEvent() );
}
