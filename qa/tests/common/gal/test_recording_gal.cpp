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
 * @file test_recording_gal.cpp
 * Tests for RECORDING_GAL, the GAL backend that records instead of rasterising.
 *
 * The backend deliberately needs no window, no GL context and no wxFrame, and
 * these tests are the standing proof of that: they construct it directly and
 * drive it through the ordinary GAL interface.
 */

#include <boost/test/unit_test.hpp>

#include <gal/recording/recording_gal.h>
#include <gal/gal_display_options.h>
#include <font/glyph.h>
#include <geometry/shape_line_chain.h>
#include <geometry/shape_poly_set.h>

using namespace KIGFX;


struct RECORDING_GAL_FIXTURE
{
    RECORDING_GAL_FIXTURE() :
            gal( options )
    {
        gal.ResizeScreen( 800, 600 );
    }

    /// Every command recorded into the current frame.
    std::vector<kgds_cmd> frameCommands() const
    {
        const kgds_stream_view view = gal.Publish();
        return std::vector<kgds_cmd>( view.frame_cmds, view.frame_cmds + view.frame_cmd_count );
    }

    /// Commands in the current frame carrying a given opcode.
    std::vector<kgds_cmd> frameCommands( kgds_op aOp ) const
    {
        std::vector<kgds_cmd> out;

        for( const kgds_cmd& cmd : frameCommands() )
        {
            if( cmd.op == aOp )
                out.push_back( cmd );
        }

        return out;
    }

    GAL_DISPLAY_OPTIONS options;
    RECORDING_GAL       gal;
};


BOOST_FIXTURE_TEST_SUITE( RecordingGal, RECORDING_GAL_FIXTURE )


BOOST_AUTO_TEST_CASE( ConstructsWithoutAWindowOrContext )
{
    // The whole approach rests on this: a GAL that can exist outside a wx
    // window is what lets the Rust host own the window instead.
    BOOST_CHECK( gal.IsInitialized() );
    BOOST_CHECK( !gal.IsOpenGlEngine() );
    BOOST_CHECK( !gal.IsCairoEngine() );
}


BOOST_AUTO_TEST_CASE( DrawingPrimitivesAreRecordedWithTheirGeometry )
{
    gal.BeginDrawing();
    gal.DrawLine( VECTOR2D( 0, 0 ), VECTOR2D( 100, 200 ) );
    gal.DrawCircle( VECTOR2D( 10, 20 ), 5 );
    gal.DrawRectangle( VECTOR2D( 1, 2 ), VECTOR2D( 3, 4 ) );
    gal.EndDrawing();

    const kgds_stream_view view = gal.Publish();

    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_LINE ).size(), 1 );
    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_CIRCLE ).size(), 1 );
    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_RECTANGLE ).size(), 1 );

    const kgds_cmd line = frameCommands( KGDS_OP_LINE ).front();
    BOOST_CHECK_EQUAL( view.frame_coords[line.arg0 + 0], 0.0 );
    BOOST_CHECK_EQUAL( view.frame_coords[line.arg0 + 1], 0.0 );
    BOOST_CHECK_EQUAL( view.frame_coords[line.arg0 + 2], 100.0 );
    BOOST_CHECK_EQUAL( view.frame_coords[line.arg0 + 3], 200.0 );

    const kgds_cmd circle = frameCommands( KGDS_OP_CIRCLE ).front();
    BOOST_CHECK_EQUAL( view.frame_coords[circle.arg0 + 0], 10.0 );
    BOOST_CHECK_EQUAL( view.frame_coords[circle.arg0 + 1], 20.0 );
    BOOST_CHECK_EQUAL( view.frame_coords[circle.arg0 + 2], 5.0 );
}


BOOST_AUTO_TEST_CASE( StateChangesAreRecordedAndAlsoAppliedToTheBaseClass )
{
    gal.BeginDrawing();
    gal.SetStrokeColor( COLOR4D( 1.0, 0.0, 0.0, 1.0 ) );
    gal.SetLineWidth( 2.5f );
    gal.SetIsFill( true );
    gal.SetIsStroke( false );
    gal.EndDrawing();

    // The base class still tracks state, because SCH_PAINTER reads it back.
    BOOST_CHECK_EQUAL( gal.GetStrokeColor(), COLOR4D( 1.0, 0.0, 0.0, 1.0 ) );
    BOOST_CHECK_EQUAL( gal.GetLineWidth(), 2.5f );
    BOOST_CHECK( gal.GetIsFill() );
    BOOST_CHECK( !gal.GetIsStroke() );

    const kgds_stream_view view = gal.Publish();

    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_SET_STROKE_COLOR ).size(), 1 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_SET_STROKE_COLOR ).front().arg0, 0xFF0000FFu );

    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_SET_LINE_WIDTH ).size(), 1 );
    const kgds_cmd width = frameCommands( KGDS_OP_SET_LINE_WIDTH ).front();
    BOOST_CHECK_EQUAL( view.frame_coords[width.arg0], 2.5 );

    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_SET_IS_FILL ).size(), 1 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_SET_IS_FILL ).front().arg0, 1u );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_SET_IS_STROKE ).front().arg0, 0u );
}


BOOST_AUTO_TEST_CASE( EveryRenderStateSetterIsRecorded )
{
    // GAL is a concrete class whose virtuals mostly have empty bodies, so a
    // backend that forgets to override one fails silently: the state changes on
    // the base class, nothing reaches the stream, and the renderer quietly draws
    // with stale state. This drives every setter a painter uses and asserts that
    // each produced its opcode, so a future omission is a test failure rather
    // than a subtle rendering bug.
    //
    // SetMinLineWidth and SetHoverColor were both missed on the first pass and
    // found only when auditing which GAL methods pcbnew calls. Hence this test.
    gal.BeginDrawing();
    gal.SetIsFill( true );
    gal.SetIsStroke( true );
    gal.SetFillColor( COLOR4D( 0.0, 0.0, 1.0, 1.0 ) );
    gal.SetStrokeColor( COLOR4D( 1.0, 0.0, 0.0, 1.0 ) );
    gal.SetHoverColor( COLOR4D( 0.0, 1.0, 0.0, 1.0 ) );
    gal.SetLineWidth( 3.0f );
    gal.SetMinLineWidth( 0.5f );
    gal.SetLayerDepth( 7.0 );
    gal.EnableDepthTest( true );
    gal.SetNegativeDrawMode( true );
    gal.EndDrawing();

    const kgds_op required[] = { KGDS_OP_SET_IS_FILL,
                                 KGDS_OP_SET_IS_STROKE,
                                 KGDS_OP_SET_FILL_COLOR,
                                 KGDS_OP_SET_STROKE_COLOR,
                                 KGDS_OP_SET_HOVER_COLOR,
                                 KGDS_OP_SET_LINE_WIDTH,
                                 KGDS_OP_SET_MIN_LINE_WIDTH,
                                 KGDS_OP_SET_LAYER_DEPTH,
                                 KGDS_OP_ENABLE_DEPTH_TEST,
                                 KGDS_OP_SET_NEGATIVE_DRAW_MODE };

    for( kgds_op op : required )
        BOOST_CHECK_MESSAGE( frameCommands( op ).size() == 1, "opcode " << op << " not recorded" );

    const kgds_stream_view view = gal.Publish();
    const kgds_cmd minWidth = frameCommands( KGDS_OP_SET_MIN_LINE_WIDTH ).front();
    BOOST_CHECK_EQUAL( view.frame_coords[minWidth.arg0], 0.5 );
}


BOOST_AUTO_TEST_CASE( LayerTargetsAndBlendModesAreRecorded )
{
    // pcbnew leans on these far harder than eeschema does: render targets are
    // switched on almost every layer, and the difference and negative blend
    // modes drive its layer overlays. Recording them is what will let the same
    // backend serve pcbnew unchanged.
    gal.BeginDrawing();
    gal.SetTarget( TARGET_CACHED );
    gal.SetTarget( TARGET_OVERLAY );
    gal.ClearTarget( TARGET_OVERLAY );
    gal.StartDiffLayer();
    gal.EndDiffLayer();
    gal.StartNegativesLayer();
    gal.EndNegativesLayer();
    gal.EndDrawing();

    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_SET_TARGET ).size(), 2 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_CLEAR_TARGET ).size(), 1 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_START_DIFF_LAYER ).size(), 1 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_END_DIFF_LAYER ).size(), 1 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_START_NEGATIVES_LAYER ).size(), 1 );
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_END_NEGATIVES_LAYER ).size(), 1 );
    BOOST_CHECK_EQUAL( gal.GetTarget(), TARGET_OVERLAY );
}


BOOST_AUTO_TEST_CASE( BitmapTextLowersToGeometryThroughTheBaseClass )
{
    // pcbnew calls BitmapText; OPENGL_GAL overrides it with a bitmap-font
    // atlas, but the base implementation resolves it through KIFONT and calls
    // back into DrawGlyph. Since this backend records glyphs as geometry, it
    // gets correct text without overriding BitmapText at all.
    gal.BeginDrawing();
    gal.SetGlyphSize( VECTOR2I( 100000, 100000 ) );
    gal.BitmapText( wxT( "R1" ), VECTOR2I( 0, 0 ), ANGLE_0 );
    gal.EndDrawing();

    const std::size_t glyphRuns =
            frameCommands( KGDS_OP_POLYLINE ).size() + frameCommands( KGDS_OP_POLYGON ).size();

    BOOST_CHECK_MESSAGE( glyphRuns > 0, "BitmapText produced no geometry" );
}


BOOST_AUTO_TEST_CASE( TransformsAreRecordedInOrder )
{
    gal.BeginDrawing();
    gal.Save();
    gal.Translate( VECTOR2D( 5, 6 ) );
    gal.Rotate( 1.5 );
    gal.Scale( VECTOR2D( 2, 3 ) );
    gal.Restore();
    gal.EndDrawing();

    const std::vector<kgds_cmd> cmds = frameCommands();
    std::vector<std::uint16_t>  ops;

    for( const kgds_cmd& cmd : cmds )
        ops.push_back( cmd.op );

    const std::vector<std::uint16_t> expected = { KGDS_OP_BEGIN_FRAME, KGDS_OP_SAVE,
                                                  KGDS_OP_TRANSLATE,   KGDS_OP_ROTATE,
                                                  KGDS_OP_SCALE,       KGDS_OP_RESTORE,
                                                  KGDS_OP_END_FRAME };

    BOOST_CHECK_EQUAL_COLLECTIONS( ops.begin(), ops.end(), expected.begin(), expected.end() );
}


BOOST_AUTO_TEST_CASE( GroupsMapOntoTheViewGeometryCache )
{
    const int group = gal.BeginGroup();
    gal.DrawCircle( VECTOR2D( 1, 2 ), 3 );
    gal.EndGroup();

    gal.BeginDrawing();
    gal.DrawGroup( group );
    gal.EndDrawing();

    const kgds_stream_view view = gal.Publish();

    BOOST_REQUIRE_EQUAL( view.group_count, 1 );
    BOOST_CHECK_EQUAL( view.groups[0].cmd_count, 1 );
    BOOST_REQUIRE_EQUAL( frameCommands( KGDS_OP_DRAW_GROUP ).size(), 1 );

    gal.DeleteGroup( group );
    BOOST_CHECK_EQUAL( gal.Publish().group_count, 0 );

    gal.ClearCache();
    BOOST_CHECK_EQUAL( gal.Publish().group_count, 0 );
}


BOOST_AUTO_TEST_CASE( RecolouringAGroupKeepsItDrawable )
{
    // VIEW recolours a cached item for selection and highlighting instead of
    // re-recording it, and it keeps using the same group id afterwards. A
    // backend that dropped the group here would make the item vanish the
    // moment it was selected.
    const int group = gal.BeginGroup();
    gal.DrawCircle( VECTOR2D( 1, 2 ), 3 );
    gal.EndGroup();

    gal.ChangeGroupColor( group, COLOR4D( 1.0, 1.0, 0.0, 1.0 ) );

    gal.BeginDrawing();
    gal.DrawGroup( group );
    gal.EndDrawing();

    BOOST_CHECK_EQUAL( gal.Publish().group_count, 1 );

    const std::vector<kgds_cmd> draws = frameCommands( KGDS_OP_DRAW_GROUP );
    BOOST_REQUIRE_EQUAL( draws.size(), 1 );
    BOOST_CHECK_EQUAL( draws[0].arg0, static_cast<std::uint32_t>( group ) );

    // The override rides on the replay rather than being baked into the group,
    // so the renderer's cached buffer for the group stays valid.
    BOOST_CHECK( draws[0].flags & KGDS_FLAG_GROUP_COLOR );
    BOOST_CHECK_EQUAL( draws[0].arg1, 0xFF00FFFFu );
}


BOOST_AUTO_TEST_CASE( GroupDepthOverrideIsCarriedOnTheReplay )
{
    const int group = gal.BeginGroup();
    gal.DrawCircle( VECTOR2D( 0, 0 ), 1 );
    gal.EndGroup();

    gal.ChangeGroupDepth( group, 42 );

    gal.BeginDrawing();
    gal.DrawGroup( group );
    gal.EndDrawing();

    const kgds_stream_view      view = gal.Publish();
    const std::vector<kgds_cmd> draws = frameCommands( KGDS_OP_DRAW_GROUP );

    BOOST_REQUIRE_EQUAL( draws.size(), 1 );
    BOOST_CHECK( draws[0].flags & KGDS_FLAG_GROUP_DEPTH );
    BOOST_REQUIRE_LT( draws[0].arg2, view.frame_coord_count );
    BOOST_CHECK_EQUAL( view.frame_coords[draws[0].arg2], 42.0 );
}


BOOST_AUTO_TEST_CASE( StrokeGlyphsLowerToPolylines )
{
    // Text never reaches the renderer as text: KIFONT has already resolved it
    // to geometry, so the recorder can flatten it and the renderer needs no
    // font handling at all.
    KIFONT::STROKE_GLYPH glyph;
    glyph.push_back( { VECTOR2D( 0, 0 ), VECTOR2D( 10, 0 ), VECTOR2D( 10, 10 ) } );
    glyph.push_back( { VECTOR2D( 20, 0 ), VECTOR2D( 30, 0 ) } );

    gal.BeginDrawing();
    gal.DrawGlyph( glyph, 0, 1 );
    gal.EndDrawing();

    const std::vector<kgds_cmd> polylines = frameCommands( KGDS_OP_POLYLINE );

    BOOST_REQUIRE_EQUAL( polylines.size(), 2 );
    BOOST_CHECK_EQUAL( polylines[0].arg1, 3u );
    BOOST_CHECK_EQUAL( polylines[1].arg1, 2u );

    // The glyph flag lets a renderer batch text separately if it wants to,
    // without needing to understand anything about fonts.
    BOOST_CHECK( polylines[0].flags & KGDS_FLAG_GLYPH );
    BOOST_CHECK( polylines[1].flags & KGDS_FLAG_GLYPH );
}


BOOST_AUTO_TEST_CASE( OutlineGlyphsLowerToPolygonsWithHoles )
{
    SHAPE_LINE_CHAIN outline;
    outline.Append( VECTOR2I( 0, 0 ) );
    outline.Append( VECTOR2I( 100, 0 ) );
    outline.Append( VECTOR2I( 100, 100 ) );
    outline.Append( VECTOR2I( 0, 100 ) );
    outline.SetClosed( true );

    SHAPE_LINE_CHAIN hole;
    hole.Append( VECTOR2I( 25, 25 ) );
    hole.Append( VECTOR2I( 75, 25 ) );
    hole.Append( VECTOR2I( 75, 75 ) );
    hole.SetClosed( true );

    SHAPE_POLY_SET poly;
    poly.AddOutline( outline );
    poly.AddHole( hole, 0 );

    KIFONT::OUTLINE_GLYPH glyph( poly );

    gal.BeginDrawing();
    gal.DrawGlyph( glyph, 0, 1 );
    gal.EndDrawing();

    const std::vector<kgds_cmd> polygons = frameCommands( KGDS_OP_POLYGON );

    BOOST_REQUIRE_EQUAL( polygons.size(), 2 );

    // The outline comes first and the hole immediately follows it, which is how
    // the renderer pairs them without needing a nesting structure.
    BOOST_CHECK( !( polygons[0].flags & KGDS_FLAG_HOLE ) );
    BOOST_CHECK( polygons[1].flags & KGDS_FLAG_HOLE );
    BOOST_CHECK( polygons[0].flags & KGDS_FLAG_GLYPH );
}


BOOST_AUTO_TEST_CASE( PolygonSetsFlattenOutlinesAndHolesInOrder )
{
    SHAPE_POLY_SET poly;

    for( int i = 0; i < 2; ++i )
    {
        SHAPE_LINE_CHAIN outline;
        outline.Append( VECTOR2I( i * 200, 0 ) );
        outline.Append( VECTOR2I( i * 200 + 100, 0 ) );
        outline.Append( VECTOR2I( i * 200 + 100, 100 ) );
        outline.SetClosed( true );
        poly.AddOutline( outline );

        SHAPE_LINE_CHAIN hole;
        hole.Append( VECTOR2I( i * 200 + 20, 20 ) );
        hole.Append( VECTOR2I( i * 200 + 60, 20 ) );
        hole.Append( VECTOR2I( i * 200 + 60, 60 ) );
        hole.SetClosed( true );
        poly.AddHole( hole, i );
    }

    gal.BeginDrawing();
    gal.DrawPolygon( poly, false );
    gal.EndDrawing();

    const std::vector<kgds_cmd> polygons = frameCommands( KGDS_OP_POLYGON );

    BOOST_REQUIRE_EQUAL( polygons.size(), 4 );
    BOOST_CHECK( !( polygons[0].flags & KGDS_FLAG_HOLE ) );
    BOOST_CHECK( polygons[1].flags & KGDS_FLAG_HOLE );
    BOOST_CHECK( !( polygons[2].flags & KGDS_FLAG_HOLE ) );
    BOOST_CHECK( polygons[3].flags & KGDS_FLAG_HOLE );
}


BOOST_AUTO_TEST_CASE( ArcSweepIsRecordedAsAbsoluteAngles )
{
    // The GAL passes a start angle and a sweep; the stream stores start and end
    // so the renderer can evaluate the arc analytically without re-deriving it.
    gal.BeginDrawing();
    gal.DrawArc( VECTOR2D( 0, 0 ), 50, EDA_ANGLE( 90.0, DEGREES_T ),
                 EDA_ANGLE( 45.0, DEGREES_T ) );
    gal.EndDrawing();

    const kgds_stream_view view = gal.Publish();
    const std::vector<kgds_cmd> arcs = frameCommands( KGDS_OP_ARC );

    BOOST_REQUIRE_EQUAL( arcs.size(), 1 );

    const double start = view.frame_coords[arcs[0].arg0 + 3];
    const double end = view.frame_coords[arcs[0].arg0 + 4];

    BOOST_CHECK_CLOSE( start, M_PI / 2.0, 1e-9 );
    BOOST_CHECK_CLOSE( end, M_PI / 2.0 + M_PI / 4.0, 1e-9 );
}


BOOST_AUTO_TEST_CASE( LineChainsCarryTheirClosedFlag )
{
    SHAPE_LINE_CHAIN open;
    open.Append( VECTOR2I( 0, 0 ) );
    open.Append( VECTOR2I( 10, 0 ) );

    SHAPE_LINE_CHAIN closed;
    closed.Append( VECTOR2I( 0, 0 ) );
    closed.Append( VECTOR2I( 10, 0 ) );
    closed.Append( VECTOR2I( 10, 10 ) );
    closed.SetClosed( true );

    gal.BeginDrawing();
    gal.DrawPolyline( open );
    gal.DrawPolyline( closed );
    gal.EndDrawing();

    const std::vector<kgds_cmd> polylines = frameCommands( KGDS_OP_POLYLINE );

    BOOST_REQUIRE_EQUAL( polylines.size(), 2 );
    BOOST_CHECK( !( polylines[0].flags & KGDS_FLAG_CLOSED ) );
    BOOST_CHECK( polylines[1].flags & KGDS_FLAG_CLOSED );
}


BOOST_AUTO_TEST_CASE( DegenerateGeometryIsDroppedRatherThanRecorded )
{
    SHAPE_LINE_CHAIN single;
    single.Append( VECTOR2I( 5, 5 ) );

    gal.BeginDrawing();
    gal.DrawPolyline( single );
    gal.DrawPolyline( std::vector<VECTOR2D>{} );
    gal.EndDrawing();

    // A one-point polyline has nothing to draw. Recording it would make the
    // renderer handle a case that cannot produce pixels.
    BOOST_CHECK_EQUAL( frameCommands( KGDS_OP_POLYLINE ).size(), 0 );
}


BOOST_AUTO_TEST_SUITE_END()
