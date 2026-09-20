// SPDX-License-Identifier: GPL-3.0-or-later
// SVG/DXF decoding and model transaction; all presentation belongs to GPUI.
#include <import_gfx/graphics_importer_sch.h>
#include <import_gfx/graphics_import_mgr.h>
#include <import_gfx/dxf_import_plugin.h>
#include <locale_io.h>

extern "C" ksch_status ksch_session_graphics_import_properties( ksch_session* session,
        ksch_property_visitor visitor, void* context )
{
    if( !session || !visitor ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        const char* names[] = { "Graphics file (SVG or DXF)", "Scale", "Origin X (mm)",
            "Origin Y (mm)", "DXF units", "DXF default line width (mm)" };
        const char* values[] = { "", "1", "0", "0", "File default", "0.1524" };
        const uint32_t kinds[] = { KSCH_PROPERTY_TEXT, KSCH_PROPERTY_NUMBER,
            KSCH_PROPERTY_DISTANCE, KSCH_PROPERTY_DISTANCE, KSCH_PROPERTY_CHOICE, KSCH_PROPERTY_DISTANCE };
        const char* units[] = { "File default", "Millimeters", "Centimeters", "Meters", "Inches", "Feet", "Mils" };
        for( size_t i = 0; i < 6; ++i )
            visitor( context, "graphics-import", names[i], values[i], kinds[i], i == 4 ? units : nullptr, i == 4 ? 7 : 0 );
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_graphics_import( ksch_session* session,
        const char* const* values, uint32_t count )
{
    if( !session || !values || count != 6 ) return KSCH_ERR_INVALID_ARG;
    for( uint32_t i = 0; i < count; ++i ) if( !values[i] ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        double scale, x, y, width;
        const auto number = [&]( int index, double& output, double low, double high )
        {
            return wxString::FromUTF8( values[index] ).ToDouble( &output )
                && std::isfinite( output ) && output >= low && output <= high;
        };
        if( !number( 1, scale, 1e-9, 1e6 ) || !number( 2, x, -100000, 100000 )
            || !number( 3, y, -100000, 100000 ) || !number( 5, width, 0, 1000 ) )
        { setError( session, "Use a positive scale and finite origin/line width values" ); return KSCH_ERR_INVALID_ARG; }
        const std::map<std::string, DXF_IMPORT_UNITS> units = {
            { "File default", DXF_IMPORT_UNITS::DEFAULT }, { "Millimeters", DXF_IMPORT_UNITS::MM },
            { "Centimeters", DXF_IMPORT_UNITS::CM }, { "Meters", DXF_IMPORT_UNITS::METERS },
            { "Inches", DXF_IMPORT_UNITS::INCH }, { "Feet", DXF_IMPORT_UNITS::FEET },
            { "Mils", DXF_IMPORT_UNITS::MILS } };
        const auto unit = units.find( values[4] );
        if( unit == units.end() ) return KSCH_ERR_INVALID_ARG;
        wxFileName file( wxString::FromUTF8( values[0] ) );
        if( !file.FileExists() ) { setError( session, "Choose an existing SVG or DXF file" ); return KSCH_ERR_IO; }
        GRAPHICS_IMPORT_MGR manager;
        auto plugin = manager.GetPluginByExt( file.GetExt().Lower() );
        if( !plugin ) { setError( session, "Unsupported graphics file format" ); return KSCH_ERR_INVALID_ARG; }
        GRAPHICS_IMPORTER_SCH importer;
        if( auto* dxf = dynamic_cast<DXF_IMPORT_PLUGIN*>( plugin.get() ) )
        { dxf->SetUnit( unit->second ); importer.SetLineWidthMM( width ); }
        else importer.SetLineWidthMM( 0 );
        importer.SetPlugin( std::move( plugin ) );
        importer.SetImportOffsetMM( VECTOR2D( x, y ) );
        LOCALE_IO locale;
        if( !importer.Load( file.GetFullPath() ) || !importer.Import( VECTOR2D( scale, scale ) ) )
        { setError( session, importer.GetMessages() ); return KSCH_ERR_IO; }
        auto& items = importer.GetItems();
        if( items.empty() ) { setError( session, "No graphic items were found" ); return KSCH_ERR_INVALID_ARG; }
        for( const auto& item : items )
            if( !dynamic_cast<SCH_ITEM*>( item.get() ) ) return KSCH_ERR_INVALID_ARG;
        auto* managerTool = session->m_Host.GetToolManager();
        managerTool->RunAction( ACTIONS::cancelInteractive );
        managerTool->RunAction( ACTIONS::selectionClear );
        SCH_COMMIT commit( managerTool );
        auto* selection = managerTool->GetTool<SCH_SELECTION_TOOL>();
        for( auto& item : items )
        {
            auto* graphic = static_cast<SCH_ITEM*>( item.release() );
            commit.Add( graphic, session->m_Host.GetScreen() );
            selection->AddItemToSel( graphic );
        }
        commit.Push( "Import Graphics" );
        return KSCH_OK;
    } );
}
