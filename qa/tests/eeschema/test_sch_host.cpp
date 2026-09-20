/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * @file
 * Tests for SCH_HOST and its C ABI: the seam between the schematic document
 * model and a renderer that is not wxWidgets.
 *
 * What is actually being asserted here is that
 * load -> KIGFX::VIEW -> SCH_PAINTER -> RECORDING_GAL -> DRAW_STREAM works on
 * real files, and keeps working. The interesting properties are:
 *
 * - a real schematic produces a non-trivial stream, not an empty one;
 * - the retained-group arena is stable across redraws of an unchanged document,
 *   because that is the property the 120 Hz pan budget rests on;
 * - the C ABI reports failure rather than unwinding into a caller that has no
 *   handler, which is the one way this seam could take a process down.
 */

#include <cmath>
#include <chrono>
#include <thread>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <qa_utils/wx_utils/unit_test_utils.h>

#include <base_units.h>
#include <gal/recording/draw_stream.h>
#include <sch_host/sch_host_abi.h>
#include <sch_commit.h>
#include <sch_label.h>
#include <sch_junction.h>
#include <lib_symbol.h>
#include <sch_line.h>
#include <sch_screen.h>
#include <schematic.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#include <env_vars.h>
#include <settings/environment.h>

// Code under test
#include <host/sch_host.h>
#include <host/sch_host_control.h>
#include <eeschema_settings.h>
#include <kiface_base.h>
#include <pgm_base.h>
#include <settings/settings_manager.h>
#include <tool/host_tool_dispatcher.h>
#include <tool/tool_manager.h>
#include <view/host_view_controls.h>
#include <view/view.h>

// The tool headers inline through their frame type, so it has to be complete here.
#include <sch_edit_frame.h>

#include <tool/common_control.h>
#include <tool/common_tools.h>
#include <tool/embed_tool.h>
#include <tool/picker_tool.h>
#include <tool/properties_tool.h>
#include <tool/zoom_tool.h>
#include <tools/ee_graphic_tool.h>
#include <tools/sch_align_tool.h>
#include <tools/sch_actions.h>
#include <tools/sch_design_block_control.h>
#include <tools/sch_drawing_tools.h>
#include <tools/sch_edit_table_tool.h>
#include <tools/sch_find_replace_tool.h>
#include <tools/sch_group_tool.h>
#include <tools/sch_inspection_tool.h>
#include <tools/sch_navigate_tool.h>
#include <tools/sch_edit_tool.h>
#include <tools/sch_editor_control.h>
#include <tools/sch_line_wire_bus_tool.h>
#include <tools/sch_move_tool.h>
#include <tools/sch_point_editor.h>
#include <tools/sch_selection_tool.h>


namespace
{

wxString eeschemaFixture( const wxString& aRelativePath )
{
    return wxString::FromUTF8( KI_TEST::GetEeschemaTestDataDir() ) + aRelativePath;
}


/// Count commands per opcode across both arenas of a published stream.
std::map<std::uint16_t, std::size_t> opcodeCounts( const kgds_stream_view& aView )
{
    std::map<std::uint16_t, std::size_t> counts;

    for( std::size_t ii = 0; ii < aView.group_cmd_count; ++ii )
        counts[aView.group_cmds[ii].op]++;

    for( std::size_t ii = 0; ii < aView.frame_cmd_count; ++ii )
        counts[aView.frame_cmds[ii].op]++;

    return counts;
}


/**
 * Every coordinate index in the stream must land inside the arena the command
 * belongs to. The renderer trusts these indices, so a stream that violates this
 * is a buffer overrun waiting to happen on the other side of the boundary.
 */
bool coordIndicesInRange( const kgds_stream_view& aView )
{
    const auto check = [&]( const kgds_cmd* aCmds, std::size_t aCount, std::size_t aCoordCount )
    {
        for( std::size_t ii = 0; ii < aCount; ++ii )
        {
            kgds_coord_ref refs[KGDS_MAX_COORD_REFS];
            const int      n = kgds_coord_refs( &aCmds[ii], refs );

            for( int jj = 0; jj < n; ++jj )
            {
                if( static_cast<std::size_t>( refs[jj].start ) + refs[jj].count > aCoordCount )
                    return false;
            }
        }

        return true;
    };

    return check( aView.group_cmds, aView.group_cmd_count, aView.group_coord_count )
           && check( aView.frame_cmds, aView.frame_cmd_count, aView.frame_coord_count );
}

} // namespace


/**
 * Installs the application settings SCH_PAINTER reads.
 *
 * qa_eeschema links the mock Kiface() from qa/mocks, whose KifaceSettings() is
 * null. SCH_PAINTER reaches for eeconfig() the first time it draws a sheet, so
 * without this every render test dies in a null dereference a long way from its
 * cause -- which is exactly how it first showed up.
 *
 * The settings manager owns the object once registered; InitSettings() only
 * hands the kiface a pointer to it.
 */
struct SCH_HOST_SETTINGS_FIXTURE
{
    SCH_HOST_SETTINGS_FIXTURE()
    {
        // The condition that matters is whether the *kiface* has settings, not
        // whether the settings manager has them registered. test_module.cpp
        // already registers an EESCHEMA_SETTINGS at startup, so guarding on the
        // registration would skip the InitSettings() call that actually matters
        // and leave SCH_PAINTER's eeconfig() null.
        //
        // SCH_HOST::ensureKifaceSettings() also covers this, so this fixture is
        // belt and braces: it keeps any future test that drives SCH_PAINTER
        // without going through SCH_HOST from hitting the same null.
        if( Kiface().KifaceSettings() )
            return;

        SETTINGS_MANAGER& manager = Pgm().GetSettingsManager();

        EESCHEMA_SETTINGS* settings = manager.GetAppSettings<EESCHEMA_SETTINGS>( "eeschema" );

        if( !settings )
        {
            settings = new EESCHEMA_SETTINGS;
            manager.RegisterSettings( settings, false );
        }

        Kiface().InitSettings( settings );
    }
};


BOOST_FIXTURE_TEST_SUITE( SchHost, SCH_HOST_SETTINGS_FIXTURE )


/**
 * A freshly constructed session owns a canvas but no document, and every query
 * that needs one says so rather than crashing.
 */
BOOST_AUTO_TEST_CASE( EmptySession )
{
    SCH_HOST host;

    BOOST_CHECK( !host.IsLoaded() );
    BOOST_CHECK_EQUAL( host.GetItemCount(), 0u );
    BOOST_CHECK( host.GetSheetHierarchy().empty() );
    BOOST_CHECK( !host.IsModified() );
    BOOST_CHECK( host.GetScreen() == nullptr );

    // Rendering nothing is legal and yields an empty-but-valid stream.
    kgds_stream_view view = host.Render();
    BOOST_CHECK_EQUAL( view.version, KGDS_VERSION );
    BOOST_CHECK_EQUAL( view.group_count, 0u );
}


/**
 * The end-to-end case: a real schematic loads, the view fills, and a frame comes
 * out with geometry in it.
 */
BOOST_AUTO_TEST_CASE( LoadAndRenderProducesGeometry )
{
    SCH_HOST host;

    BOOST_REQUIRE_MESSAGE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ),
                           host.GetLastError().ToStdString() );

    BOOST_CHECK( host.IsLoaded() );
    BOOST_CHECK_GE( host.GetSheetHierarchy().size(), 1u );
    BOOST_CHECK_GT( host.GetItemCount(), 0u );

    host.SetViewport( 1920, 1080, VECTOR2D( 0, 0 ), 1.0 );
    host.ZoomToFit();

    const kgds_stream_view view = host.Render();

    BOOST_CHECK_EQUAL( view.version, KGDS_VERSION );

    // VIEW caches every item it draws into a retained group, so a sheet with items
    // must produce groups, and the groups must have bodies.
    BOOST_CHECK_GT( view.group_count, 0u );
    BOOST_CHECK_GT( view.group_cmd_count, 0u );

    // A frame that draws those groups has at least a begin, an end, and one
    // DRAW_GROUP per cached item it decided was visible.
    BOOST_CHECK_GT( view.frame_cmd_count, 0u );

    // Geometry means coordinates. An all-state stream would mean SCH_PAINTER ran
    // but drew nothing, which is the failure mode worth catching.
    BOOST_CHECK_GT( view.group_coord_count, 0u );

    BOOST_CHECK( coordIndicesInRange( view ) );

    const std::map<std::uint16_t, std::size_t> counts = opcodeCounts( view );

    BOOST_CHECK_EQUAL( counts.count( KGDS_OP_BEGIN_FRAME ), 1u );
    BOOST_CHECK_EQUAL( counts.count( KGDS_OP_END_FRAME ), 1u );
    BOOST_CHECK_GT( counts.count( KGDS_OP_DRAW_GROUP ), 0u );

    // The painter sets a colour before it draws; if it did not, everything would be
    // recorded in whatever state the previous item left behind.
    BOOST_CHECK_GT( counts.count( KGDS_OP_SET_STROKE_COLOR ), 0u );

    // Schematics are made of lines. Any of these carrying the geometry is fine, but
    // at least one of them must.
    const std::size_t lineish = counts.count( KGDS_OP_LINE ) + counts.count( KGDS_OP_SEGMENT )
                                + counts.count( KGDS_OP_POLYLINE )
                                + counts.count( KGDS_OP_SEGMENT_CHAIN );
    BOOST_CHECK_GT( lineish, 0u );
}


/**
 * Text must arrive as geometry, tagged as such.
 *
 * This is the single assumption the Rust side leans on hardest: that it needs no
 * font engine because KIFONT has already lowered every glyph to polylines or
 * polygons before the GAL sees it. A schematic with labels and fields that
 * produced no KGDS_FLAG_GLYPH geometry would mean that stopped being true.
 */
BOOST_AUTO_TEST_CASE( TextArrivesAsTaggedGeometry )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    host.SetViewport( 1920, 1080, VECTOR2D( 0, 0 ), 1.0 );
    host.ZoomToFit();

    const kgds_stream_view view = host.Render();

    std::size_t glyphCommands = 0;

    for( std::size_t ii = 0; ii < view.group_cmd_count; ++ii )
    {
        if( view.group_cmds[ii].flags & KGDS_FLAG_GLYPH )
            ++glyphCommands;
    }

    BOOST_CHECK_GT( glyphCommands, 0u );
}


/**
 * Re-rendering an unchanged document must not grow the retained group arena.
 *
 * KIGFX::VIEW records an item's geometry into a group once and then replays it
 * with DrawGroup on every subsequent frame. If a redraw re-recorded the groups,
 * the arena would grow without bound during a pan and the renderer would
 * re-upload every buffer each frame — which is precisely the cost the two-arena
 * design exists to avoid.
 */
BOOST_AUTO_TEST_CASE( RedrawDoesNotGrowRetainedGroups )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    host.SetViewport( 1280, 800, VECTOR2D( 0, 0 ), 1.0 );
    host.ZoomToFit();

    const kgds_stream_view first = host.Render();

    const std::size_t groupCount = first.group_count;
    const std::size_t groupCmds = first.group_cmd_count;
    const std::size_t groupCoords = first.group_coord_count;

    BOOST_REQUIRE_GT( groupCount, 0u );

    // Several more frames, including one that pans, because panning is the case
    // that matters and it must not disturb the cache either.
    for( int pass = 0; pass < 4; ++pass )
    {
        if( pass == 2 )
            host.SetViewport( 1280, 800, host.GetViewCenter() + VECTOR2D( 100000, 0 ),
                              host.GetViewScale() );

        const kgds_stream_view again = host.Render();

        BOOST_CHECK_EQUAL( again.group_count, groupCount );
        BOOST_CHECK_EQUAL( again.group_cmd_count, groupCmds );
        BOOST_CHECK_EQUAL( again.group_coord_count, groupCoords );
    }
}


/**
 * The bounding box has to be believable: a real page, the right way up, and of a
 * size that is actually a paper size rather than a stray default.
 */
BOOST_AUTO_TEST_CASE( DocumentBoundingBoxIsSane )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    const BOX2I page = host.GetDocumentBBox( true );

    BOOST_CHECK_EQUAL( page.GetX(), 0 );
    BOOST_CHECK_EQUAL( page.GetY(), 0 );
    BOOST_CHECK_GT( page.GetWidth(), 0 );
    BOOST_CHECK_GT( page.GetHeight(), 0 );

    // Eeschema's internal unit is 100 nm, so a page is a few million IU, not a few
    // billion. Bound it by real paper sizes rather than by magic numbers: the
    // smallest sheet KiCad offers is A5 (210 x 148 mm) and the largest is A0
    // (1189 x 841 mm). Anything outside that is not a page.
    BOOST_CHECK_GT( page.GetWidth(), schIUScale.mmToIU( 100.0 ) );
    BOOST_CHECK_LT( page.GetWidth(), schIUScale.mmToIU( 1300.0 ) );
    BOOST_CHECK_GT( page.GetHeight(), schIUScale.mmToIU( 100.0 ) );
    BOOST_CHECK_LT( page.GetHeight(), schIUScale.mmToIU( 1300.0 ) );

    // Sheets are landscape by default, and every KiCad paper size is wider than tall
    // in that orientation.
    BOOST_CHECK_GT( page.GetWidth(), page.GetHeight() );

    // The items must sit on the page, not somewhere else entirely.
    const BOX2I items = host.GetDocumentBBox( false );

    BOOST_CHECK_GT( items.GetWidth(), 0 );
    BOOST_CHECK_GT( items.GetHeight(), 0 );
    BOOST_CHECK_LE( items.GetWidth(), page.GetWidth() );
    BOOST_CHECK_LE( items.GetHeight(), page.GetHeight() );
}


/**
 * A hierarchical document exposes every sheet, and switching between them
 * repopulates the view.
 */
BOOST_AUTO_TEST_CASE( SheetHierarchyIsEnumerable )
{
    SCH_HOST host;

    // A two-sheet hierarchy: the root plus one subsheet.
    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "issue10926_1.kicad_sch" ) ) ) );

    const std::vector<SCH_HOST_SHEET_INFO>& sheets = host.GetSheetHierarchy();

    BOOST_REQUIRE_GE( sheets.size(), 1u );

    for( const SCH_HOST_SHEET_INFO& info : sheets )
        BOOST_CHECK( !info.m_Path.IsEmpty() );

    BOOST_CHECK( !host.SetCurrentSheetIndex( sheets.size() ) );
    BOOST_CHECK_EQUAL( host.GetCurrentSheetIndex(), 0u );

    if( sheets.size() > 1 )
    {
        BOOST_CHECK( host.SetCurrentSheetIndex( 1 ) );
        BOOST_CHECK_EQUAL( host.GetCurrentSheetIndex(), 1u );

        host.SetViewport( 800, 600, VECTOR2D( 0, 0 ), 1.0 );
        host.ZoomToFit();

        const kgds_stream_view view = host.Render();
        BOOST_CHECK_EQUAL( view.version, KGDS_VERSION );
    }
}


/**
 * A serialised stream round-trips, which is what makes the checked-in golden
 * fixtures meaningful.
 */
BOOST_AUTO_TEST_CASE( StreamSerializesAndReloads )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    host.SetViewport( 1024, 768, VECTOR2D( 0, 0 ), 1.0 );
    host.ZoomToFit();

    const kgds_stream_view original = host.Render();

    std::string blob;

    {
        std::ostringstream out( std::ios::binary );
        BOOST_REQUIRE( host.Gal().Stream().Serialize( out ) );
        blob = out.str();
    }

    BOOST_CHECK_GT( blob.size(), sizeof( kgds_file_header ) );

    KIGFX::DRAW_STREAM reloaded;

    {
        std::istringstream in( blob, std::ios::binary );
        BOOST_REQUIRE( reloaded.Deserialize( in ) );
    }

    const kgds_stream_view restored = reloaded.Publish();

    BOOST_CHECK_EQUAL( restored.group_count, original.group_count );
    BOOST_CHECK_EQUAL( restored.group_cmd_count, original.group_cmd_count );
    BOOST_CHECK_EQUAL( restored.frame_cmd_count, original.frame_cmd_count );
    BOOST_CHECK_EQUAL( restored.group_coord_count, original.group_coord_count );
    BOOST_CHECK_EQUAL( restored.frame_coord_count, original.frame_coord_count );
}


/**
 * The checked-in golden streams must still decode.
 *
 * `qa/data/draw_streams/` exists so that the Rust renderer can be developed
 * against real schematic output with no C++ linked. That only works for as long
 * as the files remain readable by the current `DRAW_STREAM`, so this is the
 * guard: if the wire format changes without the fixtures being regenerated,
 * this fails rather than the Rust side failing later for reasons that look
 * unrelated.
 */
BOOST_AUTO_TEST_CASE( GoldenStreamsStillDecode )
{
    const std::filesystem::path dir =
            std::filesystem::path( KI_TEST::GetTestDataRootDir() ) / "draw_streams";

    BOOST_REQUIRE_MESSAGE( std::filesystem::is_directory( dir ),
                           "missing fixture directory: " << dir.string() );

    std::size_t checked = 0;

    for( const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator( dir ) )
    {
        if( entry.path().extension() != ".kgds" )
            continue;

        BOOST_TEST_CONTEXT( entry.path().filename().string() )
        {
            std::ifstream in( entry.path(), std::ios::binary );
            BOOST_REQUIRE( in.is_open() );

            KIGFX::DRAW_STREAM stream;
            BOOST_REQUIRE( stream.Deserialize( in ) );

            const kgds_stream_view view = stream.Publish();

            BOOST_CHECK_EQUAL( view.version, KGDS_VERSION );

            // A golden recorded from a real schematic has retained geometry in it;
            // one that does not is a fixture that stopped being useful.
            BOOST_CHECK_GT( view.group_count, 0u );
            BOOST_CHECK_GT( view.group_cmd_count, 0u );
            BOOST_CHECK_GT( view.frame_cmd_count, 0u );
            BOOST_CHECK_GT( view.group_coord_count, 0u );

            BOOST_CHECK( coordIndicesInRange( view ) );

            // Every group a frame replays must exist, or the renderer would look up
            // a buffer that was never uploaded.
            for( std::size_t ii = 0; ii < view.frame_cmd_count; ++ii )
            {
                if( view.frame_cmds[ii].op != KGDS_OP_DRAW_GROUP )
                    continue;

                BOOST_CHECK( stream.HasGroup(
                        static_cast<int>( view.frame_cmds[ii].arg0 ) ) );
            }

            ++checked;
        }
    }

    BOOST_CHECK_GT( checked, 0u );
}


BOOST_AUTO_TEST_SUITE_END()


/**
 * The input half of the seam: the host is a TOOLS_HOLDER, owns a TOOL_MANAGER and
 * turns host input into TOOL_EVENTs.
 *
 * The event translation itself is tested without eeschema in
 * `qa/tests/common/test_host_input.cpp`. What is asserted here is the wiring —
 * that a pointer position pushed in at one end is the cursor the tools would read
 * at the other — and which of the tool roster survives a holder that is not a
 * frame. That used to be none of it, then three; it is now every eeschema tool
 * but one, and the roster is pinned here so that converting another one fails
 * this test and says so.
 */
BOOST_FIXTURE_TEST_SUITE( SchHostInput, SCH_HOST_SETTINGS_FIXTURE )


BOOST_AUTO_TEST_CASE( TheHostIsItsOwnToolHolder )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.GetToolManager() != nullptr );
    BOOST_CHECK_EQUAL( host.GetToolManager()->GetToolHolder(), static_cast<TOOLS_HOLDER*>( &host ) );

    // The blocker that turned out not to be one: a null canvas is already a
    // production state, and nothing on the eeschema tool path asks for it.
    BOOST_CHECK( host.GetToolCanvas() == nullptr );

    // The view controls the tools will ask for the cursor are the host's, not wx's.
    BOOST_CHECK_EQUAL( host.GetToolManager()->GetViewControls(),
                       static_cast<KIGFX::VIEW_CONTROLS*>( &host.ViewControls() ) );
}


/**
 * Exactly which of the roster runs on a holder that is not a frame.
 *
 * A tool runs here when what it asks the holder for is a SCHEMATIC_HOLDER — the document,
 * the settings, the canvas notifications — rather than a window, which it says by
 * answering `SCH_TOOL_BASE::runsWithoutAFrame()` true. One that has not been converted
 * still sets `m_frame` from the holder and returns false when the holder is not its frame
 * type, so `TOOL_MANAGER::InitTools()` unregisters and deletes it.
 *
 * Running is not the same as working. Several of these register and then decline every
 * action they own, because what they do *is* a dialog — the find/replace tool has nowhere
 * to get search terms from, and the inspection tool's ERC lives in DIALOG_ERC. They are
 * still listed as running, because that is what this case measures, and
 * `docs/rust-migration/06-what-is-missing.md` Stage 4b says which is which.
 *
 * Keep both lists exhaustive rather than illustrative. It is what tells the next person
 * converting a tool that it worked.
 */
BOOST_AUTO_TEST_CASE( TheConvertedRosterRunsHereAndTheRestStillDeclines )
{
    SCH_HOST host;

    TOOL_MANAGER* tools = host.GetToolManager();

    BOOST_REQUIRE( tools != nullptr );

    // eeschema's own tools, all converted.
    BOOST_CHECK( tools->GetTool<SCH_SELECTION_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_MOVE_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_LINE_WIRE_BUS_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_ALIGN_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_EDIT_TABLE_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_POINT_EDITOR>() != nullptr );
    BOOST_CHECK( tools->GetTool<EE_GRAPHIC_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_NAVIGATE_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_DRAWING_TOOLS>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_EDIT_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_INSPECTION_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_FIND_REPLACE_TOOL>() != nullptr );
    BOOST_CHECK( tools->GetTool<SCH_EDITOR_CONTROL>() != nullptr );

    // Not part of SCH_EDIT_FRAME's roster: undo, redo and save for a holder with no frame.
    BOOST_CHECK( tools->GetTool<SCH_HOST_CONTROL>() != nullptr );

    // The one eeschema tool that is not converted. It is a design-block library pane and a
    // properties dialog end to end, and it declines in its own Init() rather than through
    // SCH_TOOL_BASE's — see SCH_DESIGN_BLOCK_CONTROL::Init().
    BOOST_CHECK( tools->GetTool<SCH_DESIGN_BLOCK_CONTROL>() == nullptr );

    // The two common/ tools that move the view. They ask for a CANVAS_HOLDER now
    // rather than an EDA_DRAW_FRAME, which is Stage 4b step 2, so pan, zoom,
    // zoom-to-fit, zoom-window, the grid list and the unit switch all run here.
    BOOST_CHECK( tools->GetTool<COMMON_TOOLS>() != nullptr );
    BOOST_CHECK( tools->GetTool<ZOOM_TOOL>() != nullptr );

    // The rest of common/'s roster still reads EDA_DRAW_FRAME and declines. Each is a
    // separate hoist and none of them moves the view, which is why step 2 stopped here.
    BOOST_CHECK( tools->GetTool<COMMON_CONTROL>() == nullptr );
    BOOST_CHECK( tools->GetTool<PICKER_TOOL>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_GROUP_TOOL>() == nullptr );
    BOOST_CHECK( tools->GetTool<EMBED_TOOL>() == nullptr );

    // PROPERTIES_TOOL is the exception in that list, and not because anyone converted it:
    // it is a bare TOOL_INTERACTIVE with no Init() of its own, so it never asks for a
    // frame and has run here since before any of this. It forwards a selection change to
    // whatever is showing a properties panel, which on this holder is nothing.
    BOOST_CHECK( tools->GetTool<PROPERTIES_TOOL>() != nullptr );

    // The holder's selection is now the selection tool's own, and it is empty
    // rather than absent.
    BOOST_CHECK_EQUAL( host.GetSelectionCount(), 0u );
    BOOST_CHECK_EQUAL( host.GetSelectionTool(), tools->GetTool<SCH_SELECTION_TOOL>() );
}


/**
 * A session with no document runs nothing, rather than letting a tool reach for a
 * screen that does not exist.
 *
 * `SCH_EDIT_FRAME` has a `SCHEMATIC` and an empty `SCH_SCREEN` from its constructor
 * on, so a tool may — and does — use `GetScreen()` without checking. This host has
 * neither until something is loaded. Found by this very suite: with the wire tool
 * running, `drawWires` on an empty session dereferenced null.
 */
BOOST_AUTO_TEST_CASE( AnEmptySessionRunsNothing )
{
    SCH_HOST host;

    BOOST_CHECK( !host.RunActionByName( "eeschema.InteractiveDrawingLineWireBus.drawWires" ) );
    BOOST_CHECK( !host.RunActionByName( "common.InteractiveSelection" ) );

    HOST_INPUT_EVENT event;
    event.type = HOST_INPUT_TYPE::POINTER_MOTION;
    event.position = VECTOR2D( 100, 100 );
    BOOST_CHECK( !host.DispatchInput( event ) );

    event.type = HOST_INPUT_TYPE::POINTER_DOWN;
    event.button = BUT_LEFT;
    BOOST_CHECK( !host.DispatchInput( event ) );

    event.type = HOST_INPUT_TYPE::POINTER_UP;
    BOOST_CHECK( !host.DispatchInput( event ) );

    BOOST_CHECK_EQUAL( host.GetSelectionCount(), 0u );
}


/**
 * The round trip that matters: a pointer position given to the host in screen
 * pixels is the world position the tool framework reads back.
 *
 * This is what WX_VIEW_CONTROLS answers by polling the operating system, and it is
 * the reason a second implementation had to exist at all.
 */
BOOST_AUTO_TEST_CASE( AHostPointerPositionBecomesTheCursorTheToolsWouldRead )
{
    SCH_HOST host;

    BOOST_REQUIRE_MESSAGE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ),
                           host.GetLastError().ToStdString() );

    host.SetViewportSize( 800, 600 );
    host.ZoomToFit();

    HOST_INPUT_EVENT motion;
    motion.type = HOST_INPUT_TYPE::POINTER_MOTION;
    motion.position = VECTOR2D( 100, 50 );

    host.DispatchInput( motion );

    const VECTOR2D expected = host.View().ToWorld( VECTOR2D( 100, 50 ) );

    BOOST_CHECK_CLOSE( host.ViewControls().GetMousePosition( true ).x, expected.x, 1e-9 );
    BOOST_CHECK_CLOSE( host.ViewControls().GetMousePosition( true ).y, expected.y, 1e-9 );

    // A different screen position gives a different world position, which is the
    // check that the view transform is actually involved rather than the number
    // being echoed back.
    motion.position = VECTOR2D( 700, 500 );
    host.DispatchInput( motion );

    BOOST_CHECK( host.ViewControls().GetMousePosition( true ) != expected );
}


/**
 * A whole gesture, plus a key and a cancel, over empty space. None of it is fatal
 * and none of it edits the document — the selection tool that now receives it only
 * selects — which is the check that matters for a UI forwarding its entire input
 * stream.
 *
 * The positions are deliberately not over an item; ::AClickSelectsTheItemUnderIt
 * covers the case where something is hit.
 */
BOOST_AUTO_TEST_CASE( AnUnproductiveGestureIsHarmless )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    host.SetViewportSize( 800, 600 );
    host.ZoomToFit();

    HOST_INPUT_EVENT event;

    event.type = HOST_INPUT_TYPE::POINTER_MOTION;
    event.position = VECTOR2D( 100, 50 );
    host.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::POINTER_DOWN;
    event.button = BUT_LEFT;
    host.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::POINTER_UP;
    host.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::KEY_DOWN;
    event.button = BUT_NONE;
    event.keyCode = 'W';
    host.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::CANCEL;
    host.DispatchInput( event );

    // The document is untouched by all of it, and nothing was selected because
    // there is nothing at that corner of the page.
    BOOST_CHECK( !host.IsModified() );
    BOOST_CHECK_EQUAL( host.GetSelectionCount(), 0u );
}


/**
 * An action that no registered tool handles is reported as unhandled rather than
 * asserted on, because a UI built from the whole 440-action registry will offer
 * plenty of them.
 *
 * The names below are deliberately **registered** ones. Every `TOOL_ACTION` in the
 * process is in `ACTION_MANAGER`'s list whether or not a tool exists to run it, so
 * a made-up name proves nothing: `TOOL_MANAGER::RunAction( const std::string& )`
 * answers "the name resolved", not "something ran it", and a test written against
 * an unregistered name passes either way. These are the real ids the shell's tool
 * buttons and menu items send.
 *
 * `common.InteractiveSelection` is the counter-example that keeps the rest honest:
 * it is handled, because that tool now runs here.
 */
BOOST_AUTO_TEST_CASE( AnUnhandledActionIsReportedRatherThanAsserted )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    // Symbol-library control is not registered in a schematic host.
    BOOST_CHECK( !host.RunActionByName( "eeschema.SymbolLibraryControl.newSymbol" ) );
    BOOST_CHECK( !host.RunActionByName( "no.such.action" ) );

    // And the ones that do have a tool behind them now, so that "unhandled" above means
    // something other than "this never reports handled".
    BOOST_CHECK( host.RunActionByName( "common.InteractiveSelection" ) );
    BOOST_CHECK( host.RunActionByName( "common.Control.zoomFitScreen" ) );
}


/**
 * `RefreshCanvas()` is how a tool says "the view changed", and it is the only
 * notice a consumer on the far side of the ABI gets that the frame it holds is
 * stale. Reading the flag clears it, so a consumer cannot re-render forever on one
 * request.
 */
BOOST_AUTO_TEST_CASE( ARepaintRequestIsRecordedOnceAndConsumedOnce )
{
    SCH_HOST host;

    BOOST_CHECK( !host.TakeRedrawRequest() );

    host.RefreshCanvas();

    BOOST_CHECK( host.TakeRedrawRequest() );
    BOOST_CHECK( !host.TakeRedrawRequest() );
}


BOOST_AUTO_TEST_CASE( AToolMessageIsKeptForTheHostToShow )
{
    SCH_HOST host;

    BOOST_CHECK( host.GetToolMessage().IsEmpty() );

    host.DisplayToolMsg( wxT( "Draw a wire" ) );

    BOOST_CHECK_EQUAL( host.GetToolMessage(), wxT( "Draw a wire" ) );
}


BOOST_AUTO_TEST_SUITE_END()


/**
 * Selection, driven through the host's input path by a real KiCad tool.
 *
 * This is the first thing the user does that reaches the document, and it is the
 * round trip the whole seam exists for: screen pixels in one end, `SCH_ITEM`s
 * flagged selected and a repaint request out the other, with no wxFrame anywhere in
 * between. What makes it possible is that `SCH_SELECTION_TOOL::m_editor` is a
 * SCHEMATIC_HOLDER — see `docs/rust-migration/06-what-is-missing.md` Stage 4b.
 */
BOOST_FIXTURE_TEST_SUITE( SchHostSelection, SCH_HOST_SETTINGS_FIXTURE )


/// The midpoint of the first wire on the screen: on-grid at both ends, so on-grid in
/// the middle, which keeps cursor snapping out of the way of the hit test.
VECTOR2I firstWireMidpoint( SCH_HOST& aHost )
{
    SCH_SCREEN* screen = aHost.GetScreen();

    BOOST_REQUIRE( screen );

    for( SCH_ITEM* item : screen->Items().OfType( SCH_LINE_T ) )
    {
        SCH_LINE* line = static_cast<SCH_LINE*>( item );

        if( line->GetLayer() == LAYER_WIRE )
            return ( line->GetStartPoint() + line->GetEndPoint() ) / 2;
    }

    BOOST_FAIL( "fixture has no wire to click on" );
    return VECTOR2I();
}


/// The down/up pair a UI sends for one left click at a world position.
void clickAt( SCH_HOST& aHost, const VECTOR2I& aWorld )
{
    HOST_INPUT_EVENT event;

    event.position = aHost.View().ToScreen( VECTOR2D( aWorld ) );

    event.type = HOST_INPUT_TYPE::POINTER_MOTION;
    aHost.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::POINTER_DOWN;
    event.button = BUT_LEFT;
    aHost.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::POINTER_UP;
    aHost.DispatchInput( event );
}


std::unique_ptr<SCH_HOST> loadedHost( const wxString& aFixture = wxT( "api_kitchen_sink.kicad_sch" ) )
{
    auto host = std::make_unique<SCH_HOST>();

    BOOST_REQUIRE_MESSAGE( host->LoadFile( eeschemaFixture( aFixture ) ),
                           host->GetLastError().ToStdString() );

    host->SetViewportSize( 1920, 1080 );
    host->ZoomToFit();

    return host;
}


/**
 * The one that matters: a click at an item's position selects that item.
 *
 * Note what is *not* mocked. The position goes in as screen pixels and is turned
 * into a world position by `HOST_VIEW_CONTROLS`, the hit test is
 * `SCH_COLLECTOR`'s against the real `SCH_SCREEN`, and the item that comes back is
 * the one on the schematic.
 */
BOOST_AUTO_TEST_CASE( AClickSelectsTheItemUnderIt )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    const VECTOR2I target = firstWireMidpoint( *host );

    BOOST_CHECK_EQUAL( host->GetSelectionCount(), 0u );

    clickAt( *host, target );

    BOOST_REQUIRE_EQUAL( host->GetSelectionCount(), 1u );

    EDA_ITEM* selected = host->GetCurrentSelection().Front();

    BOOST_REQUIRE( selected );
    BOOST_CHECK_EQUAL( selected->Type(), SCH_LINE_T );
    BOOST_CHECK( selected->IsSelected() );

    // The item selected is the one that was clicked, not merely something.
    SCH_LINE* line = static_cast<SCH_LINE*>( selected );
    BOOST_CHECK_EQUAL( ( line->GetStartPoint() + line->GetEndPoint() ) / 2, target );
}


/**
 * Right-clicking does not crash, which is a lower bar than it sounds.
 *
 * `TOOL_INTERACTIVE` only builds a `TOOL_MENU` when `Pgm().IsGUI()`, so every tool
 * running here holds a null `m_menu`. Three of them — the selection, move and wire
 * tools, which were the first three converted — called
 * `m_menu->ShowContextMenu()` on a right click without testing it, and a right
 * click on the canvas of the gpui editor segfaulted in `SCH_SELECTION_TOOL`.
 *
 * It survived review and a green suite because nothing dispatched a right click:
 * every input test here presses `BUT_LEFT`. The bug was in the first tool
 * converted, not in the eleven that followed — the later ones guard it — so this
 * case covers the gesture rather than any one tool, and is a right click at three
 * places a menu would differ: over an item, over empty space, and mid-drag.
 */
BOOST_AUTO_TEST_CASE( ARightClickOpensNoMenuAndDoesNotCrash )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    const VECTOR2I onItem = firstWireMidpoint( *host );

    const auto rightClickAt =
            [&]( const VECTOR2I& aWorld )
            {
                HOST_INPUT_EVENT event;

                event.position = host->View().ToScreen( VECTOR2D( aWorld ) );

                event.type = HOST_INPUT_TYPE::POINTER_MOTION;
                host->DispatchInput( event );

                event.type = HOST_INPUT_TYPE::POINTER_DOWN;
                event.button = BUT_RIGHT;
                host->DispatchInput( event );

                event.type = HOST_INPUT_TYPE::POINTER_UP;
                host->DispatchInput( event );
            };

    // Over an item, with nothing selected: the selection tool selects under the
    // cursor and then would open a menu on it.
    rightClickAt( onItem );

    // Over an item that is already selected, which is the other branch.
    rightClickAt( onItem );

    // Over empty space.
    rightClickAt( onItem + VECTOR2I( schIUScale.mmToIU( 40 ), schIUScale.mmToIU( 40 ) ) );

    // And during a left-button drag, which is the move tool's own right-click
    // branch rather than the selection tool's.
    {
        HOST_INPUT_EVENT event;
        event.position = host->View().ToScreen( VECTOR2D( onItem ) );
        event.type = HOST_INPUT_TYPE::POINTER_MOTION;
        host->DispatchInput( event );

        event.type = HOST_INPUT_TYPE::POINTER_DOWN;
        event.button = BUT_LEFT;
        host->DispatchInput( event );

        event.type = HOST_INPUT_TYPE::POINTER_MOTION;
        event.position = host->View().ToScreen(
                VECTOR2D( onItem + VECTOR2I( schIUScale.mmToIU( 5 ), 0 ) ) );
        host->DispatchInput( event );

        event.type = HOST_INPUT_TYPE::POINTER_DOWN;
        event.button = BUT_RIGHT;
        host->DispatchInput( event );

        event.type = HOST_INPUT_TYPE::POINTER_UP;
        host->DispatchInput( event );

        event.type = HOST_INPUT_TYPE::POINTER_UP;
        event.button = BUT_LEFT;
        host->DispatchInput( event );
    }

    // Reaching here at all is the assertion. That the session is still usable
    // afterwards is the second one.
    BOOST_CHECK( host->GetSchematic() != nullptr );

    clickAt( *host, onItem );
    BOOST_CHECK_EQUAL( host->GetSelectionCount(), 1u );
}


/**
 * Clicking empty space clears the selection, which is the other half of the
 * behaviour and the half a partially-wired tool still gets right by accident.
 */
BOOST_AUTO_TEST_CASE( AClickOnNothingClearsTheSelection )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    clickAt( *host, firstWireMidpoint( *host ) );

    BOOST_REQUIRE_EQUAL( host->GetSelectionCount(), 1u );

    // Far outside the drawing, but still inside the viewport after a zoom to fit.
    const BOX2I  bbox = host->GetDocumentBBox( true );
    const VECTOR2I empty( bbox.GetLeft() + 10, bbox.GetTop() + 10 );

    clickAt( *host, empty );

    BOOST_CHECK_EQUAL( host->GetSelectionCount(), 0u );
}


/**
 * Escape clears the selection too, over the same path a UI's key events take.
 */
BOOST_AUTO_TEST_CASE( CancelClearsTheSelection )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    clickAt( *host, firstWireMidpoint( *host ) );

    BOOST_REQUIRE_EQUAL( host->GetSelectionCount(), 1u );

    HOST_INPUT_EVENT cancel;
    cancel.type = HOST_INPUT_TYPE::CANCEL;

    host->DispatchInput( cancel );

    BOOST_CHECK_EQUAL( host->GetSelectionCount(), 0u );
}


/**
 * A selection is only visible if the consumer is told to re-record.
 *
 * `SCH_SELECTION_TOOL` says so by calling `ForceRefreshCanvas()`, which on a frame
 * repaints synchronously and here sets the flag `ksch_session_dispatch_input`
 * returns as `KSCH_INPUT_REDRAW`. Until this stage nothing in eeschema called it at
 * all, so the flag existed and could never be set — which is why this is asserted
 * rather than assumed.
 */
BOOST_AUTO_TEST_CASE( SelectingSomethingAsksTheConsumerToRedraw )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    // Clear whatever the load and the zoom-to-fit asked for.
    host->TakeRedrawRequest();

    clickAt( *host, firstWireMidpoint( *host ) );

    BOOST_REQUIRE_EQUAL( host->GetSelectionCount(), 1u );
    BOOST_CHECK( host->TakeRedrawRequest() );
}


/**
 * The selection reaches the recorded frame, which is the only way a user sees it.
 *
 * `SCH_PAINTER` draws a shadow behind a selected item on LAYER_SELECTION_SHADOWS,
 * so a frame recorded with something selected has geometry a frame recorded with
 * nothing selected does not. Asserting on the command count rather than on a
 * pixel keeps this a test of the seam rather than of the renderer.
 */
BOOST_AUTO_TEST_CASE( ASelectedItemChangesWhatIsRecorded )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    const kgds_stream_view clean = host->Render();
    const std::size_t      cleanCommands = clean.frame_cmd_count + clean.group_cmd_count;

    clickAt( *host, firstWireMidpoint( *host ) );

    BOOST_REQUIRE_EQUAL( host->GetSelectionCount(), 1u );

    const kgds_stream_view selected = host->Render();
    const std::size_t      selectedCommands =
            selected.frame_cmd_count + selected.group_cmd_count;

    BOOST_CHECK_GT( selectedCommands, cleanCommands );
}


/**
 * A drag selects everything inside the box it draws, which is the other half of
 * selection and the half that runs its own event loop.
 *
 * `SCH_SELECTION_TOOL::selectMultiple()` puts a `SELECTION_AREA` in the view,
 * `Wait()`s on drag events, and calls `ForceRefreshCanvas()` per motion so the box
 * follows the pointer. None of that needs a window; all of it needs the host to keep
 * delivering events into a nested loop.
 */
BOOST_AUTO_TEST_CASE( ADragSelectsEverythingInsideIt )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    const BOX2I items = host->GetDocumentBBox( false );

    BOOST_REQUIRE( items.GetWidth() > 0 );

    HOST_INPUT_EVENT event;
    event.button = BUT_LEFT;

    // A box around everything on the sheet, drawn from outside one corner to outside
    // the other so that nothing is clipped by the drag's own start point landing on an
    // item (which would move it instead).
    const VECTOR2D from = host->View().ToScreen(
            VECTOR2D( items.GetLeft() - 500000, items.GetTop() - 500000 ) );
    const VECTOR2D to = host->View().ToScreen(
            VECTOR2D( items.GetRight() + 500000, items.GetBottom() + 500000 ) );

    event.type = HOST_INPUT_TYPE::POINTER_MOTION;
    event.position = from;
    host->DispatchInput( event );

    event.type = HOST_INPUT_TYPE::POINTER_DOWN;
    host->DispatchInput( event );

    // Several steps, because the drag threshold is a distance and the tool's loop has
    // to see motion after the press to know it is a drag rather than a click.
    for( int step = 1; step <= 8; ++step )
    {
        event.type = HOST_INPUT_TYPE::POINTER_MOTION;
        event.position = from + ( to - from ) * ( step / 8.0 );
        host->DispatchInput( event );
    }

    event.type = HOST_INPUT_TYPE::POINTER_UP;
    event.position = to;
    host->DispatchInput( event );

    BOOST_CHECK_GT( host->GetSelectionCount(), 1u );
}


/**
 * The cursor shape the tools ask for is recorded for a consumer that owns the real
 * pointer. gpui cannot be told "become a crosshair" by C++, so the host keeps the
 * request and the UI reads it.
 */
BOOST_AUTO_TEST_CASE( TheToolsCursorRequestIsRecorded )
{
    std::unique_ptr<SCH_HOST> host = loadedHost();

    // SCH_SELECTION_TOOL::Main() sets this as the first thing it does, so the mere
    // fact that it is the arrow means the tool's loop is running.
    BOOST_CHECK_EQUAL( static_cast<int>( host->GetCurrentCursor() ),
                       static_cast<int>( KICURSOR::ARROW ) );
}


BOOST_AUTO_TEST_SUITE_END()


/**
 * Undo and redo of a schematic edit, on an editing context that is not a wxFrame.
 *
 * These are the first tests eeschema's undo has: `SaveCopyInUndoList` and
 * `PutDataInPreviousState` were members of `SCH_EDIT_FRAME`, so exercising them meant
 * standing up a window, and nothing in the suite did. They are now `SCH_UNDO_REDO` free
 * functions over SCHEMATIC_HOLDER — which a frame is — so what is covered here covers the
 * GUI's undo as well. That is the real reason this stage moved them rather than giving the
 * host a second implementation.
 */
BOOST_FIXTURE_TEST_SUITE( SchHostUndo, SCH_HOST_SETTINGS_FIXTURE )


/// The first label on the current sheet, which is the simplest thing to edit and check.
SCH_LABEL* firstLabel( SCH_HOST& aHost )
{
    SCH_SCREEN* screen = aHost.GetScreen();

    BOOST_REQUIRE( screen );

    for( SCH_ITEM* item : screen->Items().OfType( SCH_LABEL_T ) )
        return static_cast<SCH_LABEL*>( item );

    BOOST_FAIL( "fixture has no label to edit" );
    return nullptr;
}


std::unique_ptr<SCH_HOST> hostWithALabel()
{
    auto host = std::make_unique<SCH_HOST>();

    BOOST_REQUIRE_MESSAGE( host->LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ),
                           host->GetLastError().ToStdString() );

    host->SetViewportSize( 1920, 1080 );
    host->ZoomToFit();

    return host;
}


/**
 * An edit is recorded, undone and redone, and the document ends where it started.
 *
 * Note what is *not* here: a frame. The commit goes through the same `SCH_COMMIT` every
 * eeschema edit goes through, and the undo it records goes on the same stacks.
 */
BOOST_AUTO_TEST_CASE( HostSearchRespectsScopeAndMatchOptions )
{
    SCH_HOST host;
    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( "issue10926_1.kicad_sch" ) ) );
    BOOST_REQUIRE_GE( host.GetSheetHierarchy().size(), 2u );
    BOOST_REQUIRE( host.SetCurrentSheetIndex( 1 ) );
    auto* label = new SCH_LABEL( VECTOR2I( 100000, 100000 ), "ScopeNeedle" );
    host.AddToScreen( label );
    BOOST_REQUIRE( host.SetCurrentSheetIndex( 0 ) );
    SCH_SEARCH_DATA terms;
    terms.findString = "ScopeNeedle";
    terms.searchCurrentSheetOnly = true;
    host.SetSearchData( terms, true );
    auto* tool = host.GetToolManager()->GetTool<SCH_FIND_REPLACE_TOOL>();
    host.RunActionByName( "common.Interactive.findNext" );
    BOOST_CHECK( tool->GetLastFoundItem() == nullptr );
    terms.searchCurrentSheetOnly = false;
    host.SetSearchData( terms, true );
    host.RunActionByName( "common.Interactive.findNext" );
    BOOST_REQUIRE( tool->GetLastFoundItem() == label );
    BOOST_CHECK_EQUAL( host.GetCurrentSheetIndex(), 1u );
    terms.findString = "scopeneedle";
    terms.matchCase = true;
    host.SetSearchData( terms, true );
    host.RunActionByName( "common.Interactive.findNext" );
    BOOST_CHECK( tool->GetLastFoundItem() == nullptr );
    terms.matchCase = false;
    host.SetSearchData( terms, true );
    host.RunActionByName( "common.Interactive.findNext" );
    BOOST_CHECK( tool->GetLastFoundItem() == label );
    terms.findString = "Scope";
    terms.matchMode = EDA_SEARCH_MATCH_MODE::WHOLEWORD;
    host.SetSearchData( terms, true );
    host.RunActionByName( "common.Interactive.findNext" );
    BOOST_CHECK( tool->GetLastFoundItem() == nullptr );
}

BOOST_AUTO_TEST_CASE( HostSearchFindsReplacesAndUndoes )
{
    auto host = hostWithALabel();
    SCH_LABEL* label = firstLabel( *host );
    label->SetText( wxT( "host_search_unique" ) );
    SCH_SEARCH_DATA terms;
    terms.findString = wxT( "host_search_unique" );
    terms.replaceString = wxT( "host_replaced_unique" );
    terms.searchAndReplace = true;
    terms.searchCurrentSheetOnly = true;
    host->SetSearchData( terms, true );
    BOOST_REQUIRE( host->RunActionByName( "common.Interactive.replaceAndFindNext" ) );
    auto* tool = host->GetToolManager()->GetTool<SCH_FIND_REPLACE_TOOL>();
    BOOST_REQUIRE( tool->GetLastFoundItem() == label );
    BOOST_CHECK_EQUAL( tool->Replaced(), 0u );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( "host_search_unique" ) );
    BOOST_REQUIRE( host->RunActionByName( "common.Interactive.findNext" ) );
    BOOST_CHECK( tool->Wrapped() );
    BOOST_REQUIRE( host->RunActionByName( "common.Interactive.replaceAndFindNext" ) );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( "host_replaced_unique" ) );
    BOOST_CHECK_EQUAL( tool->Replaced(), 1u );
    BOOST_CHECK( host->IsModified() );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( "host_search_unique" ) );
    host->SetSearchData( terms, true );
    BOOST_REQUIRE( host->RunActionByName( "common.Interactive.replaceAll" ) );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( "host_replaced_unique" ) );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( "host_search_unique" ) );
    host->SetSearchData( terms, false );
    BOOST_CHECK( host->GetHostSearchData() == nullptr );
}

BOOST_AUTO_TEST_CASE( AnEditIsRecordedUndoneAndRedone )
{
    std::unique_ptr<SCH_HOST> host = hostWithALabel();

    SCH_LABEL* label = firstLabel( *host );
    const wxString original = label->GetText();

    BOOST_REQUIRE( !original.IsEmpty() );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );

    SCH_COMMIT commit( host->GetToolManager() );
    commit.Modify( label, host->GetScreen() );
    label->SetText( wxT( "EDITED_WITHOUT_A_FRAME" ) );
    commit.Push( wxT( "Rename label" ) );

    BOOST_CHECK_EQUAL( label->GetText(), wxString( wxT( "EDITED_WITHOUT_A_FRAME" ) ) );
    BOOST_CHECK( host->IsModified() );

    // The edit is on the stack, with the description a menu item would show.
    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );
    BOOST_CHECK_EQUAL( host->GetUndoActionDescription(), wxT( "Rename label" ) );
    BOOST_CHECK_EQUAL( host->GetRedoCommandCount(), 0 );

    BOOST_REQUIRE( host->Undo() );

    BOOST_CHECK_EQUAL( label->GetText(), original );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
    BOOST_REQUIRE_EQUAL( host->GetRedoCommandCount(), 1 );

    BOOST_REQUIRE( host->Redo() );

    BOOST_CHECK_EQUAL( label->GetText(), wxString( wxT( "EDITED_WITHOUT_A_FRAME" ) ) );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 1 );
    BOOST_CHECK_EQUAL( host->GetRedoCommandCount(), 0 );
}


/**
 * Undo restores the *connectivity*, not merely the geometry.
 *
 * This is the assertion that makes the rest worth having: a label's net name is derived
 * state, and getting it back means the connection graph was rebuilt from the restored
 * document rather than left describing the edited one.
 */
BOOST_AUTO_TEST_CASE( UndoRestoresDerivedStateAndNotJustTheItem )
{
    std::unique_ptr<SCH_HOST> host = hostWithALabel();

    SCH_LABEL*            label = firstLabel( *host );
    const SCH_SHEET_PATH& sheet = host->GetCurrentSheet();

    auto connectionName =
            [&]() -> wxString
            {
                std::optional<wxString> name = label->GetConnectionName( &sheet );

                return name ? *name : wxString();
            };

    const wxString before = connectionName();

    BOOST_REQUIRE( !before.IsEmpty() );

    SCH_COMMIT commit( host->GetToolManager() );
    commit.Modify( label, host->GetScreen() );
    label->SetText( wxT( "RENAMED" ) );
    commit.Push( wxT( "Rename label" ) );

    BOOST_REQUIRE_NE( connectionName(), before );

    BOOST_REQUIRE( host->Undo() );

    BOOST_CHECK_EQUAL( connectionName(), before );
}


/**
 * Undo with nothing to undo is a no-op that says so, rather than popping an empty stack.
 */
BOOST_AUTO_TEST_CASE( ThereIsNothingToUndoOnAFreshDocument )
{
    std::unique_ptr<SCH_HOST> host = hostWithALabel();

    BOOST_CHECK( !host->Undo() );
    BOOST_CHECK( !host->Redo() );
    BOOST_CHECK( !host->IsModified() );
}


/**
 * An undo asks the consumer to redraw, because the items it moved are on the screen the
 * consumer is holding a recorded frame of.
 */
BOOST_AUTO_TEST_CASE( UndoAsksTheConsumerToRedraw )
{
    std::unique_ptr<SCH_HOST> host = hostWithALabel();

    SCH_LABEL* label = firstLabel( *host );

    SCH_COMMIT commit( host->GetToolManager() );
    commit.Modify( label, host->GetScreen() );
    label->SetText( wxT( "EDITED" ) );
    commit.Push( wxT( "Rename label" ) );

    host->TakeRedrawRequest();

    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK( host->TakeRedrawRequest() );
}


/**
 * The depth limit trims the oldest command and deletes what it owns.
 *
 * `ClearUndoORRedoList` is the one piece of undo each editing context has to implement
 * itself, because whether a picked item may be deleted depends on the document. Running it
 * under a sanitiser is the point; asserting the count is what a test can do.
 */
BOOST_AUTO_TEST_CASE( TheOldestCommandIsDiscardedWhenTheStackIsFull )
{
    std::unique_ptr<SCH_HOST> host = hostWithALabel();

    SCH_LABEL* label = firstLabel( *host );

    for( int ii = 0; ii < 4; ++ii )
    {
        SCH_COMMIT commit( host->GetToolManager() );
        commit.Modify( label, host->GetScreen() );
        label->SetText( wxString::Format( wxT( "EDIT_%d" ), ii ) );
        commit.Push( wxString::Format( wxT( "Edit %d" ), ii ) );
    }

    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 4 );

    host->ClearUndoORRedoList( SCH_HOST::UNDO_LIST, 2 );

    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 2 );

    host->ClearUndoRedoList();

    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
    BOOST_CHECK_EQUAL( host->GetRedoCommandCount(), 0 );
}


BOOST_AUTO_TEST_SUITE_END()


/**
 * Editing: moving an item and drawing a wire, on a context that is not a wxFrame.
 *
 * This is the first thing a user does that *changes* the document, driven the way a user
 * drives it: select, press, drag, release. `SCH_MOVE_TOOL` and `SCH_LINE_WIRE_BUS_TOOL` are
 * the first two tools to answer `runsWithoutAFrame()` with true — everything they need from
 * the editor is SCHEMATIC_HOLDER's, and what they lose without a frame is an info-bar hint
 * and the net-collision preview.
 *
 * They arrive together because the move tool needs the wire tool: moving a wire off a
 * junction has to add one where it left.
 */
BOOST_FIXTURE_TEST_SUITE( SchHostEditing, SCH_HOST_SETTINGS_FIXTURE )


/// Press, drag in steps, release. Steps because KiCad's drag threshold is a distance and
/// the tool has to see motion after the press to know a drag from a click.
void dragBy( SCH_HOST& aHost, const VECTOR2I& aFrom, const VECTOR2I& aDelta )
{
    const VECTOR2D from = aHost.View().ToScreen( VECTOR2D( aFrom ) );
    const VECTOR2D to = aHost.View().ToScreen( VECTOR2D( aFrom + aDelta ) );

    HOST_INPUT_EVENT event;
    event.button = BUT_LEFT;

    event.type = HOST_INPUT_TYPE::POINTER_MOTION;
    event.position = from;
    aHost.DispatchInput( event );

    event.type = HOST_INPUT_TYPE::POINTER_DOWN;
    aHost.DispatchInput( event );

    for( int step = 1; step <= 8; ++step )
    {
        event.type = HOST_INPUT_TYPE::POINTER_MOTION;
        event.position = from + ( to - from ) * ( step / 8.0 );
        aHost.DispatchInput( event );
    }

    event.type = HOST_INPUT_TYPE::POINTER_UP;
    event.position = to;
    aHost.DispatchInput( event );
}


std::unique_ptr<SCH_HOST> hostForMoving()
{
    auto host = std::make_unique<SCH_HOST>();

    BOOST_REQUIRE_MESSAGE( host->LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ),
                           host->GetLastError().ToStdString() );

    // A generous viewport, so that a grid step is several pixels and a drag of one grid
    // step is unambiguously past the drag threshold.
    host->SetViewportSize( 1920, 1080 );
    host->ZoomToFit();

    return host;
}


/**
 * The move tool initialises here, which no editing tool did before.
 */
BOOST_AUTO_TEST_CASE( JunctionDeletionMergesWiresAndBusesAndRoundTrips )
{
    for( auto layer : { LAYER_WIRE, LAYER_BUS } )
    {
        const auto directory = std::filesystem::temp_directory_path()
                / ( "kicad_stage6_junction_" + std::to_string( ::getpid() ) );
        std::filesystem::create_directories( directory );
        const auto file = directory / "junction.kicad_sch";
        std::filesystem::copy_file( eeschemaFixture( "erc_label_test.kicad_sch" ).ToStdString(),
                                   file, std::filesystem::copy_options::overwrite_existing );
        SCH_HOST host;
        BOOST_REQUIRE( host.LoadFile( wxString::FromUTF8( file.string() ) ) );
        const VECTOR2I center( 100000, 100000 );
        auto* junction = new SCH_JUNCTION( center );
        host.AddToScreen( junction );
        for( VECTOR2I delta : { VECTOR2I( -10000, 0 ), VECTOR2I( 10000, 0 ),
                                VECTOR2I( 0, -10000 ), VECTOR2I( 0, 10000 ) } )
        {
            auto* line = new SCH_LINE( center, layer );
            line->SetEndPoint( center + delta );
            host.AddToScreen( line );
        }
        const wxString horizontal = layer == LAYER_BUS ? "H[0..1]" : "H";
        const wxString vertical = layer == LAYER_BUS ? "V[0..1]" : "V";
        auto* hLabel = new SCH_LABEL( center - VECTOR2I( 10000, 0 ), horizontal );
        auto* vLabel = new SCH_LABEL( center - VECTOR2I( 0, 10000 ), vertical );
        host.AddToScreen( hLabel );
        host.AddToScreen( vLabel );
        BOOST_REQUIRE( host.RecalculateConnections( nullptr, NO_CLEANUP ) );
        BOOST_REQUIRE( hLabel->GetConnectionName() );
        BOOST_REQUIRE( vLabel->GetConnectionName() );
        BOOST_CHECK_EQUAL( *hLabel->GetConnectionName(), *vLabel->GetConnectionName() );
        auto count = [&]( SCH_HOST& target )
        {
            int lines = 0;
            for( SCH_ITEM* item : target.GetScreen()->Items().Overlapping( SCH_LINE_T, center ) )
                if( item->GetLayer() == layer ) ++lines;
            return lines;
        };
        BOOST_REQUIRE_EQUAL( count( host ), 4 );
        host.GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, junction );
        host.GetToolManager()->RunAction( ACTIONS::doDelete );
        BOOST_CHECK( !host.GetScreen()->GetItem( center, 0, SCH_JUNCTION_T ) );
        BOOST_CHECK_EQUAL( count( host ), 2 );
        BOOST_REQUIRE( hLabel->GetConnectionName() );
        BOOST_REQUIRE( vLabel->GetConnectionName() );
        BOOST_CHECK( *hLabel->GetConnectionName() != *vLabel->GetConnectionName() );
        if( layer == LAYER_BUS )
        {
            BOOST_CHECK_EQUAL( hLabel->GetBusMemberNames().size(), 2 );
            BOOST_CHECK_EQUAL( vLabel->GetBusMemberNames().size(), 2 );
        }
        BOOST_REQUIRE_EQUAL( host.GetUndoCommandCount(), 1 );
        BOOST_REQUIRE( host.Undo() );
        BOOST_CHECK( host.GetScreen()->GetItem( center, 0, SCH_JUNCTION_T ) );
        BOOST_CHECK_EQUAL( count( host ), 4 );
        BOOST_CHECK_EQUAL( *hLabel->GetConnectionName(), *vLabel->GetConnectionName() );
        BOOST_REQUIRE( host.Redo() );
        BOOST_CHECK_EQUAL( count( host ), 2 );
        BOOST_REQUIRE( host.Save() );
        SCH_HOST reopened;
        BOOST_REQUIRE( reopened.LoadFile( wxString::FromUTF8( file.string() ) ) );
        BOOST_CHECK_EQUAL( count( reopened ), 2 );
        BOOST_CHECK( !reopened.GetScreen()->GetItem( center, 0, SCH_JUNCTION_T ) );
        for( SCH_ITEM* item : reopened.GetScreen()->Items().Overlapping( SCH_LINE_T, center ) )
        {
            auto* line = static_cast<SCH_LINE*>( item );
            if( line->GetLayer() != layer ) continue;
            const auto axis = line->IsEndPoint( center + VECTOR2I( 10000, 0 ) )
                    ? VECTOR2I( 10000, 0 ) : VECTOR2I( 0, 10000 );
            BOOST_CHECK( line->IsEndPoint( center - axis ) );
            BOOST_CHECK( line->IsEndPoint( center + axis ) );
        }
        std::filesystem::remove_all( directory );
    }
}

BOOST_AUTO_TEST_CASE( JunctionCleanupNeverDeduplicatesAWireAgainstABus )
{
    auto host = hostForMoving();
    const VECTOR2I center( 100000, 100000 );
    auto* junction = new SCH_JUNCTION( center );
    host->AddToScreen( junction );
    for( auto layer : { LAYER_WIRE, LAYER_BUS } )
    {
        auto* line = new SCH_LINE( center, layer );
        line->SetEndPoint( center + VECTOR2I( 10000, 0 ) );
        host->AddToScreen( line );
    }
    SCH_COMMIT commit( host->GetToolManager() );
    host->DeleteJunction( &commit, junction );
    commit.Push( "Delete Junction" );
    std::set<int> layers;
    for( SCH_ITEM* item : host->GetScreen()->Items().Overlapping( SCH_LINE_T, center ) )
        layers.insert( item->GetLayer() );
    BOOST_CHECK( layers.count( LAYER_WIRE ) );
    BOOST_CHECK( layers.count( LAYER_BUS ) );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
}

BOOST_AUTO_TEST_CASE( BodyStyleUsesOneTransactionAndRejectsNoOps )
{
    auto host = hostForMoving();
    SCH_SYMBOL* symbol = nullptr;
    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        symbol = static_cast<SCH_SYMBOL*>( item );
        break;
    }
    BOOST_REQUIRE( symbol );
    auto* library = new LIB_SYMBOL( *symbol->GetLibSymbolRef() );
    library->SetBodyStyleCount( 2, true, true );
    symbol->SetLibSymbol( library );
    symbol->SetBodyStyle( 1 );
    host->SelectBodyStyle( host->GetToolManager(), symbol, 0 );
    host->SelectBodyStyle( host->GetToolManager(), symbol, 1 );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
    SCH_COMMIT commit( host->GetToolManager() );
    host->SelectBodyStyle( host->GetToolManager(), symbol, 2, &commit );
    commit.Push( "Change Body Style" );
    BOOST_CHECK_EQUAL( symbol->GetBodyStyle(), 2 );
    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK_EQUAL( symbol->GetBodyStyle(), 1 );
    BOOST_REQUIRE( host->Redo() );
    BOOST_CHECK_EQUAL( symbol->GetBodyStyle(), 2 );
    host->SelectBodyStyle( host->GetToolManager(), symbol, 99 );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 1 );
}

BOOST_AUTO_TEST_CASE( RotatingAndMirroringSymbolsMatchNativeGeometry )
{
    auto host = hostForMoving();
    SCH_SYMBOL* symbol = nullptr;
    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        symbol = static_cast<SCH_SYMBOL*>( item );
        break;
    }
    BOOST_REQUIRE( symbol );
    const KIID id = symbol->m_Uuid;
    const auto originalPins = symbol->GetConnectionPoints();
    auto native = std::unique_ptr<SCH_SYMBOL>( static_cast<SCH_SYMBOL*>( symbol->Clone() ) );
    native->Rotate( native->GetPosition(), true );
    host->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, symbol );
    host->GetToolManager()->RunAction( SCH_ACTIONS::rotateCCW );
    BOOST_CHECK( symbol->GetConnectionPoints() == native->GetConnectionPoints() );
    BOOST_CHECK( symbol->m_Uuid == id );
    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK( symbol->GetConnectionPoints() == originalPins );
    BOOST_REQUIRE( host->Redo() );
    BOOST_CHECK( symbol->GetConnectionPoints() == native->GetConnectionPoints() );
    native->SetOrientation( SYM_MIRROR_Y );
    host->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, symbol );
    host->GetToolManager()->RunAction( SCH_ACTIONS::mirrorH );
    BOOST_CHECK( symbol->GetConnectionPoints() == native->GetConnectionPoints() );
    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 2 );
    BOOST_REQUIRE( host->Undo() );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK( symbol->GetConnectionPoints() == originalPins );
}

BOOST_AUTO_TEST_CASE( CancelMoveAndDuplicatePreservesIdentityAndUndo )
{
    auto host = hostForMoving();
    SCH_ITEM* item = nullptr;
    for( SCH_ITEM* candidate : host->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
    {
        item = candidate;
        break;
    }
    BOOST_REQUIRE( item );
    const auto id = item->m_Uuid;
    const auto position = item->GetPosition();
    const auto count = host->GetScreen()->Items().size();
    host->SaveCopyForRepeatItem( item );
    for( const char* action : { "eeschema.InteractiveMove.move", "common.Interactive.duplicate",
                               "eeschema.InteractiveEdit.repeatDrawItem" } )
    {
        if( std::string( action ).find( "repeatDrawItem" ) != std::string::npos )
        {
            // A completed first item must also roll back if the subsequent symbol is canceled.
            for( SCH_ITEM* label : host->GetScreen()->Items().OfType( SCH_LABEL_T ) )
            {
                host->SaveCopyForRepeatItem( label );
                host->AddCopyForRepeatItem( item );
                break;
            }
        }
        host->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, item );
        BOOST_REQUIRE( host->RunActionByName( action ) );
        host->GetToolManager()->RunAction( ACTIONS::cancelInteractive );
        host->GetToolManager()->RunAction( ACTIONS::cancelInteractive );
        BOOST_CHECK_EQUAL( host->GetScreen()->Items().size(), count );
        BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
        BOOST_CHECK( item->m_Uuid == id );
        BOOST_CHECK( item->GetPosition() == position );
        BOOST_CHECK( !host->IsModified() );
    }
}

BOOST_AUTO_TEST_CASE( DuplicateAndRepeatCommitOnceWithFreshIdentity )
{
    for( const char* action : { "common.Interactive.duplicate",
                               "eeschema.InteractiveEdit.repeatDrawItem" } )
    {
        auto host = hostForMoving();
        SCH_SYMBOL* symbol = nullptr;
        std::set<KIID> original;
        for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            original.insert( item->m_Uuid );
            if( !symbol ) symbol = static_cast<SCH_SYMBOL*>( item );
        }
        BOOST_REQUIRE( symbol );
        host->eeconfig()->m_AnnotatePanel.automatic = true;
        host->SaveCopyForRepeatItem( symbol );
        host->GetToolManager()->RunAction<EDA_ITEM*>( ACTIONS::selectItem, symbol );
        BOOST_REQUIRE( host->RunActionByName( action ) );
        // Returning here is essential: GPUI must regain control to deliver this click.
        BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
        TOOL_EVENT click( TC_MOUSE, TA_MOUSE_CLICK, BUT_LEFT );
        click.SetMousePosition( VECTOR2D( symbol->GetPosition() + VECTOR2I( 100000, 100000 ) ) );
        host->GetToolManager()->ProcessEvent( click );
        BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );
        SCH_SYMBOL* placed = nullptr;
        size_t count = 0;
        for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
            ++count;
            if( !original.count( item->m_Uuid ) ) placed = static_cast<SCH_SYMBOL*>( item );
        }
        BOOST_CHECK_EQUAL( count, original.size() + 1 );
        BOOST_REQUIRE( placed );
        BOOST_CHECK( placed->GetRef( &host->GetCurrentSheet() ) != symbol->GetRef( &host->GetCurrentSheet() ) );
        BOOST_CHECK( !placed->GetRef( &host->GetCurrentSheet() ).Contains( "?" ) );
        const KIID placedId = placed->m_Uuid;
        BOOST_REQUIRE( host->Undo() );
        BOOST_CHECK( !host->ResolveItem( placedId, true ) );
        BOOST_REQUIRE( host->Redo() );
        BOOST_CHECK( host->ResolveItem( placedId, true ) );
    }
}

BOOST_AUTO_TEST_CASE( TheMoveToolRunsWithoutAFrame )
{
    SCH_HOST host;

    BOOST_CHECK( host.GetToolManager()->GetTool<SCH_MOVE_TOOL>() != nullptr );
}


/**
 * Select an item, drag it, and it is somewhere else — and the move is on the undo stack,
 * so it can be taken back.
 */
BOOST_AUTO_TEST_CASE( ADragMovesTheSelectedItemAndIsUndoable )
{
    std::unique_ptr<SCH_HOST> host = hostForMoving();

    SCH_LABEL* label = nullptr;

    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_LABEL_T ) )
    {
        label = static_cast<SCH_LABEL*>( item );
        break;
    }

    BOOST_REQUIRE( label );

    const VECTOR2I origin = label->GetPosition();

    // One grid step on both axes, so the snapped result is exactly predictable: eeschema's
    // default grid is 50 mil and the fixture is on it.
    const VECTOR2I delta( schIUScale.MilsToIU( 100 ), schIUScale.MilsToIU( 100 ) );

    // Select it first, because a drag over an *unselected* item is a rubber band.
    HOST_INPUT_EVENT click;
    click.button = BUT_LEFT;
    click.position = host->View().ToScreen( VECTOR2D( origin ) );

    click.type = HOST_INPUT_TYPE::POINTER_MOTION;
    host->DispatchInput( click );
    click.type = HOST_INPUT_TYPE::POINTER_DOWN;
    host->DispatchInput( click );
    click.type = HOST_INPUT_TYPE::POINTER_UP;
    host->DispatchInput( click );

    BOOST_REQUIRE_EQUAL( host->GetSelectionCount(), 1u );

    dragBy( *host, origin, delta );

    BOOST_CHECK_EQUAL( label->GetPosition(), origin + delta );
    BOOST_CHECK( host->IsModified() );

    // And it is undoable, which is what makes it an edit rather than a mutation.
    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK_EQUAL( label->GetPosition(), origin );

    BOOST_REQUIRE( host->Redo() );
    BOOST_CHECK_EQUAL( label->GetPosition(), origin + delta );
}


/**
 * Drawing a wire, which is the other tool `SCH_MOVE_TOOL` dragged in with it.
 *
 * Moving a wire off a junction has to add one where it left, and that is
 * `SCH_LINE_WIRE_BUS_TOOL::AddJunctionsIfNeeded`; so the wire tool had to run before the
 * move tool could, and having it run means wires can be drawn.
 */
BOOST_AUTO_TEST_CASE( AWireCanBeDrawnWithThePointer )
{
    std::unique_ptr<SCH_HOST> host = hostForMoving();

    const std::size_t before = host->GetScreen()->Items().size();

    std::set<SCH_ITEM*> existingLines;

    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_LINE_T ) )
        existingLines.insert( item );

    // Two points on the grid, in a corner of the page where the fixture has nothing, so
    // that the wire connects to nothing and the geometry is predictable.
    const BOX2I    page = host->GetDocumentBBox( true );
    const VECTOR2I from( page.GetLeft() + schIUScale.MilsToIU( 2000 ),
                         page.GetBottom() - schIUScale.MilsToIU( 2000 ) );
    const VECTOR2I to = from + VECTOR2I( schIUScale.MilsToIU( 1000 ), 0 );

    const auto moveTo =
            [&]( const VECTOR2I& aWorld )
            {
                HOST_INPUT_EVENT event;
                event.type = HOST_INPUT_TYPE::POINTER_MOTION;
                event.position = host->View().ToScreen( VECTOR2D( aWorld ) );
                host->DispatchInput( event );
            };

    const auto clickAtWorld =
            [&]( const VECTOR2I& aWorld )
            {
                moveTo( aWorld );

                HOST_INPUT_EVENT event;
                event.button = BUT_LEFT;
                event.position = host->View().ToScreen( VECTOR2D( aWorld ) );

                event.type = HOST_INPUT_TYPE::POINTER_DOWN;
                host->DispatchInput( event );
                event.type = HOST_INPUT_TYPE::POINTER_UP;
                host->DispatchInput( event );
            };

    // A toolbar click must not start a wire at the old cursor (or toolbar) position.
    moveTo( from - VECTOR2I( schIUScale.MilsToIU( 1000 ), 0 ) );
    BOOST_REQUIRE( host->RunActionByName(
            "eeschema.InteractiveDrawingLineWireBus.drawWires", true ) );
    clickAtWorld( from );
    clickAtWorld( to );

    // Finish, rather than cancel: Escape during wire drawing discards the segment in
    // progress, exactly as it does in the wx editor.
    BOOST_REQUIRE( host->RunActionByName( "common.Interactive.finish" ) );

    // Which wire is new rather than where it is exactly: the click positions snap to the
    // grid, so the endpoints are the nearest grid points to what was asked for.
    SCH_LINE* drawn = nullptr;
    std::size_t newLines = 0;

    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_LINE_T ) )
    {
        if( !existingLines.count( item ) )
        {
            drawn = static_cast<SCH_LINE*>( item );
            ++newLines;
        }
    }

    BOOST_CHECK_EQUAL( newLines, 1u );
    BOOST_REQUIRE_MESSAGE( drawn, "the gesture drew no new line" );

    BOOST_CHECK_EQUAL( drawn->GetLayer(), LAYER_WIRE );

    // The span survives snapping, because the offset asked for is a whole number of grid
    // steps and both ends snap the same way.
    BOOST_CHECK_EQUAL( ( drawn->GetEndPoint() - drawn->GetStartPoint() ).EuclideanNorm(),
                       ( to - from ).EuclideanNorm() );
    BOOST_CHECK_GT( host->GetScreen()->Items().size(), before );
    BOOST_CHECK( host->IsModified() );

    // And it is undoable, so it is an edit rather than a mutation.
    BOOST_REQUIRE_GE( host->GetUndoCommandCount(), 1 );
    BOOST_REQUIRE( host->Undo() );
    BOOST_CHECK_EQUAL( host->GetScreen()->Items().size(), before );
}


/**
 * An edit survives a save and a reload, which is the last of Stage 4b's five.
 *
 * The fixture is copied to a temporary directory first, because a save writes the files it
 * was loaded from and a test that edits `qa/data/` in place would be a bug in the test
 * rather than a check of the code.
 */
BOOST_AUTO_TEST_CASE( AnEditSurvivesASaveAndAReload )
{
    const std::filesystem::path temp =
            std::filesystem::temp_directory_path()
            / ( "kicad_sch_host_save_" + std::to_string( ::getpid() ) );

    std::filesystem::create_directories( temp );

    const std::filesystem::path copy = temp / "erc_label_test.kicad_sch";

    std::filesystem::copy_file( std::filesystem::path(
                                        eeschemaFixture( wxT( "erc_label_test.kicad_sch" ) )
                                                .ToStdString() ),
                                copy, std::filesystem::copy_options::overwrite_existing );

    const wxString path = wxString::FromUTF8( copy.string() );
    const wxString edited = wxT( "SAVED_FROM_A_HOST" );

    {
        SCH_HOST host;

        BOOST_REQUIRE_MESSAGE( host.LoadFile( path ), host.GetLastError().ToStdString() );

        // A single-sheet fixture on purpose: a hierarchy's child screens keep the absolute
        // paths they were loaded from, so saving a copy of only the root would write the
        // children back over the originals.
        BOOST_REQUIRE_EQUAL( host.GetSheetHierarchy().size(), 1u );

        SCH_LABEL* label = nullptr;

        for( SCH_ITEM* item : host.GetScreen()->Items().OfType( SCH_LABEL_T ) )
        {
            label = static_cast<SCH_LABEL*>( item );
            break;
        }

        BOOST_REQUIRE( label );

        SCH_COMMIT commit( host.GetToolManager() );
        commit.Modify( label, host.GetScreen() );
        label->SetText( edited );
        commit.Push( wxT( "Rename label" ) );

        BOOST_REQUIRE( host.IsModified() );
        // Exercise the toolbar/hotkey action route, not just the direct Save API.
        BOOST_REQUIRE( host.RunActionByName( "common.Control.save" ) );

        // Saving clears the modified flags, which is what a UI's title bar reads.
        BOOST_CHECK( !host.IsModified() );
    }

    // A second session, so nothing is carried over in memory.
    {
        SCH_HOST reopened;

        BOOST_REQUIRE_MESSAGE( reopened.LoadFile( path ),
                               reopened.GetLastError().ToStdString() );

        bool found = false;

        for( SCH_ITEM* item : reopened.GetScreen()->Items().OfType( SCH_LABEL_T ) )
        {
            if( static_cast<SCH_LABEL*>( item )->GetText() == edited )
                found = true;
        }

        BOOST_CHECK_MESSAGE( found, "the edited label did not survive the save" );
        BOOST_CHECK( !reopened.IsModified() );

        // And it still renders, which is the check that the file is not merely parseable.
        const kgds_stream_view frame = reopened.Render();
        BOOST_CHECK_GT( frame.group_cmd_count, 0u );
    }

    std::filesystem::remove_all( temp );
}


/**
 * Undo, redo and save reach the host as *actions*, so a hotkey works and not only a menu.
 *
 * A hotkey is resolved inside `TOOL_MANAGER`: a UI on the far side of the C ABI can forward
 * ⌘Z but cannot intervene in what it means. `SCH_HOST_CONTROL` is the handler that makes the
 * key, a menu item and a palette entry all do the same thing.
 */
BOOST_AUTO_TEST_CASE( UndoRedoAndSaveAreActions )
{
    std::unique_ptr<SCH_HOST> host = hostForMoving();

    SCH_LABEL* label = nullptr;

    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_LABEL_T ) )
    {
        label = static_cast<SCH_LABEL*>( item );
        break;
    }

    BOOST_REQUIRE( label );

    const wxString original = label->GetText();

    SCH_COMMIT commit( host->GetToolManager() );
    commit.Modify( label, host->GetScreen() );
    label->SetText( wxT( "EDITED" ) );
    commit.Push( wxT( "Rename label" ) );

    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );

    BOOST_CHECK( host->RunActionByName( "common.Interactive.undo" ) );
    BOOST_CHECK_EQUAL( label->GetText(), original );

    BOOST_CHECK( host->RunActionByName( "common.Interactive.redo" ) );
    BOOST_CHECK_EQUAL( label->GetText(), wxString( wxT( "EDITED" ) ) );

    // An undo with nothing to undo is still *handled* — the tool ran and found the stack
    // empty, which is different from no tool having claimed the action.
    BOOST_CHECK( host->RunActionByName( "common.Interactive.undo" ) );
    BOOST_CHECK( host->RunActionByName( "common.Interactive.undo" ) );
    BOOST_CHECK_EQUAL( label->GetText(), original );
}


/**
 * The undo *hotkey*, which is the case a UI on the far side of the C ABI cannot arrange for
 * itself: it forwards a key press, and what that key means is decided inside `TOOL_MANAGER`.
 *
 * On macOS the modifier is Command, because `wx/defs.h` makes `wxMOD_CMD == wxMOD_CONTROL`
 * there and every `.DefaultHotkey( MD_CTRL + 'Z' )` in the tree is written on that
 * understanding — which is why the ABI carries the *physical* modifier and the host decides.
 * Here the event is built with `MD_CTRL` directly, which is what the host resolves ⌘ to.
 */
BOOST_AUTO_TEST_CASE( TheUndoHotkeyReachesTheHost )
{
    std::unique_ptr<SCH_HOST> host = hostForMoving();

    SCH_LABEL* label = nullptr;

    for( SCH_ITEM* item : host->GetScreen()->Items().OfType( SCH_LABEL_T ) )
    {
        label = static_cast<SCH_LABEL*>( item );
        break;
    }

    BOOST_REQUIRE( label );

    const wxString original = label->GetText();

    SCH_COMMIT commit( host->GetToolManager() );
    commit.Modify( label, host->GetScreen() );
    label->SetText( wxT( "EDITED" ) );
    commit.Push( wxT( "Rename label" ) );

    BOOST_REQUIRE_EQUAL( host->GetUndoCommandCount(), 1 );

    HOST_INPUT_EVENT key;
    key.type = HOST_INPUT_TYPE::KEY_DOWN;
    key.keyCode = 'Z';
    key.modifiers = MD_CTRL;

    BOOST_CHECK( host->DispatchInput( key ) );
    BOOST_CHECK_EQUAL( label->GetText(), original );
    BOOST_CHECK_EQUAL( host->GetUndoCommandCount(), 0 );
    BOOST_CHECK_EQUAL( host->GetRedoCommandCount(), 1 );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( SchHostAbi )

BOOST_AUTO_TEST_CASE( SymbolChooserBrowsesProjectLibrary )
{
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session(
            ksch_session_create(), ksch_session_destroy );
    BOOST_REQUIRE( session );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session.get(),
            eeschemaFixture( "variant_field_resolution/variant_field_resolution.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    const char* names = nullptr;
    BOOST_REQUIRE_EQUAL( ksch_session_symbol_libraries( session.get(), &names ), KSCH_OK );
    BOOST_CHECK( std::string( names ).find( "variant_test_lib" ) != std::string::npos );
    BOOST_REQUIRE_MESSAGE( ksch_session_browse_symbols( session.get(), "variant_test_lib", 0, &names ) == KSCH_OK,
                           ksch_session_last_error( session.get() ) );
    BOOST_CHECK( std::string( names ).find( "variant_test_lib:R_100R" ) != std::string::npos );
    BOOST_REQUIRE_EQUAL( ksch_session_browse_symbols( session.get(), "variant_test_lib", 1, &names ), KSCH_OK );
    BOOST_CHECK( std::string( names ).empty() );
    ksch_erc_result* erc = nullptr;
    BOOST_REQUIRE_EQUAL( ksch_session_run_erc( session.get(), &erc ), KSCH_OK );
    for( uint32_t i = 0; i < ksch_erc_result_count( erc ); ++i )
    {
        ksch_erc_violation violation{};
        BOOST_REQUIRE_EQUAL( ksch_erc_result_get( erc, i, &violation ), KSCH_OK );
        BOOST_CHECK( std::string( violation.message ).find( "does not include the symbol library" ) == std::string::npos );
    }
    ksch_erc_result_destroy( erc );

}

BOOST_AUTO_TEST_CASE( SymbolChooserPlacementCancelsAndUndoes )
{
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session(
            ksch_session_create(), ksch_session_destroy );
    BOOST_REQUIRE( session );
    const char* ids = nullptr;
    BOOST_CHECK_EQUAL( ksch_session_list_symbols( session.get(), &ids ), KSCH_ERR_NO_DOCUMENT );
    BOOST_CHECK_EQUAL( ksch_session_place_symbol( nullptr, "Device:R" ), KSCH_ERR_INVALID_ARG );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session.get(),
            eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_list_symbols( session.get(), &ids ), KSCH_OK );
    BOOST_REQUIRE( ids && *ids );
    const std::string id = std::string( ids ).substr( 0, std::string( ids ).find( '\n' ) );
    const std::string cached = ids;
    kgds_stream_view preview{};
    uint32_t units = 0, bodies = 0;
    BOOST_REQUIRE_MESSAGE( ksch_session_preview_symbol( session.get(), id.c_str(), 1, 1,
            &units, &bodies, &preview ) == KSCH_OK, ksch_session_last_error( session.get() ) );
    BOOST_CHECK_GE( units, 1u );
    BOOST_CHECK_GE( bodies, 1u );
    BOOST_CHECK_GT( preview.group_count, 0u );
    BOOST_CHECK_EQUAL( ksch_session_place_symbol_variant( session.get(), id.c_str(),
            units + 1, 1 ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_preview_symbol( session.get(), id.c_str(), 0, 1,
            &units, &bodies, &preview ), KSCH_ERR_INVALID_ARG );
    BOOST_REQUIRE_EQUAL( ksch_session_symbol_libraries( session.get(), &ids ), KSCH_OK );
    BOOST_REQUIRE( ids != nullptr );
    BOOST_REQUIRE_EQUAL( ksch_session_browse_symbols( session.get(), "", 0, &ids ), KSCH_OK );
    BOOST_CHECK_EQUAL( std::string( ids ), cached );
    BOOST_REQUIRE_EQUAL( ksch_session_browse_symbols( session.get(), "", 1, &ids ), KSCH_OK );
    BOOST_CHECK( std::string( ids ).find( "Device:R" ) == std::string::npos );
    BOOST_CHECK_EQUAL( ksch_session_browse_symbols( session.get(), "", 2, &ids ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_browse_symbols( session.get(), "missing-stage5-library", 0, &ids ), KSCH_ERR_INVALID_ARG );
    ksch_viewport viewport{ 800, 600, 0, 0, 0.001 };
    BOOST_REQUIRE_EQUAL( ksch_session_set_viewport( session.get(), &viewport ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_zoom_to_fit( session.get() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_place_symbol( session.get(), id.c_str() ), KSCH_OK );
    ksch_input_event cancel{};
    cancel.type = KSCH_INPUT_CANCEL;
    BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session.get(), &cancel, nullptr ), KSCH_OK );
    int changed = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session.get(), &changed ), KSCH_OK );
    BOOST_CHECK_EQUAL( changed, 0 );
    BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session.get(), &cancel, nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_place_symbol( session.get(), id.c_str() ), KSCH_OK );
    for( int type : { KSCH_INPUT_POINTER_MOTION, KSCH_INPUT_POINTER_DOWN, KSCH_INPUT_POINTER_UP } )
    {
        ksch_input_event input{};
        input.type = type;
        input.button = KSCH_BUTTON_LEFT;
        input.x = 400;
        input.y = 300;
        BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session.get(), &input, nullptr ), KSCH_OK );
    }
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session.get(), &changed ), KSCH_OK );
    BOOST_CHECK_EQUAL( changed, 1 );
    BOOST_REQUIRE_EQUAL( ksch_session_redo( session.get(), &changed ), KSCH_OK );
    BOOST_CHECK_EQUAL( changed, 1 );
}

BOOST_AUTO_TEST_CASE( SearchAbiCopiesUtf8AndReportsResults )
{
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session(
            ksch_session_create(), ksch_session_destroy );
    BOOST_REQUIRE( session );
    ksch_search_result result{};
    ksch_search_data data{};
    data.find = "B0";
    data.replace = "Grüße_測試";
    data.active = data.replace_mode = data.whole_word = data.current_sheet_only = 1;
    BOOST_CHECK_EQUAL( ksch_session_set_search_data( session.get(), &data ),
                       KSCH_ERR_NO_DOCUMENT );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session.get(),
            eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_set_search_data( session.get(), &data ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session.get(),
            "common.Interactive.findNext", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_search_result( session.get(), &result ), KSCH_OK );
    BOOST_CHECK_EQUAL( result.found, 1u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session.get(),
            "common.Interactive.replaceAndFindNext", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_search_result( session.get(), &result ), KSCH_OK );
    BOOST_CHECK_EQUAL( result.replaced, 1u );
    data.find = "Grüße_測試";
    BOOST_REQUIRE_EQUAL( ksch_session_set_search_data( session.get(), &data ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session.get(),
            "common.Interactive.findNext", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_search_result( session.get(), &result ), KSCH_OK );
    BOOST_CHECK_EQUAL( result.found, 1u );
    int undone = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session.get(), &undone ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( undone, 1 );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session.get(),
            "common.Interactive.findNext", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_search_result( session.get(), &result ), KSCH_OK );
    BOOST_CHECK_EQUAL( result.found, 0u );
    data.active = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_set_search_data( session.get(), &data ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_search_result( session.get(), &result ), KSCH_OK );
    BOOST_CHECK_EQUAL( result.found, 0u );
}



/**
 * The handle lifecycle, and the promise that null handles are errors rather than
 * crashes. Every one of these would be a segfault in a naive implementation, and
 * a segfault inside a Rust FFI call is the worst possible failure mode.
 */
BOOST_AUTO_TEST_CASE( NullHandlesAreErrors )
{
    BOOST_CHECK_EQUAL( ksch_abi_version(), KSCH_ABI_VERSION );

    BOOST_CHECK_EQUAL( ksch_session_load_file( nullptr, "x" ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_unload( nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_is_loaded( nullptr ), 0 );
    BOOST_CHECK_EQUAL( ksch_session_zoom_to_fit( nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_set_sheet( nullptr, 0 ), KSCH_ERR_INVALID_ARG );

    ksch_document_info info;
    BOOST_CHECK_EQUAL( ksch_session_document_info( nullptr, &info ), KSCH_ERR_INVALID_ARG );

    ksch_bbox box;
    BOOST_CHECK_EQUAL( ksch_session_bbox( nullptr, 1, &box ), KSCH_ERR_INVALID_ARG );

    kgds_stream_view view;
    BOOST_CHECK_EQUAL( ksch_session_render( nullptr, &view ), KSCH_ERR_INVALID_ARG );

    // A null error pointer must still be printable.
    BOOST_CHECK( ksch_session_last_error( nullptr ) != nullptr );
    BOOST_CHECK( ksch_last_global_error() != nullptr );

    // Destroying null is a no-op, as free() is.
    ksch_session_destroy( nullptr );

    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    // Null out-parameters are caught before anything is dereferenced.
    BOOST_CHECK_EQUAL( ksch_session_load_file( session, nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_render( session, nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_document_info( session, nullptr ), KSCH_ERR_INVALID_ARG );

    ksch_session_destroy( session );
}


/**
 * Every call that needs a document reports KSCH_ERR_NO_DOCUMENT on an empty
 * session instead of dereferencing a null schematic.
 */
BOOST_AUTO_TEST_CASE( CallsWithoutADocumentAreRejected )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    ksch_document_info info;
    BOOST_CHECK_EQUAL( ksch_session_document_info( session, &info ), KSCH_ERR_NO_DOCUMENT );

    ksch_bbox box;
    BOOST_CHECK_EQUAL( ksch_session_bbox( session, 1, &box ), KSCH_ERR_NO_DOCUMENT );

    kgds_stream_view view;
    BOOST_CHECK_EQUAL( ksch_session_render( session, &view ), KSCH_ERR_NO_DOCUMENT );

    BOOST_CHECK_EQUAL( ksch_session_zoom_to_fit( session ), KSCH_ERR_NO_DOCUMENT );

    std::uint32_t count = 99;
    BOOST_CHECK_EQUAL( ksch_session_sheet_count( session, &count ), KSCH_ERR_NO_DOCUMENT );

    // An error leaves the caller's buffer alone.
    BOOST_CHECK_EQUAL( count, 99u );

    // Unloading an empty session is legal and idempotent.
    BOOST_CHECK_EQUAL( ksch_session_unload( session ), KSCH_OK );

    ksch_session_destroy( session );
}


/**
 * A missing file is an error code with a message, not an exception and not a
 * crash.
 */
BOOST_AUTO_TEST_CASE( MissingFileReturnsAnError )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    const ksch_status status =
            ksch_session_load_file( session, "/nonexistent/definitely/not/here.kicad_sch" );

    BOOST_CHECK_EQUAL( status, KSCH_ERR_FILE_NOT_FOUND );
    BOOST_CHECK( std::string( ksch_session_last_error( session ) ).size() > 0 );
    BOOST_CHECK_EQUAL( ksch_session_is_loaded( session ), 0 );

    ksch_session_destroy( session );
}


/**
 * A file that exists but is not a schematic must come back as a load failure with
 * the reader's own message, having left no half-built document behind.
 */
BOOST_AUTO_TEST_CASE( CorruptFileReturnsAnError )
{
    const wxString path = wxFileName::CreateTempFileName( wxT( "kicad_sch_host_corrupt" ) );

    BOOST_REQUIRE( !path.IsEmpty() );

    {
        std::ofstream out( path.utf8_str().data(), std::ios::binary );
        // Plausible enough to get past a sniff test, malformed enough to fail parsing.
        out << "(kicad_sch (version 20230121) (generator eeschema) (this is not valid\n";
    }

    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    const ksch_status status = ksch_session_load_file( session, path.utf8_str().data() );

    BOOST_CHECK_EQUAL( status, KSCH_ERR_LOAD_FAILED );
    BOOST_CHECK_EQUAL( ksch_session_is_loaded( session ), 0 );

    // Whatever went wrong, a caller with no exception handler got a message.
    BOOST_CHECK( ksch_session_last_error( session ) != nullptr );

    // And the session is still usable afterwards.
    kgds_stream_view view;
    BOOST_CHECK_EQUAL( ksch_session_render( session, &view ), KSCH_ERR_NO_DOCUMENT );

    ksch_session_destroy( session );

    wxRemoveFile( path );
}


/**
 * Out-of-domain viewports are rejected without touching the camera.
 */
BOOST_AUTO_TEST_CASE( InvalidViewportsAreRejected )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    ksch_viewport viewport;
    viewport.width_px = 0;
    viewport.height_px = 100;
    viewport.center_x = 0.0;
    viewport.center_y = 0.0;
    viewport.scale = 1.0;

    BOOST_CHECK_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_ERR_INVALID_ARG );

    viewport.width_px = 100;
    viewport.scale = 0.0;
    BOOST_CHECK_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_ERR_INVALID_ARG );

    viewport.scale = -1.0;
    BOOST_CHECK_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_ERR_INVALID_ARG );

    viewport.scale = 1.0;
    viewport.width_px = 100000;
    BOOST_CHECK_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_ERR_INVALID_ARG );

    viewport.width_px = 640;
    viewport.height_px = 480;
    BOOST_CHECK_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_OK );

    ksch_viewport readback;
    BOOST_REQUIRE_EQUAL( ksch_session_get_viewport( session, &readback ), KSCH_OK );
    BOOST_CHECK_EQUAL( readback.width_px, 640u );
    BOOST_CHECK_EQUAL( readback.height_px, 480u );

    ksch_session_destroy( session );
}


/**
 * The whole ABI, exercised the way a Rust UI would: open, load, frame, render,
 * read the stream back.
 */
BOOST_AUTO_TEST_CASE( FullRoundTripThroughTheAbi )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    const wxString path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );

    const ksch_status status = ksch_session_load_file( session, path.utf8_str().data() );

    BOOST_REQUIRE_MESSAGE( status == KSCH_OK, ksch_session_last_error( session ) );
    BOOST_CHECK_EQUAL( ksch_session_is_loaded( session ), 1 );

    ksch_document_info info;
    BOOST_REQUIRE_EQUAL( ksch_session_document_info( session, &info ), KSCH_OK );
    BOOST_CHECK_GE( info.sheet_count, 1u );
    BOOST_CHECK_GT( info.item_count, 0u );

    // Whatever the loader left the dirty flag at, rendering must not change it:
    // recording a frame is a read of the document, not an edit.
    const std::uint32_t modifiedAfterLoad = info.modified;

    std::uint32_t sheetCount = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_sheet_count( session, &sheetCount ), KSCH_OK );
    BOOST_CHECK_EQUAL( sheetCount, info.sheet_count );

    ksch_sheet_info sheet;
    BOOST_REQUIRE_EQUAL( ksch_session_sheet_info( session, 0, &sheet ), KSCH_OK );
    BOOST_CHECK( sheet.path != nullptr );
    BOOST_CHECK_EQUAL( ksch_session_sheet_info( session, sheetCount, &sheet ),
                       KSCH_ERR_OUT_OF_RANGE );

    ksch_bbox box;
    BOOST_REQUIRE_EQUAL( ksch_session_bbox( session, 1, &box ), KSCH_OK );
    BOOST_CHECK_GT( box.width, 0.0 );
    BOOST_CHECK_GT( box.height, 0.0 );

    ksch_viewport viewport;
    viewport.width_px = 1920;
    viewport.height_px = 1080;
    viewport.center_x = box.x + box.width / 2.0;
    viewport.center_y = box.y + box.height / 2.0;
    viewport.scale = static_cast<double>( viewport.width_px ) / box.width;

    BOOST_REQUIRE_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_OK );

    kgds_stream_view view;
    BOOST_REQUIRE_EQUAL( ksch_session_render( session, &view ), KSCH_OK );

    BOOST_CHECK_EQUAL( view.version, KGDS_VERSION );
    BOOST_CHECK_GT( view.group_count, 0u );
    BOOST_CHECK_GT( view.frame_cmd_count, 0u );
    BOOST_CHECK( coordIndicesInRange( view ) );

    ksch_document_info afterRender;
    BOOST_REQUIRE_EQUAL( ksch_session_document_info( session, &afterRender ), KSCH_OK );
    BOOST_CHECK_EQUAL( afterRender.modified, modifiedAfterLoad );
    BOOST_CHECK_EQUAL( afterRender.item_count, info.item_count );

    // Publishing without rendering hands back the same buffers.
    kgds_stream_view again;
    BOOST_REQUIRE_EQUAL( ksch_session_publish( session, &again ), KSCH_OK );
    BOOST_CHECK_EQUAL( again.group_cmd_count, view.group_cmd_count );
    BOOST_CHECK_EQUAL( again.frame_cmd_count, view.frame_cmd_count );

    // And the stream writes to disk.
    const wxString out = wxFileName::CreateTempFileName( wxT( "kicad_sch_host_stream" ) );
    BOOST_REQUIRE_EQUAL( ksch_session_write_stream( session, out.utf8_str().data() ), KSCH_OK );
    BOOST_CHECK_GT( wxFileName::GetSize( out ).GetValue(), sizeof( kgds_file_header ) );
    wxRemoveFile( out );

    ksch_session_destroy( session );
}


/**
 * The input half of the ABI: three vocabularies — button ordinals, modifier bits
 * and key names — become KiCad's, and a UI on the far side gets back whether the
 * event was claimed and whether its frame is now stale.
 */
BOOST_AUTO_TEST_CASE( InputEntryPointsRejectNullArguments )
{
    ksch_input_event  event = {};
    ksch_editor_state state = {};

    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( nullptr, &event, nullptr ),
                       KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_reset_input( nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_run_action( nullptr, "x", nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_editor_state( nullptr, &state ), KSCH_ERR_INVALID_ARG );

    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, nullptr, nullptr ),
                       KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_run_action( session, nullptr, nullptr ),
                       KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_editor_state( session, nullptr ), KSCH_ERR_INVALID_ARG );

    ksch_session_destroy( session );
}


BOOST_AUTO_TEST_CASE( AnUnknownInputTypeIsAnErrorRatherThanIgnored )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    ksch_input_event event = {};
    event.type = 9999;

    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &event, nullptr ),
                       KSCH_ERR_INVALID_ARG );
    BOOST_CHECK( *ksch_session_last_error( session ) != '\0' );

    ksch_session_destroy( session );
}


/**
 * The round trip: a pointer position in screen pixels goes in and the cursor the
 * tools read comes back in internal units, having gone through the view transform.
 */
BOOST_AUTO_TEST_CASE( PointerInputMovesTheCursorReportedBack )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    const wxString path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );

    BOOST_REQUIRE_MESSAGE( ksch_session_load_file( session, path.utf8_str().data() ) == KSCH_OK,
                           ksch_session_last_error( session ) );

    ksch_viewport viewport;
    viewport.width_px = 800;
    viewport.height_px = 600;
    viewport.center_x = 0.0;
    viewport.center_y = 0.0;
    viewport.scale = 1.0;
    BOOST_REQUIRE_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_zoom_to_fit( session ), KSCH_OK );

    ksch_editor_state before = {};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &before ), KSCH_OK );

    // Nothing has reported a pointer yet, so there is nothing to draw a crosshair at.
    BOOST_CHECK( ( before.flags & KSCH_EDITOR_POINTER_OVER_CANVAS ) == 0u );

    ksch_input_event event = {};
    event.type = KSCH_INPUT_POINTER_MOTION;
    event.x = 100.0;
    event.y = 50.0;

    std::uint32_t flags = 0xffffffffu;
    BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session, &event, &flags ), KSCH_OK );

    ksch_editor_state after = {};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &after ), KSCH_OK );

    BOOST_CHECK( ( after.flags & KSCH_EDITOR_POINTER_OVER_CANVAS ) != 0u );
    BOOST_CHECK( after.cursor_x != before.cursor_x || after.cursor_y != before.cursor_y );

    // The cursor is in internal units, so it is on the scale of a schematic page
    // rather than of a pixel. A page is millions of internal units across; a
    // failure to convert would leave this at 100.
    BOOST_CHECK( std::abs( after.cursor_x ) > 1000.0 || std::abs( after.cursor_y ) > 1000.0 );

    event.type = KSCH_INPUT_POINTER_LEAVE;
    BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session, &event, nullptr ), KSCH_OK );

    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &after ), KSCH_OK );
    BOOST_CHECK( ( after.flags & KSCH_EDITOR_POINTER_OVER_CANVAS ) == 0u );

    ksch_session_destroy( session );
}


/**
 * A whole click gesture, and a key, over the ABI. The selection tool receives it;
 * nothing edits the document, because selecting is all it does.
 */
BOOST_AUTO_TEST_CASE( AClickGestureIsAcceptedAndChangesNothing )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    const wxString path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );

    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_zoom_to_fit( session ), KSCH_OK );

    ksch_document_info before = {};
    BOOST_REQUIRE_EQUAL( ksch_session_document_info( session, &before ), KSCH_OK );

    const int types[] = { KSCH_INPUT_POINTER_MOTION, KSCH_INPUT_POINTER_DOWN,
                          KSCH_INPUT_POINTER_UP, KSCH_INPUT_POINTER_DBLCLICK };

    for( int type : types )
    {
        ksch_input_event event = {};
        event.type = type;
        event.button = KSCH_BUTTON_LEFT;
        event.x = 400.0;
        event.y = 300.0;

        BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &event, nullptr ), KSCH_OK );
    }

    ksch_input_event key = {};
    key.type = KSCH_INPUT_KEY_DOWN;
    key.key = "w";
    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &key, nullptr ), KSCH_OK );

    // A key name the mapping does not know is dropped, not an error: a UI forwards
    // its whole key stream and some of it has no KiCad meaning.
    key.key = "no such key";
    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &key, nullptr ), KSCH_OK );

    key.key = nullptr;
    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &key, nullptr ), KSCH_OK );

    ksch_input_event cancel = {};
    cancel.type = KSCH_INPUT_CANCEL;
    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &cancel, nullptr ), KSCH_OK );

    BOOST_CHECK_EQUAL( ksch_session_reset_input( session ), KSCH_OK );

    ksch_document_info after = {};
    BOOST_REQUIRE_EQUAL( ksch_session_document_info( session, &after ), KSCH_OK );
    BOOST_CHECK_EQUAL( after.modified, before.modified );
    BOOST_CHECK_EQUAL( after.item_count, before.item_count );

    ksch_session_destroy( session );
}


BOOST_AUTO_TEST_CASE( AnActionNoToolHandlesIsReportedRatherThanAnError )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    std::uint32_t flags = 0xffffffffu;

    BOOST_CHECK_EQUAL( ksch_session_run_action( session, "no.such.action", &flags ), KSCH_OK );
    BOOST_CHECK( ( flags & KSCH_INPUT_HANDLED ) == 0u );

    // A document, because a session without one runs nothing at all: a tool asks the
    // editing context for the screen and uses the answer. `SchHost/AnEmptySessionRunsNothing`
    // is that rule; this case is about which actions have a tool behind them.
    const wxString path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );

    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );

    // A *registered* action with no tool behind it, which is the case that matters:
    // every action in the process is registered, so a made-up name would prove
    // nothing about whether "handled" means handled. Symbol-library control is not
    // registered in a schematic host.
    BOOST_CHECK_EQUAL( ksch_session_run_action( session, "eeschema.SymbolLibraryControl.newSymbol", &flags ),
                       KSCH_OK );
    BOOST_CHECK( ( flags & KSCH_INPUT_HANDLED ) == 0u );

    // And three that do have a tool behind them, so that "unhandled" above means something
    // other than "this never reports handled". The zoom is step 2's: COMMON_TOOLS runs
    // here now, so the view can be moved from the tool framework over the ABI.
    for( const char* name : { "common.InteractiveSelection",
                              "common.Control.zoomFitScreen",
                              "eeschema.InteractiveDrawingLineWireBus.drawWires" } )
    {
        BOOST_CHECK_EQUAL( ksch_session_run_action( session, name, &flags ), KSCH_OK );
        BOOST_CHECK_MESSAGE( ( flags & KSCH_INPUT_HANDLED ) != 0u,
                             std::string( name ) + " has a tool behind it now" );
    }

    // Leave the wire tool's loop, so that the session tears down idle.
    ksch_input_event cancel = {};
    cancel.type = KSCH_INPUT_CANCEL;
    BOOST_CHECK_EQUAL( ksch_session_dispatch_input( session, &cancel, nullptr ), KSCH_OK );

    ksch_session_destroy( session );
}


BOOST_AUTO_TEST_CASE( EditorStateStringsAreNeverNull )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session != nullptr );

    ksch_editor_state state = {};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );

    BOOST_REQUIRE( state.tool_name != nullptr );
    BOOST_REQUIRE( state.status_text != nullptr );
    BOOST_CHECK_EQUAL( state.selection_count, 0u );

    // And "no tool" is the empty string, as the header says. TOOLS_HOLDER answers an
    // empty tool stack with the selection tool's name — a sensible default for a
    // status bar that always has a tool, and a wrong answer for a UI that would then
    // display a tool which is not even registered.
    BOOST_CHECK_EQUAL( state.tool_name, "" );
    BOOST_CHECK_EQUAL( state.status_text, "" );

    ksch_session_destroy( session );
}


BOOST_AUTO_TEST_SUITE_END()


BOOST_AUTO_TEST_SUITE( SchHostActionRegistry )


/**
 * The action registry is enumerable headless.
 *
 * TOOL_ACTION's constructor pushes each action onto a process-wide list during
 * static initialisation, so this works with no frame, no window and no document.
 * The survey counted 191 shared actions plus 254 eeschema ones; qa_eeschema
 * links both sets, so the real number should be in that neighbourhood.
 */
BOOST_AUTO_TEST_CASE( ActionsAreEnumerable )
{
    const std::uint32_t count = ksch_action_count();

    BOOST_TEST_MESSAGE( "action count: " << count );

    // Generous bounds: this must not become a change-detector on a list that
    // legitimately grows, but it must catch the registry coming back empty or
    // being truncated.
    BOOST_CHECK_GT( count, 300u );
    BOOST_CHECK_LT( count, 5000u );

    std::set<std::string> names;
    std::uint32_t withIcon = 0;
    std::uint32_t withHotkey = 0;

    for( std::uint32_t ii = 0; ii < count; ++ii )
    {
        ksch_action action;
        BOOST_REQUIRE_EQUAL( ksch_action_at( ii, &action ), KSCH_OK );

        // Every string is non-null, as the header promises.
        BOOST_REQUIRE( action.name != nullptr );
        BOOST_REQUIRE( action.friendly_name != nullptr );
        BOOST_REQUIRE( action.menu_label != nullptr );
        BOOST_REQUIRE( action.tooltip != nullptr );
        BOOST_REQUIRE( action.description != nullptr );
        BOOST_REQUIRE( action.icon_name != nullptr );
        BOOST_REQUIRE( action.tool_name != nullptr );

        const std::string name = action.name;

        // The dotted name is the stable key, so it must exist and be unique.
        BOOST_CHECK( !name.empty() );
        BOOST_CHECK_MESSAGE( names.insert( name ).second, "duplicate action name: " << name );

        // "app.tool.action" — the tool name is the name minus its last component.
        BOOST_CHECK( name.find( '.' ) != std::string::npos );

        if( action.icon_name[0] != '\0' )
            ++withIcon;

        if( action.default_hotkey != 0 )
            ++withHotkey;
    }

    BOOST_CHECK_EQUAL( names.size(), count );

    // Menus and toolbars need icons and hotkeys; if either resolution path broke,
    // these would drop to zero while everything else still looked fine.
    BOOST_TEST_MESSAGE( "actions with an icon: " << withIcon );
    BOOST_TEST_MESSAGE( "actions with a default hotkey: " << withHotkey );

    BOOST_CHECK_GT( withIcon, 100u );
    BOOST_CHECK_GT( withHotkey, 50u );

    BOOST_CHECK_EQUAL( ksch_action_at( count, nullptr ), KSCH_ERR_INVALID_ARG );

    ksch_action scratch;
    BOOST_CHECK_EQUAL( ksch_action_at( count, &scratch ), KSCH_ERR_OUT_OF_RANGE );
}


/**
 * A known eeschema action resolves by name, with the icon reported as the SVG
 * base name the Rust side will look up.
 */
BOOST_AUTO_TEST_CASE( KnownActionResolvesByName )
{
    ksch_action action;

    // Declared in eeschema/tools/sch_actions.cpp with .Icon( BITMAPS::pin ) and
    // .DefaultHotkey( 'P' ).
    BOOST_REQUIRE_EQUAL(
            ksch_action_find( "eeschema.SymbolDrawing.placeSymbolPin", &action ), KSCH_OK );

    BOOST_CHECK_EQUAL( std::string( action.name ), "eeschema.SymbolDrawing.placeSymbolPin" );
    BOOST_CHECK_EQUAL( std::string( action.tool_name ), "eeschema.SymbolDrawing" );
    BOOST_CHECK( std::string( action.friendly_name ).size() > 0 );

    // The enumerator name is the SVG file name; that identity is the whole reason a
    // name is exported rather than an opaque enum value.
    BOOST_CHECK_EQUAL( std::string( action.icon_name ), "pin" );

    const wxString svg = wxString::FromUTF8( KI_TEST::GetTestDataRootDir() )
                         + wxT( "../../resources/bitmaps_png/sources/light/" )
                         + wxString::FromUTF8( action.icon_name ) + wxT( ".svg" );

    BOOST_CHECK_MESSAGE( wxFileName::FileExists( svg ),
                         "icon SVG not found where the name says it is: " << svg.ToStdString() );

    BOOST_CHECK_EQUAL( action.default_hotkey, static_cast<std::int32_t>( 'P' ) );
    BOOST_CHECK( ( action.flags & KSCH_ACTION_FLAG_ACTIVATION ) != 0 );

    BOOST_CHECK_EQUAL( ksch_action_find( "no.such.action", &action ), KSCH_ERR_OUT_OF_RANGE );
    BOOST_CHECK_EQUAL( ksch_action_find( nullptr, &action ), KSCH_ERR_INVALID_ARG );
}


BOOST_AUTO_TEST_CASE( ErcExclusionsPersistAndRejectStaleMarkers )
{
    const auto temp = std::filesystem::temp_directory_path() / ("gpui_erc_exclusion_" + std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto path = temp / "erc.kicad_sch";
    std::filesystem::copy_file(eeschemaFixture("erc_label_test.kicad_sch").ToStdString(), path,
                              std::filesystem::copy_options::overwrite_existing);
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session(ksch_session_create(), ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(), path.string().c_str()), KSCH_OK);
    ksch_erc_result* result = nullptr;
    BOOST_REQUIRE_EQUAL(ksch_session_run_erc(session.get(), &result), KSCH_OK);
    BOOST_REQUIRE_GT(ksch_erc_result_count(result), 0u);
    ksch_erc_violation violation{};
    BOOST_REQUIRE_EQUAL(ksch_erc_result_get(result, 0, &violation), KSCH_OK);
    const std::string marker = violation.marker_id;
    BOOST_REQUIRE_MESSAGE(ksch_session_exclude_erc(session.get(), marker.c_str(), 1) == KSCH_OK,
                          ksch_session_last_error(session.get()));
    ksch_erc_result_destroy(result);
    BOOST_CHECK(std::filesystem::exists(temp / "erc.kicad_pro"));
    session.reset(ksch_session_create());
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(), path.string().c_str()), KSCH_OK);
    BOOST_REQUIRE_EQUAL(ksch_session_run_erc(session.get(), &result), KSCH_OK);
    bool found = false;
    for(uint32_t i=0;i<ksch_erc_result_count(result);++i) {
        BOOST_REQUIRE_EQUAL(ksch_erc_result_get(result,i,&violation),KSCH_OK);
        found |= violation.severity == RPT_SEVERITY_EXCLUSION;
    }
    BOOST_CHECK(found);
    BOOST_CHECK_EQUAL(ksch_session_exclude_erc(session.get(), "stale-marker", 1), KSCH_ERR_INVALID_ARG);
    ksch_erc_result_destroy(result);
}

BOOST_AUTO_TEST_CASE( ErcSnapshotsOwnResultsAcrossRerunsAndSessionDestruction )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const wxString path = eeschemaFixture( wxT( "erc_label_test.kicad_sch" ) );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    ksch_document_info before{};
    BOOST_REQUIRE_EQUAL( ksch_session_document_info( session, &before ), KSCH_OK );
    ksch_erc_result* first = nullptr;
    BOOST_REQUIRE_MESSAGE( ksch_session_run_erc( session, &first ) == KSCH_OK,
                           ksch_session_last_error( session ) );
    BOOST_REQUIRE_GT( ksch_erc_result_count( first ), 0u );
    ksch_erc_violation violation{};
    BOOST_REQUIRE_EQUAL( ksch_erc_result_get( first, 0, &violation ), KSCH_OK );
    const std::string message = violation.message;
    BOOST_CHECK( !message.empty() );
    BOOST_CHECK_EQUAL( violation.sheet_index, 0u );
    BOOST_CHECK( std::isfinite( violation.x ) && std::isfinite( violation.y ) );
    ksch_erc_result* second = nullptr;
    BOOST_REQUIRE_EQUAL( ksch_session_run_erc( session, &second ), KSCH_OK );
    BOOST_CHECK_EQUAL( ksch_erc_result_count( first ), ksch_erc_result_count( second ) );
    // Markers are transient and must not make a clean document appear edited.
    ksch_document_info after{};
    BOOST_REQUIRE_EQUAL( ksch_session_document_info( session, &after ), KSCH_OK );
    BOOST_CHECK_EQUAL( before.modified, after.modified );
    kgds_stream_view stream{};
    BOOST_REQUIRE_EQUAL( ksch_session_render( session, &stream ), KSCH_OK );
    ksch_session_destroy( session );
    BOOST_CHECK_EQUAL( std::string( violation.message ), message );
    BOOST_CHECK_EQUAL( ksch_erc_result_get( first, ksch_erc_result_count( first ), &violation ),
                       KSCH_ERR_INVALID_ARG );
    ksch_erc_result_destroy( first );
    ksch_erc_result_destroy( second );
    ksch_erc_result_destroy( nullptr );
}

BOOST_AUTO_TEST_CASE( PropertiesValidateReferencesAndNoOpDoesNotCreateUndo )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const wxString path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    // Hit the visible body edge at a useful editing zoom, not the hollow centre
    // at page-fit scale where the symbol and its field hit targets overlap.
    ksch_viewport viewport{ 1920, 1080, 109.22 * 10000, 40.64 * 10000, 0.004 };
    BOOST_REQUIRE_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_get_viewport( session, &viewport ), KSCH_OK );
    for( int type : { KSCH_INPUT_POINTER_MOTION, KSCH_INPUT_POINTER_DOWN, KSCH_INPUT_POINTER_UP } )
    {
        ksch_input_event event{};
        event.type = type;
        event.button = KSCH_BUTTON_LEFT;
        event.x = ( 108.204 * 10000 - viewport.center_x ) * viewport.scale + viewport.width_px / 2.;
        event.y = ( 40.64 * 10000 - viewport.center_y ) * viewport.scale + viewport.height_px / 2.;
        BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session, &event, nullptr ), KSCH_OK );
    }
    struct Properties { std::string id; std::vector<std::string> values, names; std::vector<uint32_t> kinds; } properties;
    auto visitor = []( void* context, const char* id, const char* name, const char* value, uint32_t kind, const char* const*, uint32_t )
    {
        auto& result = *static_cast<Properties*>( context );
        result.id = id;
        result.values.emplace_back( value );
        result.names.emplace_back( name );
        result.kinds.push_back( kind );
    };
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &properties ), KSCH_OK );
    BOOST_REQUIRE_GE( properties.values.size(), 2u );
    BOOST_CHECK_EQUAL( properties.values[0], "R1" );
    auto apply = [&]()
    {
        std::vector<const char*> values;
        for( const auto& value : properties.values ) values.push_back( value.c_str() );
        return ksch_session_apply_properties( session, properties.id.c_str(), values.data(), values.size() );
    };
    BOOST_REQUIRE_EQUAL( apply(), KSCH_OK );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    for( const char* invalid : { "", "R 2", "R\n2", "R\t2" } )
    {
        properties.values[0] = invalid;
        BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
    }
    properties.values[0] = "R1";
    bool checkedDistance = false, checkedBool = false, checkedChoice = false;
    for( size_t i = 0; i < properties.values.size(); ++i )
    {
        auto original = properties.values[i];
        if( properties.kinds[i] == KSCH_PROPERTY_DISTANCE && !checkedDistance )
        {
            properties.values[i] = "nan";
            BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
            properties.values[i] = "9999999999999999999999";
            BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
            checkedDistance = true;
        }
        else if( properties.kinds[i] == KSCH_PROPERTY_BOOL && !checkedBool )
        {
            properties.values[i] = "maybe";
            BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
            checkedBool = true;
        }
        else if( properties.kinds[i] == KSCH_PROPERTY_CHOICE && !checkedChoice )
        {
            properties.values[i] = "not an allowed option";
            BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
            checkedChoice = true;
        }
        properties.values[i] = original;
    }
    BOOST_CHECK( checkedDistance && checkedBool && checkedChoice );
    properties.values[0] = "R42";
    BOOST_REQUIRE_EQUAL( apply(), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    // Reference comparison uses the current sheet instance, including after editing it.
    BOOST_REQUIRE_EQUAL( apply(), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    Properties reread;
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &reread ), KSCH_OK );
    BOOST_CHECK_EQUAL( reread.values[0], "R42" );
    int undone = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session, &undone ), KSCH_OK );
    BOOST_CHECK_EQUAL( undone, 1 );
    BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_edit_custom_field( session, properties.id.c_str(), "Reference", nullptr ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_edit_custom_field( session, properties.id.c_str(), "", "bad" ), KSCH_ERR_INVALID_ARG );
    BOOST_REQUIRE_EQUAL( ksch_session_edit_custom_field( session, properties.id.c_str(), "Assembly note", "Hand solder" ), KSCH_OK );
    BOOST_CHECK_EQUAL( ksch_session_edit_custom_field( session, properties.id.c_str(), "Assembly note", "Duplicate" ), KSCH_ERR_INVALID_ARG );
    Properties withField;
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &withField ), KSCH_OK );
    BOOST_CHECK( std::find( withField.values.begin(), withField.values.end(), "Hand solder" ) != withField.values.end() );
    BOOST_REQUIRE_EQUAL( ksch_session_edit_custom_field( session, properties.id.c_str(), "Assembly note", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session, &undone ), KSCH_OK );
    BOOST_CHECK_EQUAL( undone, 1 );
    Properties restoredField;
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &restoredField ), KSCH_OK );
    BOOST_CHECK( std::find( restoredField.values.begin(), restoredField.values.end(), "Hand solder" ) != restoredField.values.end() );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( ApplicationPreferencesAreTypedAndValidateBeforePersistence )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    struct Preferences { std::vector<std::string> values; std::vector<uint32_t> kinds; } preferences;
    auto visit = []( void* ctx, const char*, const char*, const char* value,
                    uint32_t kind, const char* const*, uint32_t )
    {
        auto& preferences = *static_cast<Preferences*>( ctx );
        preferences.values.emplace_back( value );
        preferences.kinds.push_back( kind );
    };
    BOOST_REQUIRE_EQUAL( ksch_session_preferences( session, visit, &preferences ), KSCH_OK );
    BOOST_REQUIRE_GT( preferences.values.size(), 20u );
    auto apply = [&]()
    {
        std::vector<const char*> values;
        for( const auto& value : preferences.values ) values.push_back( value.c_str() );
        return ksch_session_apply_preferences( session, values.data(), values.size() );
    };
    // A no-op must not write the user's settings files.
    BOOST_REQUIRE_EQUAL( apply(), KSCH_OK );
    const auto before = preferences.values;
    for( size_t i = 0; i < preferences.values.size(); ++i )
    {
        if( preferences.kinds[i] == KSCH_PROPERTY_BOOL ) preferences.values[i] = "maybe";
        else if( preferences.kinds[i] == KSCH_PROPERTY_INTEGER ) preferences.values[i] = "1e99";
        else continue;
        BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
        preferences.values[i] = before[i];
    }
    Preferences after;
    BOOST_REQUIRE_EQUAL( ksch_session_preferences( session, visit, &after ), KSCH_OK );
    BOOST_CHECK( after.values == before );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( SheetPinSynchronizationValidatesPlanAndUndoesChanges )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const auto fixture = eeschemaFixture( "api_kitchen_sink.kicad_sch" );
    std::ifstream input( fixture.ToStdString() );
    std::string source( ( std::istreambuf_iterator<char>( input ) ), std::istreambuf_iterator<char>() );
    const std::string originalPin = "(pin \"${param}\" input";
    auto pinAt = source.find( originalPin );
    BOOST_REQUIRE( pinAt != std::string::npos );
    source.replace( pinAt, originalPin.size(), "(pin \"OrphanForSync\" input" );
    const std::string child = "erc_test_dynamic_power_symbol_subsheet.kicad_sch";
    auto childAt = source.find( child );
    BOOST_REQUIRE( childAt != std::string::npos );
    source.replace( childAt, child.size(), eeschemaFixture( child.c_str() ).ToStdString() );
    wxFileName temporaryName( wxFileName::CreateTempFileName( "gpui-pin-sync-" ) );
    wxRemoveFile( temporaryName.GetFullPath() );
    temporaryName.SetExt( "kicad_sch" );
    const auto temporary = temporaryName.GetFullPath();
    { std::ofstream output( temporary.ToStdString() ); output << source; }
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, temporary.utf8_str().data() ), KSCH_OK );
    struct Plan { std::string id; std::vector<std::string> values, alternatives; } plan;
    auto visitor = []( void* ctx, const char* id, const char*, const char* value,
                      uint32_t kind, const char* const* choices, uint32_t count )
    {
        auto& plan = *static_cast<Plan*>( ctx );
        BOOST_REQUIRE_EQUAL( kind, KSCH_PROPERTY_CHOICE );
        BOOST_REQUIRE_GT( count, 1u );
        plan.id = id;
        plan.values.emplace_back( value );
        plan.alternatives.emplace_back( choices[1] );
    };
    BOOST_REQUIRE_EQUAL( ksch_session_sheet_pin_properties( session, 1, visitor, &plan ), KSCH_OK );
    BOOST_REQUIRE( !plan.values.empty() );
    auto apply = [&]()
    {
        std::vector<const char*> values;
        for( const auto& value : plan.values ) values.push_back( value.c_str() );
        return ksch_session_apply_sheet_pin_properties( session, 1, plan.id.c_str(), values.data(), values.size() );
    };
    BOOST_REQUIRE_EQUAL( apply(), KSCH_OK );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    plan.values[0] = "Invalid action";
    BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
    plan.values[0] = plan.alternatives[0];
    BOOST_REQUIRE_EQUAL( apply(), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    BOOST_CHECK_EQUAL( apply(), KSCH_ERR_INVALID_ARG );
    int undone = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session, &undone ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( undone, 1 );
    Plan restored;
    BOOST_REQUIRE_EQUAL( ksch_session_sheet_pin_properties( session, 1, visitor, &restored ), KSCH_OK );
    BOOST_CHECK_EQUAL( restored.id, plan.id );
    ksch_session_destroy( session );
    wxRemoveFile( temporary );
}

BOOST_AUTO_TEST_CASE( TextPlacementRequestsGpuiPropertiesAndCommitsOnce )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session,
        eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session,
        "eeschema.InteractiveDrawing.placeSchematicText", nullptr ), KSCH_OK );
    int pending = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_take_pending_properties( session, &pending ), KSCH_OK );
    if( !pending )
    {
        BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Control.cursorClick", nullptr ), KSCH_OK );
        BOOST_REQUIRE_EQUAL( ksch_session_take_pending_properties( session, &pending ), KSCH_OK );
    }
    BOOST_REQUIRE_EQUAL( pending, 1 );
    BOOST_REQUIRE_EQUAL( ksch_session_take_pending_properties( session, &pending ), KSCH_OK );
    BOOST_CHECK_EQUAL( pending, 0 );
    struct Data { std::string id; std::vector<std::string> values; } data;
    auto visitor = []( void* ctx, const char* id, const char*, const char* value,
                       uint32_t kind, const char* const*, uint32_t )
    {
        auto& data = *static_cast<Data*>( ctx );
        data.id = id;
        data.values.emplace_back( kind == KSCH_PROPERTY_MULTILINE ? "First line\nSecond line" : value );
    };
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &data ), KSCH_OK );
    std::vector<const char*> values;
    for( const auto& value : data.values ) values.push_back( value.c_str() );
    BOOST_REQUIRE_EQUAL( ksch_session_apply_properties( session, data.id.c_str(), values.data(), values.size() ), KSCH_OK );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Control.cursorClick", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( BusEntryPreviewPropertiesDoNotCreatePrematureUndo )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session,
        eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session,
        "eeschema.InteractiveDrawing.placeBusWireEntry", nullptr ), KSCH_OK );
    struct Data { std::string id; std::vector<std::string> values; bool width = false; } data;
    auto visitor = []( void* ctx, const char* uuid, const char* name, const char* value,
                       uint32_t kind, const char* const*, uint32_t )
    {
        auto& data = *static_cast<Data*>( ctx );
        data.id = uuid;
        if( std::string( name ) == "Line Width" )
        {
            BOOST_CHECK_EQUAL( kind, KSCH_PROPERTY_DISTANCE );
            data.width = true;
            data.values.emplace_back( "0.254" );
        }
        else data.values.emplace_back( value );
    };
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &data ), KSCH_OK );
    BOOST_REQUIRE( data.width );
    std::vector<const char*> values;
    for( const auto& value : data.values ) values.push_back( value.c_str() );
    BOOST_REQUIRE_EQUAL( ksch_session_apply_properties( session, data.id.c_str(), values.data(), values.size() ), KSCH_OK );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Control.cursorClick", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Interactive.cancel", nullptr ), KSCH_OK );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( RasterImagePlacementValidatesCancelsAndCommitsOnce )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session,
        eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    wxFileName file( wxFileName::CreateTempFileName( "gpui-image-" ) );
    wxRemoveFile( file.GetFullPath() );
    file.SetExt( "png" );
    wxImage image( 2, 2 );
    image.SetRGB( wxRect( 0, 0, 2, 2 ), 100, 150, 200 );
    BOOST_REQUIRE( image.SaveFile( file.GetFullPath(), wxBITMAP_TYPE_PNG ) );
    const auto filename = file.GetFullPath().ToStdString();
    const char* values[] = { filename.c_str(), "-1", "Cursor", "10", "20" };
    BOOST_CHECK_EQUAL( ksch_session_apply_image_properties( session, values, 5 ), KSCH_ERR_INVALID_ARG );
    values[1] = "2";
    BOOST_REQUIRE_EQUAL( ksch_session_apply_image_properties( session, values, 5 ), KSCH_OK );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Interactive.cancel", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    BOOST_REQUIRE_EQUAL( ksch_session_apply_image_properties( session, values, 5 ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Control.cursorClick", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Interactive.undo", nullptr ), KSCH_OK );
    values[2] = "Coordinates";
    BOOST_REQUIRE_EQUAL( ksch_session_apply_image_properties( session, values, 5 ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    ksch_session_destroy( session );
    wxRemoveFile( file.GetFullPath() );
}

BOOST_AUTO_TEST_CASE( GraphicsImportValidatesAndCreatesOneUndoTransaction )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session,
        eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    wxFileName file( wxFileName::CreateTempFileName( "gpui-import-" ) );
    wxRemoveFile( file.GetFullPath() );
    file.SetExt( "svg" );
    { std::ofstream output( file.GetFullPath().ToStdString() );
      output << R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="10mm" height="10mm" viewBox="0 0 10 10"><path d="M1 1 L9 9" stroke="black" fill="none" stroke-width="0.2"/></svg>)SVG"; }
    const auto filename = file.GetFullPath().ToStdString();
    const char* values[] = { filename.c_str(), "-1", "10", "20", "File default", "0.1524" };
    BOOST_CHECK_EQUAL( ksch_session_apply_graphics_import( session, values, 6 ), KSCH_ERR_INVALID_ARG );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    values[1] = "2";
    BOOST_REQUIRE_EQUAL( ksch_session_apply_graphics_import( session, values, 6 ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Interactive.undo", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    ksch_session_destroy( session );
    wxRemoveFile( file.GetFullPath() );
}

BOOST_AUTO_TEST_CASE( NewSheetRequestsGpuiPropertiesAndCommitsOnce )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session,
        eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session,
        "eeschema.InteractiveDrawing.drawSheet", nullptr ), KSCH_OK );
    for( int click = 0; click < 2; ++click )
        BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Control.cursorClick", nullptr ), KSCH_OK );
    int pending = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_take_pending_properties( session, &pending ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( pending, 1 );
    std::string id;
    auto visitor = []( void* ctx, const char* uuid, const char*, const char*, uint32_t,
                       const char* const*, uint32_t ) { *static_cast<std::string*>( ctx ) = uuid; };
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visitor, &id ), KSCH_OK );
    BOOST_REQUIRE( !id.empty() );
    BOOST_REQUIRE_EQUAL( ksch_session_edit_custom_field( session, id.c_str(), "Review", "Pending" ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_edit_custom_field( session, id.c_str(), "Review", nullptr ), KSCH_OK );
    wxFileName output( wxFileName::CreateTempFileName( "gpui-new-sheet-" ) );
    wxRemoveFile( output.GetFullPath() );
    output.SetExt( "kicad_sch" );
    BOOST_REQUIRE_EQUAL( ksch_session_relink_sheet( session, id.c_str(), output.GetFullPath().utf8_str().data() ), KSCH_OK );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Control.cursorClick", nullptr ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 1u );
    BOOST_CHECK( output.FileExists() );
    BOOST_REQUIRE_EQUAL( ksch_session_run_action( session, "common.Interactive.undo", nullptr ), KSCH_OK );
    ksch_session_destroy( session );
    wxRemoveFile( output.GetFullPath() );
}

BOOST_AUTO_TEST_CASE( SheetPropertiesRelinkRejectsRecursionAndLoadsReplacement )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const auto path = eeschemaFixture( "issue10926_1.kicad_sch" );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    ksch_viewport viewport{ 1920, 1080, 152.4 * 10000, 59.69 * 10000, 0.004 };
    BOOST_REQUIRE_EQUAL( ksch_session_set_viewport( session, &viewport ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_get_viewport( session, &viewport ), KSCH_OK );
    for( int type : { KSCH_INPUT_POINTER_MOTION, KSCH_INPUT_POINTER_DOWN, KSCH_INPUT_POINTER_UP } )
    {
        ksch_input_event event{};
        event.type = type;
        event.button = KSCH_BUTTON_LEFT;
        event.x = ( 152.4 * 10000 - viewport.center_x ) * viewport.scale + viewport.width_px / 2.;
        event.y = ( 59.69 * 10000 - viewport.center_y ) * viewport.scale + viewport.height_px / 2.;
        BOOST_REQUIRE_EQUAL( ksch_session_dispatch_input( session, &event, nullptr ), KSCH_OK );
    }
    std::string id;
    auto visit = []( void* ctx, const char* uuid, const char*, const char*, uint32_t,
                     const char* const*, uint32_t ) { *static_cast<std::string*>( ctx ) = uuid; };
    BOOST_REQUIRE_EQUAL( ksch_session_item_properties( session, visit, &id ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( id, "48e5c29d-dae7-410f-a801-6c402cc42990" );
    BOOST_CHECK_EQUAL( ksch_session_relink_sheet( session, id.c_str(), path.utf8_str().data() ), KSCH_ERR_INVALID_ARG );
    BOOST_CHECK_EQUAL( ksch_session_relink_sheet( session, id.c_str(), "bad.txt" ), KSCH_ERR_INVALID_ARG );
    const auto replacement = eeschemaFixture( "test_issue24663_sheet_dnp_sub.kicad_sch" );
    const auto relinkStatus = ksch_session_relink_sheet( session, id.c_str(), replacement.utf8_str().data() );
    BOOST_REQUIRE_MESSAGE( relinkStatus == KSCH_OK, ksch_session_last_error( session ) );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    uint32_t count = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_sheet_count( session, &count ), KSCH_OK );
    BOOST_CHECK_GE( count, 3u );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( ErcResultsCoverTheHierarchyFromAnyCurrentSheet )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const wxString path = eeschemaFixture( wxT( "issue10926_1.kicad_sch" ) );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    uint32_t sheetCount = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_sheet_count( session, &sheetCount ), KSCH_OK );
    BOOST_REQUIRE_GT( sheetCount, 1u );
    ksch_erc_result* rootResults = nullptr;
    BOOST_REQUIRE_EQUAL( ksch_session_run_erc( session, &rootResults ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_set_sheet( session, sheetCount - 1 ), KSCH_OK );
    ksch_erc_result* childResults = nullptr;
    BOOST_REQUIRE_EQUAL( ksch_session_run_erc( session, &childResults ), KSCH_OK );
    BOOST_CHECK_EQUAL( ksch_erc_result_count( rootResults ), ksch_erc_result_count( childResults ) );
    for( uint32_t i = 0; i < ksch_erc_result_count( childResults ); ++i )
    {
        ksch_erc_violation result{};
        BOOST_REQUIRE_EQUAL( ksch_erc_result_get( childResults, i, &result ), KSCH_OK );
        BOOST_REQUIRE_LT( result.sheet_index, sheetCount );
        BOOST_REQUIRE_EQUAL( ksch_session_set_sheet( session, result.sheet_index ), KSCH_OK );
        kgds_stream_view stream{};
        BOOST_REQUIRE_EQUAL( ksch_session_render( session, &stream ), KSCH_OK );
    }
    ksch_erc_result_destroy( rootResults );
    ksch_erc_result_destroy( childResults );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( DocumentDialogsExportCsvAndNetlistWithoutMutatingDocument )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const auto path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    const auto output = wxFileName::CreateTempFileName( "gpui-document-export-" );
    const auto outputUtf8 = output.utf8_string();
    const char* csv[] = { outputUtf8.c_str(), "no", "standard", "", "yes", "no", "", "", "" };
    BOOST_REQUIRE_EQUAL( ksch_session_apply_document_workflow( session, 6, csv, 9 ), KSCH_OK );
    BOOST_CHECK_GT( wxFileName( output ).GetSize().GetValue(), 40u );
    const char* netlist[] = { outputUtf8.c_str(), "kicad" };
    BOOST_REQUIRE_EQUAL( ksch_session_apply_document_workflow( session, 3, netlist, 2 ), KSCH_OK );
    BOOST_CHECK_GT( wxFileName( output ).GetSize().GetValue(), 100u );
    ksch_editor_state state{};
    BOOST_REQUIRE_EQUAL( ksch_session_editor_state( session, &state ), KSCH_OK );
    BOOST_CHECK_EQUAL( state.undo_count, 0u );
    BOOST_CHECK( wxRemoveFile( output ) );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_CASE( DocumentDialogsValidateAndCommitFieldsAsOneUndo )
{
    ksch_session* session = ksch_session_create();
    BOOST_REQUIRE( session );
    const auto path = eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session, path.utf8_str().data() ), KSCH_OK );
    using Fields = std::vector<std::pair<std::string,std::string>>;
    auto visitor = []( void* context, const char* name, const char* value ) {
        static_cast<Fields*>( context )->emplace_back( name, value );
    };
    auto apply = [&]( uint32_t kind, const Fields& fields ) {
        std::vector<const char*> values;
        for( const auto& field : fields ) values.push_back( field.second.c_str() );
        return ksch_session_apply_document_workflow( session, kind, values.data(), values.size() );
    };
    Fields fields;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 7, visitor, &fields ), KSCH_OK );
    BOOST_REQUIRE_GT( fields.size(), 2u );
    const auto old = fields[1].second;
    fields[1].second = "47k";
    BOOST_REQUIRE_EQUAL( apply( 7, fields ), KSCH_OK );
    Fields edited;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 7, visitor, &edited ), KSCH_OK );
    BOOST_CHECK_EQUAL( edited[1].second, "47k" );
    int undone = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session, &undone ), KSCH_OK );
    BOOST_CHECK_EQUAL( undone, 1 );
    Fields reverted;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 7, visitor, &reverted ), KSCH_OK );
    BOOST_CHECK_EQUAL( reverted[1].second, old );
    fields[0].second = "stale";
    BOOST_CHECK_EQUAL( apply( 7, fields ), KSCH_ERR_INVALID_ARG );
    Fields page;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 2, visitor, &page ), KSCH_OK );
    const auto paper = page[0].second;
    page[0].second = "invalid paper";
    BOOST_CHECK_EQUAL( apply( 2, page ), KSCH_ERR_INVALID_ARG );
    page[0].second = paper;
    page[2].second = "GPUI document settings";
    BOOST_REQUIRE_EQUAL( apply( 2, page ), KSCH_OK );
    Fields reread;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 2, visitor, &reread ), KSCH_OK );
    BOOST_CHECK_EQUAL( reread[2].second, "GPUI document settings" );
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session, &undone ), KSCH_OK );
    BOOST_CHECK_EQUAL( undone, 1 );
    Fields pageUndone;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 2, visitor, &pageUndone ), KSCH_OK );
    BOOST_CHECK_NE( pageUndone[2].second, "GPUI document settings" );
    int redone = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_redo( session, &redone ), KSCH_OK );
    BOOST_CHECK_EQUAL( redone, 1 );
    Fields pageRedone;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 2, visitor, &pageRedone ), KSCH_OK );
    BOOST_CHECK_EQUAL( pageRedone[2].second, "GPUI document settings" );
    Fields annotation;
    BOOST_REQUIRE_EQUAL( ksch_session_document_workflow( session, 0, visitor, &annotation ), KSCH_OK );
    annotation[0].second = "-1";
    BOOST_CHECK_EQUAL( apply( 0, annotation ), KSCH_ERR_INVALID_ARG );
    annotation[0].second = "10";
    annotation[3].second = "yes";
    BOOST_REQUIRE_EQUAL( apply( 0, annotation ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( ksch_session_undo( session, &undone ), KSCH_OK );
    BOOST_CHECK_EQUAL( undone, 1 );
    ksch_session_destroy( session );
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostLibrariesAndSimulation )
BOOST_AUTO_TEST_CASE( ProjectLibraryTableCrudValidatesBeforeWriting )
{
    const auto temp = std::filesystem::temp_directory_path()
            / ( "kicad_host_library_crud_" + std::to_string( ::getpid() ) );
    std::filesystem::create_directories( temp );
    const auto copy = temp / "library_test.kicad_sch";
    std::filesystem::copy_file( eeschemaFixture( "api_kitchen_sink.kicad_sch" ).ToStdString(), copy,
                                std::filesystem::copy_options::overwrite_existing );
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session( ksch_session_create(), ksch_session_destroy );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session.get(), copy.string().c_str() ), KSCH_OK );
    ksch_library_row row{ "Host_Test", "KiCad", "${KIPRJMOD}/test.kicad_sym", "", "GPUI table test", 1, 1 };
    BOOST_REQUIRE_MESSAGE( ksch_session_save_library_table( session.get(), 0, &row, 1 ) == KSCH_OK,
                           ksch_session_last_error( session.get() ) );
    std::vector<std::string> names;
    auto visitor = []( void* context, const ksch_library_row* value ) {
        static_cast<std::vector<std::string>*>( context )->push_back( value->name );
    };
    BOOST_REQUIRE_EQUAL( ksch_session_library_table( session.get(), 0, visitor, &names ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( names.size(), 1u ); BOOST_CHECK_EQUAL( names[0], "Host_Test" );
    ksch_library_row duplicates[]{ row, row };
    BOOST_CHECK_EQUAL( ksch_session_save_library_table( session.get(), 0, duplicates, 2 ), KSCH_ERR_INVALID_ARG );
    names.clear();
    BOOST_REQUIRE_EQUAL( ksch_session_library_table( session.get(), 0, visitor, &names ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( names.size(), 1u ); BOOST_CHECK_EQUAL( names[0], "Host_Test" );
    row.enabled = 0;
    BOOST_REQUIRE_EQUAL( ksch_session_save_library_table( session.get(), 0, &row, 1 ), KSCH_OK );
    const char* enabled = nullptr;
    BOOST_REQUIRE_EQUAL( ksch_session_symbol_libraries( session.get(), &enabled ), KSCH_OK );
    BOOST_CHECK( std::string( enabled ).find( "Host_Test" ) == std::string::npos );
    BOOST_REQUIRE_EQUAL( ksch_session_save_library_table( session.get(), 0, nullptr, 0 ), KSCH_OK );
    names.clear();
    BOOST_REQUIRE_EQUAL( ksch_session_library_table( session.get(), 0, visitor, &names ), KSCH_OK );
    BOOST_CHECK( names.empty() );
}
BOOST_AUTO_TEST_CASE( SimulationAnalysisRejectsInvalidCommandsWithoutStartingEngine )
{
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session( ksch_session_create(), ksch_session_destroy );
    BOOST_REQUIRE_EQUAL( ksch_session_load_file( session.get(), eeschemaFixture( "api_kitchen_sink.kicad_sch" ).utf8_str().data() ), KSCH_OK );
    const char* invalid[]{ "not an analysis", "0", "1" };
    BOOST_CHECK_EQUAL( ksch_session_apply_simulation_workflow( session.get(), 1, invalid, 3 ), KSCH_ERR_INVALID_ARG );
    std::vector<std::string> values;
    auto visitor = []( void* context, const char*, const char* value ) { static_cast<std::vector<std::string>*>( context )->push_back( value ); };
    BOOST_REQUIRE_EQUAL( ksch_session_simulation_workflow( session.get(), 1, visitor, &values ), KSCH_OK );
    BOOST_REQUIRE_EQUAL( values.size(), 3u ); BOOST_CHECK_EQUAL( values[0], ".op" );
    values.clear();
    BOOST_REQUIRE_EQUAL( ksch_session_simulation_workflow( session.get(), 2, visitor, &values ), KSCH_OK );
    BOOST_CHECK_EQUAL( values[0], "No analysis has been started" );
    BOOST_CHECK_EQUAL( ksch_session_simulation_workflow( session.get(), 0, visitor, &values ), KSCH_ERR_INVALID_ARG );
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostLibrarySymbolDialogs )
BOOST_AUTO_TEST_CASE( NewSymbolCloneRejectsOverwriteAndPinTablePersists )
{
    const auto temp=std::filesystem::temp_directory_path()/("kicad_library_symbol_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto source=std::filesystem::path(eeschemaFixture("variant_field_resolution").ToStdString());
    for(const auto* name:{"variant_field_resolution.kicad_sch","variant_field_resolution.kicad_pro","variant_test_lib.kicad_sym","sym-lib-table"})
        std::filesystem::copy_file(source/name,temp/name,std::filesystem::copy_options::overwrite_existing);
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_MESSAGE(ksch_session_load_file(session.get(),(temp/"variant_field_resolution.kicad_sch").string().c_str())==KSCH_OK,ksch_session_last_error(session.get()));
    const char* create[]{"variant_test_lib:GPUI_Clone","R","1","variant_test_lib:R_100R"};
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),4,create,4)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),4,create,4),KSCH_ERR_INVALID_ARG);
    const char* names=nullptr;
    BOOST_REQUIRE_EQUAL(ksch_session_browse_symbols(session.get(),"variant_test_lib",0,&names),KSCH_OK);
    BOOST_CHECK(std::string(names).find("variant_test_lib:GPUI_Clone")!=std::string::npos);
    BOOST_REQUIRE_EQUAL(ksch_session_run_action(session.get(),"common.Interactive.selectAll",nullptr),KSCH_OK);
    std::vector<std::string> rows;
    auto visitor=[](void* context,const char*,const char* value){static_cast<std::vector<std::string>*>(context)->push_back(value);};
    BOOST_REQUIRE_MESSAGE(ksch_session_simulation_workflow(session.get(),5,visitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_GT(rows.size(),13u);
    rows[2]="GPUI_PIN";
    std::vector<const char*> values; for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),5,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();
    BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),5,visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[2],"GPUI_PIN");
    const auto pinCount=rows.size();
    rows.insert(rows.end(),{"GPUI_NEW","ADDED","Passive","Line","Right","2.54","0","0","1.27","1.27","1","1","true",""});
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),5,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),5,visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows.size(),pinCount+14);
    size_t added=1;while(added<rows.size() && rows[added]!="GPUI_NEW")added+=14;
    BOOST_REQUIRE_LT(added,rows.size());BOOST_CHECK(!rows[added+13].empty());
    rows.erase(rows.begin()+added,rows.begin()+added+14);
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_EQUAL(ksch_session_apply_simulation_workflow(session.get(),5,values.data(),values.size()),KSCH_OK);
    rows.clear();
    BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),3,visitor,&rows),KSCH_OK);
    rows[1]="Description edited by GPUI";
    rows.insert(rows.end(),{"GPUI_CUSTOM_FIELD","persistent user value"});
    values.clear(); for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),3,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear(); BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),3,visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[1],"Description edited by GPUI");
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"GPUI_CUSTOM_FIELD")!=rows.end());
    rows.clear(); BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),8,visitor,&rows),KSCH_OK);
    BOOST_REQUIRE_GT(rows.size(),3u);
    const auto original=rows[3]; rows[3]="GPUI_BULK_VALUE";
    values.clear(); for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),8,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear(); BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),8,visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[3],"GPUI_BULK_VALUE");
    rows[1]="stale revision";
    values.clear(); for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),8,values.data(),values.size()),KSCH_ERR_INVALID_ARG);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),18,visitor,&rows),KSCH_OK);
    const size_t pins=std::stoul(rows[2]);const size_t firstMap=rows.size();rows.push_back("GPUI_Map");rows.push_back("Package_Test:Demo");
    for(size_t i=0;i<pins;++i)rows.push_back(i==0?"[2,3]":rows[3+i]);
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),18,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),18,visitor,&rows),KSCH_OK);
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"GPUI_Map")!=rows.end());
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"[2,3]")!=rows.end());
    rows[firstMap+2]="[1,";values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_CHECK(ksch_session_apply_simulation_workflow(session.get(),18,values.data(),values.size())!=KSCH_OK);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),22,visitor,&rows),KSCH_OK);
    const auto beforeMaps=rows;const size_t instancePins=std::stoul(rows[2]);rows.push_back("GPUI_InstanceMap");rows.push_back("");
    for(size_t i=0;i<instancePins;++i)rows.push_back("9");
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),22,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),22,visitor,&rows),KSCH_OK);
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"GPUI_InstanceMap")!=rows.end());
    BOOST_REQUIRE_EQUAL(ksch_session_run_action(session.get(),"common.Interactive.undo",nullptr),KSCH_OK);
    BOOST_REQUIRE_EQUAL(ksch_session_run_action(session.get(),"common.Interactive.selectAll",nullptr),KSCH_OK);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),22,visitor,&rows),KSCH_OK);
    BOOST_CHECK(rows==beforeMaps);
    rows.clear();BOOST_REQUIRE_MESSAGE(ksch_session_simulation_workflow(session.get(),19,visitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    auto modelVisitor=[](void* context,const char* name,const char* value){if(!std::string(name).starts_with("Choice "))static_cast<std::vector<std::string>*>(context)->push_back(value);};
    std::vector<std::string> selector;
    BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),20,modelVisitor,&selector),KSCH_OK);
    BOOST_REQUIRE_EQUAL(selector.size(),7u);
    selector[1]=eeschemaFixture("ibis_v4_1_series_pin_mapping.ibs").utf8_string();selector[2]="SeriesSwitchDevice";selector[3]="1";selector[4]="Input";selector[5]="false";selector[6]="";
    values.clear();for(const auto& value:selector)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),20,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),20,modelVisitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[2],"SeriesSwitchDevice");BOOST_CHECK_EQUAL(rows[3],"1");BOOST_CHECK_EQUAL(rows[4],"Input");
    rows.clear();BOOST_REQUIRE_MESSAGE(ksch_session_simulation_workflow(session.get(),19,modelVisitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_GT(rows.size(),2u);
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),19,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),0,modelVisitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[5],selector[1]);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),21,modelVisitor,&rows),KSCH_OK);BOOST_REQUIRE_EQUAL(rows.size(),1u);



}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostLibraryConnectionDialogs )
BOOST_AUTO_TEST_CASE( ConnectionSettingsSaveAndValidateWithoutConnecting )
{
    const auto temp=std::filesystem::temp_directory_path()/("kicad_library_config_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto schematic=temp/"config.kicad_sch";
    std::filesystem::copy_file(eeschemaFixture("api_kitchen_sink.kicad_sch").ToStdString(),schematic,std::filesystem::copy_options::overwrite_existing);
    const auto config=temp/"test.kicad_dbl";
    { std::ofstream output(config); output<<R"({"meta":{"version":1},"source":{"type":"odbc","dsn":"GPUI_TEST","timeout":5},"libraries":[],"cache":{"max_size":10,"max_age":30}})"; }
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),schematic.string().c_str()),KSCH_OK);
    const auto uri=config.string();
    ksch_library_row row{"TestDB","Database",uri.c_str(),"","",1,1};
    BOOST_REQUIRE_EQUAL(ksch_session_save_library_table(session.get(),0,&row,1),KSCH_OK);
    std::vector<std::string> rows;
    auto visitor=[](void* context,const char*,const char* value){static_cast<std::vector<std::string>*>(context)->push_back(value);};
    BOOST_REQUIRE_MESSAGE(ksch_session_configure_library(session.get(),0,"TestDB",visitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_EQUAL(rows.size(),9u); BOOST_CHECK_EQUAL(rows[3],"GPUI_TEST");
    rows[3]="GPUI_CHANGED";
    std::vector<const char*> values; for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),7,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear(); BOOST_REQUIRE_EQUAL(ksch_session_configure_library(session.get(),0,"TestDB",visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[3],"GPUI_CHANGED");
    rows[7]="-1";
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),7,values.data(),values.size()),KSCH_ERR_INVALID_ARG);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_configure_library(session.get(),0,"TestDB",visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[7],"10");
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostSimulationEngine )
BOOST_AUTO_TEST_CASE( OperatingPointRunsWithoutSimulatorFrame )
{
    struct BuildRuntime {
        wxString previous;
        bool present;
        BuildRuntime() : present(wxGetEnv("KICAD_RUN_FROM_BUILD_DIR",&previous)) { wxSetEnv("KICAD_RUN_FROM_BUILD_DIR","1"); }
        ~BuildRuntime() { if(present)wxSetEnv("KICAD_RUN_FROM_BUILD_DIR",previous);else wxUnsetEnv("KICAD_RUN_FROM_BUILD_DIR"); }
    } buildRuntime;

    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),eeschemaFixture("spice_netlists/rlc/rlc.kicad_sch").utf8_str().data()),KSCH_OK);
    const char* values[]{".op","240","1"};
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),1,values,3)==KSCH_OK,ksch_session_last_error(session.get()));
    std::vector<std::string> rows;
    auto visitor=[](void* context,const char*,const char* value){static_cast<std::vector<std::string>*>(context)->push_back(value);};
    for(int i=0;i<200;++i) {
        rows.clear(); BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),2,visitor,&rows),KSCH_OK);
        if(rows.size()>1 && rows[0]=="Finished")break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    BOOST_REQUIRE(!rows.empty());BOOST_CHECK_EQUAL(rows[0],"Finished");
    BOOST_CHECK_GT(rows.size(),1u);
    const char* signal[]{"gpui_signal","1 + 2"};
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),14,signal,2)==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_MESSAGE(ksch_session_simulation_workflow(session.get(),17,visitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(rows[0],"gpui_signal");
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"0,3,0")!=rows.end());
    const char* partialEdit[]{"gpui_signal","8","zz_invalid","gpui_missing_vector + 1"};
    BOOST_CHECK(ksch_session_apply_simulation_workflow(session.get(),14,partialEdit,4)!=KSCH_OK);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),17,visitor,&rows),KSCH_OK);
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"0,3,0")!=rows.end());
    const char* invalidEdit[]{"gpui_signal","gpui_missing_vector + 1"};
    BOOST_CHECK(ksch_session_apply_simulation_workflow(session.get(),14,invalidEdit,2)!=KSCH_OK);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),17,visitor,&rows),KSCH_OK);
    BOOST_CHECK(std::find(rows.begin(),rows.end(),"0,3,0")!=rows.end());
    const char* commandSubstitution[]{"bad","`echo 1`"};
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),14,commandSubstitution,2),KSCH_ERR_INVALID_ARG);
    const char* extraControl[]{".op\n.control\necho unintended\n.endc","240","1"};
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),1,extraControl,3),KSCH_ERR_INVALID_ARG);
    const char* injected[]{"bad","1; quit"};
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),14,injected,2),KSCH_ERR_INVALID_ARG);
    // The fixture contains .tran 1u 10m. The requested analysis must replace it.
    const char* transient[]{".tran 10u 1m","240","1"};
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),1,transient,3)==KSCH_OK,ksch_session_last_error(session.get()));
    for(int i=0;i<200;++i){rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),2,visitor,&rows),KSCH_OK);if(!rows.empty()&&rows[0]=="Finished")break;std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    BOOST_REQUIRE(!rows.empty());BOOST_REQUIRE_EQUAL(rows[0],"Finished");
    const char* timeVector[]{"time"};BOOST_REQUIRE_EQUAL(ksch_session_apply_simulation_workflow(session.get(),17,timeVector,1),KSCH_OK);
    std::vector<double> times;
    auto samples=[](void* context,const char* name,const char* value){if(std::string(name)=="Sample")static_cast<std::vector<double>*>(context)->push_back(std::stod(value));};
    BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),17,samples,&times),KSCH_OK);
    BOOST_REQUIRE_GT(times.size(),1u);BOOST_CHECK_SMALL(times.back()-0.001,1e-9);

}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostInheritedLibraryDialogs )
BOOST_AUTO_TEST_CASE( PinDialogTargetsRootAndRejectsDirectDerivedMutation )
{
    const auto temp=std::filesystem::temp_directory_path()/("kicad_library_derived_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto source=std::filesystem::path(eeschemaFixture("variant_field_resolution").ToStdString());
    for(const auto* name:{"variant_field_resolution.kicad_sch","variant_field_resolution.kicad_pro","variant_test_lib.kicad_sym","sym-lib-table"})
        std::filesystem::copy_file(source/name,temp/name,std::filesystem::copy_options::overwrite_existing);
    {
        const auto file=temp/"variant_test_lib.kicad_sym";
        std::ifstream input(file);std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
        const auto pos=text.find_last_of(')');BOOST_REQUIRE(pos!=std::string::npos);
        text.insert(pos,R"((symbol "GPUI_Derived" (extends "R_100R") (property "Reference" "R" (at 0 0 0) (effects (font (size 1.27 1.27)))) (property "Value" "GPUI_Derived" (at 0 0 0) (effects (font (size 1.27 1.27)))))
)");
        std::ofstream output(file);output<<text;
    }
    {
        const auto file=temp/"variant_field_resolution.kicad_sch";
        std::ifstream input(file);std::string text((std::istreambuf_iterator<char>(input)),{});input.close();
        const std::string old="R_100R",replacement="GPUI_Derived";
        for(size_t pos=0;(pos=text.find(old,pos))!=std::string::npos;pos+=replacement.size())text.replace(pos,old.size(),replacement);
        std::ofstream output(file);output<<text;
    }
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_MESSAGE(ksch_session_load_file(session.get(),(temp/"variant_field_resolution.kicad_sch").string().c_str())==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_EQUAL(ksch_session_run_action(session.get(),"common.Interactive.selectAll",nullptr),KSCH_OK);
    std::vector<std::string> rows;
    auto visitor=[](void* context,const char*,const char* value){static_cast<std::vector<std::string>*>(context)->push_back(value);};
    BOOST_REQUIRE_MESSAGE(ksch_session_simulation_workflow(session.get(),5,visitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_GT(rows.size(),13u);BOOST_CHECK_EQUAL(rows[0],"variant_test_lib:R_100R");
    const auto original=rows[2];rows[0]="variant_test_lib:GPUI_Derived";rows[2]="DO_NOT_CHANGE_ROOT";
    std::vector<const char*> values;for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),5,values.data(),values.size()),KSCH_ERR_INVALID_ARG);
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),5,visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[2],original);
    rows.clear();BOOST_REQUIRE_MESSAGE(ksch_session_simulation_workflow(session.get(),16,visitor,&rows)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_GT(rows.size(),7u);BOOST_CHECK_EQUAL(rows[0],"variant_test_lib:GPUI_Derived");
    const auto schema=rows.back();rows.back()="stale field schema";
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_CHECK_EQUAL(ksch_session_apply_simulation_workflow(session.get(),16,values.data(),values.size()),KSCH_ERR_INVALID_ARG);
    rows.back()=schema;
    rows[5]="true";
    values.clear();for(const auto& value:rows)values.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_simulation_workflow(session.get(),16,values.data(),values.size())==KSCH_OK,ksch_session_last_error(session.get()));
    rows.clear();BOOST_REQUIRE_EQUAL(ksch_session_simulation_workflow(session.get(),3,visitor,&rows),KSCH_OK);
    BOOST_CHECK_EQUAL(rows[0],"variant_test_lib:GPUI_Derived");
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostDocumentSetup )
BOOST_AUTO_TEST_CASE( BusAliasesAndNetclassesValidateAndPersistWithoutDialogs )
{
    const auto temp = std::filesystem::temp_directory_path() / ("gpui_setup_" + std::to_string(::getpid()));
    std::filesystem::create_directories( temp );
    const auto path = temp / "setup.kicad_sch";
    std::filesystem::copy_file( eeschemaFixture("api_kitchen_sink.kicad_sch").ToStdString(), path,
                               std::filesystem::copy_options::overwrite_existing );
    std::unique_ptr<ksch_session, decltype(&ksch_session_destroy)> session(ksch_session_create(), ksch_session_destroy);
    BOOST_REQUIRE_EQUAL( ksch_session_load_file(session.get(),path.string().c_str()), KSCH_OK );
    using Fields = std::vector<std::pair<std::string,std::string>>;
    auto read = [&]( uint32_t kind ) {
        Fields fields;
        auto visit=[](void* data,const char* name,const char* value) {static_cast<Fields*>(data)->emplace_back(name,value);};
        BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),kind,visit,&fields),KSCH_OK);
        return fields;
    };
    auto apply = [&](uint32_t kind, const Fields& fields) {
        std::vector<const char*> values; for(const auto& field:fields) values.push_back(field.second.c_str());
        return ksch_session_apply_document_workflow(session.get(),kind,values.data(),values.size());
    };
    auto aliases=read(13); aliases[1].second="DATA"; aliases[2].second="D0,D1,D2";
    BOOST_REQUIRE_MESSAGE(apply(13,aliases)==KSCH_OK,ksch_session_last_error(session.get()));
    aliases=read(13);
    BOOST_CHECK(std::any_of(aliases.begin(),aliases.end(),[](const auto& item){return item.first=="DATA" && item.second=="D0, D1, D2";}));
    aliases[1].second="DATA"; aliases[2].second="DATA";
    BOOST_CHECK_EQUAL(apply(13,aliases),KSCH_ERR_INVALID_ARG);
    auto classes=read(14); classes[1].second="Fast"; classes[2].second="0.25"; classes[8].second="D*";
    BOOST_REQUIRE_MESSAGE(apply(14,classes)==KSCH_OK,ksch_session_last_error(session.get()));
    classes=read(14);
    BOOST_CHECK(std::any_of(classes.begin(),classes.end(),[](const auto& item){return item.first=="Fast";}));
    classes[1].second="Fast"; classes[6].second="0.2"; classes[7].second="0.4";
    BOOST_CHECK_EQUAL(apply(14,classes),KSCH_ERR_INVALID_ARG);
    BOOST_CHECK(std::filesystem::exists(temp/"setup.kicad_pro"));
    auto readSetup = [&]() {
        Fields fields;
        auto visit=[](void* data,const char*,const char* name,const char* value,uint32_t,const char* const*,uint32_t) {
            static_cast<Fields*>(data)->emplace_back(name,value);
        };
        BOOST_REQUIRE_EQUAL(ksch_session_setup(session.get(),visit,&fields),KSCH_OK);
        return fields;
    };
    auto setupBefore=readSetup();
    auto ercIndex=std::find_if(setupBefore.begin(),setupBefore.end(),[](const auto& field){return field.first.rfind("ERC: ",0)==0;})-setupBefore.begin();
    BOOST_REQUIRE_LT(static_cast<size_t>(ercIndex),setupBefore.size());
    const auto sourceProject=temp/"import_source.kicad_pro";
    std::filesystem::copy_file(temp/"setup.kicad_pro",sourceProject,std::filesystem::copy_options::overwrite_existing);
    auto setupChanged=setupBefore;
    setupChanged[ercIndex].second=setupBefore[ercIndex].second=="Ignore"?"Error":"Ignore";
    std::vector<const char*> setupValues;for(const auto& field:setupChanged)setupValues.push_back(field.second.c_str());
    BOOST_REQUIRE_EQUAL(ksch_session_apply_setup(session.get(),setupValues.data(),setupValues.size()),KSCH_OK);
    aliases=read(13); aliases[1].second="EXTRA"; aliases[2].second="E0,E1";
    BOOST_REQUIRE_MESSAGE(apply(13,aliases)==KSCH_OK,ksch_session_last_error(session.get()));
    auto importFields=read(18); importFields[0].second=sourceProject.string();
    BOOST_REQUIRE_MESSAGE(apply(18,importFields)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(readSetup()[ercIndex].second,setupBefore[ercIndex].second);
    aliases=read(13);
    BOOST_CHECK(std::none_of(aliases.begin(),aliases.end(),[](const auto& item){return item.first=="EXTRA";}));
    auto global=read(17); global[2].second="1.5";
    BOOST_REQUIRE_MESSAGE(apply(17,global)==KSCH_OK,ksch_session_last_error(session.get()));
    int undone=0;
    BOOST_REQUIRE_EQUAL(ksch_session_undo(session.get(),&undone),KSCH_OK); BOOST_CHECK_EQUAL(undone,1);
    session.reset(ksch_session_create());
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),path.string().c_str()),KSCH_OK);
    aliases=read(13);
    BOOST_CHECK(std::any_of(aliases.begin(),aliases.end(),[](const auto& item){return item.first=="DATA";}));
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostScalarSetup )
BOOST_AUTO_TEST_CASE( ValidateWholeRequestBeforeSavingAndReloadTypedSettings )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_scalar_setup_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto path=temp/"scalar.kicad_sch";
    std::filesystem::copy_file(eeschemaFixture("api_kitchen_sink.kicad_sch").ToStdString(),path,std::filesystem::copy_options::overwrite_existing);
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),path.string().c_str()),KSCH_OK);
    using Fields=std::vector<std::pair<std::string,std::string>>;
    auto read=[&]() {
        Fields fields;
        auto visit=[](void* data,const char*,const char* name,const char* value,uint32_t,const char* const*,uint32_t) {
            static_cast<Fields*>(data)->emplace_back(name,value);
        };
        BOOST_REQUIRE_EQUAL(ksch_session_setup(session.get(),visit,&fields),KSCH_OK);
        return fields;
    };
    auto apply=[&](const Fields& fields) {
        std::vector<const char*> values; for(const auto& field:fields) values.push_back(field.second.c_str());
        return ksch_session_apply_setup(session.get(),values.data(),values.size());
    };
    const auto original=read();
    auto edited=original;
    BOOST_REQUIRE_GE(edited.size(),2u);
    edited[0].second="2"; edited[1].second="invalid";
    BOOST_CHECK_EQUAL(apply(edited),KSCH_ERR_INVALID_ARG);
    BOOST_CHECK(read()==original);
    edited[1].second=original[1].second;
    BOOST_REQUIRE_MESSAGE(apply(edited)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(read()[0].second,"2");
    BOOST_CHECK(std::filesystem::exists(temp/"scalar.kicad_pro"));
    session.reset(ksch_session_create());
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),path.string().c_str()),KSCH_OK);
    BOOST_CHECK_EQUAL(read()[0].second,"2");
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostDocumentMigration )
BOOST_AUTO_TEST_CASE( BoardImportPreviewThenApplyAndUndoUsesNativeBackannotation )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_backannotation_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto pcb=temp/"changes.kicad_pcb";
    {std::ofstream out(pcb);out<<R"PCB((kicad_pcb (version 20250101) (footprint "Resistor_SMD:R_0603_1608Metric" (property "Reference" "R1") (property "Value" "47k") (path ""))))PCB";}
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),eeschemaFixture("api_kitchen_sink.kicad_sch").utf8_str().data()),KSCH_OK);
    using Fields=std::vector<std::pair<std::string,std::string>>;
    auto read=[&](uint32_t kind){Fields values;auto visit=[](void* data,const char* name,const char* value){static_cast<Fields*>(data)->emplace_back(name,value);};
        BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),kind,visit,&values),KSCH_OK);return values;};
    auto apply=[&](uint32_t kind,const Fields& fields){std::vector<const char*> values;for(const auto& field:fields)values.push_back(field.second.c_str());return ksch_session_apply_document_workflow(session.get(),kind,values.data(),values.size());};
    auto value=[&](){const auto fields=read(7);auto found=std::find_if(fields.begin(),fields.end(),[](const auto& field){return field.first=="R1 / Value";});BOOST_REQUIRE(found!=fields.end());return found->second;};
    const auto old=value();
    auto fields=read(20);fields[1].second=pcb.string();fields[2].second="yes";fields[3].second="no";fields[7].second="no";
    BOOST_REQUIRE_MESSAGE(apply(20,fields)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(value(),old);
    BOOST_CHECK(!read(20).back().second.empty());
    fields[0].second="apply";
    BOOST_REQUIRE_MESSAGE(apply(20,fields)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(value(),"47k");
    int undone=0;BOOST_REQUIRE_EQUAL(ksch_session_undo(session.get(),&undone),KSCH_OK);BOOST_CHECK_EQUAL(undone,1);
    BOOST_CHECK_EQUAL(value(),old);
}
BOOST_AUTO_TEST_CASE( BusMigrationDetectsConflictsAndCommitsOneUndo )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_bus_migration_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    std::ifstream input(eeschemaFixture("api_kitchen_sink.kicad_sch").ToStdString());
    std::string source((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    const auto end=source.rfind(')');BOOST_REQUIRE(end!=std::string::npos);
    source.insert(end,R"SCH(
(bus (pts (xy 400 400) (xy 450 400)) (stroke (width 0) (type default)) (uuid "30000000-0000-4000-8000-000000000001"))
(label "DATA[0..3]" (at 400 400 0) (effects (font (size 1.27 1.27))) (uuid "30000000-0000-4000-8000-000000000002"))
(label "ADDR[0..7]" (at 450 400 0) (effects (font (size 1.27 1.27))) (uuid "30000000-0000-4000-8000-000000000003"))
)SCH");
    const auto path=temp/"migration.kicad_sch";{std::ofstream out(path);out<<source;}
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_MESSAGE(ksch_session_load_file(session.get(),path.string().c_str())==KSCH_OK,ksch_session_last_error(session.get()));
    auto read=[&](){std::vector<std::string> values;auto visit=[](void* data,const char*,const char* value){static_cast<std::vector<std::string>*>(data)->push_back(value);};BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),19,visit,&values),KSCH_OK);return values;};
    auto values=read();BOOST_REQUIRE_GE(values.size(),3u);
    values[2]="BUS[0..7]";
    std::vector<const char*> pointers;for(const auto& value:values)pointers.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_document_workflow(session.get(),19,pointers.data(),pointers.size())==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_LT(read().size(),values.size());
    int undone=0;BOOST_REQUIRE_EQUAL(ksch_session_undo(session.get(),&undone),KSCH_OK);BOOST_CHECK_EQUAL(undone,1);
    BOOST_CHECK_EQUAL(read().size(),values.size());
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostFieldCaseAndDataSources )
BOOST_AUTO_TEST_CASE( JoinConflictingFieldNamesThenUndo )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_field_case_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    std::ifstream input(eeschemaFixture("api_kitchen_sink.kicad_sch").ToStdString());
    std::string source((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    const auto at=source.find("(property \"Reference\" \"R1\"");BOOST_REQUIRE(at!=std::string::npos);
    source.insert(at,R"FIELDS((property "Part" "One" (at 110 40 0) (effects (font (size 1.27 1.27))))
(property "part" "Two" (at 110 42 0) (effects (font (size 1.27 1.27))))
)FIELDS");
    const auto path=temp/"fields.kicad_sch";{std::ofstream out(path);out<<source;}
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_MESSAGE(ksch_session_load_file(session.get(),path.string().c_str())==KSCH_OK,ksch_session_last_error(session.get()));
    auto read=[&](uint32_t kind){std::vector<std::string> values;auto visit=[](void* data,const char*,const char* value){static_cast<std::vector<std::string>*>(data)->push_back(value);};BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),kind,visit,&values),KSCH_OK);return values;};
    auto values=read(21);BOOST_REQUIRE_GE(values.size(),5u);values[4]="join";
    std::vector<const char*> pointers;for(const auto& value:values)pointers.push_back(value.c_str());
    BOOST_REQUIRE_MESSAGE(ksch_session_apply_document_workflow(session.get(),21,pointers.data(),pointers.size())==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_EQUAL(read(21).size(),2u);
    auto fields=read(7);BOOST_CHECK(std::find(fields.begin(),fields.end(),"One; Two")!=fields.end());
    int undone=0;BOOST_REQUIRE_EQUAL(ksch_session_undo(session.get(),&undone),KSCH_OK);BOOST_CHECK_EQUAL(undone,1);
    BOOST_CHECK_EQUAL(read(21).size(),values.size());
}
BOOST_AUTO_TEST_CASE( DataSourceManagementRejectsMissingArchiveWithoutShowingDialogs )
{
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),eeschemaFixture("api_kitchen_sink.kicad_sch").utf8_str().data()),KSCH_OK);
    std::vector<std::string> values;
    auto visit=[](void* data,const char*,const char* value){static_cast<std::vector<std::string>*>(data)->push_back(value);};
    BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),22,visit,&values),KSCH_OK);
    BOOST_REQUIRE_GE(values.size(),5u);values[0]="install";values[1]="/nonexistent/gpui-source.zip";
    std::vector<const char*> pointers;for(const auto& value:values)pointers.push_back(value.c_str());
    BOOST_CHECK_EQUAL(ksch_session_apply_document_workflow(session.get(),22,pointers.data(),pointers.size()),KSCH_ERR_INVALID_ARG);
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostDataSourceRollback )
BOOST_AUTO_TEST_CASE( InvalidReplacementArchiveRestoresInstalledFiles )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_pcm_rollback_"+std::to_string(::getpid()));
    const auto package=temp/"resources"/"org_kicad_gpui_rollback_test";
    std::filesystem::create_directories(package);
    { std::ofstream original(package/"original.txt"); original<<"preserve installed data"; }
    struct EnvironmentGuard
    {
        ENV_VAR_MAP old=Pgm().GetLocalEnvVariables();
        wxString oldRuntime;
        bool hadRuntime=wxGetEnv("KICAD_RUN_FROM_BUILD_DIR",&oldRuntime);
        wxString oldStock;
        bool hadStock=wxGetEnv("KICAD_STOCK_DATA_HOME",&oldStock);
        ~EnvironmentGuard() {
            Pgm().GetLocalEnvVariables()=old;
            if(hadRuntime)wxSetEnv("KICAD_RUN_FROM_BUILD_DIR",oldRuntime);else wxUnsetEnv("KICAD_RUN_FROM_BUILD_DIR");
            if(hadStock)wxSetEnv("KICAD_STOCK_DATA_HOME",oldStock);else wxUnsetEnv("KICAD_STOCK_DATA_HOME");
        }
    } environment;
    Pgm().GetLocalEnvVariables()[ENV_VAR::GetVersionedEnvVarName("3RD_PARTY")]=ENV_VAR_ITEM(wxString::FromUTF8(temp.string()));
    wxUnsetEnv("KICAD_RUN_FROM_BUILD_DIR");
    const auto stock=std::filesystem::path(eeschemaFixture("../../../kicad/pcm").ToStdString()).lexically_normal();
    BOOST_REQUIRE(std::filesystem::exists(stock/"schemas"/"pcm.v2.schema.json"));
    wxSetEnv("KICAD_STOCK_DATA_HOME",wxString::FromUTF8(stock.string()));
    const auto archive=temp/"bad.zip";
    {
        wxFFileOutputStream output(wxString::FromUTF8(archive.string()));
        wxZipOutputStream zip(output);
        const std::string metadata=R"({"name":"Rollback test","description":"test","description_full":"test","identifier":"org.kicad.gpui.rollback.test","type":"datasource","author":{"name":"KiCad","contact":{}},"license":"GPL-3.0","resources":{},"versions":[{"kicad_version":"7.0.0","version":"1.0.0","status":"stable"}]})";
        zip.PutNextEntry("metadata.json");zip.Write(metadata.data(),metadata.size());
        zip.PutNextEntry("resources/replacement.txt");zip.Write("partial",7);
        zip.PutNextEntry("resources/../../escape.txt");zip.Write("invalid",7);
        BOOST_REQUIRE(zip.Close());
    }
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),eeschemaFixture("api_kitchen_sink.kicad_sch").utf8_str().data()),KSCH_OK);
    std::vector<std::string> values;
    auto visit=[](void* data,const char*,const char* value){static_cast<std::vector<std::string>*>(data)->push_back(value);};
    BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),22,visit,&values),KSCH_OK);
    values[0]="install";values[1]=archive.string();values[3]="yes";
    std::vector<const char*> pointers;for(const auto& value:values)pointers.push_back(value.c_str());
    BOOST_REQUIRE_EQUAL(ksch_session_apply_document_workflow(session.get(),22,pointers.data(),pointers.size()),KSCH_ERR_INVALID_ARG);
    BOOST_CHECK_MESSAGE(std::string(ksch_session_last_error(session.get())).find("restoring previous files")!=std::string::npos,
                        ksch_session_last_error(session.get()));
    std::ifstream original(package/"original.txt");std::string contents((std::istreambuf_iterator<char>(original)),{});
    BOOST_CHECK_EQUAL(contents,"preserve installed data");
    BOOST_CHECK(!std::filesystem::exists(package/"replacement.txt"));
    BOOST_CHECK(!std::filesystem::exists(temp/"escape.txt"));
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostNetChainSettings )
BOOST_AUTO_TEST_CASE( RenameAndCreatePersistProjectChainClasses )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_chain_settings_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto path=temp/"chains.kicad_sch";
    std::filesystem::copy_file(eeschemaFixture("net_chains_four_nets_labeled.kicad_sch").ToStdString(),path,std::filesystem::copy_options::overwrite_existing);
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),path.string().c_str()),KSCH_OK);
    using Fields=std::vector<std::pair<std::string,std::string>>;
    auto read=[&](uint32_t kind){Fields fields;auto visit=[](void* data,const char* name,const char* value){static_cast<Fields*>(data)->emplace_back(name,value);};BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),kind,visit,&fields),KSCH_OK);return fields;};
    auto apply=[&](uint32_t kind,const Fields& fields){std::vector<const char*> values;for(const auto& field:fields)values.push_back(field.second.c_str());return ksch_session_apply_document_workflow(session.get(),kind,values.data(),values.size());};
    // The fixture supplies a potential chain, not a committed one. Create it
    // through the same native service before testing rename and persistence.
    auto initial=read(16);initial[0].second="SIG";
    initial[4].second="TP1";initial[5].second="1";initial[6].second="TP2";initial[7].second="1";
    for(size_t i=8;i<initial.size();++i){if(!initial[1].second.empty())initial[1].second+=",";initial[1].second+=initial[i].second;}
    BOOST_REQUIRE_MESSAGE(apply(16,initial)==KSCH_OK,ksch_session_last_error(session.get()));
    auto setup=read(15);setup[1].second="SIG";setup[2].second="Renamed";setup[3].second="HighSpeed";
    BOOST_REQUIRE_MESSAGE(apply(15,setup)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_EQUAL(ksch_session_save(session.get()),KSCH_OK);
    auto reopen=[&](){session.reset(ksch_session_create());BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),path.string().c_str()),KSCH_OK);};
    reopen();setup=read(15);
    BOOST_REQUIRE(std::any_of(setup.begin(),setup.end(),[](const auto& field){return field.first=="Renamed" && field.second.find("class: HighSpeed")!=std::string::npos;}));
    setup[0].second="delete";setup[1].second="Renamed";
    BOOST_REQUIRE_MESSAGE(apply(15,setup)==KSCH_OK,ksch_session_last_error(session.get()));
    auto create=read(16);create[0].second="Manual";create[3].second="ManualClass";
    create[4].second="TP1";create[5].second="1";create[6].second="TP2";create[7].second="1";
    for(size_t i=8;i<create.size();++i){if(!create[1].second.empty())create[1].second+=",";create[1].second+=create[i].second;}
    BOOST_REQUIRE_MESSAGE(apply(16,create)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_REQUIRE_EQUAL(ksch_session_save(session.get()),KSCH_OK);
    reopen();setup=read(15);
    BOOST_CHECK(std::any_of(setup.begin(),setup.end(),[](const auto& field){return field.first=="Manual" && field.second.find("class: ManualClass")!=std::string::npos;}));
}
BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE( SchHostSchematicImport )
BOOST_AUTO_TEST_CASE( AppendAndHierarchicalImportCommitAndUndo )
{
    const auto temp=std::filesystem::temp_directory_path()/("gpui_schematic_import_"+std::to_string(::getpid()));
    std::filesystem::create_directories(temp);
    const auto root=temp/"root.kicad_sch",source=temp/"source.kicad_sch";
    std::filesystem::copy_file(eeschemaFixture("net_chains_four_nets_labeled.kicad_sch").ToStdString(),root,std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(root,source,std::filesystem::copy_options::overwrite_existing);
    std::unique_ptr<ksch_session,decltype(&ksch_session_destroy)> session(ksch_session_create(),ksch_session_destroy);
    BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),root.string().c_str()),KSCH_OK);
    auto read=[&](uint32_t kind){std::vector<std::string> fields;auto visit=[](void* data,const char*,const char* value){static_cast<std::vector<std::string>*>(data)->push_back(value);};BOOST_REQUIRE_EQUAL(ksch_session_document_workflow(session.get(),kind,visit,&fields),KSCH_OK);return fields;};
    auto apply=[&](const std::vector<std::string>& fields){std::vector<const char*> values;for(const auto& field:fields)values.push_back(field.c_str());return ksch_session_apply_document_workflow(session.get(),23,values.data(),values.size());};
    const auto originalFields=read(7).size();
    auto fields=read(23);fields[0]="append";fields[1]=source.string();fields[3]="invalid";
    BOOST_CHECK_EQUAL(apply(fields),KSCH_ERR_INVALID_ARG);BOOST_CHECK_EQUAL(read(7).size(),originalFields);
    fields[3]="25";
    BOOST_REQUIRE_MESSAGE(apply(fields)==KSCH_OK,ksch_session_last_error(session.get()));
    BOOST_CHECK_GT(read(7).size(),originalFields);
    int undone=0;BOOST_REQUIRE_EQUAL(ksch_session_undo(session.get(),&undone),KSCH_OK);BOOST_CHECK_EQUAL(undone,1);
    BOOST_CHECK_EQUAL(read(7).size(),originalFields);
    fields[0]="sheet";fields[2]="Imported";
    BOOST_REQUIRE_MESSAGE(apply(fields)==KSCH_OK,ksch_session_last_error(session.get()));
    uint32_t count=0;BOOST_REQUIRE_EQUAL(ksch_session_sheet_count(session.get(),&count),KSCH_OK);BOOST_CHECK_EQUAL(count,2u);
    BOOST_REQUIRE_EQUAL(ksch_session_undo(session.get(),&undone),KSCH_OK);BOOST_CHECK_EQUAL(undone,1);
    BOOST_REQUIRE_EQUAL(ksch_session_sheet_count(session.get(),&count),KSCH_OK);BOOST_CHECK_EQUAL(count,1u);
    BOOST_REQUIRE_EQUAL(ksch_session_redo(session.get(),&undone),KSCH_OK);BOOST_CHECK_EQUAL(undone,1);
    BOOST_REQUIRE_EQUAL(ksch_session_save(session.get()),KSCH_OK);
    session.reset(ksch_session_create());BOOST_REQUIRE_EQUAL(ksch_session_load_file(session.get(),root.string().c_str()),KSCH_OK);
    BOOST_REQUIRE_EQUAL(ksch_session_sheet_count(session.get(),&count),KSCH_OK);BOOST_CHECK_EQUAL(count,2u);
}
BOOST_AUTO_TEST_SUITE_END()
