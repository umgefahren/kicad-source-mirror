/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <schematic_holder.h>

#include <core/kicad_algo.h>
#include <sch_commit.h>
#include <sch_line.h>
#include <sch_symbol.h>
#include <tool/tool_manager.h>
#include <tool/actions.h>
#include <tools/sch_selection_tool.h>
#include <sch_item.h>
#include <sch_label.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <schematic.h>


const std::vector<std::unique_ptr<SCH_ITEM>>& SCHEMATIC_HOLDER::GetRepeatItems() const
{
    // An editor with no repeat-item list answers the empty one rather than making every
    // caller test for absence.
    static const std::vector<std::unique_ptr<SCH_ITEM>> none;

    return none;
}


void SCHEMATIC_HOLDER::AutoRotateItem( SCH_SCREEN* aScreen, SCH_ITEM* aItem )
{
    SCHEMATIC* schematic = GetSchematic();

    if( !schematic || !aScreen )
        return;

    const SCH_SHEET_PATH& sheet = schematic->CurrentSheet();

    if( aItem->Type() == SCH_GLOBAL_LABEL_T || aItem->Type() == SCH_HIER_LABEL_T )
    {
        SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( aItem );

        if( label->AutoRotateOnPlacement() )
        {
            SPIN_STYLE spin = aScreen->GetLabelOrientationForPoint( label->GetPosition(), label->GetSpinStyle(),
                                                                    &sheet );

            if( spin != label->GetSpinStyle() )
            {
                label->SetSpinStyle( spin );

                for( SCH_ITEM* item : aScreen->Items().OfType( SCH_GLOBAL_LABEL_T ) )
                {
                    SCH_LABEL_BASE* otherLabel = static_cast<SCH_LABEL_BASE*>( item );

                    if( otherLabel != label && otherLabel->GetText() == label->GetText() )
                        otherLabel->AutoplaceFields( aScreen, AUTOPLACE_AUTO );
                }
            }
        }
    }
}


void SCHEMATIC_HOLDER::DeleteJunction( SCH_COMMIT* aCommit, SCH_ITEM* aJunction )
{
    SCH_SCREEN*         screen = GetScreen();
    SCH_SELECTION_TOOL* selectionTool = GetSelectionTool();

    aJunction->SetFlags( STRUCT_DELETED );
    RemoveFromScreen( aJunction, screen );
    aCommit->Removed( aJunction, screen );

    /// Note that std::list or similar is required here as we may insert values in the
    /// loop below.  This will invalidate iterators in a std::vector or std::deque
    std::list<SCH_LINE*> lines;

    for( SCH_ITEM* item : screen->Items().Overlapping( SCH_LINE_T, aJunction->GetPosition() ) )
    {
        SCH_LINE* line = static_cast<SCH_LINE*>( item );

        if( ( line->IsWire() || line->IsBus() )
                && line->IsEndPoint( aJunction->GetPosition() )
                && !( line->GetEditFlags() & STRUCT_DELETED ) )
        {
            lines.push_back( line );
        }
    }

    alg::for_all_pairs( lines.begin(), lines.end(),
            [&]( SCH_LINE* firstLine, SCH_LINE* secondLine )
            {
                if( ( firstLine->GetEditFlags() & STRUCT_DELETED )
                        || ( secondLine->GetEditFlags() & STRUCT_DELETED )
                        || firstLine->GetLayer() != secondLine->GetLayer()
                        || !secondLine->IsParallel( firstLine ) )
                {
                    return;
                }

                // Remove identical lines
                if( firstLine->IsEndPoint( secondLine->GetStartPoint() )
                        && firstLine->IsEndPoint( secondLine->GetEndPoint() ) )
                {
                    firstLine->SetFlags( STRUCT_DELETED );
                    return;
                }

                // Try to merge the remaining lines
                if( SCH_LINE* new_line = secondLine->MergeOverlap( screen, firstLine, false ) )
                {
                    firstLine->SetFlags( STRUCT_DELETED );
                    secondLine->SetFlags( STRUCT_DELETED );
                    AddToScreen( new_line, screen );
                    aCommit->Added( new_line, screen );

                    if( new_line->IsSelected() )
                        selectionTool->AddItemToSel( new_line, true /*quiet mode*/ );

                    lines.push_back( new_line );
                }
            } );

    for( SCH_LINE* line : lines )
    {
        if( line->GetEditFlags() & STRUCT_DELETED )
        {
            if( line->IsSelected() )
                selectionTool->RemoveItemFromSel( line, true /*quiet mode*/ );

            RemoveFromScreen( line, screen );
            aCommit->Removed( line, screen );
        }
    }
}

void SCHEMATIC_HOLDER::SelectBodyStyle( TOOL_MANAGER* aToolManager, SCH_SYMBOL* aSymbol, int aBodyStyle, SCH_COMMIT* aCommit )
{
    if( !aSymbol || !aSymbol->GetLibSymbolRef() )
        return;

    const int bodyStyleCount = aSymbol->GetLibSymbolRef()->GetBodyStyleCount();
    const int currentBodyStyle = aSymbol->GetBodyStyle();

    if( bodyStyleCount <= 1 || aBodyStyle < 1 || currentBodyStyle == aBodyStyle )
        return;

    if( aBodyStyle > bodyStyleCount )
        aBodyStyle = bodyStyleCount;

    if( currentBodyStyle == aBodyStyle )
        return;

    SCH_COMMIT  localCommit( aToolManager );
    SCH_COMMIT* commit = aCommit ? aCommit : &localCommit;

    // A symbol with edit flags was already staged by the command in progress
    if( !aSymbol->GetEditFlags() )
        commit->Modify( aSymbol, GetScreen() );

    aSymbol->SetBodyStyle( aBodyStyle );

    // If selected make sure all the now-included pins are selected
    if( aSymbol->IsSelected() )
        aToolManager->RunAction<EDA_ITEM*>( ACTIONS::selectItem, aSymbol );

    if( !localCommit.Empty() )
        localCommit.Push( _( "Change Body Style" ) );
}
