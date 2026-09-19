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


#ifndef SCH_FIND_REPLACE_TOOL_H
#define SCH_FIND_REPLACE_TOOL_H

#include <sch_base_frame.h>
#include <tools/sch_tool_base.h>

class SCHEMATIC;
struct EDA_SEARCH_DATA;


/**
 * Handle actions specific to the schematic editor.
 */
class SCH_FIND_REPLACE_TOOL : public wxEvtHandler, public SCH_TOOL_BASE<SCH_BASE_FRAME>
{
public:
    SCH_FIND_REPLACE_TOOL()  :
            SCH_TOOL_BASE<SCH_BASE_FRAME>( "eeschema.FindReplace" ),
            m_foundItemHighlighted( false )
    { }

    ~SCH_FIND_REPLACE_TOOL() { }

    /**
     * Runs on an editing context that is not a wxFrame.
     *
     * The searching and replacing themselves are the document's — the screen, the sheet
     * list and the items are all reached through SCHEMATIC_HOLDER — but the *terms* are
     * not: EDA_SEARCH_DATA is owned by EDA_DRAW_FRAME and filled in by DIALOG_SCH_FIND,
     * and neither this tool nor SCHEMATIC_HOLDER has a copy. ::m_lastSearchString is a
     * cache for deciding when to restart a search, not a search term.
     *
     * So every action here declines without a window, at the one line that asks for the
     * search data: ::FindAndReplace (which *is* the dialog), ::FindNext, ::HasMatch,
     * ::ReplaceAndFindNext, ::ReplaceAll and ::UpdateFind. What this buys today is that
     * the tool registers and answers events instead of being absent — the ones bound to
     * the selection arrive whether or not anyone is finding anything — and that the rest
     * of the body already goes through the document, so giving the search terms a
     * non-wx route is the only thing left to do.
     *
     * Beyond the terms, three smaller things are windows and stay guarded: the find
     * dialog itself (which doubles as "is a search in progress"), ::FindNext scrolling
     * the canvas to what it found, and its "Reached end of sheet"/"No matches found"
     * status text.
     */
    bool runsWithoutAFrame() const override { return true; }

    int FindAndReplace( const TOOL_EVENT& aEvent );

    int FindNext( const TOOL_EVENT& aEvent );
    bool HasMatch();

    SCH_ITEM* GetLastFoundItem() const { return m_afterItem; }

    int ReplaceAndFindNext( const TOOL_EVENT& aEvent );
    int ReplaceAll( const TOOL_EVENT& aEvent );

    int UpdateFind( const TOOL_EVENT& aEvent );

private:
    ///< Set up handlers for various events.
    void setTransitions() override;

    /**
     * Advance the search and returns the next matching item after \a aAfter.
     *
     * @param aScreen Pointer to the screen used for searching
     * @param aAfter Starting match to compare
     * @param aData Search data to compare against or NULL to match the first item found
     * @param reverse Search in reverse (find previous)
     * @return pointer to the next search item found or NULL if nothing found
     */
    SCH_ITEM* nextMatch( SCH_SCREEN* aScreen, SCH_SHEET_PATH* aSheet, SCH_ITEM* aAfter,
                         EDA_SEARCH_DATA& aData, bool reverse );
    EDA_ITEM* getCurrentMatch();

    /**
     * What is being searched for, or **null** when there is no window holding it.
     *
     * This is the one thing in this tool that has no route through SCHEMATIC_HOLDER; see
     * ::runsWithoutAFrame. Every action starts by asking for it and declines on null.
     */
    EDA_SEARCH_DATA* searchData() const;

    /// The schematic being edited, or null for the symbol editor and the symbol viewer.
    SCHEMATIC* getSchematic() const;

    /// The sheet being edited, or null where the editing context has no sheets.
    SCH_SHEET_PATH* getCurrentSheet() const;

private:
    SCH_ITEM*   m_afterItem = nullptr;
    SCH_SCREEN* m_afterItemScreen = nullptr;
    wxString    m_lastSearchString;
    bool        m_foundItemHighlighted;
};


#endif // SCH_FIND_REPLACE_TOOL_H
