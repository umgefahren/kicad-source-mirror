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

#include <sch_edit_frame.h>
#include <symbol_edit_frame.h>
#include <tools/sch_selection_tool.h>
#include <tool/tool_manager.h>
#include <sch_commit.h>
#include <sch_sheet_pin.h>
#include <schematic.h>
#include <tools/sch_find_replace_tool.h>
#include <sch_sheet_path.h>
#include <schematic_holder.h>
#include <tool/tools_holder.h>
#include "sch_actions.h"


EDA_SEARCH_DATA* SCH_FIND_REPLACE_TOOL::searchData() const
{
    // EDA_DRAW_FRAME owns the search terms and DIALOG_SCH_FIND fills them in; there is
    // no copy of them on SCHEMATIC_HOLDER and none on this tool. See ::runsWithoutAFrame.
    return m_frame ? &m_frame->GetFindReplaceData() : nullptr;
}


SCHEMATIC* SCH_FIND_REPLACE_TOOL::getSchematic() const
{
    // SCH_EDIT_FRAME::Schematic() is asked first so that a frame answers exactly what it
    // answered before. A holder that is not a frame is asked for the document instead,
    // which is the same object by a different route.
    if( SCH_EDIT_FRAME* editFrame = dynamic_cast<SCH_EDIT_FRAME*>( m_frame ) )
        return &editFrame->Schematic();

    return m_editor->IsSchematicEditor() ? m_editor->GetSchematic() : nullptr;
}


SCH_SHEET_PATH* SCH_FIND_REPLACE_TOOL::getCurrentSheet() const
{
    // Null in the symbol editor and the symbol viewer, which have no sheets to search
    // across; SCH_EDIT_FRAME::GetCurrentSheet() is itself SCHEMATIC::CurrentSheet().
    SCHEMATIC* sch = getSchematic();

    return sch ? &sch->CurrentSheet() : nullptr;
}


int SCH_FIND_REPLACE_TOOL::FindAndReplace( const TOOL_EVENT& aEvent )
{
    // This action *is* the find dialog: it opens it and then repaints what it matches.
    // Without a window to parent it there is nothing it can do, and nowhere for a
    // headless caller to type a search string.
    if( !m_frame )
        return 0;

    m_frame->ShowFindReplaceDialog( aEvent.IsAction( &ACTIONS::findAndReplace ) );
    return UpdateFind( aEvent );
}


int SCH_FIND_REPLACE_TOOL::UpdateFind( const TOOL_EVENT& aEvent )
{
    EDA_SEARCH_DATA* searchTerms = searchData();

    // No window, no search terms, and so nothing to brighten or un-brighten. The events
    // this is bound to are selection changes, which arrive regardless. Past here there
    // is a frame, which is what lets the "is the find dialog open?" tests below stand.
    if( !searchTerms )
        return 0;

    EDA_SEARCH_DATA& data = *searchTerms;
    SCH_SEARCH_DATA* schSearchData = dynamic_cast<SCH_SEARCH_DATA*>( &data );
    SCH_SHEET_PATH*  sheetPath = getCurrentSheet();
    bool             selectedOnly = schSearchData ? schSearchData->searchSelectedOnly : false;

    auto visit =
            [&]( EDA_ITEM* aItem, SCH_SHEET_PATH* aSheet )
            {
                // We may get triggered when the dialog is not opened due to binding
                // SelectedItemsModified we also get triggered when the find dialog is
                // closed....so we need to double check the dialog is open.
                if( m_frame->GetFindReplaceDialog() != nullptr
                        && !data.findString.IsEmpty()
                        && aItem->Matches( data, aSheet )
                        && ( !selectedOnly || aItem->IsSelected() ) )
                {
                    aItem->SetForceVisible( true );
                    m_selectionTool->BrightenItem( aItem );
                    m_foundItemHighlighted = true;
                }
                else if( aItem->HasFlag( BRIGHTENED ) || aItem->IsForceVisible() )
                {
                    aItem->SetForceVisible( false );
                    m_selectionTool->UnbrightenItem( aItem );
                }
            };

    auto visitAll =
            [&]()
            {
                if( SYMBOL_EDIT_FRAME* symbolEditor = dynamic_cast<SYMBOL_EDIT_FRAME*>( m_frame ) )
                {
                    if( LIB_SYMBOL* symbol = symbolEditor->GetCurSymbol() )
                    {
                        for( SCH_ITEM& item : symbol->GetDrawItems() )
                            visit( &item, nullptr );
                    }
                }
                else
                {
                    for( SCH_ITEM* item : m_editor->GetScreen()->Items() )
                    {
                        visit( item, sheetPath );

                        item->RunOnChildren(
                                [&]( SCH_ITEM* aChild )
                                {
                                    visit( aChild, sheetPath );
                                },
                                RECURSE_MODE::NO_RECURSE );
                    }
                }
            };

    if( aEvent.IsAction( &ACTIONS::find ) || aEvent.IsAction( &ACTIONS::findAndReplace )
        || aEvent.IsAction( &ACTIONS::updateFind ) )
    {
        m_foundItemHighlighted = false;
        visitAll();
    }
    else if( aEvent.Matches( EVENTS::SelectedItemsModified ) )
    {
        for( EDA_ITEM* item : m_selectionTool->GetSelection() )
            visit( item, sheetPath );
    }
    else if( aEvent.Matches( EVENTS::PointSelectedEvent )
             || aEvent.Matches( EVENTS::SelectedEvent )
             || aEvent.Matches( EVENTS::UnselectedEvent )
             || aEvent.Matches( EVENTS::ClearedEvent ) )
    {
        if( !m_frame->GetFindReplaceDialog() )
        {
            if( m_foundItemHighlighted )
            {
                m_foundItemHighlighted = false;
                visitAll();
            }
        }
        else if( selectedOnly )
        {
            // Normal find modifies the selection, but selection-based find does not, so we want
            // to start over in the items we are searching through when the selection changes
            m_afterItem = nullptr;
            m_afterItemScreen = nullptr;
            visitAll();
        }
    }
    else if( m_foundItemHighlighted )
    {
        m_foundItemHighlighted = false;
        visitAll();
    }

    getView()->UpdateItems();

    // TOOLS_HOLDER::RefreshCanvas() is EDA_DRAW_FRAME::GetCanvas()->Refresh(), and it is
    // answered by whatever owns the canvas rather than by a window.
    m_toolMgr->GetToolHolder()->RefreshCanvas();

    return 0;
}


SCH_ITEM* SCH_FIND_REPLACE_TOOL::nextMatch( SCH_SCREEN* aScreen, SCH_SHEET_PATH* aSheet, SCH_ITEM* aAfter,
                                            EDA_SEARCH_DATA& aData, bool reversed )
{
    SCH_SEARCH_DATA*       schSearchData = dynamic_cast<SCH_SEARCH_DATA*>( &aData );
    bool                   selectedOnly = schSearchData ? schSearchData->searchSelectedOnly : false;
    bool                   past_item = !aAfter;
    std::vector<SCH_ITEM*> sorted_items;

    auto addItem =
            [&](SCH_ITEM* item)
            {
                sorted_items.push_back( item );

                if( item->Type() == SCH_SYMBOL_T )
                {
                    SCH_SYMBOL* cmp = static_cast<SCH_SYMBOL*>( item );

                    for( SCH_FIELD& field : cmp->GetFields() )
                        sorted_items.push_back( &field );

                    for( SCH_PIN* pin : cmp->GetPins() )
                        sorted_items.push_back( pin );
                }
                else if( item->Type() == SCH_SHEET_T )
                {
                    SCH_SHEET* sheet = static_cast<SCH_SHEET*>( item );

                    for( SCH_FIELD& field : sheet->GetFields() )
                        sorted_items.push_back( &field );

                    for( SCH_SHEET_PIN* pin : sheet->GetPins() )
                        sorted_items.push_back( pin );
                }
                else if( item->IsType( { SCH_LABEL_LOCATE_ANY_T } ) )
                {
                    SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( item );

                    for( SCH_FIELD& field : label->GetFields() )
                        sorted_items.push_back( &field );
                }
            };

    if( selectedOnly )
    {
        for( EDA_ITEM* item : m_selectionTool->GetSelection() )
            addItem( static_cast<SCH_ITEM*>( item ) );
    }
    else if( SYMBOL_EDIT_FRAME* symbolEditor = dynamic_cast<SYMBOL_EDIT_FRAME*>( m_frame ) )
    {
        if( LIB_SYMBOL* symbol = symbolEditor->GetCurSymbol() )
        {
            for( SCH_ITEM& item : symbol->GetDrawItems() )
                addItem( &item );
        }
    }
    else
    {
        for( SCH_ITEM* item : aScreen->Items() )
            addItem( item );
    }

    std::sort( sorted_items.begin(), sorted_items.end(),
            [&]( SCH_ITEM* a, SCH_ITEM* b )
            {
                if( a->GetPosition().x == b->GetPosition().x )
                {
                    // Ensure deterministic sort
                    if( a->GetPosition().y == b->GetPosition().y )
                        return a->m_Uuid < b->m_Uuid;

                    return a->GetPosition().y < b->GetPosition().y;
                }
                else
                    return a->GetPosition().x < b->GetPosition().x;
            } );

    if( reversed )
        std::reverse( sorted_items.begin(), sorted_items.end() );

    for( SCH_ITEM* item : sorted_items )
    {
        if( item == aAfter )
        {
            past_item = true;
        }
        else if( past_item )
        {
            if( aData.markersOnly && item->Type() == SCH_MARKER_T )
                return item;

            if( item->Matches( aData, aSheet ) )
                return item;
        }
    }

    return nullptr;
}


int SCH_FIND_REPLACE_TOOL::FindNext( const TOOL_EVENT& aEvent )
{
    EDA_SEARCH_DATA* searchTerms = searchData();

    // The walk below is all document — screens, sheets and items — but what to look for
    // is the dialog's, so there is nothing to search for without a window.
    if( !searchTerms )
        return 0;

    EDA_SEARCH_DATA& data            = *searchTerms;
    bool             searchAllSheets = false;
    bool             selectedOnly    = false;
    bool             isReversed      = aEvent.IsAction( &ACTIONS::findPrevious );
    SCH_ITEM*        item            = nullptr;
    SCH_SHEET_PATH*  currentSheet    = nullptr;
    SCH_SHEET_PATH*  afterSheet      = nullptr;

    try
    {
        const SCH_SEARCH_DATA& schSearchData = dynamic_cast<const SCH_SEARCH_DATA&>( data );

        if( SCH_SHEET_PATH* sheet = getCurrentSheet() )
        {
            currentSheet = afterSheet = sheet;
            searchAllSheets = !schSearchData.searchCurrentSheetOnly;
        }

        selectedOnly = schSearchData.searchSelectedOnly;
    }
    catch( const std::bad_cast& )
    {
    }

    if( aEvent.IsAction( &ACTIONS::findNextMarker ) )
        data.markersOnly = true;
    else if( data.findString.IsEmpty() )
        return FindAndReplace( ACTIONS::find.MakeEvent() );

    if( m_afterItem && m_afterItemScreen != m_editor->GetScreen() )
    {
        m_afterItem = nullptr;
        m_afterItemScreen = nullptr;
    }

    if( data.findString != m_lastSearchString )
    {
        m_afterItem = nullptr;
        m_afterItemScreen = nullptr;
        m_lastSearchString = data.findString;
    }

    bool wrappedAround = false;

    for( int attempt = 0; attempt < 2; ++attempt )
    {
        if( attempt == 1 )
        {
            if( m_afterItem == nullptr )
                break; // already a fresh session; nothing to wrap to

            m_afterItem = nullptr;
            m_afterItemScreen = nullptr;
            afterSheet = nullptr;
            wrappedAround = true;
        }

        bool freshSession = ( m_afterItem == nullptr );

        if( afterSheet || !searchAllSheets || selectedOnly )
            item = nextMatch( m_editor->GetScreen(), currentSheet, m_afterItem, data, isReversed );

        if( !item && searchAllSheets && !selectedOnly )
        {
            if( SCHEMATIC* schematic = getSchematic() )
            {
                SCH_SCREENS    screens( schematic->Root() );
                SCH_SHEET_LIST paths;

                screens.BuildClientSheetPathList();

                for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
                {
                    for( SCH_SHEET_PATH& sheet : screen->GetClientSheetPaths() )
                        paths.push_back( sheet );
                }

                paths.SortByPageNumbers( false );

                if( isReversed )
                    std::reverse( paths.begin(), paths.end() );

                for( SCH_SHEET_PATH& sheet : paths )
                {
                    if( afterSheet && !freshSession )
                    {
                        if( afterSheet->GetCurrentHash() == sheet.GetCurrentHash() )
                            afterSheet = nullptr;

                        continue;
                    }

                    item = nextMatch( sheet.LastScreen(), &sheet, nullptr, data, isReversed );

                    if( item )
                    {
                        if( schematic->CurrentSheet() != sheet )
                            m_toolMgr->RunAction<SCH_SHEET_PATH*>( SCH_ACTIONS::changeSheet, &sheet );

                        break;
                    }
                }
            }
        }

        if( item )
            break;
    }

    if( item )
    {
        m_afterItem = item;
        m_afterItemScreen = m_editor->GetScreen();

        if( !selectedOnly )
        {
            m_selectionTool->ClearSelection();
            m_selectionTool->AddItemToSel( item );
        }

        if( !item->HasFlag( BRIGHTENED ) )
        {
            // Clear any previous brightening
            UpdateFind( aEvent );

            // Brighten (and show) found object
            item->SetForceVisible( true );
            m_selectionTool->BrightenItem( item );
            m_foundItemHighlighted = true;
        }

        // A window: scrolling the view so the match is on screen is the frame's, and so
        // is the keyboard focus it takes. Finding the item, selecting it and brightening
        // it have all already happened.
        if( m_frame )
            m_frame->FocusOnLocation( item->GetBoundingBox().GetCenter() );

        m_toolMgr->GetToolHolder()->RefreshCanvas();

        // A window: the transient status text over the find dialog. Without one the
        // caller learns that the search wrapped from what it got back instead.
        if( wrappedAround && m_frame )
        {
            wxString msg;

            if( m_frame->GetFrameType() == FRAME_SCH_SYMBOL_EDITOR )
                msg = _( "Reached end of symbol." );
            else if( searchAllSheets )
                msg = _( "Reached end of schematic." );
            else
                msg = _( "Reached end of sheet." );

            m_frame->ShowFindReplaceStatus( msg, 2000 );
        }
    }
    else
    {
        m_afterItem = nullptr;
        m_afterItemScreen = nullptr;

        // A window: the same transient status text, so without one the caller finds out
        // there was no match from the selection being unchanged.
        if( m_frame )
            m_frame->ShowFindReplaceStatus( _( "No matches found." ), 2000 );
    }

    return 0;
}

EDA_ITEM* SCH_FIND_REPLACE_TOOL::getCurrentMatch()
{
    EDA_SEARCH_DATA* searchTerms = searchData();

    // Which of the two the current match is depends on a search option, so with no
    // search data there is no way to say. Every caller has already declined by here.
    if( !searchTerms )
        return nullptr;

    SCH_SEARCH_DATA* schSearchData = dynamic_cast<SCH_SEARCH_DATA*>( searchTerms );
    bool             selectedOnly = schSearchData ? schSearchData->searchSelectedOnly : false;

    return selectedOnly ? m_afterItem : m_selectionTool->GetSelection().Front();
}

bool SCH_FIND_REPLACE_TOOL::HasMatch()
{
    EDA_SEARCH_DATA* searchTerms = searchData();

    // Nothing to match against without the search terms.
    if( !searchTerms )
        return false;

    EDA_ITEM*       match = getCurrentMatch();
    SCH_SHEET_PATH* sheetPath = getCurrentSheet();

    return match && match->Matches( *searchTerms, sheetPath );
}


int SCH_FIND_REPLACE_TOOL::ReplaceAndFindNext( const TOOL_EVENT& aEvent )
{
    EDA_SEARCH_DATA* searchTerms = searchData();

    // The replacement text is part of the search data, so this declines for the same
    // reason ::FindNext does: there is nowhere for a headless caller to put it yet.
    if( !searchTerms )
        return 0;

    EDA_SEARCH_DATA& data = *searchTerms;
    EDA_ITEM*        item = getCurrentMatch();
    SCH_SHEET_PATH*  currentSheet = getCurrentSheet();

    if( data.findString.IsEmpty() )
        return FindAndReplace( ACTIONS::find.MakeEvent() );

    if( item && HasMatch() )
    {
        SCH_COMMIT commit( m_toolMgr );
        SCH_ITEM* sch_item = static_cast<SCH_ITEM*>( item );

        commit.Modify( sch_item, m_editor->GetScreen(), RECURSE_MODE::NO_RECURSE );

        if( item->Replace( data, currentSheet ) )
        {
            if( currentSheet )
                currentSheet->UpdateAllScreenReferences();

            commit.Push( wxS( "Find and Replace" ) );
        }
        else
        {
            // Nothing changed, but Modify() bumped the connectivity revision of the screen.
            // A holder that is not editing a schematic does nothing here, as the symbol
            // editor did before.
            m_editor->RecalculateConnections( nullptr, NO_CLEANUP );
        }

        FindNext( ACTIONS::findNext.MakeEvent() );
    }

    return 0;
}


int SCH_FIND_REPLACE_TOOL::ReplaceAll( const TOOL_EVENT& aEvent )
{
    EDA_SEARCH_DATA* searchTerms = searchData();

    // The whole of the replace-all walk below is the document's, and the commit it
    // pushes works on a holder that is not a frame. What it has no route to is *what*
    // to replace with; see ::runsWithoutAFrame.
    if( !searchTerms )
        return 0;

    EDA_SEARCH_DATA& data = *searchTerms;
    SCH_SHEET_PATH*  currentSheet = nullptr;
    bool             currentSheetOnly = true;
    bool             selectedOnly = false;

    try
    {
        const SCH_SEARCH_DATA& schSearchData = dynamic_cast<const SCH_SEARCH_DATA&>( data );

        if( SCH_SHEET_PATH* sheet = getCurrentSheet() )
        {
            currentSheet = sheet;
            currentSheetOnly = schSearchData.searchCurrentSheetOnly;
        }

        selectedOnly = schSearchData.searchSelectedOnly;
    }
    catch( const std::bad_cast& )
    {
    }

    SCH_COMMIT commit( m_toolMgr );

    if( data.findString.IsEmpty() )
        return FindAndReplace( ACTIONS::find.MakeEvent() );

    auto doReplace =
            [&]( SCH_ITEM* aItem, SCH_SHEET_PATH* aSheet, EDA_SEARCH_DATA& aData )
            {
                wxCHECK_RET( aSheet, wxT( "must have a sheetpath for undo" ) );

                commit.Modify( aItem, aSheet->LastScreen(), RECURSE_MODE::NO_RECURSE );

                if( aItem->Replace( aData, aSheet ) )
                    m_editor->UpdateItem( aItem, false, true );
            };

    if( currentSheetOnly || selectedOnly )
    {
        if( currentSheet )
        {
            SCH_ITEM* item = nextMatch( m_editor->GetScreen(), currentSheet, nullptr, data, false );

            while( item )
            {
                if( !selectedOnly || item->IsSelected() )
                    doReplace( item, currentSheet, data );

                item = nextMatch( m_editor->GetScreen(), currentSheet, item, data, false );
            }
        }
    }
    else if( SCHEMATIC* schematic = getSchematic() )
    {
        SCH_SHEET_LIST allSheets = schematic->Hierarchy();
        SCH_SCREENS    screens( schematic->Root() );

        for( SCH_SCREEN* screen = screens.GetFirst(); screen; screen = screens.GetNext() )
        {
            SCH_SHEET_LIST sheets = allSheets.FindAllSheetsForScreen( screen );

            for( unsigned ii = 0; ii < sheets.size(); ++ii )
            {
                SCH_ITEM* item = nextMatch( screen, &sheets[ii], nullptr, data, false );

                while( item )
                {
                    if( ii == 0 )
                    {
                        doReplace( item, &sheets[0], data );
                    }
                    else if( item->Type() == SCH_FIELD_T )
                    {
                        SCH_FIELD* field = static_cast<SCH_FIELD*>( item );

                        // References must be handled for each distinct sheet
                        if( field->GetId() == FIELD_T::REFERENCE )
                            doReplace( field, &sheets[ii], data );
                    }

                    item = nextMatch( screen, &sheets[ii], item, data, false );
                }
            }
        }
    }

    if( !commit.Empty() )
    {
        commit.Push( wxS( "Find and Replace All" ) );

        if( currentSheet )
            currentSheet->UpdateAllScreenReferences();
    }

    return 0;
}


void SCH_FIND_REPLACE_TOOL::setTransitions()
{
    Go( &SCH_FIND_REPLACE_TOOL::FindAndReplace,        ACTIONS::find.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::FindAndReplace,        ACTIONS::findAndReplace.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::FindNext,              ACTIONS::findNext.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::FindNext,              ACTIONS::findPrevious.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::FindNext,              ACTIONS::findNextMarker.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::ReplaceAndFindNext,    ACTIONS::replaceAndFindNext.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::ReplaceAll,            ACTIONS::replaceAll.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::UpdateFind,            ACTIONS::updateFind.MakeEvent() );
    Go( &SCH_FIND_REPLACE_TOOL::UpdateFind,            EVENTS::SelectedItemsModified );
    Go( &SCH_FIND_REPLACE_TOOL::UpdateFind,            EVENTS::PointSelectedEvent );
    Go( &SCH_FIND_REPLACE_TOOL::UpdateFind,            EVENTS::SelectedEvent );
    Go( &SCH_FIND_REPLACE_TOOL::UpdateFind,            EVENTS::UnselectedEvent );
    Go( &SCH_FIND_REPLACE_TOOL::UpdateFind,            EVENTS::ClearedEvent );
}
