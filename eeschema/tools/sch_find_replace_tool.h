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

    /// Search terms may be supplied by either the wx frame or a host holder.
    bool runsWithoutAFrame() const override { return true; }

    void ResetSearch();
    void Reset( RESET_REASON aReason ) override;
    bool Wrapped() const { return m_wrapped; }
    unsigned Replaced() const { return m_replaced; }
    VECTOR2I LastFoundCenter() const { return m_lastFoundCenter; }

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

    /// Active search terms from the wx frame or the host holder.
    EDA_SEARCH_DATA* searchData() const;

    /// The schematic being edited, or null for the symbol editor and the symbol viewer.
    SCHEMATIC* getSchematic() const;

    /// The sheet being edited, or null where the editing context has no sheets.
    SCH_SHEET_PATH* getCurrentSheet() const;

private:
    VECTOR2I m_lastFoundCenter;
    bool m_wrapped = false;
    unsigned m_replaced = 0;
    SCH_ITEM*   m_afterItem = nullptr;
    SCH_SCREEN* m_afterItemScreen = nullptr;
    wxString    m_lastSearchString;
    bool        m_foundItemHighlighted;
};


#endif // SCH_FIND_REPLACE_TOOL_H
