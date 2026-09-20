// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <eda_item.h>
#include <page_info.h>
#include <title_block.h>
#include <schematic.h>
#include <sch_screen.h>
#include <set>

/** Native page-setting transaction, usable without an EDA_DRAW_FRAME.
 * Screen UUIDs resolve at undo time; no document pointer survives in the snapshot.
 */
class SCH_PAGE_SETTINGS_UNDO_ITEM : public EDA_ITEM
{
public:
    struct PAGE_STATE
    {
        KIID screen;
        PAGE_INFO page;
        TITLE_BLOCK title;
    };

    SCH_PAGE_SETTINGS_UNDO_ITEM( SCHEMATIC& schematic, SCH_SCREEN* current, bool all ) :
            EDA_ITEM( WS_PROXY_UNDO_ITEM_PLUS_T )
    {
        std::set<KIID> seen;
        for( const auto& sheet : schematic.Hierarchy() )
        {
            auto* screen = sheet.LastScreen();
            if( all || screen == current ) m_pageNumbers.emplace_back(sheet.PathAsString(),sheet.GetPageNumber());
            if( ( all || screen == current ) && seen.insert( screen->GetUuid() ).second )
                m_pages.push_back( { screen->GetUuid(), screen->GetPageSettings(), screen->GetTitleBlock() } );
        }
    }

    void Swap( SCHEMATIC& schematic )
    {
        for(auto& state:m_pageNumbers)
            for(auto sheet:schematic.Hierarchy())
                if(sheet.PathAsString()==state.first)
                {
                    const auto page=sheet.GetPageNumber(); sheet.SetPageNumber(state.second); state.second=page; break;
                }
        schematic.RefreshHierarchy();
        for( auto& state : m_pages )
            for( const auto& sheet : schematic.Hierarchy() )
            {
                auto* screen = sheet.LastScreen();
                if( screen->GetUuid() != state.screen ) continue;
                const auto page = screen->GetPageSettings();
                const auto title = screen->GetTitleBlock();
                screen->SetPageSettings( state.page );
                screen->SetTitleBlock( state.title );
                state.page = page; state.title = title;
                break;
            }
    }
    wxString GetClass() const override { return "SCH_PAGE_SETTINGS_UNDO_ITEM"; }
#if defined(DEBUG)
    void Show( int, std::ostream& ) const override {}
#endif
private:
    std::vector<PAGE_STATE> m_pages;
    std::vector<std::pair<wxString,wxString>> m_pageNumbers;
};
