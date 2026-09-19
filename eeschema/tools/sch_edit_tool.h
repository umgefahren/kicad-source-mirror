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

#pragma once

#include <tools/sch_tool_base.h>
#include <sch_base_frame.h>


class SCH_EDIT_TOOL : public SCH_TOOL_BASE<SCH_EDIT_FRAME>
{
public:
    SCH_EDIT_TOOL();
    ~SCH_EDIT_TOOL() = default;

    static const std::vector<KICAD_T> RotatableItems;
    static const std::vector<KICAD_T> SwappableItems;

    /// @copydoc TOOL_INTERACTIVE::Init()
    bool Init() override;

    /**
     * Runs on an editing context that is not a wxFrame.
     *
     * The split is between the edits that move an item and the edits that ask the user
     * what an item should say. The geometric half — rotate, mirror, swap, repeat, delete,
     * autoplace, justify, lock and the label/text type conversion — needs only the screen,
     * the undo stack, the repeat-item list and the connection rebuild, all of which are
     * SCHEMATIC_HOLDER's, and runs unchanged here. The property half *is* a dialog:
     * ::Properties, ::EditProperties, ::EditField, ::ChangeSymbols, ::SetVariantSymbol,
     * ::SwapPins, ::CleanupSheetPins, ::EditPageNumber, ::GlobalEdit and ::FixERCError all
     * decline when there is no window to parent them, and say so at the site.
     *
     * What the geometric half itself loses without a frame is the right-click menus, the
     * info-bar messages, the hierarchy navigator refresh, and three pieces of model work
     * that happen to live on SCH_EDIT_FRAME rather than on the interface: junction cleanup
     * after a delete (`DeleteJunction`), body-style cycling (`SelectBodyStyle`) and the
     * automatic annotation of a repeated symbol (`AnnotateSymbols`).
     */
    bool runsWithoutAFrame() const override { return true; }

    int Rotate( const TOOL_EVENT& aEvent );
    int Mirror( const TOOL_EVENT& aEvent );
    int Swap( const TOOL_EVENT& aEvent );
    int SwapPins( const TOOL_EVENT& aEvent );
    int SwapPinLabels( const TOOL_EVENT& aEvent );
    int SwapUnitLabels( const TOOL_EVENT& aEvent );

    int RepeatDrawItem( const TOOL_EVENT& aEvent );

    int Properties( const TOOL_EVENT& aEvent );
    int EditField( const TOOL_EVENT& aEvent );
    int AutoplaceFields( const TOOL_EVENT& aEvent );
    int ChangeSymbols( const TOOL_EVENT& aEvent );
    int SetVariantSymbol( const TOOL_EVENT& aEvent );
    int ClearVariantSymbol( const TOOL_EVENT& aEvent );
    int CycleBodyStyle( const TOOL_EVENT& aEvent );
    int EditPageNumber( const TOOL_EVENT& aEvent );

    /**
     * Change a text type to another one.
     *
     * The new text, label, hierarchical label, or global label is created from the old text
     * and the old text object is deleted.
     *
     * A tricky case is when the 'old" text is being edited (i.e. moving) because we must
     * create a new text, and prepare the undo/redo command data for this change and the
     * current move/edit command
     */
    int ChangeTextType( const TOOL_EVENT& aEvent );

    int JustifyText( const TOOL_EVENT& aEvent );

    int CleanupSheetPins( const TOOL_EVENT& aEvent );
    int GlobalEdit( const TOOL_EVENT& aEvent );

    ///< Lock/unlock selected items.
    int ToggleLock( const TOOL_EVENT& aEvent );
    int Lock( const TOOL_EVENT& aEvent );
    int Unlock( const TOOL_EVENT& aEvent );

    ///< Delete the selected items, or the item under the cursor.
    int DoDelete( const TOOL_EVENT& aEvent );

    /// Drag and drop
    int DdAppendFile( const TOOL_EVENT& aEvent );
    int DdAddImage( const TOOL_EVENT& aEvent );

    /// Modify Attributes (DNP, Exclude, etc.)  All attributes are
    /// set to true unless all symbols already have the attribute set to true.
    int SetAttribute( const TOOL_EVENT& aEvent );

    void EditProperties( EDA_ITEM* aItem );

    wxString FixERCErrorMenuText( const std::shared_ptr<RC_ITEM>& aERCItem );
    void FixERCError( const std::shared_ptr<RC_ITEM>& aERCItem );

private:
    void editFieldText( SCH_FIELD* aField );

    /**
     * The guard ::GlobalEdit cannot carry itself.
     *
     * That method's body lives beside the dialog it opens, in
     * dialogs/dialog_global_edit_text_and_graphics.cpp, so the "this action is the dialog"
     * early return goes here, at the only place that dispatches to it.
     */
    int globalEdit( const TOOL_EVENT& aEvent );

    ///< EDA_DRAW_FRAME::GetNearestGridPosition, asked of the view rather than of a canvas.
    VECTOR2I getNearestGridPosition( const VECTOR2I& aPosition ) const;

    ///< EDA_DRAW_FRAME::GetNearestHalfGridPosition, asked of the view rather than of a canvas.
    VECTOR2I getNearestHalfGridPosition( const VECTOR2I& aPosition ) const;

    ///< SCH_EDIT_FRAME::TestDanglingEnds, asked of the screen and the view.
    void testDanglingEnds();

    void collectUnits( const SCH_SELECTION& aSelection,
                       std::set<std::pair<SCH_SYMBOL*, SCH_SCREEN*>>& aCollectedUnits );

    ///< How to modify a property for selected items.
    enum MODIFY_MODE { ON, OFF, TOGGLE };

    int modifyLockSelected( MODIFY_MODE aMode );

    ///< Set up handlers for various events.
    void setTransitions() override;
};
