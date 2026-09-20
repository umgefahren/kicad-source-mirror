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

class PICKED_ITEMS_LIST;
class SCHEMATIC_HOLDER;
class SCH_ITEM;
class SCH_SCREEN;

enum class UNDO_REDO;

/**
 * Undo and redo for a schematic, independent of what is editing it.
 *
 * These were members of `SCH_EDIT_FRAME`, reachable only through a window. They are free
 * functions over #SCHEMATIC_HOLDER so that a non-wx editing context — `SCH_HOST` — can
 * undo as well, which is what makes an edit made from a non-wx UI something other than
 * one-way. `SCH_EDIT_FRAME`'s methods of the same names forward here, so the GUI runs this
 * code and cannot drift from it.
 *
 * Native page settings use `SCH_PAGE_SETTINGS_UNDO_ITEM` and require no frame.
 * The legacy drawing-sheet proxy, hierarchy navigator and variant selector remain
 * frame-specific. Each of those is reached by downcasting
 * the editor and skipped when the answer is null; ::PutDataInPreviousState says so at each
 * site.
 *
 * The stacks are #UNDO_REDO_HOLDER's, which every editing context with undo also is; these
 * functions find them by downcast and do nothing when there are none.
 */
namespace SCH_UNDO_REDO
{

/**
 * Create a new entry in the undo list from one item.
 *
 * @param aScreen the screen the item is on.
 * @param aAppend true to add to the newest command rather than starting a new one.
 */
void SaveCopyInUndoList( SCHEMATIC_HOLDER& aEditor, SCH_SCREEN* aScreen, SCH_ITEM* aItem,
                         UNDO_REDO aCommandType, bool aAppend );

/**
 * Create a new entry in the undo list from a list of items.
 *
 * Items whose status is #UNDO_REDO::CHANGED get a copy made here if the caller did not
 * make one, and the repeat-item list is cloned into the command so that undo restores it.
 */
void SaveCopyInUndoList( SCHEMATIC_HOLDER& aEditor, const PICKED_ITEMS_LIST& aItemsList,
                         UNDO_REDO aTypeCommand, bool aAppend );

/**
 * Restore the state \a aList describes, and rewrite \a aList to describe the state that
 * was replaced — so that applying it again is the inverse.
 *
 * That in-place inversion is why undo and redo are one function.
 */
void PutDataInPreviousState( SCHEMATIC_HOLDER& aEditor, PICKED_ITEMS_LIST* aList );

/**
 * Undo the newest command, and move it to the redo stack.
 *
 * @return false if there was nothing to undo.
 */
bool Undo( SCHEMATIC_HOLDER& aEditor );

/**
 * Redo the newest undone command, and move it back to the undo stack.
 *
 * @return false if there was nothing to redo.
 */
bool Redo( SCHEMATIC_HOLDER& aEditor );

/**
 * Undo the newest command **without** logging a corresponding redo.
 *
 * Used to cancel an operation in progress, where offering to redo a cancellation would be
 * meaningless.
 */
void Rollback( SCHEMATIC_HOLDER& aEditor );

} // namespace SCH_UNDO_REDO
