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
 * A TOOL_MANAGER whose holder is not a frame.
 *
 * `TOOL_MANAGER::GetToolHolder()` returns a `TOOLS_HOLDER*`, and eeschema's frames reach
 * it through `EDA_BASE_FRAME`, alongside `wxFrame` and `KIWAY_HOLDER`. Downcasting it
 * with `static_cast` therefore adjusts the pointer by the offset of a `TOOLS_HOLDER`
 * subobject inside a frame — which, for a holder that is not a frame, does not exist. The
 * result is a plausible non-null pointer to nothing, and the first virtual call through it
 * is undefined behaviour that no null check can catch.
 *
 * A non-wx host installs exactly such a holder, so these tests install one too and assert
 * what makes that defined: an edit still goes through `SCH_COMMIT`, minus the undo stack
 * and the canvas that live on the frame; every tool that needs a frame declines to
 * initialise instead of running off a wild pointer; and `TOOL_MANAGER` drops the ones that
 * declined, so none is left holding the pointer.
 */

#include <boost/test/unit_test.hpp>

#include <memory>

#include <schematic.h>
#include <sch_commit.h>
#include <sch_label.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <tool/tool_manager.h>
#include <tool/tools_holder.h>
#include <view/view.h>

// The tool headers below inline through their frame type, so it has to be complete here.
#include <sch_edit_frame.h>
#include <symbol_edit_frame.h>

#include <tools/ee_graphic_tool.h>
#include <tools/sch_align_tool.h>
#include <tools/sch_design_block_control.h>
#include <tools/sch_drawing_tools.h>
#include <tools/sch_edit_table_tool.h>
#include <tools/sch_edit_tool.h>
#include <tools/sch_editor_control.h>
#include <tools/sch_find_replace_tool.h>
#include <tools/sch_inspection_tool.h>
#include <tools/sch_line_wire_bus_tool.h>
#include <tools/sch_move_tool.h>
#include <tools/sch_navigate_tool.h>
#include <tools/sch_point_editor.h>
#include <tools/sch_selection_tool.h>
#include <tools/symbol_editor_control.h>
#include <tools/symbol_editor_drawing_tools.h>
#include <tools/symbol_editor_edit_tool.h>
#include <tools/symbol_editor_move_tool.h>
#include <tools/symbol_editor_pin_tool.h>

#include <schematic_utils/schematic_file_util.h>
#include <settings/settings_manager.h>


namespace
{

/**
 * The minimum a non-wx host has to be: `GetToolCanvas()` is `TOOLS_HOLDER`'s only pure
 * virtual, and returning null is already a supported state — `SIMULATOR_FRAME` and
 * `MERGETOOL_FRAME` both do it in production, and `TOOL_DISPATCHER` null-checks it.
 */
class NON_FRAME_HOLDER : public TOOLS_HOLDER
{
public:
    wxWindow* GetToolCanvas() const override { return nullptr; }
};


SCH_LABEL* findLabel( SCH_SCREEN& aScreen, const wxString& aText )
{
    for( SCH_ITEM* item : aScreen.Items().OfType( SCH_LABEL_T ) )
    {
        SCH_LABEL* label = static_cast<SCH_LABEL*>( item );

        if( label->GetText() == aText )
            return label;
    }

    return nullptr;
}

} // namespace


BOOST_AUTO_TEST_SUITE( NonFrameToolsHolder )


/**
 * Every edit in eeschema goes through SCH_COMMIT, and its TOOL_MANAGER constructor
 * downcasts the holder before it does anything else. So this is the path that has to
 * survive a holder that is not a frame, and what it must do is edit the document and skip
 * the two things that only a frame has: the undo stack and the canvas.
 *
 * Note that the push here does *not* pass SKIP_UNDO. Deciding to skip it is the commit's
 * job, precisely because a caller cannot know whether a frame is installed.
 */
BOOST_AUTO_TEST_CASE( ACommitEditsTheDocumentWithoutAFrame )
{
    SETTINGS_MANAGER           settings;
    std::unique_ptr<SCHEMATIC> schematic;
    KI_TEST::LoadSchematic( settings, "netlists/multinetclasses/multinetclasses", schematic );

    const SCH_SHEET_PATH path = schematic->Hierarchy().front();
    SCH_SCREEN*          screen = path.LastScreen();
    SCH_LABEL*           label = findLabel( *screen, wxT( "NET_2" ) );

    BOOST_REQUIRE( label );

    NON_FRAME_HOLDER holder;
    TOOL_MANAGER     mgr;
    mgr.SetEnvironment( schematic.get(), nullptr, nullptr, nullptr, &holder );

    BOOST_REQUIRE( mgr.GetToolHolder() == &holder );

    screen->SetContentModified( false );

    SCH_COMMIT commit( &mgr );
    commit.Modify( label, screen );
    label->SetText( wxT( "RENAMED_WITHOUT_A_FRAME" ) );
    commit.Push( wxT( "Rename label" ) );

    BOOST_CHECK( commit.Empty() );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( wxT( "RENAMED_WITHOUT_A_FRAME" ) ) );

    // The document is dirty, even though OnModify() lives on the frame: the commit marks
    // the screen itself.
    BOOST_CHECK( screen->IsContentModified() );

    // Connectivity is a document concern, so it is rebuilt with or without a frame.
    const auto name = label->GetConnectionName( &path );
    BOOST_REQUIRE( name );
    BOOST_CHECK_EQUAL( *name, wxString( wxT( "/RENAMED_WITHOUT_A_FRAME" ) ) );
}


/**
 * A tool that needs a frame has no defined behaviour without one — `m_frame` is its route
 * to the screen, the selection, the undo stack and every dialog — so the right answer is
 * to decline, which `Init()` returning false already means.
 *
 * This is deliberately the whole eeschema roster, both editors', because the property
 * being asserted is "no eeschema tool believes a non-frame holder is its frame" and a
 * sampled version of that is worth much less.
 *
 * Note what this does *not* contradict. Most of the roster now answers
 * `runsWithoutAFrame()` true and runs on `SCH_HOST` — see
 * `test_sch_host.cpp`. They still decline here, and for the earlier of
 * `SCH_TOOL_BASE::Init()`'s two reasons rather than the later one: NON_FRAME_HOLDER is a
 * bare TOOLS_HOLDER and not a SCHEMATIC_HOLDER, so there is no editing context to ask for
 * a screen at all, and `runsWithoutAFrame()` is never reached. A holder that offers
 * neither a window nor a document is not something any of these tools can run on, and that
 * is the property this case pins.
 */
BOOST_AUTO_TEST_CASE( EveryEeschemaToolDeclinesAHolderThatIsNotAFrame )
{
    SETTINGS_MANAGER           settings;
    std::unique_ptr<SCHEMATIC> schematic;
    KI_TEST::LoadSchematic( settings, "netlists/multinetclasses/multinetclasses", schematic );

    NON_FRAME_HOLDER holder;

    // A KIGFX::VIEW, because SCH_SELECTION_TOOL's destructor unlinks itself from it without
    // a null check. A non-wx host has one — SCH_HOST owns a SCH_VIEW — so supplying one here
    // is the test matching the host rather than a workaround.
    KIGFX::VIEW view;

    // One TOOL_MANAGER per tool, so that a tool which does the wrong thing names itself
    // instead of taking the rest of the roster down with it.
    const auto declines =
            [&]( TOOL_BASE* aTool )
            {
                TOOL_MANAGER mgr;   // owns aTool from RegisterTool() on
                mgr.SetEnvironment( schematic.get(), &view, nullptr, nullptr, &holder );
                mgr.RegisterTool( aTool );

                BOOST_TEST_CONTEXT( aTool->GetName() )
                {
                    BOOST_CHECK( !aTool->Init() );
                }
            };

    // Schematic editor
    declines( new SCH_SELECTION_TOOL );
    declines( new SCH_DRAWING_TOOLS );
    declines( new EE_GRAPHIC_TOOL );
    declines( new SCH_LINE_WIRE_BUS_TOOL );
    declines( new SCH_MOVE_TOOL );
    declines( new SCH_ALIGN_TOOL );
    declines( new SCH_EDIT_TOOL );
    declines( new SCH_EDIT_TABLE_TOOL );
    declines( new SCH_INSPECTION_TOOL );
    declines( new SCH_DESIGN_BLOCK_CONTROL );
    declines( new SCH_EDITOR_CONTROL );
    declines( new SCH_FIND_REPLACE_TOOL );
    declines( new SCH_POINT_EDITOR );
    declines( new SCH_NAVIGATE_TOOL );

    // Symbol editor
    declines( new SYMBOL_EDITOR_PIN_TOOL );
    declines( new SYMBOL_EDITOR_DRAWING_TOOLS );
    declines( new SYMBOL_EDITOR_MOVE_TOOL );
    declines( new SYMBOL_EDITOR_EDIT_TOOL );
    declines( new SYMBOL_EDITOR_CONTROL );
}


/**
 * And the consequence of that, through the framework: `TOOL_MANAGER::InitTools()`
 * unregisters and deletes a tool whose `Init()` returns false, so a non-frame holder ends
 * up with no tool at all rather than with tools holding a pointer that is not a frame.
 */
BOOST_AUTO_TEST_CASE( InitToolsDropsWhatDeclinedAHolderThatIsNotAFrame )
{
    SETTINGS_MANAGER           settings;
    std::unique_ptr<SCHEMATIC> schematic;
    KI_TEST::LoadSchematic( settings, "netlists/multinetclasses/multinetclasses", schematic );

    NON_FRAME_HOLDER holder;
    KIGFX::VIEW      view;
    TOOL_MANAGER     mgr;
    mgr.SetEnvironment( schematic.get(), &view, nullptr, nullptr, &holder );

    mgr.RegisterTool( new SCH_SELECTION_TOOL );
    mgr.RegisterTool( new SCH_EDITOR_CONTROL );
    mgr.RegisterTool( new SCH_MOVE_TOOL );

    BOOST_REQUIRE( mgr.GetTool<SCH_SELECTION_TOOL>() != nullptr );

    mgr.InitTools();

    BOOST_CHECK( mgr.GetTool<SCH_SELECTION_TOOL>() == nullptr );
    BOOST_CHECK( mgr.GetTool<SCH_EDITOR_CONTROL>() == nullptr );
    BOOST_CHECK( mgr.GetTool<SCH_MOVE_TOOL>() == nullptr );
}


/**
 * The same holder, but installed as null — which is what `EESCHEMA_HELPERS` does for the
 * CLI, and is the state the `frame && ...` guards were written for. Kept alongside the
 * above so that the two cases are visibly one behaviour rather than two.
 */
BOOST_AUTO_TEST_CASE( ACommitEditsTheDocumentWithNoHolderAtAll )
{
    SETTINGS_MANAGER           settings;
    std::unique_ptr<SCHEMATIC> schematic;
    KI_TEST::LoadSchematic( settings, "netlists/multinetclasses/multinetclasses", schematic );

    const SCH_SHEET_PATH path = schematic->Hierarchy().front();
    SCH_SCREEN*          screen = path.LastScreen();
    SCH_LABEL*           label = findLabel( *screen, wxT( "NET_2" ) );

    BOOST_REQUIRE( label );

    TOOL_MANAGER mgr;
    mgr.SetEnvironment( schematic.get(), nullptr, nullptr, nullptr, nullptr );

    SCH_COMMIT commit( &mgr );
    commit.Modify( label, screen );
    label->SetText( wxT( "RENAMED_WITHOUT_A_HOLDER" ) );
    commit.Push( wxT( "Rename label" ) );

    BOOST_CHECK( commit.Empty() );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( wxT( "RENAMED_WITHOUT_A_HOLDER" ) ) );
}


BOOST_AUTO_TEST_SUITE_END()
