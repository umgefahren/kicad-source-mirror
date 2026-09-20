// SPDX-License-Identifier: GPL-3.0-or-later
// Application settings are persisted through their native stores; presentation is GPUI.
#include <settings/common_settings.h>
#include <settings/settings_manager.h>
#include <eeschema_settings.h>
#include <sch_render_settings.h>
#include <settings/color_settings.h>

namespace
{
std::vector<HOST_SETUP_FIELD> hostPreferenceFields()
{
    auto* common = Pgm().GetCommonSettings();
    auto* editor = GetAppSettings<EESCHEMA_SETTINGS>( "eeschema" );
    std::vector<HOST_SETUP_FIELD> fields;
    auto boolean = [&]( const char* name, bool& target )
    {
        auto* ptr = &target;
        fields.push_back( { name, KSCH_PROPERTY_BOOL, [=]() { return *ptr ? "true" : "false"; },
            [=]( const wxString& value, bool apply )
            { if( value != "true" && value != "false" ) return false; if( apply ) *ptr = value == "true"; return true; } } );
    };
    auto number = [&]( const char* name, auto& target, double low, double high )
    {
        auto* ptr = &target;
        fields.push_back( { name, std::is_integral_v<std::remove_reference_t<decltype( target )>> ? KSCH_PROPERTY_INTEGER : KSCH_PROPERTY_NUMBER,
            [=]() { return wxString::Format( "%.12g", static_cast<double>( *ptr ) ); },
            [=]( const wxString& value, bool apply )
            {
                double parsed;
                if( !value.ToDouble( &parsed ) || !std::isfinite( parsed ) || parsed < low || parsed > high ) return false;
                if constexpr( std::is_integral_v<std::remove_reference_t<decltype( *ptr )>> )
                    if( std::floor( parsed ) != parsed ) return false;
                if( apply ) *ptr = static_cast<std::remove_reference_t<decltype( *ptr )>>( parsed );
                return true;
            } } );
    };
    auto text = [&]( const char* name, wxString& target )
    {
        auto* ptr = &target;
        fields.push_back( { name, KSCH_PROPERTY_TEXT, [=]() { return *ptr; },
            [=]( const wxString& value, bool apply ) { if( apply ) *ptr = value; return true; } } );
    };
    boolean( "Editing / Start tools immediately", common->m_Input.immediate_actions );
    boolean( "Editing / Move pointer when moving items", common->m_Input.warp_mouse_on_move );
    boolean( "Editing / Drag moves rather than stretches", editor->m_Input.drag_is_move );
    boolean( "Editing / Escape clears net highlighting", editor->m_Input.esc_clears_net_highlight );
    boolean( "Editing / Allow unconstrained pin swaps", editor->m_Input.allow_unconstrained_pin_swaps );
    boolean( "Editing / Start wires at pins", editor->m_Drawing.auto_start_wires );
    number( "Editing / Repeated label increment", editor->m_Drawing.repeat_label_increment, -1000, 1000 );
    boolean( "Fields / Automatically place fields", editor->m_AutoplaceFields.enable );
    boolean( "Fields / Allow automatic justification", editor->m_AutoplaceFields.allow_rejustify );
    boolean( "Fields / Align automatic fields to grid", editor->m_AutoplaceFields.align_to_grid );
    boolean( "Display / Hidden pins", editor->m_Appearance.show_hidden_pins );
    boolean( "Display / Hidden fields", editor->m_Appearance.show_hidden_fields );
    boolean( "Display / Alternate pin icons", editor->m_Appearance.show_pin_alt_icons );
    boolean( "Display / Directive labels", editor->m_Appearance.show_directive_labels );
    boolean( "Display / Remapped pin numbers", editor->m_Appearance.show_remapped_pin_numbers );
    boolean( "Display / Simulation exclusions", editor->m_Appearance.mark_sim_exclusions );
    boolean( "Display / ERC errors", editor->m_Appearance.show_erc_errors );
    boolean( "Display / ERC warnings", editor->m_Appearance.show_erc_warnings );
    boolean( "Display / ERC exclusions", editor->m_Appearance.show_erc_exclusions );
    boolean( "Selection / Selecting a pin selects its symbol", editor->m_Selection.select_pin_selects_symbol );
    boolean( "Selection / Draw selected children", editor->m_Selection.draw_selected_children );
    boolean( "Selection / Fill selected shapes", editor->m_Selection.fill_shapes );
    number( "Selection / Outline thickness", editor->m_Selection.selection_thickness, 1, 20 );
    number( "Selection / Highlight thickness", editor->m_Selection.highlight_thickness, 1, 20 );
    boolean( "Selection / Highlight netclass colors", editor->m_Selection.highlight_netclass_colors );
    number( "Selection / Netclass highlight opacity", editor->m_Selection.highlight_netclass_colors_alpha, 0, 1 );
    boolean( "Backups / Enabled", common->m_Backup.enabled );
    boolean( "Backups / Back up on autosave", common->m_Backup.backup_on_autosave );
    number( "Backups / Maximum archives", common->m_Backup.limit_total_files, 0, 100000 );
    number( "Backups / Maximum archives per day", common->m_Backup.limit_daily_files, 0, 100000 );
    number( "Backups / Minimum interval (seconds)", common->m_Backup.min_interval, 0, 86400 );
    number( "General / Recent files retained", common->m_System.file_history_size, 0, 1000 );
    text( "General / Text editor", common->m_System.text_editor );
    text( "General / File explorer", common->m_System.file_explorer );
    boolean( "General / Use system PDF viewer", common->m_System.use_system_pdf_viewer );
    text( "General / PDF viewer", common->m_System.pdf_viewer_name );
    boolean( "Files / Embed datasheets by default", common->m_EmbedFileDefaults.datasheet );
    boolean( "Files / Embed drawing sheets by default", common->m_EmbedFileDefaults.drawing_sheet );
    boolean( "Files / Embed simulation models by default", common->m_EmbedFileDefaults.sim_model );
    auto templates = std::make_shared<std::vector<TEMPLATE_FIELDNAME>>(
        common->m_FieldNameTemplates.GetTemplateFieldNames( TEMPLATES::SCOPE::GLOBAL ) );
    templates->emplace_back( wxEmptyString );
    const auto storeTemplates = [=]()
    {
        common->m_FieldNameTemplates.DeleteFieldNameTemplates( TEMPLATES::SCOPE::GLOBAL );
        for( const auto& field : *templates )
            if( !field.m_Name.empty() ) common->m_FieldNameTemplates.AddTemplateFieldName( field, TEMPLATES::SCOPE::GLOBAL );
    };
    for( size_t i = 0; i < templates->size(); ++i )
    {
        const std::string prefix = "Field templates / " + std::to_string( i + 1 ) + " / ";
        fields.push_back( { prefix + "Name (blank removes)", KSCH_PROPERTY_TEXT,
            [=]() { return templates->at( i ).m_Name; },
            [=]( const wxString& value, bool apply )
            {
                wxString trimmed = value;
                trimmed.Trim().Trim( false );
                if( value != trimmed || value.find_first_of( "\r\n\t" ) != wxString::npos ) return false;
                if( !value.empty() )
                {
                    for( FIELD_T id : MANDATORY_FIELDS )
                        if( value.CmpNoCase( GetDefaultFieldName( id, UNTRANSLATED ) ) == 0 ) return false;
                    for( size_t j = 0; j < templates->size(); ++j )
                        if( i != j && templates->at( j ).m_Name == value ) return false;
                }
                if( apply ) { templates->at( i ).m_Name = value; storeTemplates(); }
                return true;
            } } );
        for( bool url : { false, true } )
            fields.push_back( { prefix + ( url ? "URL field" : "Visible" ), KSCH_PROPERTY_BOOL,
                [=]() { return ( url ? templates->at( i ).m_URL : templates->at( i ).m_Visible ) ? "true" : "false"; },
                [=]( const wxString& value, bool apply )
                {
                    if( value != "true" && value != "false" ) return false;
                    if( apply )
                    {
                        ( url ? templates->at( i ).m_URL : templates->at( i ).m_Visible ) = value == "true";
                        storeTemplates();
                    }
                    return true;
                } } );
    }
    std::vector<std::string> themes;
    for( auto* theme : Pgm().GetSettingsManager().GetColorSettingsList() ) themes.push_back( theme->GetFilename().utf8_string() );
    fields.push_back( { "Colors / Theme", KSCH_PROPERTY_CHOICE,
        [=]() { return GetColorSettings( editor->m_ColorTheme )->GetFilename(); },
        [=]( const wxString& value, bool apply )
        {
            if( std::find( themes.begin(), themes.end(), value.utf8_string() ) == themes.end() ) return false;
            if( apply ) editor->m_ColorTheme = value;
            return true;
        }, themes } );
    const std::vector<std::pair<const char*, int>> layers = {
        { "Background", LAYER_SCHEMATIC_BACKGROUND }, { "Wires", LAYER_WIRE },
        { "Buses", LAYER_BUS }, { "Junctions", LAYER_JUNCTION }, { "Local labels", LAYER_LOCLABEL },
        { "Global labels", LAYER_GLOBLABEL }, { "Hierarchical labels", LAYER_HIERLABEL },
        { "Symbol body", LAYER_DEVICE }, { "Symbol fields", LAYER_FIELDS }, { "Pins", LAYER_PIN },
        { "Text and graphics", LAYER_NOTES }, { "Sheet outlines", LAYER_SHEET },
        { "Selection", LAYER_SELECTION_SHADOWS }, { "ERC errors", LAYER_ERC_ERR }, { "ERC warnings", LAYER_ERC_WARN }
    };
    for( auto [name, layer] : layers )
        fields.push_back( { std::string( "Colors / " ) + name, KSCH_PROPERTY_COLOR,
            [=]() { return GetColorSettings( editor->m_ColorTheme )->GetColor( layer ).ToCSSString(); },
            [=]( const wxString& value, bool apply )
            {
                KIGFX::COLOR4D color;
                if( !color.SetFromWxString( value ) ) return false;
                if( apply )
                {
                    auto* theme = GetColorSettings( editor->m_ColorTheme );
                    if( theme->IsReadOnly() )
                    {
                        auto* editable = Pgm().GetSettingsManager().AddNewColorSettings( "gpui-custom" );
                        *editable = *theme;
                        editable->SetFilename( "gpui-custom" );
                        editable->SetName( "GPUI Custom" );
                        editable->SetReadOnly( false );
                        theme = editable;
                        editor->m_ColorTheme = "gpui-custom";
                    }
                    theme->SetColor( layer, color );
                }
                return true;
            } } );
    return fields;
}
}

extern "C" ksch_status ksch_session_preferences( ksch_session* session, ksch_property_visitor visitor, void* context )
{
    if( !session || !visitor ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        for( const auto& field : hostPreferenceFields() )
        {
            std::vector<const char*> choices;
            for( const auto& choice : field.choices ) choices.push_back( choice.c_str() );
            visitor( context, "application-preferences", field.name.c_str(), field.read().utf8_string().c_str(),
                     field.kind, choices.data(), choices.size() );
        }
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_preferences( ksch_session* session,
        const char* const* values, uint32_t count )
{
    if( !session || ( count && !values ) ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        auto fields = hostPreferenceFields();
        if( fields.size() != count ) return KSCH_ERR_INVALID_ARG;
        for( size_t i = 0; i < fields.size(); ++i )
            if( !values[i] || !fields[i].write( wxString::FromUTF8( values[i] ), false ) )
            { setError( session, wxString( "Invalid preference: " ) + fields[i].name ); return KSCH_ERR_INVALID_ARG; }
        std::set<wxString> templateNames;
        for( size_t i = 0; i < fields.size(); ++i )
            if( fields[i].name.starts_with( "Field templates / " ) && fields[i].kind == KSCH_PROPERTY_TEXT )
            {
                const wxString name = wxString::FromUTF8( values[i] );
                if( !name.empty() && !templateNames.insert( name ).second )
                { setError( session, "Template field names must be unique" ); return KSCH_ERR_INVALID_ARG; }
            }
        bool changed = false;
        for( size_t i = 0; i < fields.size(); ++i ) changed |= fields[i].read() != wxString::FromUTF8( values[i] );
        if( !changed ) return KSCH_OK;
        std::vector<wxString> before;
        for( const auto& field : fields ) before.push_back( field.read() );
        bool colorsChanged = false;
        for( size_t i = 0; i < fields.size(); ++i )
            if( before[i] != wxString::FromUTF8( values[i] ) )
            {
                fields[i].write( wxString::FromUTF8( values[i] ), true );
                colorsChanged |= fields[i].kind == KSCH_PROPERTY_COLOR;
            }
        auto* common = Pgm().GetCommonSettings();
        auto* editor = GetAppSettings<EESCHEMA_SETTINGS>( "eeschema" );
        auto& manager = Pgm().GetSettingsManager();
        const wxString commonPath = manager.GetPathForSettingsFile( common );
        const wxString editorPath = manager.GetPathForSettingsFile( editor );
        if( !common->SaveToFile( commonPath, true ) || !editor->SaveToFile( editorPath, true ) )
        {
            for( size_t i = 0; i < fields.size(); ++i ) fields[i].write( before[i], true );
            common->SaveToFile( commonPath, true );
            editor->SaveToFile( editorPath, true );
            setError( session, "Could not save application preferences" ); return KSCH_ERR_IO;
        }
        if( colorsChanged )
            manager.SaveColorSettings( GetColorSettings( editor->m_ColorTheme ), "schematic" );
        auto& render = session->m_Host.RenderSettings();
        render.LoadColors( GetColorSettings( editor->m_ColorTheme ) );
        session->m_Host.Gal().SetClearColor( render.GetBackgroundColor() );
        render.m_ShowHiddenPins = editor->m_Appearance.show_hidden_pins;
        render.m_ShowHiddenFields = editor->m_Appearance.show_hidden_fields;
        render.m_ShowPinAltIcons = editor->m_Appearance.show_pin_alt_icons;
        render.m_ShowRemappedPinNumbers = editor->m_Appearance.show_remapped_pin_numbers;
        session->m_Host.View().SetLayerVisible( LAYER_ERC_ERR, editor->m_Appearance.show_erc_errors );
        session->m_Host.View().SetLayerVisible( LAYER_ERC_WARN, editor->m_Appearance.show_erc_warnings );
        session->m_Host.View().SetLayerVisible( LAYER_ERC_EXCLUSION, editor->m_Appearance.show_erc_exclusions );
        session->m_Host.View().SetLayerVisible( LAYER_NETCLASS_REFS, editor->m_Appearance.show_directive_labels );
        session->m_Host.View().UpdateAllItems( KIGFX::ALL );
        session->m_Host.RefreshCanvas();
        return KSCH_OK;
    } );
}
