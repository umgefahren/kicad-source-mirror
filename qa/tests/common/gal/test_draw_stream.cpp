/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
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

/**
 * @file test_draw_stream.cpp
 * Tests for DRAW_STREAM, the buffer behind the recording GAL backend.
 *
 * The stream is read across an FFI boundary by a renderer written in another
 * language, so two properties matter beyond ordinary correctness:
 *
 *  - retained group data must be stable across frames, because that is what
 *    lets the renderer keep one GPU buffer per cached item and upload nothing
 *    while panning;
 *  - a stream read from disk must be validated rather than trusted, because a
 *    malformed index would otherwise become an out-of-bounds read in the
 *    consumer.
 */

#include <boost/test/unit_test.hpp>

#include <gal/recording/draw_stream.h>

#include <sstream>

using namespace KIGFX;


BOOST_AUTO_TEST_SUITE( DrawStream )


/// Record a one-command group holding a circle at a known position.
static int recordCircle( DRAW_STREAM& aStream, double aX, double aY, double aR )
{
    const int id = aStream.BeginGroup();
    const double centre[3] = { aX, aY, aR };

    aStream.Emit( KGDS_OP_CIRCLE, 0, aStream.PushCoords( centre, 3 ) );
    aStream.EndGroup();

    return id;
}


BOOST_AUTO_TEST_CASE( EmptyStreamIsWellFormed )
{
    DRAW_STREAM      stream;
    kgds_stream_view view = stream.Publish();

    BOOST_CHECK_EQUAL( view.version, KGDS_VERSION );
    BOOST_CHECK_EQUAL( view.group_count, 0 );
    BOOST_CHECK_EQUAL( view.group_cmd_count, 0 );
    BOOST_CHECK_EQUAL( view.frame_cmd_count, 0 );
}


BOOST_AUTO_TEST_CASE( GroupIdsAreUniqueAndNeverReused )
{
    DRAW_STREAM stream;

    const int a = recordCircle( stream, 0, 0, 10 );
    const int b = recordCircle( stream, 1, 1, 10 );

    BOOST_CHECK_NE( a, b );
    BOOST_CHECK( stream.HasGroup( a ) );
    BOOST_CHECK( stream.HasGroup( b ) );

    stream.DeleteGroup( a );
    BOOST_CHECK( !stream.HasGroup( a ) );

    // A renderer caches GPU buffers keyed by group id. Handing a deleted id
    // back out would make it serve another item's geometry.
    const int c = recordCircle( stream, 2, 2, 10 );
    BOOST_CHECK_NE( c, a );
    BOOST_CHECK_NE( c, b );
}


BOOST_AUTO_TEST_CASE( FrameAndGroupDataUseSeparateArenas )
{
    DRAW_STREAM stream;
    const int   group = recordCircle( stream, 5, 5, 50 );

    stream.BeginFrame( 800, 600 );
    stream.DrawGroup( group );
    stream.EndFrame();

    const kgds_stream_view view = stream.Publish();

    BOOST_CHECK_EQUAL( view.group_cmd_count, 1 );
    BOOST_CHECK_EQUAL( view.group_coord_count, 3 );

    // The frame refers to the group rather than repeating its geometry.
    BOOST_CHECK_EQUAL( view.frame_coord_count, 0 );
    BOOST_REQUIRE_EQUAL( view.frame_cmd_count, 3 );
    BOOST_CHECK_EQUAL( view.frame_cmds[0].op, KGDS_OP_BEGIN_FRAME );
    BOOST_CHECK_EQUAL( view.frame_cmds[1].op, KGDS_OP_DRAW_GROUP );
    BOOST_CHECK_EQUAL( view.frame_cmds[1].arg0, static_cast<std::uint32_t>( group ) );
    BOOST_CHECK_EQUAL( view.frame_cmds[2].op, KGDS_OP_END_FRAME );
}


BOOST_AUTO_TEST_CASE( RedrawingAnUnchangedViewCostsNothing )
{
    DRAW_STREAM stream;
    const int   a = recordCircle( stream, 0, 0, 10 );
    const int   b = recordCircle( stream, 100, 100, 20 );

    const auto drawFrame = [&]()
    {
        stream.BeginFrame( 1920, 1080 );
        stream.DrawGroup( a );
        stream.DrawGroup( b );
        stream.EndFrame();
    };

    drawFrame();

    const kgds_stream_view first = stream.Publish();
    const std::size_t      cmds = first.group_cmd_count;
    const std::size_t      coords = first.group_coord_count;

    // This is the property the 120 Hz target depends on: panning re-emits a
    // handful of frame commands and touches no retained geometry whatsoever.
    for( int i = 0; i < 200; ++i )
        drawFrame();

    const kgds_stream_view later = stream.Publish();

    BOOST_CHECK_EQUAL( later.group_cmd_count, cmds );
    BOOST_CHECK_EQUAL( later.group_coord_count, coords );
    BOOST_CHECK_EQUAL( later.frame_cmd_count, 4 );
}


BOOST_AUTO_TEST_CASE( GroupsRecordedBetweenFramesStayValid )
{
    // KIGFX::VIEW caches an item during its update pass and replays it during
    // its draw pass, so recording a group between two frames is the normal
    // case, not an edge case.
    DRAW_STREAM stream;
    const int   first = recordCircle( stream, 0, 0, 10 );

    for( int i = 0; i < 5; ++i )
    {
        const int extra = recordCircle( stream, i, i, 5 );

        stream.BeginFrame( 640, 480 );
        stream.DrawGroup( first );
        stream.DrawGroup( extra );
        stream.EndFrame();
    }

    const kgds_stream_view view = stream.Publish();

    BOOST_CHECK_EQUAL( view.group_count, 6 );

    // Every group body, and every coordinate it refers to, must still be in
    // range of the arena it belongs to.
    for( std::size_t i = 0; i < view.group_count; ++i )
    {
        const kgds_group& group = view.groups[i];

        BOOST_REQUIRE_LE( group.first_cmd + group.cmd_count, view.group_cmd_count );

        for( std::uint32_t c = 0; c < group.cmd_count; ++c )
        {
            kgds_coord_ref refs[KGDS_MAX_COORD_REFS];
            const int      n = kgds_coord_refs( &view.group_cmds[group.first_cmd + c], refs );

            for( int r = 0; r < n; ++r )
                BOOST_CHECK_LE( refs[r].start + refs[r].count, view.group_coord_count );
        }
    }
}


BOOST_AUTO_TEST_CASE( CompactionRelocatesSurvivingGeometry )
{
    DRAW_STREAM stream;
    const int   doomed = recordCircle( stream, 1, 2, 3 );
    const int   kept = recordCircle( stream, 7, 8, 9 );

    stream.DeleteGroup( doomed );
    stream.Compact();

    const kgds_stream_view view = stream.Publish();

    BOOST_REQUIRE_EQUAL( view.group_count, 1 );
    BOOST_CHECK_EQUAL( view.groups[0].id, static_cast<std::uint32_t>( kept ) );

    // The deleted group's coordinates are gone...
    BOOST_CHECK_EQUAL( view.group_coord_count, 3 );

    // ...and the survivor's command was rewritten to point at their new home.
    const kgds_cmd& cmd = view.group_cmds[view.groups[0].first_cmd];
    BOOST_REQUIRE_EQUAL( cmd.op, KGDS_OP_CIRCLE );
    BOOST_CHECK_EQUAL( view.group_coords[cmd.arg0 + 0], 7.0 );
    BOOST_CHECK_EQUAL( view.group_coords[cmd.arg0 + 1], 8.0 );
    BOOST_CHECK_EQUAL( view.group_coords[cmd.arg0 + 2], 9.0 );
}


/**
 * Compaction must relocate commands whose coordinate indices do not start at
 * arg0.
 *
 * Most opcodes keep their first coordinate run's index in arg0, but
 * KGDS_OP_BITMAP keeps an image-table index there and starts its runs at arg1,
 * and KGDS_OP_SEGMENT_CHAIN keeps a point count in arg1 with its width index in
 * arg2. A compaction that assumed "run n lives in arg n" destroyed the image
 * index and left the geometry indices pointing past the end of the arena, which
 * the consumer would have read straight off the end.
 */
BOOST_AUTO_TEST_CASE( CompactionRelocatesNonArg0Indices )
{
    DRAW_STREAM stream;

    const int doomed = recordCircle( stream, 1, 2, 3 );

    const std::uint8_t pixel[4] = { 255, 0, 0, 255 };

    // Two images, so that a clobbered index is distinguishable from a correct one.
    stream.PushImage( 1, 1, pixel, 4 );
    const std::uint32_t image = stream.PushImage( 1, 1, pixel, 4 );

    const int bitmapGroup = stream.BeginGroup();
    const double transform[6] = { 10, 11, 12, 13, 14, 15 };
    const std::uint32_t transformAt = stream.PushCoords( transform, 6 );
    const std::uint32_t alphaAt = stream.PushCoord( 0.5 );
    stream.Emit( KGDS_OP_BITMAP, 0, image, transformAt, alphaAt );
    stream.EndGroup();

    const int chainGroup = stream.BeginGroup();
    const double points[6] = { 0, 0, 1, 1, 2, 2 };
    const std::uint32_t pointsAt = stream.PushCoords( points, 6 );
    const std::uint32_t widthAt = stream.PushCoord( 7.5 );
    stream.Emit( KGDS_OP_SEGMENT_CHAIN, 0, pointsAt, 3, widthAt );
    stream.EndGroup();

    // Delete the circle to leave a hole, then reclaim it.
    stream.DeleteGroup( doomed );
    stream.Compact();

    const kgds_stream_view view = stream.Publish();

    BOOST_REQUIRE_EQUAL( view.group_count, 2 );

    const kgds_group* bitmap = nullptr;
    const kgds_group* chain = nullptr;

    for( std::size_t ii = 0; ii < view.group_count; ++ii )
    {
        if( view.groups[ii].id == static_cast<std::uint32_t>( bitmapGroup ) )
            bitmap = &view.groups[ii];
        else if( view.groups[ii].id == static_cast<std::uint32_t>( chainGroup ) )
            chain = &view.groups[ii];
    }

    BOOST_REQUIRE( bitmap && chain );

    const kgds_cmd& bitmapCmd = view.group_cmds[bitmap->first_cmd];
    BOOST_REQUIRE_EQUAL( bitmapCmd.op, KGDS_OP_BITMAP );

    // arg0 is an image-table index and must survive untouched.
    BOOST_CHECK_EQUAL( bitmapCmd.arg0, image );

    BOOST_REQUIRE_LE( bitmapCmd.arg1 + 6u, view.group_coord_count );
    BOOST_REQUIRE_LE( bitmapCmd.arg2 + 1u, view.group_coord_count );

    for( int ii = 0; ii < 6; ++ii )
        BOOST_CHECK_EQUAL( view.group_coords[bitmapCmd.arg1 + ii], 10.0 + ii );

    BOOST_CHECK_EQUAL( view.group_coords[bitmapCmd.arg2], 0.5 );

    const kgds_cmd& chainCmd = view.group_cmds[chain->first_cmd];
    BOOST_REQUIRE_EQUAL( chainCmd.op, KGDS_OP_SEGMENT_CHAIN );

    // arg1 is a point count, not an index.
    BOOST_CHECK_EQUAL( chainCmd.arg1, 3u );

    BOOST_REQUIRE_LE( chainCmd.arg0 + 6u, view.group_coord_count );
    BOOST_REQUIRE_LE( chainCmd.arg2 + 1u, view.group_coord_count );

    for( int ii = 0; ii < 6; ++ii )
        BOOST_CHECK_EQUAL( view.group_coords[chainCmd.arg0 + ii], points[ii] );

    BOOST_CHECK_EQUAL( view.group_coords[chainCmd.arg2], 7.5 );

    // And every index in the compacted stream is in range, which is the invariant
    // the consumer's bounds check relies on.
    for( std::size_t ii = 0; ii < view.group_cmd_count; ++ii )
    {
        kgds_coord_ref refs[KGDS_MAX_COORD_REFS];
        const int      n = kgds_coord_refs( &view.group_cmds[ii], refs );

        for( int r = 0; r < n; ++r )
        {
            BOOST_CHECK_LE( static_cast<std::size_t>( refs[r].start ) + refs[r].count,
                            view.group_coord_count );
        }
    }
}


BOOST_AUTO_TEST_CASE( ClearCacheDropsEverythingRetained )
{
    DRAW_STREAM stream;
    recordCircle( stream, 0, 0, 1 );
    recordCircle( stream, 1, 1, 1 );

    stream.ClearGroups();

    const kgds_stream_view view = stream.Publish();
    BOOST_CHECK_EQUAL( view.group_count, 0 );
    BOOST_CHECK_EQUAL( view.group_cmd_count, 0 );
    BOOST_CHECK_EQUAL( view.group_coord_count, 0 );
}


BOOST_AUTO_TEST_CASE( SerialisationRoundTrips )
{
    DRAW_STREAM stream;
    const int   group = recordCircle( stream, 1.5, -2.5, 12.25 );

    stream.BeginFrame( 1024, 768 );
    stream.DrawGroup( group );

    const double corners[4] = { 0.0, 0.0, 100.0, 50.0 };
    stream.Emit( KGDS_OP_RECTANGLE, 0, stream.PushCoords( corners, 4 ) );
    stream.EndFrame();

    std::stringstream buffer( std::ios::in | std::ios::out | std::ios::binary );
    BOOST_REQUIRE( stream.Serialize( buffer ) );

    DRAW_STREAM restored;
    buffer.seekg( 0 );
    BOOST_REQUIRE( restored.Deserialize( buffer ) );

    const kgds_stream_view a = stream.Publish();
    const kgds_stream_view b = restored.Publish();

    BOOST_CHECK_EQUAL( b.group_cmd_count, a.group_cmd_count );
    BOOST_CHECK_EQUAL( b.group_coord_count, a.group_coord_count );
    BOOST_CHECK_EQUAL( b.group_count, a.group_count );
    BOOST_CHECK_EQUAL( b.frame_cmd_count, a.frame_cmd_count );
    BOOST_CHECK_EQUAL( b.frame_coord_count, a.frame_coord_count );

    for( std::size_t i = 0; i < a.group_coord_count; ++i )
        BOOST_CHECK_EQUAL( b.group_coords[i], a.group_coords[i] );

    for( std::size_t i = 0; i < a.frame_coord_count; ++i )
        BOOST_CHECK_EQUAL( b.frame_coords[i], a.frame_coords[i] );

    // Ids from the file must not be handed out again by a later BeginGroup().
    const int fresh = restored.BeginGroup();
    restored.EndGroup();
    BOOST_CHECK_NE( fresh, group );
}


BOOST_AUTO_TEST_CASE( MalformedStreamsAreRejected )
{
    DRAW_STREAM stream;

    const auto readsBack = []( DRAW_STREAM& aStream, const std::string& aBytes )
    {
        std::stringstream buffer( aBytes, std::ios::in | std::ios::binary );
        return aStream.Deserialize( buffer );
    };

    const auto headerBytes = []( const kgds_file_header& aHeader )
    {
        return std::string( reinterpret_cast<const char*>( &aHeader ), sizeof( aHeader ) );
    };

    BOOST_CHECK( !readsBack( stream, std::string() ) );

    {
        kgds_file_header header = {};
        header.magic = 0xDEADBEEF;
        BOOST_CHECK( !readsBack( stream, headerBytes( header ) ) );
    }

    {
        kgds_file_header header = {};
        header.magic = KGDS_MAGIC;
        header.version = KGDS_VERSION + 1;
        BOOST_CHECK( !readsBack( stream, headerBytes( header ) ) );
    }

    {
        // A count large enough to be an allocation attack.
        kgds_file_header header = {};
        header.magic = KGDS_MAGIC;
        header.version = KGDS_VERSION;
        header.group_coord_count = ~0ull;
        BOOST_CHECK( !readsBack( stream, headerBytes( header ) ) );
    }

    {
        // A command whose coordinate index runs past the end of its arena.
        kgds_file_header header = {};
        header.magic = KGDS_MAGIC;
        header.version = KGDS_VERSION;
        header.group_cmd_count = 1;
        header.group_coord_count = 0;

        kgds_cmd cmd = {};
        cmd.op = KGDS_OP_CIRCLE;
        cmd.arg0 = 0;

        std::string bytes = headerBytes( header );
        bytes.append( reinterpret_cast<const char*>( &cmd ), sizeof( cmd ) );
        bytes.append( 8, '\0' );

        BOOST_CHECK( !readsBack( stream, bytes ) );
    }

    {
        // A group whose body runs past the end of the command array.
        kgds_file_header header = {};
        header.magic = KGDS_MAGIC;
        header.version = KGDS_VERSION;
        header.group_count = 1;
        header.group_cmd_count = 0;

        kgds_group group = {};
        group.id = 1;
        group.first_cmd = 0;
        group.cmd_count = 5;

        std::string bytes = headerBytes( header );
        bytes.append( reinterpret_cast<const char*>( &group ), sizeof( group ) );

        BOOST_CHECK( !readsBack( stream, bytes ) );
    }
}


BOOST_AUTO_TEST_CASE( ColourPackingClampsAndRoundTrips )
{
    BOOST_CHECK_EQUAL( DRAW_STREAM::PackColor( COLOR4D( 0.0, 0.0, 0.0, 0.0 ) ), 0x00000000u );
    BOOST_CHECK_EQUAL( DRAW_STREAM::PackColor( COLOR4D( 1.0, 1.0, 1.0, 1.0 ) ), 0xFFFFFFFFu );
    BOOST_CHECK_EQUAL( DRAW_STREAM::PackColor( COLOR4D( 1.0, 0.0, 0.0, 1.0 ) ), 0xFF0000FFu );

    BOOST_CHECK_EQUAL( DRAW_STREAM::PackColor( COLOR4D( 0.5, 0.25, 0.75, 1.0 ) ), 0xFFBF4080u );

    // COLOR4D asserts on out-of-range components, so the clamp inside the
    // packing routine is tested directly. It is defence in depth: the packed
    // value crosses an FFI boundary, and a component that wrapped instead of
    // clamping would arrive at the renderer as an unrelated colour.
    BOOST_CHECK_EQUAL( kgds_pack_color( 2.0, -1.0, 0.0, 1.0 ), 0xFF0000FFu );
    BOOST_CHECK_EQUAL( kgds_pack_color( 0.0, 0.0, 0.0, 5.0 ), 0xFF000000u );
}


BOOST_AUTO_TEST_SUITE_END()
