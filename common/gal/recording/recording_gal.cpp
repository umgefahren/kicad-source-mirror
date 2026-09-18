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

#include <gal/recording/recording_gal.h>

#include <bitmap_base.h>
#include <font/glyph.h>
#include <geometry/shape_line_chain.h>
#include <geometry/shape_poly_set.h>

#include <wx/image.h>

namespace KIGFX
{

RECORDING_GAL::RECORDING_GAL( GAL_DISPLAY_OPTIONS& aDisplayOptions ) :
        GAL( aDisplayOptions ),
        m_currentTarget( TARGET_NONCACHED )
{
}


RECORDING_GAL::~RECORDING_GAL() = default;


// ------------------------------------------------------------------- helpers

void RECORDING_GAL::emitPointRun( kgds_op aOp, std::uint16_t aFlags, const VECTOR2D* aPoints,
                                  std::size_t aCount )
{
    if( aCount < 2 )
        return;

    const std::uint32_t start = m_stream.PushPoints( aPoints, aCount );

    m_stream.Emit( aOp, aFlags, start, static_cast<std::uint32_t>( aCount ) );
}


void RECORDING_GAL::emitLineChain( kgds_op aOp, std::uint16_t aFlags,
                                   const SHAPE_LINE_CHAIN& aChain, double aWidth )
{
    const int count = aChain.PointCount();

    if( count < 2 )
        return;

    m_scratch.clear();
    m_scratch.reserve( static_cast<std::size_t>( count ) );

    for( int i = 0; i < count; ++i )
    {
        const VECTOR2I& p = aChain.CPoint( i );
        m_scratch.emplace_back( static_cast<double>( p.x ), static_cast<double>( p.y ) );
    }

    std::uint16_t flags = aFlags;

    if( aChain.IsClosed() )
        flags |= KGDS_FLAG_CLOSED;

    const std::uint32_t start = m_stream.PushPoints( m_scratch.data(), m_scratch.size() );
    const std::uint32_t n = static_cast<std::uint32_t>( m_scratch.size() );

    if( aOp == KGDS_OP_SEGMENT_CHAIN )
        m_stream.Emit( aOp, flags, start, n, m_stream.PushCoord( aWidth ) );
    else
        m_stream.Emit( aOp, flags, start, n );
}


// ------------------------------------------------------------------- drawing

void RECORDING_GAL::DrawLine( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )
{
    const double xy[4] = { aStartPoint.x, aStartPoint.y, aEndPoint.x, aEndPoint.y };

    m_stream.Emit( KGDS_OP_LINE, 0, m_stream.PushCoords( xy, 4 ) );
}


void RECORDING_GAL::DrawSegment( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint,
                                 double aWidth )
{
    const double xy[4] = { aStartPoint.x, aStartPoint.y, aEndPoint.x, aEndPoint.y };
    const std::uint32_t pts = m_stream.PushCoords( xy, 4 );

    m_stream.Emit( KGDS_OP_SEGMENT, 0, pts, m_stream.PushCoord( aWidth ) );
}


void RECORDING_GAL::DrawSegmentChain( const std::vector<VECTOR2D>& aPointList, double aWidth )
{
    if( aPointList.size() < 2 )
        return;

    const std::uint32_t start = m_stream.PushPoints( aPointList.data(), aPointList.size() );

    m_stream.Emit( KGDS_OP_SEGMENT_CHAIN, 0, start,
                   static_cast<std::uint32_t>( aPointList.size() ),
                   m_stream.PushCoord( aWidth ) );
}


void RECORDING_GAL::DrawSegmentChain( const SHAPE_LINE_CHAIN& aLineChain, double aWidth )
{
    emitLineChain( KGDS_OP_SEGMENT_CHAIN, 0, aLineChain, aWidth );
}


void RECORDING_GAL::DrawPolyline( const std::deque<VECTOR2D>& aPointList )
{
    m_scratch.assign( aPointList.begin(), aPointList.end() );
    emitPointRun( KGDS_OP_POLYLINE, 0, m_scratch.data(), m_scratch.size() );
}


void RECORDING_GAL::DrawPolyline( const std::vector<VECTOR2D>& aPointList )
{
    emitPointRun( KGDS_OP_POLYLINE, 0, aPointList.data(), aPointList.size() );
}


void RECORDING_GAL::DrawPolyline( const VECTOR2D aPointList[], int aListSize )
{
    if( aListSize > 0 )
        emitPointRun( KGDS_OP_POLYLINE, 0, aPointList, static_cast<std::size_t>( aListSize ) );
}


void RECORDING_GAL::DrawPolyline( const SHAPE_LINE_CHAIN& aLineChain )
{
    emitLineChain( KGDS_OP_POLYLINE, 0, aLineChain );
}


void RECORDING_GAL::DrawPolylines( const std::vector<std::vector<VECTOR2D>>& aPointLists )
{
    for( const std::vector<VECTOR2D>& list : aPointLists )
        emitPointRun( KGDS_OP_POLYLINE, 0, list.data(), list.size() );
}


void RECORDING_GAL::DrawCircle( const VECTOR2D& aCenterPoint, double aRadius )
{
    const double c[3] = { aCenterPoint.x, aCenterPoint.y, aRadius };

    m_stream.Emit( KGDS_OP_CIRCLE, 0, m_stream.PushCoords( c, 3 ) );
}


void RECORDING_GAL::DrawHoleWall( const VECTOR2D& aCenterPoint, double aHoleRadius,
                                  double aWallWidth )
{
    const double c[3] = { aCenterPoint.x, aCenterPoint.y, aHoleRadius };
    const std::uint32_t centre = m_stream.PushCoords( c, 3 );

    m_stream.Emit( KGDS_OP_HOLE_WALL, 0, centre, m_stream.PushCoord( aWallWidth ) );
}


void RECORDING_GAL::DrawArc( const VECTOR2D& aCenterPoint, double aRadius,
                             const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aAngle )
{
    // The stream stores absolute start and end angles in radians; the GAL
    // interface passes a start and a sweep.
    const double start = aStartAngle.AsRadians();
    const double a[5] = { aCenterPoint.x, aCenterPoint.y, aRadius, start,
                          start + aAngle.AsRadians() };

    m_stream.Emit( KGDS_OP_ARC, 0, m_stream.PushCoords( a, 5 ) );
}


void RECORDING_GAL::DrawArcSegment( const VECTOR2D& aCenterPoint, double aRadius,
                                    const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aAngle,
                                    double aWidth, double aMaxError )
{
    // aMaxError bounds the chord error of a polygonal approximation. The
    // renderer evaluates the arc analytically in a shader, so it has no
    // approximation to bound and the value is deliberately dropped.
    (void) aMaxError;

    const double start = aStartAngle.AsRadians();
    const double a[5] = { aCenterPoint.x, aCenterPoint.y, aRadius, start,
                          start + aAngle.AsRadians() };
    const std::uint32_t arc = m_stream.PushCoords( a, 5 );

    m_stream.Emit( KGDS_OP_ARC_SEGMENT, 0, arc, m_stream.PushCoord( aWidth ) );
}


void RECORDING_GAL::DrawEllipse( const VECTOR2D& aCenterPoint, double aMajorRadius,
                                 double aMinorRadius, const EDA_ANGLE& aRotation )
{
    const double e[4] = { aCenterPoint.x, aCenterPoint.y, aMajorRadius, aMinorRadius };
    const std::uint32_t ellipse = m_stream.PushCoords( e, 4 );

    m_stream.Emit( KGDS_OP_ELLIPSE, 0, ellipse, m_stream.PushCoord( aRotation.AsRadians() ) );
}


void RECORDING_GAL::DrawEllipseArc( const VECTOR2D& aCenterPoint, double aMajorRadius,
                                    double aMinorRadius, const EDA_ANGLE& aRotation,
                                    const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aEndAngle )
{
    const double e[4] = { aCenterPoint.x, aCenterPoint.y, aMajorRadius, aMinorRadius };
    const std::uint32_t ellipse = m_stream.PushCoords( e, 4 );

    const double angles[3] = { aRotation.AsRadians(), aStartAngle.AsRadians(),
                               aEndAngle.AsRadians() };
    const std::uint32_t angleRun = m_stream.PushCoords( angles, 3 );

    m_stream.Emit( KGDS_OP_ELLIPSE_ARC, 0, ellipse, angleRun,
                   m_stream.PushCoord( GetLineWidth() ) );
}


void RECORDING_GAL::DrawRectangle( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint )
{
    const double r[4] = { aStartPoint.x, aStartPoint.y, aEndPoint.x, aEndPoint.y };

    m_stream.Emit( KGDS_OP_RECTANGLE, 0, m_stream.PushCoords( r, 4 ) );
}


void RECORDING_GAL::DrawGlyph( const KIFONT::GLYPH& aGlyph, int aNth, int aTotal )
{
    (void) aNth;
    (void) aTotal;

    // Glyphs arrive already resolved to geometry by KIFONT, so there is nothing
    // font-specific to record and the renderer needs no text support. The glyph
    // flag is carried through only so that a renderer may batch text
    // separately if it wants to.
    if( aGlyph.IsStroke() )
    {
        const auto& strokes = static_cast<const KIFONT::STROKE_GLYPH&>( aGlyph );

        for( const std::vector<VECTOR2D>& pointList : strokes )
            emitPointRun( KGDS_OP_POLYLINE, KGDS_FLAG_GLYPH, pointList.data(), pointList.size() );
    }
    else if( aGlyph.IsOutline() )
    {
        const auto& outline = static_cast<const KIFONT::OUTLINE_GLYPH&>( aGlyph );

        for( int i = 0; i < outline.OutlineCount(); ++i )
        {
            emitLineChain( KGDS_OP_POLYGON, KGDS_FLAG_GLYPH, outline.COutline( i ) );

            for( int h = 0; h < outline.HoleCount( i ); ++h )
            {
                emitLineChain( KGDS_OP_POLYGON, KGDS_FLAG_GLYPH | KGDS_FLAG_HOLE,
                               outline.CHole( i, h ) );
            }
        }
    }
}


void RECORDING_GAL::DrawPolygon( const std::deque<VECTOR2D>& aPointList )
{
    m_scratch.assign( aPointList.begin(), aPointList.end() );
    emitPointRun( KGDS_OP_POLYGON, 0, m_scratch.data(), m_scratch.size() );
}


void RECORDING_GAL::DrawPolygon( const VECTOR2D aPointList[], int aListSize )
{
    if( aListSize > 0 )
        emitPointRun( KGDS_OP_POLYGON, 0, aPointList, static_cast<std::size_t>( aListSize ) );
}


void RECORDING_GAL::DrawPolygon( const SHAPE_POLY_SET& aPolySet, bool aStrokeTriangulation )
{
    // The renderer tessellates, so the triangulation hint is not useful here.
    (void) aStrokeTriangulation;

    for( int i = 0; i < aPolySet.OutlineCount(); ++i )
    {
        emitLineChain( KGDS_OP_POLYGON, 0, aPolySet.COutline( i ) );

        // Holes immediately follow the outline they belong to, which is how the
        // renderer pairs them up without needing a nesting structure.
        for( int h = 0; h < aPolySet.HoleCount( i ); ++h )
            emitLineChain( KGDS_OP_POLYGON, KGDS_FLAG_HOLE, aPolySet.CHole( i, h ) );
    }
}


void RECORDING_GAL::DrawPolygon( const SHAPE_LINE_CHAIN& aPolySet )
{
    emitLineChain( KGDS_OP_POLYGON, 0, aPolySet );
}


void RECORDING_GAL::DrawCurve( const VECTOR2D& aStartPoint, const VECTOR2D& aControlPointA,
                               const VECTOR2D& aControlPointB, const VECTOR2D& aEndPoint,
                               double aFilterValue )
{
    (void) aFilterValue;

    const double c[8] = { aStartPoint.x,    aStartPoint.y,    aControlPointA.x, aControlPointA.y,
                          aControlPointB.x, aControlPointB.y, aEndPoint.x,      aEndPoint.y };

    m_stream.Emit( KGDS_OP_CURVE, 0, m_stream.PushCoords( c, 8 ) );
}


std::uint32_t RECORDING_GAL::internBitmap( const BITMAP_BASE& aBitmap )
{
    const KIID id = aBitmap.GetImageID();
    auto       cached = m_bitmapCache.find( id );

    if( cached != m_bitmapCache.end() )
        return cached->second;

    const wxImage* image = aBitmap.GetImageData();

    if( !image || !image->IsOk() )
        return NO_IMAGE;

    const int width = image->GetWidth();
    const int height = image->GetHeight();
    const std::size_t pixels = static_cast<std::size_t>( width ) * height;

    // wxImage keeps colour and alpha in separate planes; the stream wants them
    // interleaved as straight RGBA8.
    std::vector<std::uint8_t> rgba( pixels * 4 );
    const unsigned char*      rgb = image->GetData();
    const unsigned char*      alpha = image->HasAlpha() ? image->GetAlpha() : nullptr;

    for( std::size_t i = 0; i < pixels; ++i )
    {
        rgba[i * 4 + 0] = rgb[i * 3 + 0];
        rgba[i * 4 + 1] = rgb[i * 3 + 1];
        rgba[i * 4 + 2] = rgb[i * 3 + 2];
        rgba[i * 4 + 3] = alpha ? alpha[i] : 255;
    }

    const std::uint32_t index = m_stream.PushImage( static_cast<std::uint32_t>( width ),
                                                    static_cast<std::uint32_t>( height ),
                                                    rgba.data(), rgba.size() );

    m_bitmapCache[id] = index;

    return index;
}


void RECORDING_GAL::DrawBitmap( const BITMAP_BASE& aBitmap, double aAlphaBlend )
{
    const std::uint32_t image = internBitmap( aBitmap );

    if( image == NO_IMAGE )
        return;

    // The bitmap is placed by the transform already on the stack; what is
    // recorded here is the size it should cover in world units, expressed as an
    // affine so that the renderer needs no separate scaling convention.
    const VECTOR2I size = aBitmap.GetSizePixels();
    const double   scale = aBitmap.GetScale();
    const double   w = size.x * scale;
    const double   h = size.y * scale;

    const double placement[6] = { w, 0.0, 0.0, h, -w / 2.0, -h / 2.0 };

    m_stream.Emit( KGDS_OP_BITMAP, 0, image, m_stream.PushCoords( placement, 6 ),
                   m_stream.PushCoord( aAlphaBlend ) );
}


// ------------------------------------------------------------ screen/targets

void RECORDING_GAL::ResizeScreen( int aWidth, int aHeight )
{
    m_screenSize = VECTOR2I( aWidth, aHeight );
}


void RECORDING_GAL::ClearScreen()
{
    m_stream.Emit( KGDS_OP_CLEAR_SCREEN, 0, DRAW_STREAM::PackColor( m_clearColor ) );
}


void RECORDING_GAL::SetTarget( RENDER_TARGET aTarget )
{
    m_currentTarget = aTarget;
    m_stream.Emit( KGDS_OP_SET_TARGET, 0, static_cast<std::uint32_t>( aTarget ) );
}


void RECORDING_GAL::ClearTarget( RENDER_TARGET aTarget )
{
    m_stream.Emit( KGDS_OP_CLEAR_TARGET, 0, static_cast<std::uint32_t>( aTarget ) );
}


bool RECORDING_GAL::HasTarget( RENDER_TARGET aTarget )
{
    (void) aTarget;

    // Every target is representable in the stream; the renderer decides how to
    // composite them.
    return true;
}


void RECORDING_GAL::SetNegativeDrawMode( bool aSetting )
{
    m_stream.Emit( KGDS_OP_SET_NEGATIVE_DRAW_MODE, 0, aSetting ? 1u : 0u );
}


void RECORDING_GAL::StartDiffLayer()
{
    m_stream.Emit( KGDS_OP_START_DIFF_LAYER, 0 );
}


void RECORDING_GAL::EndDiffLayer()
{
    m_stream.Emit( KGDS_OP_END_DIFF_LAYER, 0 );
}


void RECORDING_GAL::StartNegativesLayer()
{
    m_stream.Emit( KGDS_OP_START_NEGATIVES_LAYER, 0 );
}


void RECORDING_GAL::EndNegativesLayer()
{
    m_stream.Emit( KGDS_OP_END_NEGATIVES_LAYER, 0 );
}


// ---------------------------------------------------------------------- state

void RECORDING_GAL::SetIsFill( bool aIsFillEnabled )
{
    GAL::SetIsFill( aIsFillEnabled );
    m_stream.Emit( KGDS_OP_SET_IS_FILL, 0, aIsFillEnabled ? 1u : 0u );
}


void RECORDING_GAL::SetIsStroke( bool aIsStrokeEnabled )
{
    GAL::SetIsStroke( aIsStrokeEnabled );
    m_stream.Emit( KGDS_OP_SET_IS_STROKE, 0, aIsStrokeEnabled ? 1u : 0u );
}


void RECORDING_GAL::SetFillColor( const COLOR4D& aColor )
{
    GAL::SetFillColor( aColor );
    m_stream.Emit( KGDS_OP_SET_FILL_COLOR, 0, DRAW_STREAM::PackColor( aColor ) );
}


void RECORDING_GAL::SetStrokeColor( const COLOR4D& aColor )
{
    GAL::SetStrokeColor( aColor );
    m_stream.Emit( KGDS_OP_SET_STROKE_COLOR, 0, DRAW_STREAM::PackColor( aColor ) );
}


void RECORDING_GAL::SetHoverColor( const COLOR4D& aColor )
{
    GAL::SetHoverColor( aColor );
    m_stream.Emit( KGDS_OP_SET_HOVER_COLOR, 0, DRAW_STREAM::PackColor( aColor ) );
}


void RECORDING_GAL::SetLineWidth( float aLineWidth )
{
    GAL::SetLineWidth( aLineWidth );
    m_stream.Emit( KGDS_OP_SET_LINE_WIDTH, 0, m_stream.PushCoord( aLineWidth ) );
}


void RECORDING_GAL::SetMinLineWidth( float aLineWidth )
{
    // The floor a stroke width is clamped to after scaling. Without it a
    // renderer would let hairlines vanish when zoomed out, which is exactly
    // what this setting exists to prevent.
    GAL::SetMinLineWidth( aLineWidth );
    m_stream.Emit( KGDS_OP_SET_MIN_LINE_WIDTH, 0, m_stream.PushCoord( aLineWidth ) );
}


void RECORDING_GAL::SetLayerDepth( double aLayerDepth )
{
    GAL::SetLayerDepth( aLayerDepth );
    m_stream.Emit( KGDS_OP_SET_LAYER_DEPTH, 0, m_stream.PushCoord( aLayerDepth ) );
}


void RECORDING_GAL::EnableDepthTest( bool aEnabled )
{
    m_stream.Emit( KGDS_OP_ENABLE_DEPTH_TEST, 0, aEnabled ? 1u : 0u );
}


// ----------------------------------------------------------------- transforms

void RECORDING_GAL::Transform( const MATRIX3x3D& aTransformation )
{
    const double m[6] = { aTransformation.m_data[0][0], aTransformation.m_data[0][1],
                          aTransformation.m_data[0][2], aTransformation.m_data[1][0],
                          aTransformation.m_data[1][1], aTransformation.m_data[1][2] };

    m_stream.Emit( KGDS_OP_TRANSFORM, 0, m_stream.PushCoords( m, 6 ) );
}


void RECORDING_GAL::Rotate( double aAngle )
{
    m_stream.Emit( KGDS_OP_ROTATE, 0, m_stream.PushCoord( aAngle ) );
}


void RECORDING_GAL::Translate( const VECTOR2D& aTranslation )
{
    m_stream.Emit( KGDS_OP_TRANSLATE, 0, m_stream.PushPoint( aTranslation ) );
}


void RECORDING_GAL::Scale( const VECTOR2D& aScale )
{
    m_stream.Emit( KGDS_OP_SCALE, 0, m_stream.PushPoint( aScale ) );
}


void RECORDING_GAL::Save()
{
    m_stream.Emit( KGDS_OP_SAVE, 0 );
}


void RECORDING_GAL::Restore()
{
    m_stream.Emit( KGDS_OP_RESTORE, 0 );
}


// --------------------------------------------------------------------- groups

int RECORDING_GAL::BeginGroup()
{
    return m_stream.BeginGroup();
}


void RECORDING_GAL::EndGroup()
{
    m_stream.EndGroup();
}


void RECORDING_GAL::DrawGroup( int aGroupNumber )
{
    auto override = m_groupOverrides.find( aGroupNumber );

    if( override == m_groupOverrides.end() )
    {
        m_stream.DrawGroup( aGroupNumber );
        return;
    }

    if( !m_stream.HasGroup( aGroupNumber ) )
        return;

    std::uint16_t flags = 0;
    std::uint32_t color = 0;
    std::uint32_t depth = 0;

    if( override->second.hasColor )
    {
        flags |= KGDS_FLAG_GROUP_COLOR;
        color = DRAW_STREAM::PackColor( override->second.color );
    }

    if( override->second.hasDepth )
    {
        flags |= KGDS_FLAG_GROUP_DEPTH;
        depth = m_stream.PushCoord( override->second.depth );
    }

    m_stream.Emit( KGDS_OP_DRAW_GROUP, flags, static_cast<std::uint32_t>( aGroupNumber ), color,
                   depth );
}


void RECORDING_GAL::ChangeGroupColor( int aGroupNumber, const COLOR4D& aNewColor )
{
    // VIEW recolours a cached item — for selection and highlighting — instead
    // of re-recording it. Rewriting the recorded commands would invalidate the
    // renderer's cached buffer for this group, so the new colour is remembered
    // here and travels with the replay instead.
    GROUP_OVERRIDE& override = m_groupOverrides[aGroupNumber];
    override.hasColor = true;
    override.color = aNewColor;
}


void RECORDING_GAL::ChangeGroupDepth( int aGroupNumber, int aDepth )
{
    GROUP_OVERRIDE& override = m_groupOverrides[aGroupNumber];
    override.hasDepth = true;
    override.depth = static_cast<double>( aDepth );
}


void RECORDING_GAL::DeleteGroup( int aGroupNumber )
{
    // Ids are never reused, but dropping the override keeps the map from
    // growing over a long editing session.
    m_groupOverrides.erase( aGroupNumber );
    m_stream.DeleteGroup( aGroupNumber );
}


void RECORDING_GAL::ClearCache()
{
    m_stream.ClearGroups();
    m_bitmapCache.clear();
    m_groupOverrides.clear();
}


// ------------------------------------------------------------------ overlays

void RECORDING_GAL::DrawGrid()
{
    if( !m_gridVisibility )
        return;

    // The grid is recorded as its parameters rather than as the thousands of
    // lines it expands to. A fragment shader draws it in a single quad, which
    // keeps a dense grid from dominating the frame's command count.
    const double params[6] = { m_gridOrigin.x,
                               m_gridOrigin.y,
                               m_gridSize.x,
                               m_gridSize.y,
                               static_cast<double>( m_gridLineWidth ),
                               static_cast<double>( static_cast<int>( m_gridStyle ) ) };

    m_stream.Emit( KGDS_OP_GRID, 0, m_stream.PushCoords( params, 6 ),
                   DRAW_STREAM::PackColor( m_gridColor ) );
}


void RECORDING_GAL::DrawCursor( const VECTOR2D& aCursorPosition )
{
    m_stream.Emit( KGDS_OP_CURSOR, 0, m_stream.PushPoint( aCursorPosition ),
                   DRAW_STREAM::PackColor( m_cursorColor ) );
}


// ------------------------------------------------------------ frame lifecycle

void RECORDING_GAL::BeginDrawing()
{
    m_stream.BeginFrame( m_screenSize.x, m_screenSize.y );
}


void RECORDING_GAL::EndDrawing()
{
    m_stream.EndFrame();
}

} // namespace KIGFX
