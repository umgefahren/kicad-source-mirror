// SPDX-License-Identifier: GPL-3.0-or-later
// Raster decoding/placement; GPUI owns the file and placement options.
#include <sch_bitmap.h>

extern "C" ksch_status ksch_session_image_properties( ksch_session* session,
        ksch_property_visitor visitor, void* context )
{
    if( !session || !visitor ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        const char* names[] = { "Image file", "Scale", "Placement", "Center X (mm)", "Center Y (mm)" };
        const char* values[] = { "", "1", "Cursor", "0", "0" };
        const uint32_t kinds[] = { KSCH_PROPERTY_TEXT, KSCH_PROPERTY_NUMBER, KSCH_PROPERTY_CHOICE,
            KSCH_PROPERTY_DISTANCE, KSCH_PROPERTY_DISTANCE };
        const char* placement[] = { "Cursor", "Coordinates" };
        for( size_t i = 0; i < 5; ++i )
            visitor( context, "image-placement", names[i], values[i], kinds[i],
                     i == 2 ? placement : nullptr, i == 2 ? 2 : 0 );
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_image_properties( ksch_session* session,
        const char* const* values, uint32_t count )
{
    if( !session || !values || count != 5 ) return KSCH_ERR_INVALID_ARG;
    for( uint32_t i = 0; i < count; ++i ) if( !values[i] ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        double scale, x, y;
        const auto number = [&]( int index, double& output, double low, double high )
        {
            return wxString::FromUTF8( values[index] ).ToDouble( &output )
                && std::isfinite( output ) && output >= low && output <= high;
        };
        const std::string placement( values[2] );
        if( !number( 1, scale, 1e-6, 1e4 ) || !number( 3, x, -100000, 100000 )
            || !number( 4, y, -100000, 100000 )
            || ( placement != "Cursor" && placement != "Coordinates" ) )
        { setError( session, "Use a positive image scale and valid placement coordinates" ); return KSCH_ERR_INVALID_ARG; }
        const wxString file = wxString::FromUTF8( values[0] );
        auto image = std::make_unique<SCH_BITMAP>( VECTOR2I( schIUScale.mmToIU( x ), schIUScale.mmToIU( y ) ) );
        if( !wxFileExists( file ) || !image->GetReferenceImage().ReadImageFile( file ) )
        { setError( session, "Could not load image file" ); return KSCH_ERR_IO; }
        image->GetReferenceImage().SetImageScale( scale );
        const auto size = image->GetBoundingBox().GetSize();
        if( size.x <= 0 || size.y <= 0 )
        { setError( session, "Image dimensions are outside the supported range" ); return KSCH_ERR_INVALID_ARG; }
        auto* manager = session->m_Host.GetToolManager();
        manager->RunAction( ACTIONS::cancelInteractive );
        manager->RunAction( ACTIONS::selectionClear );
        if( placement == "Cursor" )
        {
            image->SetFlags( IS_NEW | IS_MOVING );
            manager->RunAction( SCH_ACTIONS::placeImage, image.release() );
        }
        else
        {
            SCH_COMMIT commit( manager );
            auto* raw = image.release();
            commit.Add( raw, session->m_Host.GetScreen() );
            commit.Push( "Place Image" );
            manager->GetTool<SCH_SELECTION_TOOL>()->AddItemToSel( raw );
        }
        return KSCH_OK;
    } );
}
