/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2020 CERN
 * @author Maciej Suminski <maciej.suminski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
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

#include <eda_draw_frame.h>
#include <tool/actions.h>
#include <tool/tool_manager.h>
#include <tool/properties_tool.h>
#include <widgets/properties_panel.h>


int PROPERTIES_TOOL::UpdateProperties( const TOOL_EVENT& aEvent )
{
    // Checked, so that the null test below can actually fire. getEditFrame<T>() is a
    // static_cast of the tool holder, which for a holder that is not a frame yields a
    // plausible non-null pointer to nothing — so the guard as written was unreachable
    // and the call was undefined behaviour. This tool needs no frame to exist, only to
    // do anything, which is why it declines the work rather than declining to run.
    EDA_DRAW_FRAME* editFrame = dynamic_cast<EDA_DRAW_FRAME*>( m_toolMgr->GetToolHolder() );

    if( editFrame )
        editFrame->UpdateProperties();

    return 0;
}


void PROPERTIES_TOOL::setTransitions()
{
    TOOL_EVENT undoRedoPostEvt = { TC_MESSAGE, TA_UNDO_REDO_POST, AS_GLOBAL };
    Go( &PROPERTIES_TOOL::UpdateProperties, undoRedoPostEvt );
    Go( &PROPERTIES_TOOL::UpdateProperties, EVENTS::PointSelectedEvent );
    Go( &PROPERTIES_TOOL::UpdateProperties, EVENTS::SelectedEvent );
    Go( &PROPERTIES_TOOL::UpdateProperties, EVENTS::UnselectedEvent );
    Go( &PROPERTIES_TOOL::UpdateProperties, EVENTS::ClearedEvent );
    Go( &PROPERTIES_TOOL::UpdateProperties, EVENTS::SelectedItemsModified );
}
