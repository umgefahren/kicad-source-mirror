/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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

#pragma once

#include <undo_redo_container.h>
#include <wx/string.h>

/// Maximum number of commands each stack keeps; zero means no limit.
#define DEFAULT_MAX_UNDO_ITEMS 0

/**
 * The undo and redo stacks, for anything that edits a document.
 *
 * These lived on `EDA_BASE_FRAME` — `PICKED_ITEMS_LIST`-based and entirely wx-free, but
 * reachable only through a `wxFrame`. They are here so that a document owner which is not
 * a window can have them: `SCH_HOST` does, which is what lets an edit made from a non-wx
 * UI be undone rather than being one-way.
 *
 * Nothing about the mechanism changed in the move. Every frame in KiCad still gets these
 * methods by inheritance, and the derived-class overrides of ::ClearUndoORRedoList are
 * still where the decision about *deleting* picked items is made — that is
 * document-specific, because an item in a stack may or may not also be on a screen, and
 * only the editor knows.
 */
class UNDO_REDO_HOLDER
{
public:
    /**
     * Specify whether we are interacting with the undo or redo stacks.
     */
    enum UNDO_REDO_LIST
    {
        UNDO_LIST,
        REDO_LIST
    };

    UNDO_REDO_HOLDER() : m_undoRedoCountMax( DEFAULT_MAX_UNDO_ITEMS ) {}

    virtual ~UNDO_REDO_HOLDER() = default;

    /**
     * Remove the \a aItemCount of old commands from \a aList and delete commands, pickers
     * and picked items if needed.
     *
     * Because picked items must be deleted only if they are not in use, the real work is
     * done by an override that knows the document — see #SCH_SCREEN and #PCB_SCREEN.
     * Commands are deleted from the older to the last.
     *
     * @param aList = the #UNDO_REDO_CONTAINER of commands.
     * @param aItemCount number of old commands to delete. -1 to remove all old commands
     *                   this will empty the list of commands.
     */
    virtual void ClearUndoORRedoList( UNDO_REDO_LIST aList, int aItemCount = -1 )
    { }

    /**
     * Clear the undo and redo list using #ClearUndoORRedoList()
     *
     * Picked items are deleted by ClearUndoORRedoList() according to their status.
     */
    virtual void ClearUndoRedoList();

    /**
     * Add a command to undo in the undo list.
     *
     * Delete the very old commands when the max count of undo commands is reached.
     */
    virtual void PushCommandToUndoList( PICKED_ITEMS_LIST* aItem );

    /**
     * Add a command to redo in the redo list.
     *
     * Delete the very old commands when the max count of redo commands is reached.
     */
    virtual void PushCommandToRedoList( PICKED_ITEMS_LIST* aItem );

    /**
     * Return the last command to undo and remove it from list, nothing is deleted.
     */
    virtual PICKED_ITEMS_LIST* PopCommandFromUndoList();

    /**
     * Return the last command to undo and remove it from list, nothing is deleted.
     */
    virtual PICKED_ITEMS_LIST* PopCommandFromRedoList();

    virtual int GetUndoCommandCount() const { return m_undoList.m_CommandsList.size(); }
    virtual int GetRedoCommandCount() const { return m_redoList.m_CommandsList.size(); }

    virtual wxString GetUndoActionDescription() const;
    virtual wxString GetRedoActionDescription() const;

    int GetMaxUndoItems() const { return m_undoRedoCountMax; }

protected:
    int                 m_undoRedoCountMax; // undo/Redo command Max depth

    UNDO_REDO_CONTAINER m_undoList;         // Objects list for the undo command (old data)
    UNDO_REDO_CONTAINER m_redoList;         // Objects list for the redo command (old data)
};
