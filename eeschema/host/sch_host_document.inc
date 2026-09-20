// SPDX-License-Identifier: GPL-3.0-or-later
// Included by the ABI translation unit to share its exception and session lifetime guards.
#include <sch_reference_list.h>
#include <schematic_settings.h>
#include <sch_page_settings_undo.h>
#include <sch_view.h>
#include <wx/tokenzr.h>
#include <project/project_file.h>
#include <sch_plotter.h>
#include <page_info.h>
#include <title_block.h>
#include <reporter.h>
#include <netlist_exporters/netlist_exporter_kicad.h>
#include <netlist_exporters/netlist_exporter_xml.h>
#include <netlist_exporters/netlist_exporter_spice.h>
#include <netlist_exporters/netlist_exporter_cadstar.h>
#include <netlist_exporters/netlist_exporter_orcadpcb2.h>
#include <netlist_exporters/netlist_exporter_pads.h>
#include <netlist_exporters/netlist_exporter_allegro.h>

#include "sch_host_document_setup.inc"

namespace
{
using DOCUMENT_FIELDS = std::vector<std::pair<wxString, wxString>>;

SCH_REFERENCE_LIST documentReferences( SCH_HOST& host )
{
    SCH_REFERENCE_LIST refs;
    host.GetSchematic()->Hierarchy().GetSymbols( refs, SYMBOL_FILTER_ALL, true );
    // R-tree item order changes after commits; table indices must identify stable instances.
    std::vector<SCH_REFERENCE> ordered;
    for( size_t i = 0; i < refs.GetCount(); ++i ) ordered.push_back( refs[i] );
    std::sort( ordered.begin(), ordered.end(), []( const auto& a, const auto& b ) {
        const auto aPath = a.GetSheetPath().PathAsString();
        const auto bPath = b.GetSheetPath().PathAsString();
        if( aPath != bPath ) return aPath < bPath;
        return a.GetSymbol()->m_Uuid < b.GetSymbol()->m_Uuid;
    } );
    refs.Clear();
    for( const auto& ref : ordered ) refs.AddItem( ref );
    refs.SetRefDesTracker( host.GetSchematic()->Settings().m_refDesTracker );
    return refs;
}

// Detect deletion, hierarchy changes and symbol replacement while the table is open.
wxString documentIdentity( const SCH_REFERENCE_LIST& refs )
{
    wxString value;
    for( size_t i = 0; i < refs.GetCount(); ++i )
    {
        value += refs[i].GetSymbol()->m_Uuid.AsString() + wxS( ":" )
                 + refs[i].GetSheetPath().PathAsString() + wxS( ";" );
        for( const auto& field : refs[i].GetSymbol()->GetFields() )
            value += wxString::Format( "%zu:", field.GetName().length() ) + field.GetName();
    }
    return value;
}

DOCUMENT_FIELDS documentFields( ksch_session* session, uint32_t kind )
{
    auto& host = session->m_Host;
    if( kind >= 13 ) return setupDocumentFields( session, kind );
    auto refs = documentReferences( host );
    auto* screen = host.GetScreen();
    switch( kind )
    {
    case 0: return { { "First number", "1" }, { "Order (x/y)", "x" },
                     { "Numbering (incremental/100/1000)", "incremental" },
                     { "Reset existing (yes/no)", "no" }, { "Scope", "all" } };
    case 1: return { { "Increment", "1" }, { "Scope", "all" } };
    case 2:
    {
        DOCUMENT_FIELDS fields = { { "Paper (A0-A5, A-E, Letter, Legal)", screen->GetPageSettings().GetTypeAsString() },
                     { "Portrait (yes/no)", screen->GetPageSettings().IsPortrait() ? "yes" : "no" },
                     { "Title", screen->GetTitleBlock().GetTitle() },
                     { "Date", screen->GetTitleBlock().GetDate() },
                     { "Revision", screen->GetTitleBlock().GetRevision() },
                     { "Company", screen->GetTitleBlock().GetCompany() } };
        for( int i = 0; i < 9; ++i )
            fields.emplace_back( wxString::Format( "Comment %d", i+1 ), screen->GetTitleBlock().GetComment( i ) );
        fields.emplace_back( "Apply to all sheets (yes/no)", "no" );
        fields.emplace_back( "Sheet identity", host.GetCurrentSheet().PathAsString() );
        fields.emplace_back( "Sheet page number", host.GetCurrentSheet().GetPageNumber() );
        return fields;
    }
    case 3: return { { "Output file", "" }, { "Format (kicad/xml/spice/cadstar/orcad/pads/allegro)", "kicad" } };
    case 4: return { { "Output directory", "" }, { "Format (pdf/svg/dxf/ps/hpgl/png)", "pdf" },
                     { "All sheets (yes/no)", "yes" }, { "Black and white (yes/no)", "no" },
                     { "Drawing sheet (yes/no)", "yes" } };
    case 5: return { { "PDF output directory", "" }, { "Format", "pdf" },
                     { "All sheets (yes/no)", "yes" }, { "Black and white (yes/no)", "yes" },
                     { "Drawing sheet (yes/no)", "yes" } };
    case 6:
    {
        wxString names;
        for( const auto& preset : host.GetSchematic()->Settings().m_BomPresets )
        { if( !names.empty() ) names += ", "; names += preset.name; }
        return { { "CSV output file", "" }, { "Include excluded symbols (yes/no)", "no" },
                     { "Column preset", "standard" }, { "Custom columns (comma separated)", "Reference,Value,Footprint,Quantity" },
                     { "Group identical components (yes/no)", "yes" }, { "Exclude DNP (yes/no)", "no" },
                     { "Saved preset name", "" }, { "Save these options as preset", "" }, { "Available saved presets", names } };
    }
    case 7:
    case 12:
    {
        DOCUMENT_FIELDS fields = { { "Document identity (do not edit)", documentIdentity( refs ) } };
        for( size_t i = 0; i < refs.GetCount(); ++i )
        {
            auto& ref = refs[i];
            auto* symbol = ref.GetSymbol();
            auto label = symbol->GetRef( &ref.GetSheetPath() );
            if( kind == 12 )
                fields.emplace_back( label + " Library ID", symbol->GetLibId().Format() );
            else
            {
                for( auto& field : symbol->GetFields() )
                    if( field.GetId() != FIELD_T::REFERENCE )
                        fields.emplace_back( label + " / " + field.GetName(), field.GetText( &ref.GetSheetPath(), host.GetSchematic()->GetCurrentVariant() ) );
            }
        }
        return fields;
    }
    case 8: case 10:
        return { { "Existing library ID", "" }, { "Replacement library ID", "" } };
    case 11:
    {
        DOCUMENT_FIELDS fields = { { "Existing library ID", "" }, { "Replacement library ID", "" } };
        std::set<wxString> inspected;
        for( size_t i = 0; i < refs.GetCount(); ++i )
        {
            auto* symbol = refs[i].GetSymbol();
            const wxString id = symbol->GetLibId().Format();
            if( !inspected.insert( id ).second ) continue;
            wxString issue;
            try
            {
                auto* adapter = symbolLibraries( session );
                adapter->LoadOne( symbol->GetLibId().GetLibNickname() );
                auto* library = adapter->LoadSymbol( symbol->GetLibId() );
                if( !library ) issue = "Library symbol missing";
                else if( !symbol->GetLibSymbolRef() ) issue = "Schematic symbol definition missing";
                else if( symbol->GetLibSymbolRef()->Compare( *library->Flatten() ) != 0 ) issue = "Cached definition differs from library";
            }
            catch( ... ) { issue = "Library unavailable; cached schematic definition retained"; }
            if( !issue.empty() ) fields.emplace_back( id, issue );
        }
        if( fields.size() == 2 ) fields.emplace_back( "Inspection", "No missing or conflicting symbol definitions found" );
        return fields;
    }
    case 9: return { { "Library ID (empty = all)", "" } };
    default: throw std::invalid_argument( "Unknown document workflow" );
    }
}

wxString csvCell( wxString value )
{
    value.Replace( "\"", "\"\"" );
    return "\"" + value + "\"";
}
}

extern "C" ksch_status ksch_session_document_workflow( ksch_session* session, uint32_t kind,
                                                       ksch_document_visitor visitor, void* context )
{
    if( !session || !visitor || kind > 23 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        for( const auto& [name, value] : documentFields( session, kind ) )
            visitor( context, name.utf8_string().c_str(), value.utf8_string().c_str() );
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_document_workflow( ksch_session* session, uint32_t kind,
                                                             const char* const* values, uint32_t count )
{
    if( !session || !values || kind > 23 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status {
        auto& host = session->m_Host;
        if( !host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto invalid = [&]( const wxString& message ) { setError( session, message ); return KSCH_ERR_INVALID_ARG; };
        auto fields = documentFields( session, kind );
        if( count != fields.size() ) return invalid( "Document changed; reopen this dialog" );
        std::vector<wxString> v;
        for( uint32_t i = 0; i < count; ++i )
        {
            if( !values[i] ) return KSCH_ERR_INVALID_ARG;
            v.push_back( wxString::FromUTF8( values[i] ) );
        }
        if( kind >= 13 ) return applySetupDocument( session, kind, v );
        auto boolean = [&]( size_t i ) { return v[i] == "yes" || v[i] == "no"; };
        auto refs = documentReferences( host );
        if( kind == 0 || kind == 1 )
        {
            if( !host.GetSchematic()->GetCurrentVariant().empty() )
                return invalid( "Annotation requires the base variant" );
            long number;
            if( !v[0].ToLong( &number ) || number < (kind == 0 ? 1 : -1000000) || number > 1000000 )
                return invalid( "Enter a valid reference number or increment" );
            if( kind == 0 && ( (v[1] != "x" && v[1] != "y") || !boolean( 3 )
                    || (v[2] != "incremental" && v[2] != "100" && v[2] != "1000") ) )
                return invalid( "Choose x/y, incremental/100/1000, and yes/no" );
            const auto scope = v[kind == 0 ? 4 : 1];
            if( scope != "all" && scope != "sheet" && scope != "selection" )
                return invalid( "Choose all sheets, current sheet, or selected symbols" );
            SCH_REFERENCE_LIST additional;
            if( scope != "all" )
            {
                SCH_REFERENCE_LIST selected;
                for( size_t i = 0; i < refs.GetCount(); ++i )
                {
                    bool include = refs[i].GetSheetPath() == host.GetCurrentSheet();
                    if( scope == "selection" ) include = include && refs[i].GetSymbol()->IsSelected();
                    (include ? selected : additional).AddItem( refs[i] );
                }
                refs = selected;
                refs.SetRefDesTracker( host.GetSchematic()->Settings().m_refDesTracker );
            }
            if( refs.GetCount() == 0 ) return invalid( "No symbols in this scope" );
            std::vector<wxString> incremented;
            if( kind == 1 )
            {
                for( size_t i = 0; i < refs.GetCount(); ++i )
                {
                    auto ref = refs[i].GetSymbol()->GetRef( &refs[i].GetSheetPath() );
                    size_t split = ref.length();
                    while( split && wxIsdigit( ref[split-1] ) ) --split;
                    long old;
                    if( split == ref.length() || !ref.Mid( split ).ToLong( &old ) || old + number < 1 )
                        return invalid( "All symbols must be annotated and remain positive" );
                    const auto next = ref.Left( split ) + wxString::Format( "%ld", old + number );
                    for( size_t j = 0; j < additional.GetCount(); ++j )
                        if( additional[j].GetSymbol()->GetRef( &additional[j].GetSheetPath() ) == next )
                            return invalid( "Increment would collide with a reference outside the scope" );
                    incremented.push_back( next );
                }
            }
            SCH_COMMIT commit( host.GetToolManager() );
            for( size_t i = 0; i < refs.GetCount(); ++i )
                commit.Modify( refs[i].GetSymbol(), refs[i].GetSheetPath().LastScreen(), RECURSE_MODE::NO_RECURSE );
            if( kind == 1 )
                for( size_t i = 0; i < refs.GetCount(); ++i )
                    refs[i].GetSymbol()->SetRef( &refs[i].GetSheetPath(), incremented[i] );
            else
            {
                const auto order = v[1] == "x" ? SORT_BY_X_POSITION : SORT_BY_Y_POSITION;
                const auto algo = v[2] == "100" ? SHEET_NUMBER_X_100 : v[2] == "1000" ? SHEET_NUMBER_X_1000 : INCREMENTAL_BY_REF;
                if( v[3] == "yes" )
                    refs.ReannotateByOptions( order, algo, number - 1, additional, false, nullptr );
                else
                {
                    refs.SplitReferences();
                    refs.AnnotateByOptions( order, algo, number - 1, {}, additional, false );
                }
                refs.UpdateAnnotation();
            }
            commit.Push( kind == 0 ? "Annotate Schematic" : "Increment Annotations" );
        }
        else if( kind == 2 )
        {
            if( v[16] != host.GetCurrentSheet().PathAsString() )
                return invalid( "Current sheet changed; reopen Page Settings" );
            if( v[17].empty() || std::any_of(v[17].begin(),v[17].end(),[](wxUniChar c){return !wxIsalnum(c);}) )
                return invalid("Sheet page number must contain letters and numbers only");
            PAGE_INFO page;
            if( !boolean( 1 ) || !boolean( 15 ) || !page.SetType( v[0], v[1] == "yes" ) )
                return invalid( "Invalid paper size or portrait setting" );
            auto title = host.GetScreen()->GetTitleBlock();
            title.SetTitle( v[2] ); title.SetDate( v[3] ); title.SetRevision( v[4] ); title.SetCompany( v[5] );
            for( int i = 0; i < 9; ++i ) title.SetComment( i, v[6+i] );
            PICKED_ITEMS_LIST undo;
            undo.PushItem( ITEM_PICKER( host.GetScreen(),
                    new SCH_PAGE_SETTINGS_UNDO_ITEM( *host.GetSchematic(), host.GetScreen(), v[15] == "yes" ),
                    UNDO_REDO::PAGESETTINGS ) );
            host.SaveCopyInUndoList( undo, UNDO_REDO::PAGESETTINGS, false );
            for( auto& sheet : host.GetSchematic()->Hierarchy() )
            {
                if( v[15] == "yes" || sheet.LastScreen() == host.GetScreen() )
                {
                    sheet.LastScreen()->SetPageSettings( page );
                    sheet.LastScreen()->SetTitleBlock( title );
                }
            }
            auto path=host.GetCurrentSheet(); path.SetPageNumber(v[17]);
            host.GetSchematic()->RefreshHierarchy();
            host.View().UpdateAllItems( KIGFX::ALL );
            host.OnModify(); host.RefreshCanvas();
        }
        else if( kind == 3 )
        {
            if( v[0].empty() ) return invalid( "Choose an output file" );
            std::unique_ptr<NETLIST_EXPORTER_BASE> exporter;
            auto* schematic = host.GetSchematic();
            if( v[1] == "kicad" ) exporter = std::make_unique<NETLIST_EXPORTER_KICAD>( schematic );
            else if( v[1] == "xml" ) exporter = std::make_unique<NETLIST_EXPORTER_XML>( schematic );
            else if( v[1] == "spice" ) exporter = std::make_unique<NETLIST_EXPORTER_SPICE>( schematic );
            else if( v[1] == "cadstar" ) exporter = std::make_unique<NETLIST_EXPORTER_CADSTAR>( schematic );
            else if( v[1] == "orcad" ) exporter = std::make_unique<NETLIST_EXPORTER_ORCADPCB2>( schematic );
            else if( v[1] == "pads" ) exporter = std::make_unique<NETLIST_EXPORTER_PADS>( schematic );
            else if( v[1] == "allegro" ) exporter = std::make_unique<NETLIST_EXPORTER_ALLEGRO>( schematic );
            else return invalid( "Unknown netlist format" );
            WX_STRING_REPORTER reporter;
            if( !exporter->WriteNetlist( v[0], 0, reporter ) ) return invalid( reporter.GetMessages() );
        }
        else if( kind == 4 || kind == 5 )
        {
            if( v[0].empty() || !boolean( 2 ) || !boolean( 3 ) || !boolean( 4 ) )
                return invalid( "Choose an output directory and yes/no options" );
            PLOT_FORMAT format;
            if( v[1] == "pdf" ) format = PLOT_FORMAT::PDF;
            else if( v[1] == "svg" ) format = PLOT_FORMAT::SVG;
            else if( v[1] == "dxf" ) format = PLOT_FORMAT::DXF;
            else if( v[1] == "ps" ) format = PLOT_FORMAT::POST;
            else if( v[1] == "hpgl" ) format = PLOT_FORMAT::HPGL;
            else if( v[1] == "png" ) format = PLOT_FORMAT::PNG;
            else return invalid( "Unknown plot format" );
            if( kind == 5 && format != PLOT_FORMAT::PDF ) return invalid( "Print output must use PDF" );
            SCH_PLOT_OPTS opts;
            opts.m_outputDirectory = v[0]; opts.m_plotAll = v[2] == "yes";
            opts.m_blackAndWhite = v[3] == "yes"; opts.m_plotDrawingSheet = v[4] == "yes";
            opts.m_variant = host.GetSchematic()->GetCurrentVariant();
            if( !opts.m_plotAll ) opts.m_plotPages.push_back( host.GetCurrentSheet().GetPageNumber() );
            WX_STRING_REPORTER reporter;
            SCH_PLOTTER plotter( host.GetSchematic() );
            plotter.Plot( format, opts, host.GetRenderSettings(), &reporter );
            if( plotter.GetOutputFilePaths().empty() ) return invalid( reporter.GetMessages() );
        }
        else if( kind == 6 )
        {
            if( v[0].empty() || !boolean( 1 ) || !boolean( 4 ) || !boolean( 5 ) )
                return invalid( "Choose a CSV output file and valid options" );
            wxString columnsText;
            std::set<wxString> groupColumns;
            auto displayColumn = []( const wxString& name ) {
                if( name == "${QUANTITY}" ) return wxString( "Quantity" );
                if( name == "${DNP}" ) return wxString( "DNP" );
                return name;
            };
            if( !v[7].empty() && host.GetSchematic()->Project().GetProjectDirectory().empty() )
                return invalid( "Open a saved KiCad project before saving a BOM preset" );
            if( v[2] == "standard" ) columnsText = "Reference,Value,Footprint,Quantity";
            else if( v[2] == "manufacturing" ) columnsText = "Reference,Value,Footprint,Datasheet,Manufacturer,MPN,Quantity,DNP";
            else if( v[2] == "custom" ) columnsText = v[3];
            else if( v[2] == "saved" )
            {
                const auto& presets = host.GetSchematic()->Settings().m_BomPresets;
                const auto found = std::find_if( presets.begin(), presets.end(), [&]( const auto& item ) { return item.name == v[6]; } );
                if( found == presets.end() ) return invalid( "Saved BOM preset not found" );
                for( const auto& field : found->fieldsOrdered )
                {
                    const auto name = displayColumn( field.name );
                    if( field.show ) { if( !columnsText.empty() ) columnsText += ","; columnsText += name; }
                    if( field.groupBy ) groupColumns.insert( name );
                }
                v[1] = found->includeExcludedFromBOM ? "yes" : "no";
                v[4] = found->groupSymbols ? "yes" : "no";
                v[5] = found->excludeDNP ? "yes" : "no";
            }
            else return invalid( "Choose a column preset" );
            std::vector<wxString> columns;
            wxStringTokenizer tokenizer( columnsText, ",", wxTOKEN_RET_EMPTY );
            while( tokenizer.HasMoreTokens() )
            {
                auto column = tokenizer.GetNextToken().Trim().Trim( false );
                if( column.empty() || std::find( columns.begin(), columns.end(), column ) != columns.end() )
                    return invalid( "Column names must be nonempty and unique" );
                columns.push_back( column );
            }
            if( columns.empty() ) return invalid( "Choose at least one CSV column" );
            if( !v[7].empty() )
                for( const auto& preset : host.GetSchematic()->Settings().m_BomPresets )
                    if( preset.name == v[7] && preset.readOnly ) return invalid( "This preset is read-only" );
            struct BOM_ROW { std::vector<wxString> cells; std::vector<wxString> references; };
            std::map<wxString, BOM_ROW> rows;
            std::set<wxString> emitted;
            const auto variant = host.GetSchematic()->GetCurrentVariant();
            for( size_t i = 0; i < refs.GetCount(); ++i )
            {
                auto& ref = refs[i]; auto* symbol = ref.GetSymbol();
                auto reference = symbol->GetRef( &ref.GetSheetPath() );
                const bool dnp = ref.GetSymbolDNP( variant );
                if( (v[1] == "no" && ref.GetSymbolExcludedFromBOM( variant )) || (v[5] == "yes" && dnp)
                        || !emitted.insert( reference ).second ) continue;
                std::vector<wxString> cells;
                wxString key = v[4] == "yes" ? wxString() : reference;
                for( const auto& column : columns )
                {
                    wxString value;
                    if( column == "Library ID" ) value = symbol->GetLibId().Format();
                    else if( column == "DNP" ) value = dnp ? "yes" : "no";
                    else if( column != "Reference" && column != "Quantity" )
                        if( auto* field = symbol->GetField( column ) ) value = field->GetText( &ref.GetSheetPath(), variant );
                    cells.push_back( value );
                    if( column != "Reference" && column != "Quantity"
                            && (v[2] != "saved" || groupColumns.count( column )) )
                        key += wxString::Format( "%zu:", value.length() ) + value;
                }
                auto& row = rows[key]; row.cells = cells; row.references.push_back( reference );
            }
            std::ofstream output( v[0].utf8_string(), std::ios::binary );
            if( !output ) return invalid( "Cannot open CSV output file" );
            for( size_t i = 0; i < columns.size(); ++i )
                output << ( i ? "," : "" ) << csvCell( columns[i] ).utf8_string();
            output << "\r\n";
            for( auto& [key, row] : rows )
            {
                std::sort( row.references.begin(), row.references.end() );
                wxString references;
                for( const auto& ref : row.references ) { if( !references.empty() ) references += ", "; references += ref; }
                for( size_t i = 0; i < columns.size(); ++i )
                {
                    if( columns[i] == "Reference" ) row.cells[i] = references;
                    if( columns[i] == "Quantity" ) row.cells[i] = wxString::Format( "%zu", row.references.size() );
                    output << ( i ? "," : "" ) << csvCell( row.cells[i] ).utf8_string();
                }
                output << "\r\n";
            }
            output.flush();
            if( !output ) return invalid( "Writing CSV failed" );
            if( !v[7].empty() )
            {
                auto& settings = host.GetSchematic()->Settings();
                const auto oldPresets = settings.m_BomPresets;
                BOM_PRESET preset;
                preset.name = v[7]; preset.groupSymbols = v[4] == "yes";
                preset.excludeDNP = v[5] == "yes"; preset.includeExcludedFromBOM = v[1] == "yes";
                for( const auto& column : columns )
                    preset.fieldsOrdered.push_back( { column == "Quantity" ? wxString( "${QUANTITY}" ) : column == "DNP" ? wxString( "${DNP}" ) : column,
                                                     column, true, column != "Reference" && column != "Quantity" } );
                auto found = std::find_if( settings.m_BomPresets.begin(), settings.m_BomPresets.end(), [&]( const auto& item ) { return item.name == preset.name; } );
                if( found == settings.m_BomPresets.end() ) settings.m_BomPresets.push_back( preset );
                else *found = preset;
                settings.SaveToFile();
                auto& project = host.GetSchematic()->Project();
                if( !project.GetProjectFile().SaveToFile( project.GetProjectDirectory(), true ) )
                {
                    settings.m_BomPresets = oldPresets; settings.SaveToFile();
                    return invalid( "CSV exported, but saving the project BOM preset failed" );
                }
            }
        }
        else
        {
            if( ( kind == 7 || kind == 12 ) && v[0] != documentIdentity( refs ) )
                return invalid( "Document changed; reopen the table" );
            // Load and validate every replacement before touching a model object.
            std::vector<std::pair<LIB_ID, std::unique_ptr<LIB_SYMBOL>>> replacements( refs.GetCount() );
            std::vector<bool> change( refs.GetCount(), false );
            for( size_t i = 0; i < refs.GetCount() && kind != 7; ++i )
            {
                auto* symbol = refs[i].GetSymbol();
                wxString target;
                if( kind == 12 ) target = v[i+1];
                else if( kind == 9 && (v[0].empty() || wxString( symbol->GetLibId().Format() ) == v[0]) ) target = symbol->GetLibId().Format();
                else if( kind != 9 && wxString( symbol->GetLibId().Format() ) == v[0] ) target = v[1];
                else continue;
                LIB_ID id;
                if( id.Parse( target ) >= 0 || id.GetLibNickname().empty() ) return invalid( "Use a qualified Library:Symbol ID" );
                auto* adapter = symbolLibraries( session );
                adapter->LoadOne( id.GetLibNickname() );
                auto* library = adapter->LoadSymbol( id );
                if( !library ) return invalid( "Replacement symbol not found: " + target );
                if( symbol->GetUnitSelection( &refs[i].GetSheetPath() ) > library->GetUnitCount() ) return invalid( "Replacement has fewer units than this symbol" );
                replacements[i] = { id, library->Flatten() };
                change[i] = true;
            }
            // Validate every field before starting the transaction.
            size_t fieldIndex = 1;
            if( kind == 7 )
                for( size_t i = 0; i < refs.GetCount(); ++i )
                    for( auto& field : refs[i].GetSymbol()->GetFields() )
                        if( field.GetId() != FIELD_T::REFERENCE )
                        {
                            const auto& value = v[fieldIndex++];
                            const auto error = GetFieldValidationErrorMessage( field.GetId(), value );
                            if( !error.empty() ) return invalid( error );
                            change[i] = change[i] || field.GetText( &refs[i].GetSheetPath(), host.GetSchematic()->GetCurrentVariant() ) != value;
                        }
            if( std::none_of( change.begin(), change.end(), []( bool item ) { return item; } ) )
                return kind == 7 ? KSCH_OK : invalid( "No symbols match the requested library ID" );
            SCH_COMMIT commit( host.GetToolManager() );
            fieldIndex = 1;
            for( size_t i = 0; i < refs.GetCount(); ++i )
            {
                auto& ref = refs[i]; auto* symbol = ref.GetSymbol();
                if( !change[i] )
                {
                    if( kind == 7 )
                        for( const auto& field : symbol->GetFields() )
                            if( field.GetId() != FIELD_T::REFERENCE ) ++fieldIndex;
                    continue;
                }
                commit.Modify( symbol, ref.GetSheetPath().LastScreen(), RECURSE_MODE::NO_RECURSE );
                if( kind == 7 )
                {
                    for( auto& field : symbol->GetFields() )
                        if( field.GetId() != FIELD_T::REFERENCE )
                            field.SetText( v[fieldIndex++], &ref.GetSheetPath(), host.GetSchematic()->GetCurrentVariant() );
                }
                else
                {
                    symbol->SetLibId( replacements[i].first );
                    symbol->SetLibSymbol( replacements[i].second.release() );
                }
            }
            commit.Push( kind == 7 ? "Edit Symbol Fields" : "Update Symbol Libraries" );
        }
        return KSCH_OK;
    } );
}
