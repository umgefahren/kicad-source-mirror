// SPDX-License-Identifier: GPL-3.0-or-later
// Schematic setup data and validation; presentation belongs to GPUI.
#include <schematic_settings.h>
#include <sch_view.h>
#include <project/project_file.h>
#include <cmath>
#include <erc/erc.h>
#include <erc/erc_item.h>
#include <erc/erc_settings.h>
#include <sch_pin.h>

namespace
{
struct HOST_SETUP_FIELD
{
    std::string name;
    uint32_t kind;
    std::function<wxString()> read;
    std::function<bool( const wxString&, bool )> write;
    std::vector<std::string> choices = {};
};

std::vector<HOST_SETUP_FIELD> hostSetupFields( SCHEMATIC_SETTINGS& settings )
{
    std::vector<HOST_SETUP_FIELD> result;
    auto number = [&]( const char* name, auto& target, double scale, double low, double high )
    {
        auto* ptr = &target;
        result.push_back( { name, scale != 1 ? KSCH_PROPERTY_DISTANCE :
            ( std::is_integral_v<std::remove_reference_t<decltype( target )>> ? KSCH_PROPERTY_INTEGER : KSCH_PROPERTY_NUMBER ),
            [=]() { return wxString::Format( "%.12g", static_cast<double>( *ptr ) / scale ); },
            [=]( const wxString& value, bool apply )
            {
                double parsed;
                if( !value.ToDouble( &parsed ) || !std::isfinite( parsed )
                    || parsed < low || parsed > high ) return false;
                if constexpr( std::is_integral_v<std::remove_reference_t<decltype( *ptr )>> )
                    if( scale == 1 && std::floor( parsed ) != parsed ) return false;
                if( apply ) *ptr = static_cast<std::remove_reference_t<decltype( *ptr )>>( parsed * scale );
                return true;
            } } );
    };
    auto choice = [&]( const char* name, int& target, std::vector<std::string> choices )
    {
        auto* ptr = &target;
        result.push_back( { name, KSCH_PROPERTY_CHOICE,
            [=]() { return wxString::FromUTF8( choices.at( *ptr ) ); },
            [=]( const wxString& value, bool apply )
            {
                auto found = std::find( choices.begin(), choices.end(), value.utf8_string() );
                if( found == choices.end() ) return false;
                if( apply ) *ptr = std::distance( choices.begin(), found );
                return true;
            }, choices } );
    };
    auto boolean = [&]( const char* name, bool& target )
    {
        auto* ptr = &target;
        result.push_back( { name, KSCH_PROPERTY_BOOL,
            [=]() { return *ptr ? wxS( "true" ) : wxS( "false" ); },
            [=]( const wxString& value, bool apply )
            {
                if( value != "true" && value != "false" ) return false;
                if( apply ) *ptr = value == "true";
                return true;
            } } );
    };
    auto text = [&]( const char* name, wxString& target )
    {
        auto* ptr = &target;
        result.push_back( { name, KSCH_PROPERTY_TEXT, [=]() { return *ptr; },
            [=]( const wxString& value, bool apply ) { if( apply ) *ptr = value; return true; } } );
    };
    const double mm = schIUScale.mmToIU( 1.0 );
    number( "Default text size (mm)", settings.m_DefaultTextSize, mm, 0.01, 100 );
    number( "Default line width (mm)", settings.m_DefaultLineWidth, mm, 0.001, 10 );
    number( "Pin symbol size (mm)", settings.m_PinSymbolSize, mm, 0.01, 100 );
    number( "Connection grid (mm)", settings.m_ConnectionGridSize, mm,
            schIUScale.MilsToIU( MIN_CONNECTION_GRID_MILS ) / mm, 254 );
    boolean( "Show DNP markers", settings.m_ShowDNPMarkers );
    choice( "Junction size", settings.m_JunctionSizeChoice, {"None", "Smallest", "Small", "Medium", "Large", "Largest"} );
    choice( "Wire hop size", settings.m_HopOverSizeChoice, {"None", "Smallest", "Small", "Medium", "Large", "Largest"} );
    boolean( "Show intersheet references", settings.m_IntersheetRefsShow );
    boolean( "Include own page in references", settings.m_IntersheetRefsListOwnPage );
    boolean( "Abbreviate intersheet references", settings.m_IntersheetRefsFormatShort );
    text( "Intersheet reference prefix", settings.m_IntersheetRefsPrefix );
    text( "Intersheet reference suffix", settings.m_IntersheetRefsSuffix );
    number( "Text offset ratio", settings.m_TextOffsetRatio, 1, 0, 10 );
    number( "Overbar height ratio", settings.m_FontMetrics.m_OverbarHeight, 1, 0, 10 );
    number( "Dash length ratio", settings.m_DashedLineDashRatio, 1, 0.01, 100 );
    number( "Dash gap ratio", settings.m_DashedLineGapRatio, 1, 0.01, 100 );
    number( "Label size ratio", settings.m_LabelSizeRatio, 1, 0.01, 10 );
    number( "Voltage significant digits", settings.m_OPO_VPrecision, 1, 1, 12 );
    text( "Voltage display range", settings.m_OPO_VRange );
    number( "Current significant digits", settings.m_OPO_IPrecision, 1, 1, 12 );
    text( "Current display range", settings.m_OPO_IRange );
    auto& parity = settings.m_SymbolParity;
    boolean( "Check missing symbol fields", parity.m_MissingFields );
    boolean( "Check extra symbol fields", parity.m_ExtraFields );
    boolean( "Check symbol field values", parity.m_FieldTexts );
    boolean( "Check field visibility", parity.m_FieldVisibilities );
    boolean( "Check field styles", parity.m_FieldStyles );
    boolean( "Check field positions", parity.m_FieldPositions );
    boolean( "Check pin visibility", parity.m_PinVisibilities );
    boolean( "Check alternate pin functions", parity.m_PinAltFunctions );
    boolean( "Check exclude-from-board flags", parity.m_ExcludeFromBoardFlags );
    boolean( "Check DNP flags", parity.m_DNPFlags );
    boolean( "Check exclude-from-BOM flags", parity.m_ExcludeFromBOMFlags );
    boolean( "Check exclude-from-position-file flags", parity.m_ExcludeFromPosFileFlags );
    return result;
}
std::vector<HOST_SETUP_FIELD> hostAllSetupFields( SCHEMATIC_SETTINGS& settings, ERC_SETTINGS& ercSettings )
{
    auto fields = hostSetupFields( settings );
    auto* erc = &ercSettings;
    for( const auto& itemRef : ERC_ITEM::GetItemsWithSeverities() )
    {
        const auto& item = itemRef.get();
        const int code = item.GetErrorCode();
        if( code <= 0 ) continue;
        fields.push_back( { "ERC: " + item.GetErrorText( false ).utf8_string(), KSCH_PROPERTY_CHOICE,
            [=]() { auto severity = erc->GetSeverity( code );
                return severity == RPT_SEVERITY_ERROR ? wxS("Error") : severity == RPT_SEVERITY_WARNING ? wxS("Warning") : wxS("Ignore"); },
            [=]( const wxString& value, bool apply ) {
                if( value != "Error" && value != "Warning" && value != "Ignore" ) return false;
                if( apply ) erc->SetSeverity( code, value == "Error" ? RPT_SEVERITY_ERROR : value == "Warning" ? RPT_SEVERITY_WARNING : RPT_SEVERITY_IGNORE );
                return true;
            }, { "Error", "Warning", "Ignore" } } );
    }
    for( int i = 0; i < ELECTRICAL_PINTYPES_TOTAL - 1; ++i )
        for( int j = 0; j <= i; ++j )
        {
            auto name = "Pin compatibility: " + CommentERC_H[i].utf8_string()
                        + " / " + CommentERC_H[j].utf8_string();
            const std::vector<std::string> choices{ "Allowed", "Warning", "Error", "Unconnected" };
            fields.push_back( { name, KSCH_PROPERTY_CHOICE,
                [=]() { return wxString::FromUTF8( choices.at( static_cast<size_t>( erc->GetPinMapValue(i,j) ) ) ); },
                [=]( const wxString& value, bool apply ) {
                    auto found = std::find( choices.begin(), choices.end(), value.utf8_string() );
                    if( found == choices.end() ) return false;
                    if( apply ) {
                        auto level = static_cast<PIN_ERROR>( std::distance( choices.begin(), found ) );
                        erc->SetPinMapValue(i,j,level); erc->SetPinMapValue(j,i,level);
                    }
                    return true;
                }, choices } );
        }
    return fields;
}

std::vector<HOST_SETUP_FIELD> hostAllSetupFields( SCHEMATIC& schematic )
{
    return hostAllSetupFields( schematic.Settings(), schematic.ErcSettings() );
}

}

extern "C" ksch_status ksch_session_setup( ksch_session* session, ksch_property_visitor visitor, void* context )
{
    if( !session || !visitor ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        for( const auto& field : hostAllSetupFields( *session->m_Host.GetSchematic() ) )
        {
            std::vector<const char*> choices;
            for( const auto& choice : field.choices ) choices.push_back( choice.c_str() );
            visitor( context, "schematic-setup", field.name.c_str(), field.read().utf8_string().c_str(),
                     field.kind, choices.data(), choices.size() );
        }
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_setup( ksch_session* session,
                                               const char* const* values, uint32_t count )
{
    if( !session || !values ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        auto& host = session->m_Host;
        if( !host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto& settings = host.GetSchematic()->Settings();
        const auto fields = hostAllSetupFields( *host.GetSchematic() );
        if( count != fields.size() ) return KSCH_ERR_INVALID_ARG;
        std::vector<wxString> old;
        for( size_t i = 0; i < fields.size(); ++i )
        {
            if( !values[i] || !fields[i].write( wxString::FromUTF8( values[i] ), false ) )
            { session->m_Error = std::string( "Invalid value: " ) + fields[i].name; return KSCH_ERR_INVALID_ARG; }
            old.push_back( fields[i].read() );
        }
        for( size_t i = 0; i < fields.size(); ++i ) fields[i].write( wxString::FromUTF8( values[i] ), true );
        settings.SaveToFile();
        host.GetSchematic()->ErcSettings().SaveToFile();
        auto& project = host.GetSchematic()->Project();
        if( !project.GetProjectFile().SaveToFile( project.GetProjectDirectory(), true ) )
        {
            for( size_t i = 0; i < fields.size(); ++i ) fields[i].write( old[i], true );
            settings.SaveToFile();
            host.GetSchematic()->ErcSettings().SaveToFile();
            session->m_Error = "Could not save schematic project settings";
            return KSCH_ERR_IO;
        }
        host.View().UpdateAllItems( KIGFX::ALL );
        host.RefreshCanvas();
        return KSCH_OK;
    } );
}
