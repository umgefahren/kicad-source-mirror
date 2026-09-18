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

#include <gal/recording/draw_stream.h>

#include <algorithm>
#include <cstring>
#include <istream>
#include <limits>
#include <ostream>

namespace KIGFX
{

namespace
{
/// Sections of a serialised stream are padded to this many bytes.
constexpr std::size_t SECTION_ALIGNMENT = 8;

/// Compact() runs once this share of the arenas is unreachable.
constexpr double COMPACT_THRESHOLD = 0.4;

/// Refuse absurd counts when reading a file rather than trying to allocate them.
constexpr std::uint64_t MAX_REASONABLE_COUNT = 1ull << 32;

std::size_t alignUp( std::size_t aValue )
{
    return ( aValue + SECTION_ALIGNMENT - 1 ) & ~( SECTION_ALIGNMENT - 1 );
}

void writePadding( std::ostream& aOut, std::size_t aWritten )
{
    static const char zeros[SECTION_ALIGNMENT] = { 0 };
    const std::size_t pad = alignUp( aWritten ) - aWritten;

    if( pad )
        aOut.write( zeros, static_cast<std::streamsize>( pad ) );
}

bool skipPadding( std::istream& aIn, std::size_t aRead )
{
    const std::size_t pad = alignUp( aRead ) - aRead;

    if( pad )
        aIn.seekg( static_cast<std::streamoff>( pad ), std::ios::cur );

    return static_cast<bool>( aIn );
}


/// The argument slots that can hold a coordinate-arena index.
constexpr int COORD_ARG_SLOTS = 3;


/**
 * Work out which argument slot each of a command's coordinate runs was read from.
 *
 * kgds_coord_refs() says *where* a command's geometry lives, but not *which*
 * argument slot the index came out of, and those are not the same thing. Most
 * opcodes keep their first run's index in arg0, their second in arg1 and so on —
 * but ::KGDS_OP_BITMAP keeps an image-table index in arg0 and starts its runs at
 * arg1, ::KGDS_OP_SEGMENT_CHAIN keeps a point count in arg1 and its width index
 * in arg2, and ::KGDS_OP_DRAW_GROUP keeps a group id in arg0 and its optional
 * depth override in arg2. Compaction has to write each relocated index back into
 * the slot it came from, so it has to know which one that is.
 *
 * Duplicating the ABI's table here is exactly what the shared header exists to
 * prevent — the two copies would drift, and the symptom would be corrupt
 * geometry long after the change that caused it. So the mapping is recovered
 * from the ABI function itself: poke a value that appears nowhere else in the
 * command into one slot at a time and see which run's start follows it.
 *
 * @param aCmd      the command to describe.
 * @param aRefCount the number of runs kgds_coord_refs() reported for it.
 * @param aSlots    receives the slot index for each run, or -1 if a run's index
 *                  came from somewhere this cannot see.
 */
void coordRefSlots( const kgds_cmd& aCmd, int aRefCount, int aSlots[KGDS_MAX_COORD_REFS] )
{
    for( int r = 0; r < KGDS_MAX_COORD_REFS; ++r )
        aSlots[r] = -1;

    if( aRefCount <= 0 )
        return;

    // The probe only works if the value is distinguishable from what the command
    // already holds. Two candidates are plenty: a command has three slots, so at
    // least one of any three distinct values is unused.
    static const std::uint32_t candidates[] = { 0xFEEDFACEu, 0xDEADBEEFu, 0xCAFEBABEu };

    std::uint32_t sentinel = candidates[0];

    for( std::uint32_t candidate : candidates )
    {
        if( aCmd.arg0 != candidate && aCmd.arg1 != candidate && aCmd.arg2 != candidate )
        {
            sentinel = candidate;
            break;
        }
    }

    for( int slot = 0; slot < COORD_ARG_SLOTS; ++slot )
    {
        kgds_cmd probe = aCmd;

        std::uint32_t* const args[COORD_ARG_SLOTS] = { &probe.arg0, &probe.arg1, &probe.arg2 };
        *args[slot] = sentinel;

        kgds_coord_ref probed[KGDS_MAX_COORD_REFS];
        const int      n = kgds_coord_refs( &probe, probed );

        // Poking a slot must not change which runs exist; if it did, the slot held
        // something other than an index and the probe tells us nothing.
        if( n != aRefCount )
            continue;

        for( int r = 0; r < n; ++r )
        {
            if( probed[r].start == sentinel && aSlots[r] < 0 )
                aSlots[r] = slot;
        }
    }
}
} // namespace


DRAW_STREAM::DRAW_STREAM()
{
    Clear();
}


void DRAW_STREAM::Clear()
{
    m_groupCmds.clear();
    m_groupCoords.clear();
    m_frameCmds.clear();
    m_frameCoords.clear();
    m_strings.clear();
    m_groups.clear();
    m_images.clear();
    m_imageData.clear();
    m_groupIndex.clear();

    m_nextGroupId = 1;
    m_openGroup = -1;
    m_openGroupFirstCmd = 0;

    m_frameOpen = false;

    m_deadCoords = 0;
    m_deadCmds = 0;
}


// ---------------------------------------------------------------------- frames

void DRAW_STREAM::BeginFrame( int aWidth, int aHeight )
{
    // The frame arena is independent of the group arena, so starting a frame is
    // just a pair of clears. Nothing a group refers to can be affected, however
    // the two passes interleave.
    m_frameCmds.clear();
    m_frameCoords.clear();
    m_frameOpen = true;

    Emit( KGDS_OP_BEGIN_FRAME, 0, static_cast<std::uint32_t>( aWidth ),
          static_cast<std::uint32_t>( aHeight ) );
}


void DRAW_STREAM::EndFrame()
{
    if( !m_frameOpen )
        return;

    Emit( KGDS_OP_END_FRAME, 0 );
    m_frameOpen = false;

    if( FragmentationRatio() > COMPACT_THRESHOLD )
        Compact();
}


// ---------------------------------------------------------------------- groups

int DRAW_STREAM::BeginGroup()
{
    const int id = m_nextGroupId++;

    m_openGroup = id;
    m_openGroupFirstCmd = m_groupCmds.size();

    kgds_group group = {};
    group.id = static_cast<std::uint32_t>( id );
    group.serial = 1;
    group.first_cmd = static_cast<std::uint32_t>( m_openGroupFirstCmd );
    group.cmd_count = 0;

    m_groupIndex[id] = m_groups.size();
    m_groups.push_back( group );

    return id;
}


void DRAW_STREAM::EndGroup()
{
    if( m_openGroup < 0 )
        return;

    auto it = m_groupIndex.find( m_openGroup );

    if( it != m_groupIndex.end() )
    {
        kgds_group& group = m_groups[it->second];
        group.cmd_count = static_cast<std::uint32_t>( m_groupCmds.size() - m_openGroupFirstCmd );
    }

    m_openGroup = -1;
}


void DRAW_STREAM::DrawGroup( int aGroupId )
{
    if( !HasGroup( aGroupId ) )
        return;

    Emit( KGDS_OP_DRAW_GROUP, 0, static_cast<std::uint32_t>( aGroupId ) );
}


bool DRAW_STREAM::HasGroup( int aGroupId ) const
{
    return m_groupIndex.find( aGroupId ) != m_groupIndex.end();
}


void DRAW_STREAM::DeleteGroup( int aGroupId )
{
    auto it = m_groupIndex.find( aGroupId );

    if( it == m_groupIndex.end() )
        return;

    const std::size_t index = it->second;
    const kgds_group& group = m_groups[index];

    // Account for the space the group leaves behind so that Compact() can
    // decide when reclaiming it is worth the rebuild.
    m_deadCmds += group.cmd_count;

    for( std::uint32_t i = 0; i < group.cmd_count; ++i )
    {
        const kgds_cmd& cmd = m_groupCmds[group.first_cmd + i];
        kgds_coord_ref  refs[KGDS_MAX_COORD_REFS];
        const int       n = kgds_coord_refs( &cmd, refs );

        for( int r = 0; r < n; ++r )
            m_deadCoords += refs[r].count;
    }

    // Swap-remove keeps the group table dense; the index map is patched for the
    // element that moved. Ids are never reused, so a stale id in the renderer's
    // cache simply misses.
    const std::size_t last = m_groups.size() - 1;

    if( index != last )
    {
        m_groups[index] = m_groups[last];
        m_groupIndex[static_cast<int>( m_groups[index].id )] = index;
    }

    m_groups.pop_back();
    m_groupIndex.erase( it );

    if( m_openGroup == aGroupId )
        m_openGroup = -1;
}


void DRAW_STREAM::ClearGroups()
{
    m_groups.clear();
    m_groupIndex.clear();
    m_openGroup = -1;

    m_groupCmds.clear();
    m_groupCoords.clear();
    m_deadCmds = 0;
    m_deadCoords = 0;
}


double DRAW_STREAM::FragmentationRatio() const
{
    const std::size_t live = m_groupCmds.size() + m_groupCoords.size();

    if( live == 0 )
        return 0.0;

    return static_cast<double>( m_deadCmds + m_deadCoords ) / static_cast<double>( live );
}


void DRAW_STREAM::Compact()
{
    // Compaction rewrites command indices, so it cannot run with a group or a
    // frame half-recorded.
    if( m_openGroup >= 0 )
        return;

    std::vector<kgds_cmd> cmds;
    std::vector<double>   coords;
    cmds.reserve( m_groupCmds.size() - std::min( m_deadCmds, m_groupCmds.size() ) );
    coords.reserve( m_groupCoords.size() - std::min( m_deadCoords, m_groupCoords.size() ) );

    for( kgds_group& group : m_groups )
    {
        const std::uint32_t oldFirst = group.first_cmd;
        group.first_cmd = static_cast<std::uint32_t>( cmds.size() );

        for( std::uint32_t i = 0; i < group.cmd_count; ++i )
        {
            kgds_cmd cmd = m_groupCmds[oldFirst + i];

            // Relocate the geometry this command points at, then rewrite the
            // command's arguments to match. The argument slots that hold
            // coordinate indices are described once, in the shared ABI header,
            // so producer and consumer cannot disagree about them.
            kgds_coord_ref refs[KGDS_MAX_COORD_REFS];
            const int      n = kgds_coord_refs( &cmd, refs );

            int slots[KGDS_MAX_COORD_REFS];
            coordRefSlots( cmd, n, slots );

            std::uint32_t* const args[COORD_ARG_SLOTS] = { &cmd.arg0, &cmd.arg1, &cmd.arg2 };

            for( int r = 0; r < n; ++r )
            {
                const std::uint32_t moved = static_cast<std::uint32_t>( coords.size() );

                if( refs[r].count
                    && static_cast<std::size_t>( refs[r].start ) + refs[r].count
                               <= m_groupCoords.size() )
                {
                    coords.insert( coords.end(), m_groupCoords.begin() + refs[r].start,
                                   m_groupCoords.begin() + refs[r].start + refs[r].count );
                }
                else
                {
                    // A run that does not fit the arena means the stream was already
                    // damaged. Reserve the space anyway so that the compacted result
                    // still satisfies "every index is in range", which is the
                    // invariant the consumer's bounds check relies on. Dropping the
                    // run instead would leave an index pointing past the end.
                    coords.resize( coords.size() + refs[r].count );
                }

                // Write the new index back into the slot the ABI read it from. That
                // is usually arg0, arg1, arg2 in order -- but not always, and
                // assuming so silently corrupts the commands where it does not hold.
                if( slots[r] >= 0 )
                    *args[slots[r]] = moved;
            }

            cmds.push_back( cmd );
        }
    }

    m_groupCmds.swap( cmds );
    m_groupCoords.swap( coords );
    m_deadCmds = 0;
    m_deadCoords = 0;
}


// ------------------------------------------------------------------- recording

void DRAW_STREAM::Emit( kgds_op aOp, std::uint16_t aFlags, std::uint32_t aArg0,
                        std::uint32_t aArg1, std::uint32_t aArg2, std::uint32_t aArg3,
                        std::uint32_t aArg4 )
{
    kgds_cmd cmd;
    cmd.op = static_cast<std::uint16_t>( aOp );
    cmd.flags = aFlags;
    cmd.arg0 = aArg0;
    cmd.arg1 = aArg1;
    cmd.arg2 = aArg2;
    cmd.arg3 = aArg3;
    cmd.arg4 = aArg4;

    // A command and the coordinates it refers to must land in the same arena,
    // so both follow the same rule: the group arena while a group is open, the
    // frame arena otherwise.
    if( recordingGroup() )
        m_groupCmds.push_back( cmd );
    else
        m_frameCmds.push_back( cmd );
}


std::uint32_t DRAW_STREAM::PushCoords( const double* aValues, std::size_t aCount )
{
    std::vector<double>& arena = recordingGroup() ? m_groupCoords : m_frameCoords;
    const std::uint32_t  start = static_cast<std::uint32_t>( arena.size() );

    arena.insert( arena.end(), aValues, aValues + aCount );

    return start;
}


std::uint32_t DRAW_STREAM::PushPoint( const VECTOR2D& aPoint )
{
    const double xy[2] = { aPoint.x, aPoint.y };

    return PushCoords( xy, 2 );
}


std::uint32_t DRAW_STREAM::PushPoints( const VECTOR2D* aPoints, std::size_t aCount )
{
    std::vector<double>& arena = recordingGroup() ? m_groupCoords : m_frameCoords;
    const std::uint32_t  start = static_cast<std::uint32_t>( arena.size() );

    arena.resize( arena.size() + aCount * 2 );

    for( std::size_t i = 0; i < aCount; ++i )
    {
        arena[start + i * 2] = aPoints[i].x;
        arena[start + i * 2 + 1] = aPoints[i].y;
    }

    return start;
}


std::uint32_t DRAW_STREAM::PushString( const char* aBytes, std::size_t aLength )
{
    const std::uint32_t start = static_cast<std::uint32_t>( m_strings.size() );

    m_strings.insert( m_strings.end(), aBytes, aBytes + aLength );
    m_strings.push_back( 0 );

    return start;
}


std::uint32_t DRAW_STREAM::PushImage( std::uint32_t aWidth, std::uint32_t aHeight,
                                      const std::uint8_t* aRgba, std::size_t aByteCount )
{
    kgds_image image = {};
    image.width = aWidth;
    image.height = aHeight;
    image.format = KGDS_IMAGE_RGBA8;
    image.data_offset = m_imageData.size();
    image.data_length = aByteCount;

    m_imageData.insert( m_imageData.end(), aRgba, aRgba + aByteCount );

    const std::uint32_t index = static_cast<std::uint32_t>( m_images.size() );
    m_images.push_back( image );

    return index;
}


std::uint32_t DRAW_STREAM::PackColor( const COLOR4D& aColor )
{
    return kgds_pack_color( aColor.r, aColor.g, aColor.b, aColor.a );
}


// --------------------------------------------------------------------- reading

kgds_stream_view DRAW_STREAM::Publish() const
{
    kgds_stream_view view = {};

    view.version = KGDS_VERSION;
    view.flags = 0;

    view.group_cmds = m_groupCmds.empty() ? nullptr : m_groupCmds.data();
    view.group_cmd_count = m_groupCmds.size();

    view.group_coords = m_groupCoords.empty() ? nullptr : m_groupCoords.data();
    view.group_coord_count = m_groupCoords.size();

    view.groups = m_groups.empty() ? nullptr : m_groups.data();
    view.group_count = m_groups.size();

    view.frame_cmds = m_frameCmds.empty() ? nullptr : m_frameCmds.data();
    view.frame_cmd_count = m_frameCmds.size();

    view.frame_coords = m_frameCoords.empty() ? nullptr : m_frameCoords.data();
    view.frame_coord_count = m_frameCoords.size();

    view.strings = m_strings.empty() ? nullptr : m_strings.data();
    view.string_bytes = m_strings.size();

    view.images = m_images.empty() ? nullptr : m_images.data();
    view.image_count = m_images.size();

    view.image_data = m_imageData.empty() ? nullptr : m_imageData.data();
    view.image_bytes = m_imageData.size();

    return view;
}


std::size_t DRAW_STREAM::MemoryUsage() const
{
    return m_groupCmds.capacity() * sizeof( kgds_cmd )
           + m_frameCmds.capacity() * sizeof( kgds_cmd )
           + m_groupCoords.capacity() * sizeof( double )
           + m_frameCoords.capacity() * sizeof( double ) + m_strings.capacity()
           + m_groups.capacity() * sizeof( kgds_group ) + m_images.capacity() * sizeof( kgds_image )
           + m_imageData.capacity();
}


// --------------------------------------------------------------- serialisation

bool DRAW_STREAM::Serialize( std::ostream& aOut ) const
{
    kgds_file_header header = {};
    header.magic = KGDS_MAGIC;
    header.version = KGDS_VERSION;
    header.flags = 0;
    header.group_cmd_count = m_groupCmds.size();
    header.group_coord_count = m_groupCoords.size();
    header.group_count = m_groups.size();
    header.frame_cmd_count = m_frameCmds.size();
    header.frame_coord_count = m_frameCoords.size();
    header.string_bytes = m_strings.size();
    header.image_count = m_images.size();
    header.image_bytes = m_imageData.size();

    aOut.write( reinterpret_cast<const char*>( &header ), sizeof( header ) );

    const auto section = [&]( const void* aData, std::size_t aBytes )
    {
        if( aBytes )
            aOut.write( static_cast<const char*>( aData ), static_cast<std::streamsize>( aBytes ) );

        writePadding( aOut, aBytes );
    };

    section( m_groupCmds.data(), m_groupCmds.size() * sizeof( kgds_cmd ) );
    section( m_groupCoords.data(), m_groupCoords.size() * sizeof( double ) );
    section( m_groups.data(), m_groups.size() * sizeof( kgds_group ) );
    section( m_frameCmds.data(), m_frameCmds.size() * sizeof( kgds_cmd ) );
    section( m_frameCoords.data(), m_frameCoords.size() * sizeof( double ) );
    section( m_strings.data(), m_strings.size() );
    section( m_images.data(), m_images.size() * sizeof( kgds_image ) );
    section( m_imageData.data(), m_imageData.size() );

    return static_cast<bool>( aOut );
}


bool DRAW_STREAM::Deserialize( std::istream& aIn )
{
    kgds_file_header header = {};

    aIn.read( reinterpret_cast<char*>( &header ), sizeof( header ) );

    if( !aIn || header.magic != KGDS_MAGIC || header.version != KGDS_VERSION )
        return false;

    // The file may be untrusted, so refuse implausible sizes before they become
    // allocation requests.
    const std::uint64_t counts[] = { header.group_cmd_count,   header.group_coord_count,
                                     header.group_count,       header.frame_cmd_count,
                                     header.frame_coord_count, header.string_bytes,
                                     header.image_count,       header.image_bytes };

    for( std::uint64_t count : counts )
    {
        if( count > MAX_REASONABLE_COUNT )
            return false;
    }

    Clear();

    const auto section = [&]( void* aData, std::size_t aBytes ) -> bool
    {
        if( aBytes )
            aIn.read( static_cast<char*>( aData ), static_cast<std::streamsize>( aBytes ) );

        return static_cast<bool>( aIn ) && skipPadding( aIn, aBytes );
    };

    m_groupCmds.resize( static_cast<std::size_t>( header.group_cmd_count ) );
    m_groupCoords.resize( static_cast<std::size_t>( header.group_coord_count ) );
    m_groups.resize( static_cast<std::size_t>( header.group_count ) );
    m_frameCmds.resize( static_cast<std::size_t>( header.frame_cmd_count ) );
    m_frameCoords.resize( static_cast<std::size_t>( header.frame_coord_count ) );
    m_strings.resize( static_cast<std::size_t>( header.string_bytes ) );
    m_images.resize( static_cast<std::size_t>( header.image_count ) );
    m_imageData.resize( static_cast<std::size_t>( header.image_bytes ) );

    if( !section( m_groupCmds.data(), m_groupCmds.size() * sizeof( kgds_cmd ) )
        || !section( m_groupCoords.data(), m_groupCoords.size() * sizeof( double ) )
        || !section( m_groups.data(), m_groups.size() * sizeof( kgds_group ) )
        || !section( m_frameCmds.data(), m_frameCmds.size() * sizeof( kgds_cmd ) )
        || !section( m_frameCoords.data(), m_frameCoords.size() * sizeof( double ) )
        || !section( m_strings.data(), m_strings.size() )
        || !section( m_images.data(), m_images.size() * sizeof( kgds_image ) )
        || !section( m_imageData.data(), m_imageData.size() ) )
    {
        Clear();
        return false;
    }

    // Rebuild the id map, and make sure a later BeginGroup() cannot collide
    // with an id that came out of the file: a renderer keyed on (id, serial)
    // would otherwise serve stale geometry for a reused id.
    for( std::size_t i = 0; i < m_groups.size(); ++i )
    {
        const int id = static_cast<int>( m_groups[i].id );

        if( static_cast<std::uint64_t>( m_groups[i].first_cmd ) + m_groups[i].cmd_count
            > m_groupCmds.size() )
        {
            Clear();
            return false;
        }

        m_groupIndex[id] = i;
        m_nextGroupId = std::max( m_nextGroupId, id + 1 );
    }

    // An image's pixels must lie inside the image arena. A consumer uploads
    // image_data[data_offset .. data_offset + data_length] to a texture without
    // looking further, so an out-of-range extent here is an out-of-bounds read
    // there.
    for( const kgds_image& image : m_images )
    {
        if( image.data_offset + image.data_length > m_imageData.size()
            || image.data_offset > m_imageData.size() )
        {
            Clear();
            return false;
        }
    }

    // Every coordinate index in the file must be in range before any consumer
    // is allowed to dereference it.
    const auto checkArena = [&]( const std::vector<kgds_cmd>& aCmds,
                                 std::size_t                  aCoordCount ) -> bool
    {
        for( const kgds_cmd& cmd : aCmds )
        {
            if( cmd.op >= KGDS_OP_MAX )
                return false;

            // KGDS_OP_BITMAP's arg0 is an image-table index rather than a
            // coordinate, so kgds_coord_refs() does not cover it.
            if( cmd.op == KGDS_OP_BITMAP && cmd.arg0 >= m_images.size() )
                return false;

            kgds_coord_ref refs[KGDS_MAX_COORD_REFS];
            const int      n = kgds_coord_refs( &cmd, refs );

            for( int r = 0; r < n; ++r )
            {
                if( static_cast<std::uint64_t>( refs[r].start ) + refs[r].count > aCoordCount )
                    return false;
            }
        }

        return true;
    };

    if( !checkArena( m_groupCmds, m_groupCoords.size() )
        || !checkArena( m_frameCmds, m_frameCoords.size() ) )
    {
        Clear();
        return false;
    }

    return true;
}

} // namespace KIGFX
