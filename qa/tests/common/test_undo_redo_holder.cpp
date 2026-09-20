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

/**
 * @file
 * The undo and redo stacks, on something that is not a wxFrame.
 *
 * These lived on `EDA_BASE_FRAME` and were reachable only through a window, which is why
 * they had no direct test: exercising them meant standing up a frame. Now that they are
 * `UNDO_REDO_HOLDER`, the push/pop/trim logic can be driven on its own — and that logic is
 * worth pinning, because the trim is the part with an off-by-one available and because a
 * document owner that is not a window is about to rely on it (`SCH_HOST`).
 */

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <memory>
#include <vector>

#include <undo_redo_holder.h>


namespace
{

/**
 * The minimum a holder has to be.
 *
 * `ClearUndoORRedoList` is the one thing a holder must supply, because whether a picked
 * item may be *deleted* depends on the document — an item on a stack may also be on a
 * screen. This one records the requests instead of deleting anything, which is what lets
 * the test assert on the trim.
 */
class RECORDING_HOLDER : public UNDO_REDO_HOLDER
{
public:
    struct REQUEST
    {
        UNDO_REDO_LIST m_List;
        int            m_ItemCount;
    };

    void ClearUndoORRedoList( UNDO_REDO_LIST aList, int aItemCount = -1 ) override
    {
        m_Requests.push_back( { aList, aItemCount } );

        UNDO_REDO_CONTAINER& list = ( aList == UNDO_LIST ) ? m_undoList : m_redoList;

        if( aItemCount < 0 )
        {
            list.ClearCommandList();
            return;
        }

        for( int ii = 0; ii < aItemCount && !list.m_CommandsList.empty(); ++ii )
        {
            delete list.m_CommandsList.front();
            list.m_CommandsList.erase( list.m_CommandsList.begin() );
        }
    }

    void SetMaxItems( int aMax ) { m_undoRedoCountMax = aMax; }

    std::vector<REQUEST> m_Requests;
};


PICKED_ITEMS_LIST* command( const wxString& aDescription )
{
    PICKED_ITEMS_LIST* list = new PICKED_ITEMS_LIST();

    list->SetDescription( aDescription );

    return list;
}

} // namespace


BOOST_AUTO_TEST_SUITE( UndoRedoHolder )


/**
 * A holder starts empty, and says so rather than dereferencing an empty stack for its
 * description — which is what a menu item asks for on startup.
 */
BOOST_AUTO_TEST_CASE( AnEmptyHolderHasNothingToUndo )
{
    RECORDING_HOLDER holder;

    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 0 );
    BOOST_CHECK_EQUAL( holder.GetRedoCommandCount(), 0 );
    BOOST_CHECK( holder.GetUndoActionDescription().IsEmpty() );
    BOOST_CHECK( holder.GetRedoActionDescription().IsEmpty() );
    BOOST_CHECK( holder.PopCommandFromUndoList() == nullptr );
    BOOST_CHECK( holder.PopCommandFromRedoList() == nullptr );
}


/**
 * Last in, first out, on both stacks, and the description is the newest command's —
 * which is what "Undo <thing>" in a menu shows.
 */
BOOST_AUTO_TEST_CASE( CommandsComeBackNewestFirst )
{
    RECORDING_HOLDER holder;

    holder.PushCommandToUndoList( command( wxT( "Move" ) ) );
    holder.PushCommandToUndoList( command( wxT( "Delete" ) ) );

    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 2 );
    BOOST_CHECK_EQUAL( holder.GetUndoActionDescription(), wxT( "Delete" ) );

    std::unique_ptr<PICKED_ITEMS_LIST> newest( holder.PopCommandFromUndoList() );

    BOOST_REQUIRE( newest );
    BOOST_CHECK_EQUAL( newest->GetDescription(), wxT( "Delete" ) );
    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 1 );
    BOOST_CHECK_EQUAL( holder.GetUndoActionDescription(), wxT( "Move" ) );

    holder.PushCommandToRedoList( command( wxT( "Delete" ) ) );

    BOOST_CHECK_EQUAL( holder.GetRedoCommandCount(), 1 );
    BOOST_CHECK_EQUAL( holder.GetRedoActionDescription(), wxT( "Delete" ) );

    std::unique_ptr<PICKED_ITEMS_LIST> redo( holder.PopCommandFromRedoList() );

    BOOST_REQUIRE( redo );
    BOOST_CHECK_EQUAL( holder.GetRedoCommandCount(), 0 );
}


/**
 * The depth limit trims the *oldest* commands, one request per overflow, and asks the
 * document to do the deleting.
 *
 * Zero means no limit, which is the shipped default (`DEFAULT_MAX_UNDO_ITEMS`), so the
 * limit has to be set for this to be observable at all.
 */
BOOST_AUTO_TEST_CASE( TheDepthLimitTrimsTheOldest )
{
    RECORDING_HOLDER holder;

    holder.SetMaxItems( 2 );

    holder.PushCommandToUndoList( command( wxT( "one" ) ) );
    holder.PushCommandToUndoList( command( wxT( "two" ) ) );

    BOOST_CHECK( holder.m_Requests.empty() );
    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 2 );

    holder.PushCommandToUndoList( command( wxT( "three" ) ) );

    BOOST_REQUIRE_EQUAL( holder.m_Requests.size(), 1u );
    BOOST_CHECK_EQUAL( holder.m_Requests[0].m_List, UNDO_REDO_HOLDER::UNDO_LIST );
    BOOST_CHECK_EQUAL( holder.m_Requests[0].m_ItemCount, 1 );

    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 2 );
    BOOST_CHECK_EQUAL( holder.GetUndoActionDescription(), wxT( "three" ) );

    // "one" is the one that went, not "three".
    std::unique_ptr<PICKED_ITEMS_LIST> newest( holder.PopCommandFromUndoList() );
    std::unique_ptr<PICKED_ITEMS_LIST> older( holder.PopCommandFromUndoList() );

    BOOST_REQUIRE( newest );
    BOOST_REQUIRE( older );
    BOOST_CHECK_EQUAL( newest->GetDescription(), wxT( "three" ) );
    BOOST_CHECK_EQUAL( older->GetDescription(), wxT( "two" ) );
}


/**
 * No limit means no trimming, which is what `DEFAULT_MAX_UNDO_ITEMS` being zero asks for.
 */
BOOST_AUTO_TEST_CASE( ZeroMeansNoLimit )
{
    RECORDING_HOLDER holder;

    BOOST_REQUIRE_EQUAL( holder.GetMaxUndoItems(), DEFAULT_MAX_UNDO_ITEMS );
    BOOST_REQUIRE_EQUAL( DEFAULT_MAX_UNDO_ITEMS, 0 );

    for( int ii = 0; ii < 50; ++ii )
        holder.PushCommandToUndoList( command( wxString::Format( wxT( "%d" ), ii ) ) );

    BOOST_CHECK( holder.m_Requests.empty() );
    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 50 );

    holder.ClearUndoRedoList();

    // Both stacks, in that order, each asked to clear everything.
    BOOST_REQUIRE_EQUAL( holder.m_Requests.size(), 2u );
    BOOST_CHECK_EQUAL( holder.m_Requests[0].m_List, UNDO_REDO_HOLDER::UNDO_LIST );
    BOOST_CHECK_EQUAL( holder.m_Requests[0].m_ItemCount, -1 );
    BOOST_CHECK_EQUAL( holder.m_Requests[1].m_List, UNDO_REDO_HOLDER::REDO_LIST );
    BOOST_CHECK_EQUAL( holder.m_Requests[1].m_ItemCount, -1 );

    BOOST_CHECK_EQUAL( holder.GetUndoCommandCount(), 0 );
}


BOOST_AUTO_TEST_SUITE_END()
