/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004 Jean-Pierre Charras, jaen-pierre.charras@gipsa-lab.inpg.com
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

#include <undo_redo_holder.h>


void UNDO_REDO_HOLDER::ClearUndoRedoList()
{
    ClearUndoORRedoList( UNDO_LIST );
    ClearUndoORRedoList( REDO_LIST );
}


void UNDO_REDO_HOLDER::PushCommandToUndoList( PICKED_ITEMS_LIST* aNewitem )
{
    m_undoList.PushCommand( aNewitem );

    // Delete the extra items, if count max reached
    if( m_undoRedoCountMax > 0 )
    {
        int extraitems = GetUndoCommandCount() - m_undoRedoCountMax;

        if( extraitems > 0 )
            ClearUndoORRedoList( UNDO_LIST, extraitems );
    }
}


void UNDO_REDO_HOLDER::PushCommandToRedoList( PICKED_ITEMS_LIST* aNewitem )
{
    m_redoList.PushCommand( aNewitem );

    // Delete the extra items, if count max reached
    if( m_undoRedoCountMax > 0 )
    {
        int extraitems = GetRedoCommandCount() - m_undoRedoCountMax;

        if( extraitems > 0 )
            ClearUndoORRedoList( REDO_LIST, extraitems );
    }
}


PICKED_ITEMS_LIST* UNDO_REDO_HOLDER::PopCommandFromUndoList()
{
    return m_undoList.PopCommand();
}


PICKED_ITEMS_LIST* UNDO_REDO_HOLDER::PopCommandFromRedoList()
{
    return m_redoList.PopCommand();
}


wxString UNDO_REDO_HOLDER::GetUndoActionDescription() const
{
    if( GetUndoCommandCount() > 0 )
        return m_undoList.m_CommandsList.back()->GetDescription();

    return wxEmptyString;
}


wxString UNDO_REDO_HOLDER::GetRedoActionDescription() const
{
    if( GetRedoCommandCount() > 0 )
        return m_redoList.m_CommandsList.back()->GetDescription();

    return wxEmptyString;
}
