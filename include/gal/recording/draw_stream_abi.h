/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
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
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef KICAD_DRAW_STREAM_ABI_H
#define KICAD_DRAW_STREAM_ABI_H

/**
 * @file draw_stream_abi.h
 * @brief The C ABI shared by RECORDING_GAL and the Rust wgpu renderer.
 *
 * This header is the single source of truth for the draw stream: a flat,
 * device-independent recording of the calls SCH_PAINTER makes into KIGFX::GAL.
 * It is deliberately plain C with no dependency on wxWidgets, KiCad types or
 * the C++ standard library, so that it can be consumed unchanged by
 * `bindgen` on the Rust side and checked for layout agreement at build time.
 *
 * ## Why a stream at all
 *
 * SCH_PAINTER already encodes every rule about how a schematic looks. Rather
 * than porting those rules, RECORDING_GAL implements the GAL interface by
 * appending commands here, and the renderer turns commands into pixels. The
 * drawing logic therefore stays in C++, unmodified and still covered by its
 * own tests, while rasterisation moves to wgpu.
 *
 * ## Memory layout
 *
 * A stream is a command array plus a few side arenas. Commands are fixed-size
 * so that they can be indexed directly; anything variable-length (point runs,
 * strings, image pixels) lives in an arena and is referenced by offset. The
 * whole thing is therefore relocatable, serialisable to disk unchanged, and
 * readable from Rust with no parsing and no allocation.
 *
 * Coordinates are stored as `double` in KiCad internal units. This matters:
 * internal units are nanometres, and an A0 sheet spans over 1.2e9 of them,
 * which exceeds the 24-bit mantissa of a float. The renderer narrows to f32
 * only after subtracting the camera origin, where the range is small.
 *
 * ## Retained groups
 *
 * KIGFX::VIEW caches per-item geometry through GAL::BeginGroup/EndGroup and
 * replays it with DrawGroup. The stream preserves that structure: group bodies
 * are recorded once into the command array and listed in the group table, and
 * a frame refers to them by id. The renderer keeps one GPU buffer per group and
 * re-uploads only groups whose serial has changed, which is what makes a
 * 120 Hz pan of a large schematic affordable.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Magic number at the head of a serialised stream: "KGDS", little-endian. */
#define KGDS_MAGIC 0x5344474Bu

/**
 * ABI version. Bump on any change to the opcode list, command layout or
 * section table. The Rust side refuses to decode a version it does not know,
 * and a compile-time assertion keeps both sides in step.
 */
#define KGDS_VERSION 1u

/* ------------------------------------------------------------------ opcodes */

/**
 * Draw stream opcodes.
 *
 * Numbering is grouped and stable; append within a group, never renumber.
 * Every opcode documents how it uses the `arg` slots of ::kgds_cmd. An
 * unqualified "coords[n]" means n consecutive doubles starting at the given
 * index in the coordinate arena.
 */
typedef enum kgds_op
{
    /* -- 0x00 structural ------------------------------------------------- */

    KGDS_OP_NOP = 0x00,

    /** Begin a frame. arg0/arg1: viewport width/height in pixels. */
    KGDS_OP_BEGIN_FRAME = 0x01,

    /** End a frame. No arguments. */
    KGDS_OP_END_FRAME = 0x02,

    /** Clear the screen. arg0: packed RGBA8 clear colour. */
    KGDS_OP_CLEAR_SCREEN = 0x03,

    /**
     * Replay a cached group. arg0: group id, to be looked up in the group
     * table. Emitted by GAL::DrawGroup.
     *
     * flags may carry ::KGDS_FLAG_GROUP_COLOR, in which case arg1 is a packed
     * RGBA8 colour overriding every colour in the group, and/or
     * ::KGDS_FLAG_GROUP_DEPTH, in which case arg2 indexes one scalar giving the
     * replay depth.
     */
    KGDS_OP_DRAW_GROUP = 0x04,

    /**
     * Select the render target subsequent commands belong to.
     * arg0: ::kgds_target.
     */
    KGDS_OP_SET_TARGET = 0x05,

    /** Clear a render target. arg0: ::kgds_target. */
    KGDS_OP_CLEAR_TARGET = 0x06,

    /**
     * Mark the start/end of a layer drawn with a difference blend, used for
     * pcbnew's layer overlays. Recorded so the renderer can honour it.
     */
    KGDS_OP_START_DIFF_LAYER = 0x07,
    KGDS_OP_END_DIFF_LAYER = 0x08,
    KGDS_OP_START_NEGATIVES_LAYER = 0x09,
    KGDS_OP_END_NEGATIVES_LAYER = 0x0A,

    /* -- 0x10 render state ----------------------------------------------- */

    /** arg0: 0 or 1. */
    KGDS_OP_SET_IS_FILL = 0x10,
    /** arg0: 0 or 1. */
    KGDS_OP_SET_IS_STROKE = 0x11,
    /** arg0: packed RGBA8. */
    KGDS_OP_SET_FILL_COLOR = 0x12,
    /** arg0: packed RGBA8. */
    KGDS_OP_SET_STROKE_COLOR = 0x13,
    /** arg0: packed RGBA8. */
    KGDS_OP_SET_HOVER_COLOR = 0x14,
    /** arg0: coords[1], the line width in internal units. */
    KGDS_OP_SET_LINE_WIDTH = 0x15,
    /** arg0: coords[1]. A floor applied to stroke widths after scaling. */
    KGDS_OP_SET_MIN_LINE_WIDTH = 0x16,
    /** arg0: coords[1]. Depth is used for painter's-algorithm ordering. */
    KGDS_OP_SET_LAYER_DEPTH = 0x17,
    /** arg0: 0 or 1. */
    KGDS_OP_SET_NEGATIVE_DRAW_MODE = 0x18,
    /** arg0: 0 or 1. */
    KGDS_OP_ENABLE_DEPTH_TEST = 0x19,

    /* -- 0x20 transforms -------------------------------------------------- */

    /** arg0: coords[6], a 3x3 affine as a b c d e f (row-major, last row 0 0 1). */
    KGDS_OP_TRANSFORM = 0x20,
    /** arg0: coords[1], radians. */
    KGDS_OP_ROTATE = 0x21,
    /** arg0: coords[2]. */
    KGDS_OP_TRANSLATE = 0x22,
    /** arg0: coords[2]. */
    KGDS_OP_SCALE = 0x23,
    /** Push the transform stack. */
    KGDS_OP_SAVE = 0x24,
    /** Pop the transform stack. */
    KGDS_OP_RESTORE = 0x25,

    /* -- 0x30 geometry ---------------------------------------------------- */

    /** arg0: coords[4] = x0 y0 x1 y1. Stroked with the current line width. */
    KGDS_OP_LINE = 0x30,

    /** arg0: coords[4] = x0 y0 x1 y1. arg1: coords[1] = width. Round caps. */
    KGDS_OP_SEGMENT = 0x31,

    /** arg0: coords[2*n]. arg1: n points. arg2: coords[1] = width. */
    KGDS_OP_SEGMENT_CHAIN = 0x32,

    /**
     * arg0: coords[2*n]. arg1: n points.
     * flags: KGDS_FLAG_CLOSED if the chain is closed.
     */
    KGDS_OP_POLYLINE = 0x33,

    /**
     * arg0: coords[2*n]. arg1: n points.
     * flags: KGDS_FLAG_HOLE if this contour is a hole in the preceding
     * outline, which is how SHAPE_POLY_SET contours are flattened.
     */
    KGDS_OP_POLYGON = 0x34,

    /** arg0: coords[3] = cx cy r. */
    KGDS_OP_CIRCLE = 0x35,

    /** arg0: coords[5] = cx cy r startAngle endAngle. Angles in radians. */
    KGDS_OP_ARC = 0x36,

    /** arg0: coords[5] as ::KGDS_OP_ARC. arg1: coords[1] = width. */
    KGDS_OP_ARC_SEGMENT = 0x37,

    /** arg0: coords[4] = x0 y0 x1 y1, an axis-aligned rectangle. */
    KGDS_OP_RECTANGLE = 0x38,

    /** arg0: coords[8] = start, controlA, controlB, end. Cubic Bezier. */
    KGDS_OP_CURVE = 0x39,

    /** arg0: coords[4] = cx cy majorRadius minorRadius. arg1: coords[1] = rotation. */
    KGDS_OP_ELLIPSE = 0x3A,

    /**
     * arg0: coords[4] as ::KGDS_OP_ELLIPSE. arg1: coords[3] = rotation,
     * startAngle, endAngle. arg2: coords[1] = width.
     */
    KGDS_OP_ELLIPSE_ARC = 0x3B,

    /** arg0: coords[3] = cx cy radius. arg1: coords[1] = width. pcbnew only. */
    KGDS_OP_HOLE_WALL = 0x3C,

    /**
     * arg0: index into the image table. arg1: coords[6] = the placement
     * transform. arg2: coords[1] = alpha blend factor in [0,1].
     */
    KGDS_OP_BITMAP = 0x3D,

    /* -- 0x40 overlays ---------------------------------------------------- */

    /**
     * The construction grid. arg0: coords[6] = originX originY sizeX sizeY
     * lineWidthPx, and the grid style as a double. arg1: packed RGBA8.
     *
     * The grid is passed through as parameters rather than as the thousands of
     * lines it expands to, because a shader can draw it in one quad.
     */
    KGDS_OP_GRID = 0x40,

    /** arg0: coords[2] = cursor position. arg1: packed RGBA8. */
    KGDS_OP_CURSOR = 0x41,

    KGDS_OP_MAX
} kgds_op;

/** Bit flags in ::kgds_cmd::flags. */
enum kgds_flag
{
    /** The contour is a hole rather than an outline. */
    KGDS_FLAG_HOLE = 1u << 0,
    /** The point run forms a closed loop. */
    KGDS_FLAG_CLOSED = 1u << 1,
    /**
     * The geometry came from a glyph. The renderer may batch it differently,
     * but it is ordinary geometry and needs no font support.
     */
    KGDS_FLAG_GLYPH = 1u << 2,

    /**
     * On ::KGDS_OP_DRAW_GROUP: arg1 holds a packed RGBA8 colour that replaces
     * every stroke and fill colour inside the group for this replay.
     *
     * KIGFX::VIEW recolours a cached item in place — for selection and
     * highlighting — rather than re-recording it, so the override has to ride
     * on the replay instead of being baked into the group body. Baking it in
     * would mean rewriting recorded commands and invalidating the renderer's
     * cached buffer for that group, which is exactly what the cache exists to
     * avoid.
     */
    KGDS_FLAG_GROUP_COLOR = 1u << 3,

    /**
     * On ::KGDS_OP_DRAW_GROUP: arg2 is a coordinate index to one scalar, the
     * depth at which to replay the group. Set for the same reason as
     * ::KGDS_FLAG_GROUP_COLOR.
     */
    KGDS_FLAG_GROUP_DEPTH = 1u << 4
};

/** Matches KIGFX::RENDER_TARGET. */
typedef enum kgds_target
{
    KGDS_TARGET_CACHED = 0,
    KGDS_TARGET_NONCACHED = 1,
    KGDS_TARGET_OVERLAY = 2,
    KGDS_TARGET_TEMP = 3
} kgds_target;

/* ------------------------------------------------------------------ records */

/**
 * One recorded call. Fixed at 24 bytes so the array can be indexed directly
 * and so that commands pack densely: a cache line holds two of them with no
 * command straddling the boundary.
 *
 * The meaning of each `arg` slot is opcode-specific and documented on the
 * opcode. Slots are either an immediate value, a packed RGBA8 colour, a count,
 * or an index into the coordinate arena. No opcode needs more than three, and
 * the spare slots exist so that new opcodes do not force an ABI break.
 */
typedef struct kgds_cmd
{
    uint16_t op;    /**< A ::kgds_op. */
    uint16_t flags; /**< A bitwise-or of ::kgds_flag. */
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arg4;
} kgds_cmd;

/**
 * A cached group, as produced by GAL::BeginGroup/EndGroup.
 *
 * `serial` changes whenever the group's contents are re-recorded. The renderer
 * compares it against the serial of the GPU buffer it already holds, and skips
 * the upload when they match.
 */
typedef struct kgds_group
{
    uint32_t id;
    uint32_t serial;
    uint32_t first_cmd; /**< Index of the group body's first command in group_cmds. */
    uint32_t cmd_count;
} kgds_group;

/** Pixel format of an entry in the image table. */
typedef enum kgds_image_format
{
    KGDS_IMAGE_RGBA8 = 0
} kgds_image_format;

/** An embedded bitmap, referenced by ::KGDS_OP_BITMAP. */
typedef struct kgds_image
{
    uint32_t width;
    uint32_t height;
    uint32_t format;      /**< A ::kgds_image_format. */
    uint32_t reserved;
    uint64_t data_offset; /**< Byte offset into the image arena. */
    uint64_t data_length;
} kgds_image;

/**
 * A borrowed view of a complete stream.
 *
 * This is what crosses the FFI boundary. Every pointer is owned by the C++
 * side and stays valid until the next recording pass; the renderer reads
 * through it without copying.
 *
 * ## Two arenas, deliberately
 *
 * Retained group geometry and this frame's commands live in completely
 * separate command arrays and coordinate arenas. That separation is not an
 * implementation detail — it falls out of how KIGFX::VIEW works. VIEW caches an
 * item's geometry into a group during its update pass and replays it during its
 * draw pass, so group recording and frame recording interleave in time. Sharing
 * one arena between them would mean either relocating indices or leaking a
 * block per frame.
 *
 * With two arenas, a frame is rebuilt by clearing the frame arrays and
 * recording into them, while group data is untouched and its indices stay
 * valid forever. A pan therefore costs a few hundred bytes of frame commands
 * and re-uploads nothing.
 *
 * A command's coordinate indices refer to the arena that command lives in:
 * commands reached through a group body index into `group_coords`, and
 * commands in the frame body index into `frame_coords`.
 */
typedef struct kgds_stream_view
{
    uint32_t version; /**< ::KGDS_VERSION the producer was built against. */
    uint32_t flags;

    /** Retained geometry, one body per cached item. Indices use group_coords. */
    const kgds_cmd* group_cmds;
    size_t          group_cmd_count;

    const double* group_coords;
    size_t        group_coord_count;

    /** The group table. Each entry's first_cmd indexes group_cmds. */
    const kgds_group* groups;
    size_t            group_count;

    /** This frame's command list. Indices use frame_coords. */
    const kgds_cmd* frame_cmds;
    size_t          frame_cmd_count;

    const double* frame_coords;
    size_t        frame_coord_count;

    const uint8_t* strings;
    size_t         string_bytes;

    const kgds_image* images;
    size_t            image_count;

    const uint8_t* image_data;
    size_t         image_bytes;
} kgds_stream_view;

/* ------------------------------------------------------- packed colour help */

/** Pack four components in [0,1] into RGBA8, as stored in colour arguments. */
static inline uint32_t kgds_pack_color( double r, double g, double b, double a )
{
    /* Clamp first: COLOR4D values can exceed the unit range after blending. */
    const double cr = r < 0.0 ? 0.0 : ( r > 1.0 ? 1.0 : r );
    const double cg = g < 0.0 ? 0.0 : ( g > 1.0 ? 1.0 : g );
    const double cb = b < 0.0 ? 0.0 : ( b > 1.0 ? 1.0 : b );
    const double ca = a < 0.0 ? 0.0 : ( a > 1.0 ? 1.0 : a );

    return (uint32_t) ( cr * 255.0 + 0.5 ) | ( (uint32_t) ( cg * 255.0 + 0.5 ) << 8 )
           | ( (uint32_t) ( cb * 255.0 + 0.5 ) << 16 )
           | ( (uint32_t) ( ca * 255.0 + 0.5 ) << 24 );
}

/* ------------------------------------------------ coordinate reference map */

/**
 * One run of scalars a command refers to in the coordinate arena.
 */
typedef struct kgds_coord_ref
{
    uint32_t start; /**< Index of the first scalar. */
    uint32_t count; /**< Number of scalars. */
} kgds_coord_ref;

/** Upper bound on the number of coordinate runs any single command refers to. */
#define KGDS_MAX_COORD_REFS 3

/**
 * Report which parts of the coordinate arena a command refers to.
 *
 * Both sides of the FFI boundary need this and must agree exactly: the producer
 * uses it to relocate live geometry when compacting the arena, and the consumer
 * uses it to bounds-check a stream before trusting any index in it. Keeping the
 * single table here, in the shared header, is what stops the two from drifting.
 *
 * @param aCmd  the command to describe.
 * @param aOut  receives up to ::KGDS_MAX_COORD_REFS runs.
 * @return the number of runs written, which is 0 for commands that carry no
 *         coordinates (state changes, group references, frame markers).
 */
static inline int kgds_coord_refs( const kgds_cmd* aCmd, kgds_coord_ref* aOut )
{
    int n = 0;

#define KGDS_REF( start_, count_ )                                                                 \
    do                                                                                             \
    {                                                                                              \
        aOut[n].start = ( start_ );                                                                \
        aOut[n].count = ( count_ );                                                                \
        ++n;                                                                                       \
    } while( 0 )

    switch( (kgds_op) aCmd->op )
    {
    case KGDS_OP_SET_LINE_WIDTH:
    case KGDS_OP_SET_MIN_LINE_WIDTH:
    case KGDS_OP_SET_LAYER_DEPTH:
    case KGDS_OP_ROTATE:
        KGDS_REF( aCmd->arg0, 1 );
        break;

    case KGDS_OP_TRANSLATE:
    case KGDS_OP_SCALE:
    case KGDS_OP_CURSOR:
        KGDS_REF( aCmd->arg0, 2 );
        break;

    case KGDS_OP_DRAW_GROUP:
        /* arg0 is a group id, not a coordinate. Only the optional depth
         * override refers to the arena. */
        if( aCmd->flags & KGDS_FLAG_GROUP_DEPTH )
            KGDS_REF( aCmd->arg2, 1 );

        break;

    case KGDS_OP_TRANSFORM:
    case KGDS_OP_GRID:
        KGDS_REF( aCmd->arg0, 6 );
        break;

    case KGDS_OP_LINE:
    case KGDS_OP_RECTANGLE:
        KGDS_REF( aCmd->arg0, 4 );
        break;

    case KGDS_OP_SEGMENT:
        KGDS_REF( aCmd->arg0, 4 );
        KGDS_REF( aCmd->arg1, 1 );
        break;

    case KGDS_OP_SEGMENT_CHAIN:
        KGDS_REF( aCmd->arg0, 2u * aCmd->arg1 );
        KGDS_REF( aCmd->arg2, 1 );
        break;

    case KGDS_OP_POLYLINE:
    case KGDS_OP_POLYGON:
        KGDS_REF( aCmd->arg0, 2u * aCmd->arg1 );
        break;

    case KGDS_OP_CIRCLE:
        KGDS_REF( aCmd->arg0, 3 );
        break;

    case KGDS_OP_HOLE_WALL:
        KGDS_REF( aCmd->arg0, 3 );
        KGDS_REF( aCmd->arg1, 1 );
        break;

    case KGDS_OP_ARC:
        KGDS_REF( aCmd->arg0, 5 );
        break;

    case KGDS_OP_ARC_SEGMENT:
        KGDS_REF( aCmd->arg0, 5 );
        KGDS_REF( aCmd->arg1, 1 );
        break;

    case KGDS_OP_CURVE:
        KGDS_REF( aCmd->arg0, 8 );
        break;

    case KGDS_OP_ELLIPSE:
        KGDS_REF( aCmd->arg0, 4 );
        KGDS_REF( aCmd->arg1, 1 );
        break;

    case KGDS_OP_ELLIPSE_ARC:
        KGDS_REF( aCmd->arg0, 4 );
        KGDS_REF( aCmd->arg1, 3 );
        KGDS_REF( aCmd->arg2, 1 );
        break;

    case KGDS_OP_BITMAP:
        /* arg0 is an image table index, not a coordinate. */
        KGDS_REF( aCmd->arg1, 6 );
        KGDS_REF( aCmd->arg2, 1 );
        break;

    default:
        break;
    }

#undef KGDS_REF

    return n;
}

/* ------------------------------------------------- serialised file sections */

/**
 * Header of a stream serialised to disk.
 *
 * Golden-file tests record a stream from a real schematic and check it in, so
 * that the renderer can be developed and tested without linking any C++.
 * Sections follow the header in the order declared here, each aligned to 8
 * bytes.
 */
typedef struct kgds_file_header
{
    uint32_t magic;   /**< ::KGDS_MAGIC */
    uint32_t version; /**< ::KGDS_VERSION */
    uint32_t flags;
    uint32_t reserved;

    uint64_t group_cmd_count;
    uint64_t group_coord_count;
    uint64_t group_count;
    uint64_t frame_cmd_count;
    uint64_t frame_coord_count;
    uint64_t string_bytes;
    uint64_t image_count;
    uint64_t image_bytes;
} kgds_file_header;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* Layout agreement is load-bearing across the FFI boundary, so assert it here
 * rather than discovering a mismatch as corrupt geometry at runtime. The Rust
 * decoder carries the matching assertions. */
#if defined( __cplusplus ) && __cplusplus >= 201103L
static_assert( sizeof( kgds_cmd ) == 24, "kgds_cmd must stay 24 bytes" );
static_assert( sizeof( kgds_group ) == 16, "kgds_group must stay 16 bytes" );
static_assert( sizeof( kgds_image ) == 32, "kgds_image must stay 32 bytes" );
static_assert( sizeof( kgds_file_header ) == 80, "kgds_file_header must stay 80 bytes" );
#elif defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
_Static_assert( sizeof( kgds_cmd ) == 24, "kgds_cmd must stay 24 bytes" );
_Static_assert( sizeof( kgds_group ) == 16, "kgds_group must stay 16 bytes" );
_Static_assert( sizeof( kgds_image ) == 32, "kgds_image must stay 32 bytes" );
_Static_assert( sizeof( kgds_file_header ) == 80, "kgds_file_header must stay 80 bytes" );
#endif

#endif /* KICAD_DRAW_STREAM_ABI_H */
