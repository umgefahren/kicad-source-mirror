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

#ifndef KICAD_GAL_RECORDING_DRAW_STREAM_H
#define KICAD_GAL_RECORDING_DRAW_STREAM_H

#include <cstdint>
#include <iosfwd>
#include <unordered_map>
#include <vector>

#include <gal/gal.h>
#include <gal/recording/draw_stream_abi.h>
#include <gal/color4d.h>
#include <math/vector2d.h>

namespace KIGFX
{

/**
 * The buffers behind a recorded draw stream, and the only place that knows how
 * the stream is laid out.
 *
 * RECORDING_GAL translates GAL calls into calls on this class; the renderer on
 * the other side of the FFI boundary reads the result through Publish(). The
 * split exists so that the buffer mechanics — arena management, group
 * lifetimes, serialisation — can be tested without constructing a GAL.
 *
 * ## Two arenas
 *
 * Retained group geometry and the current frame's commands are kept in separate
 * command arrays and separate coordinate arenas. This is forced by how
 * KIGFX::VIEW behaves: it records an item's geometry into a group during its
 * update pass and replays the group during its draw pass, so group recording
 * and frame recording interleave over time. A single shared arena would mean
 * either relocating group indices after every frame or leaking one block per
 * frame — at 120 Hz, neither is acceptable.
 *
 * Keeping them apart makes both cases trivial. A frame is rebuilt by clearing
 * the frame arrays; group data is never touched and its indices stay valid for
 * as long as the group lives. Recording routes to one pair or the other by a
 * single rule: while a group is open, everything goes to the group arena;
 * otherwise it goes to the frame arena.
 */
class GAL_API DRAW_STREAM
{
public:
    DRAW_STREAM();

    /// Drop every recorded command, group and arena byte.
    void Clear();

    // ------------------------------------------------------------- frames

    /**
     * Open a frame, discarding the previous frame's commands and coordinates.
     *
     * @p aWidth and @p aHeight are the viewport size in pixels.
     */
    void BeginFrame( int aWidth, int aHeight );

    /// Close the frame, committing its commands into the published stream.
    void EndFrame();

    bool IsFrameOpen() const { return m_frameOpen; }

    // ------------------------------------------------------------- groups

    /**
     * Start recording a cached group and return its id.
     *
     * Ids are never reused while a group is alive, so a stale id from the
     * renderer's cache cannot alias a different group's geometry.
     */
    int BeginGroup();

    /// Finish the group opened by BeginGroup().
    void EndGroup();

    /// Emit a reference to a previously recorded group.
    void DrawGroup( int aGroupId );

    /// Forget a group. Its arena space is recovered by the next Compact().
    void DeleteGroup( int aGroupId );

    bool HasGroup( int aGroupId ) const;

    /// Drop every group, as GAL::ClearCache() does.
    void ClearGroups();

    std::size_t GroupCount() const { return m_groups.size(); }

    /**
     * Rebuild the group arenas, dropping everything no longer reachable from a
     * live group.
     *
     * Deleting a group leaves a hole, and a long editing session would
     * otherwise grow the arenas without bound. Group ids and serials survive
     * compaction; command indices do not, so every live group is left with its
     * serial intact but its recorded position rewritten.
     */
    void Compact();

    /// Fraction of the arenas that is no longer reachable, in [0,1].
    double FragmentationRatio() const;

    // ---------------------------------------------------------- recording

    /**
     * Append a command with up to five pre-encoded arguments.
     *
     * Routes to the group arena while a group is open, and to the frame arena
     * otherwise. PushCoords() and friends follow the same rule, so a command
     * and the coordinates it refers to always land in the same arena.
     */
    void Emit( kgds_op aOp, std::uint16_t aFlags, std::uint32_t aArg0 = 0,
               std::uint32_t aArg1 = 0, std::uint32_t aArg2 = 0, std::uint32_t aArg3 = 0,
               std::uint32_t aArg4 = 0 );

    /// Copy scalars into the coordinate arena and return their start index.
    std::uint32_t PushCoords( const double* aValues, std::size_t aCount );

    std::uint32_t PushCoord( double aValue ) { return PushCoords( &aValue, 1 ); }

    std::uint32_t PushPoint( const VECTOR2D& aPoint );

    /// Copy a run of points, returning the index of the first coordinate.
    std::uint32_t PushPoints( const VECTOR2D* aPoints, std::size_t aCount );

    /// Intern a string, returning its offset in the string arena.
    std::uint32_t PushString( const char* aBytes, std::size_t aLength );

    /**
     * Copy an RGBA8 image into the image arena and return its table index.
     *
     * Identical images are not deduplicated; BITMAP_BASE instances are already
     * shared by the document model, and the caller holds the mapping.
     */
    std::uint32_t PushImage( std::uint32_t aWidth, std::uint32_t aHeight, const std::uint8_t* aRgba,
                             std::size_t aByteCount );

    static std::uint32_t PackColor( const COLOR4D& aColor );

    // ------------------------------------------------------------ reading

    /**
     * A borrowed view of the stream.
     *
     * The pointers stay valid until the next mutating call. Handing this across
     * the FFI boundary copies nothing.
     */
    kgds_stream_view Publish() const;

    /// Number of commands recorded into retained group bodies.
    std::size_t GroupCommandCount() const { return m_groupCmds.size(); }

    /// Number of commands in the frame currently recorded.
    std::size_t FrameCommandCount() const { return m_frameCmds.size(); }

    /// Approximate resident size of every arena, for diagnostics.
    std::size_t MemoryUsage() const;

    // ------------------------------------------------------ serialisation

    /**
     * Write the stream in the kgds_file_header format.
     *
     * Golden-file tests record a stream from a real schematic and check the
     * result in, which is what lets the Rust renderer be developed and tested
     * without linking any of this.
     */
    bool Serialize( std::ostream& aOut ) const;

    /// Read a stream written by Serialize(). Returns false on malformed input.
    bool Deserialize( std::istream& aIn );

private:
    /// True while a group is open, meaning recording targets the group arena.
    bool recordingGroup() const { return m_openGroup >= 0; }

    /// Retained geometry, persisting across frames.
    std::vector<kgds_cmd> m_groupCmds;
    std::vector<double>   m_groupCoords;

    /// The current frame, cleared and rebuilt by every BeginFrame().
    std::vector<kgds_cmd> m_frameCmds;
    std::vector<double>   m_frameCoords;

    std::vector<std::uint8_t> m_strings;
    std::vector<kgds_group>   m_groups;
    std::vector<kgds_image>   m_images;
    std::vector<std::uint8_t> m_imageData;

    /// Group id to index in m_groups.
    std::unordered_map<int, std::size_t> m_groupIndex;

    int         m_nextGroupId;
    int         m_openGroup;        ///< -1 when no group is being recorded.
    std::size_t m_openGroupFirstCmd;

    bool m_frameOpen;

    /// Group arena entries no longer reachable from a live group.
    std::size_t m_deadCoords;
    std::size_t m_deadCmds;
};

} // namespace KIGFX

#endif // KICAD_GAL_RECORDING_DRAW_STREAM_H
