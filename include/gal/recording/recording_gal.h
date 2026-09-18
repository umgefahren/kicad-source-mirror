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

#ifndef KICAD_GAL_RECORDING_RECORDING_GAL_H
#define KICAD_GAL_RECORDING_RECORDING_GAL_H

#include <map>

#include <gal/graphics_abstraction_layer.h>
#include <gal/recording/draw_stream.h>
#include <kiid.h>

namespace KIGFX
{

/**
 * A GAL backend that records drawing commands instead of rasterising them.
 *
 * ## Why this exists
 *
 * SCH_PAINTER already holds every rule about how a schematic looks — pin
 * graphic styles, field placement, junction dot sizing, fill conventions,
 * selection highlighting — and expresses all of it through this interface.
 * Rather than porting those rules to a new renderer and inevitably drifting
 * from them, this backend captures the calls verbatim into a flat, device
 * independent DRAW_STREAM. A renderer on the other side of the FFI boundary
 * turns that stream into pixels.
 *
 * The practical consequences are worth being explicit about:
 *
 * - Appearance cannot diverge by accident. There is exactly one implementation
 *   of the drawing rules and it is the existing C++ one.
 * - The backend needs no window, no GL context and no wxWidgets. It can be
 *   constructed in a unit test and asserted against.
 * - KIGFX::VIEW's geometry cache maps directly onto the stream's groups, so the
 *   renderer can keep one GPU buffer per cached item and upload nothing while
 *   panning.
 *
 * ## Text
 *
 * Text never reaches this class as text. KIFONT resolves it to KIFONT::GLYPH
 * objects first, and those are already geometry: a STROKE_GLYPH is a set of
 * polylines and an OUTLINE_GLYPH is a SHAPE_POLY_SET. Both are lowered here to
 * ordinary polyline and polygon commands, which means the renderer needs no
 * font handling at all and typography stays identical to every other KiCad
 * canvas.
 */
class GAL_API RECORDING_GAL : public GAL
{
public:
    explicit RECORDING_GAL( GAL_DISPLAY_OPTIONS& aDisplayOptions );
    ~RECORDING_GAL() override;

    /// The stream being recorded into.
    DRAW_STREAM&       Stream() { return m_stream; }
    const DRAW_STREAM& Stream() const { return m_stream; }

    /// Borrow the recorded stream for handing across the FFI boundary.
    kgds_stream_view Publish() const { return m_stream.Publish(); }

    bool IsInitialized() const override { return true; }
    bool IsVisible() const override { return true; }

    // ------------------------------------------------------- drawing methods

    void DrawLine( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint ) override;
    void DrawSegment( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint,
                      double aWidth ) override;
    void DrawSegmentChain( const std::vector<VECTOR2D>& aPointList, double aWidth ) override;
    void DrawSegmentChain( const SHAPE_LINE_CHAIN& aLineChain, double aWidth ) override;

    void DrawPolyline( const std::deque<VECTOR2D>& aPointList ) override;
    void DrawPolyline( const std::vector<VECTOR2D>& aPointList ) override;
    void DrawPolyline( const VECTOR2D aPointList[], int aListSize ) override;
    void DrawPolyline( const SHAPE_LINE_CHAIN& aLineChain ) override;
    void DrawPolylines( const std::vector<std::vector<VECTOR2D>>& aPointLists ) override;

    void DrawCircle( const VECTOR2D& aCenterPoint, double aRadius ) override;
    void DrawHoleWall( const VECTOR2D& aCenterPoint, double aHoleRadius,
                       double aWallWidth ) override;
    void DrawArc( const VECTOR2D& aCenterPoint, double aRadius, const EDA_ANGLE& aStartAngle,
                  const EDA_ANGLE& aAngle ) override;
    void DrawArcSegment( const VECTOR2D& aCenterPoint, double aRadius,
                         const EDA_ANGLE& aStartAngle, const EDA_ANGLE& aAngle, double aWidth,
                         double aMaxError ) override;
    void DrawEllipse( const VECTOR2D& aCenterPoint, double aMajorRadius, double aMinorRadius,
                      const EDA_ANGLE& aRotation ) override;
    void DrawEllipseArc( const VECTOR2D& aCenterPoint, double aMajorRadius, double aMinorRadius,
                         const EDA_ANGLE& aRotation, const EDA_ANGLE& aStartAngle,
                         const EDA_ANGLE& aEndAngle ) override;

    void DrawRectangle( const VECTOR2D& aStartPoint, const VECTOR2D& aEndPoint ) override;

    void DrawGlyph( const KIFONT::GLYPH& aGlyph, int aNth, int aTotal ) override;

    void DrawPolygon( const std::deque<VECTOR2D>& aPointList ) override;
    void DrawPolygon( const VECTOR2D aPointList[], int aListSize ) override;
    void DrawPolygon( const SHAPE_POLY_SET& aPolySet, bool aStrokeTriangulation ) override;
    void DrawPolygon( const SHAPE_LINE_CHAIN& aPolySet ) override;

    void DrawCurve( const VECTOR2D& aStartPoint, const VECTOR2D& aControlPointA,
                    const VECTOR2D& aControlPointB, const VECTOR2D& aEndPoint,
                    double aFilterValue ) override;

    void DrawBitmap( const BITMAP_BASE& aBitmap, double aAlphaBlend ) override;

    // --------------------------------------------------------- screen/target

    void ResizeScreen( int aWidth, int aHeight ) override;
    void ClearScreen() override;

    void SetTarget( RENDER_TARGET aTarget ) override;
    RENDER_TARGET GetTarget() const override { return m_currentTarget; }
    void ClearTarget( RENDER_TARGET aTarget ) override;
    bool HasTarget( RENDER_TARGET aTarget ) override;

    void SetNegativeDrawMode( bool aSetting ) override;
    void StartDiffLayer() override;
    void EndDiffLayer() override;
    void StartNegativesLayer() override;
    void EndNegativesLayer() override;

    // ---------------------------------------------------------------- state

    void SetIsFill( bool aIsFillEnabled ) override;
    void SetIsStroke( bool aIsStrokeEnabled ) override;
    void SetFillColor( const COLOR4D& aColor ) override;
    void SetStrokeColor( const COLOR4D& aColor ) override;
    void SetHoverColor( const COLOR4D& aColor ) override;
    void SetLineWidth( float aLineWidth ) override;
    void SetMinLineWidth( float aLineWidth ) override;
    void SetLayerDepth( double aLayerDepth ) override;
    void EnableDepthTest( bool aEnabled ) override;

    // ----------------------------------------------------------- transforms

    void Transform( const MATRIX3x3D& aTransformation ) override;
    void Rotate( double aAngle ) override;
    void Translate( const VECTOR2D& aTranslation ) override;
    void Scale( const VECTOR2D& aScale ) override;
    void Save() override;
    void Restore() override;

    // --------------------------------------------------------------- groups

    int  BeginGroup() override;
    void EndGroup() override;
    void DrawGroup( int aGroupNumber ) override;
    void ChangeGroupColor( int aGroupNumber, const COLOR4D& aNewColor ) override;
    void ChangeGroupDepth( int aGroupNumber, int aDepth ) override;
    void DeleteGroup( int aGroupNumber ) override;
    void ClearCache() override;

    // -------------------------------------------------------------- overlays

    void DrawGrid() override;
    void DrawCursor( const VECTOR2D& aCursorPosition ) override;

    // ------------------------------------------------------- frame lifecycle

    void BeginDrawing() override;
    void EndDrawing() override;

private:
    /// Record a run of points as one command.
    void emitPointRun( kgds_op aOp, std::uint16_t aFlags, const VECTOR2D* aPoints,
                       std::size_t aCount );

    /// Record a SHAPE_LINE_CHAIN's points, converting from integer coordinates.
    void emitLineChain( kgds_op aOp, std::uint16_t aFlags, const SHAPE_LINE_CHAIN& aChain,
                        double aWidth = 0.0 );

    /// Returned by internBitmap() when a bitmap has no usable pixel data.
    static constexpr std::uint32_t NO_IMAGE = ~0u;

    /// Interleave a wxImage's RGB and alpha planes into the stream's arena.
    std::uint32_t internBitmap( const BITMAP_BASE& aBitmap );

    /**
     * A recolour or depth change applied to a cached group at replay time.
     *
     * VIEW adjusts a cached item's colour and depth without re-recording it, so
     * these ride on the DrawGroup command rather than being baked into the
     * group body. Baking them in would invalidate the renderer's cached buffer
     * for the group, which defeats the purpose of caching it.
     */
    struct GROUP_OVERRIDE
    {
        bool    hasColor = false;
        COLOR4D color;
        bool    hasDepth = false;
        double  depth = 0.0;
    };

    DRAW_STREAM m_stream;

    RENDER_TARGET m_currentTarget;

    /// Bitmaps already copied into the stream, keyed by their document id, so
    /// that a schematic with a repeated logo stores its pixels once.
    std::map<KIID, std::uint32_t> m_bitmapCache;

    /// Replay-time overrides, keyed by group id.
    std::map<int, GROUP_OVERRIDE> m_groupOverrides;

    /// Scratch buffer reused when converting integer point runs, to keep a
    /// redraw from allocating once per polygon.
    std::vector<VECTOR2D> m_scratch;
};

} // namespace KIGFX

#endif // KICAD_GAL_RECORDING_RECORDING_GAL_H
