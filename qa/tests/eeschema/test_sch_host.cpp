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
#include <sch_screen.h>
#include <schematic.h>
#include <wx/filename.h>

// Code under test
#include <host/sch_host.h>
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

#include <tools/sch_drawing_tools.h>
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
 * at the other — and the one uncomfortable fact this stage inherited: the tool
 * roster is registered and none of it survives, because every eeschema tool
 * declines a holder that is not a frame. That is deliberate (Stage 3) and is
 * pinned here so that the day a tool learns to run without one, this test fails
 * and says so.
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
 * The honest state of Stage 4, in one assertion.
 *
 * Every one of these classes sets `m_frame` from the tool holder and returns false
 * when the holder is not its frame type, so `TOOL_MANAGER::InitTools()` unregisters
 * and deletes all of them. Input therefore reaches the framework and stops there.
 * Making any one of them run is a decision about what `m_frame` means for a host
 * that is not a frame — see `docs/rust-migration/06-what-is-missing.md` Stage 4.
 */
BOOST_AUTO_TEST_CASE( TheToolRosterIsRegisteredAndNoneOfItSurvivesYet )
{
    SCH_HOST host;

    TOOL_MANAGER* tools = host.GetToolManager();

    BOOST_REQUIRE( tools != nullptr );

    BOOST_CHECK( tools->GetTool<SCH_SELECTION_TOOL>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_MOVE_TOOL>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_LINE_WIRE_BUS_TOOL>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_EDIT_TOOL>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_DRAWING_TOOLS>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_EDITOR_CONTROL>() == nullptr );
    BOOST_CHECK( tools->GetTool<SCH_POINT_EDITOR>() == nullptr );

    // ...and with no selection tool, the holder's selection is the empty one rather
    // than a dereference of nothing.
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
 * With no tool registered, nothing claims an event. Saying so is the point: the
 * dispatcher is wired and the tools are absent, and those are two separate facts.
 */
BOOST_AUTO_TEST_CASE( InputWithNoToolIsUnclaimedRatherThanFatal )
{
    SCH_HOST host;

    BOOST_REQUIRE( host.LoadFile( eeschemaFixture( wxT( "api_kitchen_sink.kicad_sch" ) ) ) );

    host.SetViewportSize( 800, 600 );
    host.ZoomToFit();

    HOST_INPUT_EVENT event;

    event.type = HOST_INPUT_TYPE::POINTER_MOTION;
    event.position = VECTOR2D( 100, 50 );
    BOOST_CHECK( !host.DispatchInput( event ) );

    event.type = HOST_INPUT_TYPE::POINTER_DOWN;
    event.button = BUT_LEFT;
    BOOST_CHECK( !host.DispatchInput( event ) );

    event.type = HOST_INPUT_TYPE::POINTER_UP;
    BOOST_CHECK( !host.DispatchInput( event ) );

    event.type = HOST_INPUT_TYPE::KEY_DOWN;
    event.button = BUT_NONE;
    event.keyCode = 'W';
    BOOST_CHECK( !host.DispatchInput( event ) );

    event.type = HOST_INPUT_TYPE::CANCEL;
    BOOST_CHECK( !host.DispatchInput( event ) );

    // The document is untouched by all of it.
    BOOST_CHECK( !host.IsModified() );
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
 */
BOOST_AUTO_TEST_CASE( AnUnhandledActionIsReportedRatherThanAsserted )
{
    SCH_HOST host;

    BOOST_CHECK( !host.RunActionByName( "eeschema.InteractiveDrawingLineWireBus.drawWires" ) );
    BOOST_CHECK( !host.RunActionByName( "common.Control.zoomFitScreen" ) );
    BOOST_CHECK( !host.RunActionByName( "common.InteractiveSelection" ) );
    BOOST_CHECK( !host.RunActionByName( "no.such.action" ) );
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


BOOST_AUTO_TEST_SUITE( SchHostAbi )


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

    // No tool can run on a non-frame holder yet, so nothing claims a motion event.
    BOOST_CHECK( ( flags & KSCH_INPUT_HANDLED ) == 0u );

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
 * A whole click gesture, and a key, over the ABI. Nothing claims any of it while
 * the tool roster declines a non-frame holder, and nothing edits the document —
 * which is the state this stage leaves the seam in, stated as an assertion rather
 * than as a sentence in a document.
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

    // A *registered* action with no tool behind it, which is the case that matters:
    // every action in the process is registered, so a made-up name would prove
    // nothing about whether "handled" means handled.
    for( const char* name : { "eeschema.InteractiveDrawingLineWireBus.drawWires",
                              "common.Control.zoomFitScreen", "common.InteractiveSelection" } )
    {
        BOOST_CHECK_EQUAL( ksch_session_run_action( session, name, &flags ), KSCH_OK );
        BOOST_CHECK_MESSAGE( ( flags & KSCH_INPUT_HANDLED ) == 0u,
                             std::string( name ) + " cannot have been handled: no tool ran" );
    }

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


BOOST_AUTO_TEST_SUITE_END()
