// SPDX-License-Identifier: GPL-3.0-or-later
// Model services for project setup and bulk schematic editing; no UI toolkit calls.
#include <bus_alias.h>
#include <project/net_settings.h>
#include <connectivity/conn_facade.h>
#include <connectivity/conn_netchain_manager.h>
#include <sch_netchain.h>
#include <connection_graph.h>
#include <sch_connection.h>
#include <sch_line.h>
#include <sch_shape.h>
#include <sch_sheet.h>
#include <cmath>
#include <pcm.h>
#include <pcm_task_manager.h>
#include "sch_host_backannotation.inc"
#include "sch_host_sheet_import.inc"

namespace
{
using SETUP_FIELDS = std::vector<std::pair<wxString, wxString>>;
wxString setupJoined( const auto& values )
{
    wxString result;
    for( const auto& value : values ) { if( !result.empty() ) result += ", "; result += value; }
    return result;
}

struct HOST_BUS_MIGRATION
{
    wxString identity, description, proposal;
    SCH_SCREEN* screen;
    std::vector<SCH_ITEM*> labels;
};
std::vector<HOST_BUS_MIGRATION> migrationGroups( SCHEMATIC& schematic )
{
    std::vector<HOST_BUS_MIGRATION> result;
    std::set<wxString> seen;
    for( const auto& sheet : schematic.Hierarchy() )
    for( auto* candidate : sheet.LastScreen()->Items() )
    {
        if( !dynamic_cast<SCH_LABEL_BASE*>(candidate) ) continue;
        const auto group=schematic.Connectivity().GetSubgraphForItem(candidate->m_Uuid,sheet.Path());
        if( !group || !group->IsBus() ) continue;
        std::vector<SCH_ITEM*> labels;
        std::set<wxString> names;
        for(auto* item:group->Items())
        {
            auto* label=dynamic_cast<SCH_LABEL_BASE*>(item); if(!label) continue;
            SCH_CONNECTION connection(schematic.ConnectionGraph()); connection.ConfigureFromLabel(label->GetText());
            if(connection.Type()!=CONNECTION_TYPE::BUS) continue;
            labels.push_back(label); names.insert(label->GetText());
        }
        if(names.size()<2) continue;
        std::sort(labels.begin(),labels.end(),[](const auto* a,const auto* b){return a->m_Uuid < b->m_Uuid;});
        wxString identity=sheet.PathAsString(), description=sheet.PathHumanReadable()+" / ", prefix;
        long first=LONG_MAX,last=LONG_MIN;
        for(auto* item:labels)
        {
            auto* label=static_cast<SCH_LABEL_BASE*>(item);
            SCH_CONNECTION connection(schematic.ConnectionGraph()); connection.ConfigureFromLabel(label->GetText());
            if(prefix.empty()) prefix=connection.VectorPrefix();
            first=std::min(first,connection.VectorStart()); last=std::max(last,connection.VectorEnd());
            identity += ":"+label->m_Uuid.AsString(); description += label->GetText()+" ";
        }
        if(seen.insert(identity).second)
            result.push_back({identity,description,prefix+wxString::Format("[%ld..%ld]",first,last),sheet.LastScreen(),labels});
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.identity < b.identity;});
    return result;
}

struct HOST_FIELD_CONFLICT
{
    SCH_SYMBOL* symbol;
    SCH_SCREEN* screen;
    wxString identity, description;
    std::vector<std::pair<wxString,wxString>> fields;
};
std::vector<HOST_FIELD_CONFLICT> fieldCaseConflicts(SCHEMATIC& schematic)
{
    std::vector<HOST_FIELD_CONFLICT> result;
    std::set<KIID> visited;
    for(const auto& sheet:schematic.Hierarchy())
        for(auto* item:sheet.LastScreen()->Items().OfType(SCH_SYMBOL_T))
        {
            auto* symbol=static_cast<SCH_SYMBOL*>(item);
            if(!visited.insert(symbol->m_Uuid).second)continue;
            std::map<wxString,std::vector<std::pair<wxString,wxString>>> groups;
            for(const auto& field:symbol->GetFields())
                if(!field.IsMandatory())groups[field.GetName().Lower()].emplace_back(field.GetName(),field.GetText());
            for(const auto& [key,fields]:groups)
            {
                if(fields.size()<2)continue;
                wxString identity=symbol->m_Uuid.AsString()+":"+key;
                wxString description=symbol->GetRef(&sheet)+" / "+sheet.PathHumanReadable();
                for(const auto& [name,value]:fields)
                {
                    identity+=wxString::Format("%zu:",name.length())+name+wxString::Format("%zu:",value.length())+value;
                    description+="\n"+name+" = "+value;
                }
                result.push_back({symbol,sheet.LastScreen(),identity,description,fields});
            }
        }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return a.identity<b.identity;});
    return result;
}

SETUP_FIELDS setupDocumentFields( ksch_session* session, uint32_t kind )
{
    auto& host = session->m_Host;
    auto& schematic = *host.GetSchematic();
    SETUP_FIELDS fields;
    if( kind == 23 )
        fields={{"Import mode","sheet"},{"Source schematic (.kicad_sch)",""},{"Sheet name","Imported"},
                {"X position / append offset (mm)","25"},{"Y position / append offset (mm)","25"},
                {"Keep annotations","no"},{"Group appended items","yes"}};
    else if( kind == 13 )
    {
        fields = { { "Operation", "save" }, { "Alias name", "" }, { "Members (comma separated)", "" } };
        for( const auto& alias : schematic.GetAllBusAliases() )
            fields.emplace_back( alias->GetName(), setupJoined( alias->Members() ) );
    }
    else if( kind == 14 )
    {
        fields = { { "Operation", "save" }, { "Netclass name", "Default" }, { "Wire width (mm)", "0.1524" },
                   { "Bus width (mm)", "0.3048" }, { "Track width (mm)", "0.25" }, { "Clearance (mm)", "0.2" },
                   { "Via diameter (mm)", "0.6" }, { "Via drill (mm)", "0.3" }, { "Net name pattern (optional)", "" } };
        auto ns = schematic.Project().GetProjectFile().NetSettings();
        auto classes = ns->GetNetclasses(); classes["Default"] = ns->GetDefaultNetclass();
        for( const auto& [name, nc] : classes )
            fields.emplace_back( name, wxString::Format( "Wire %.4g mm; bus %.4g mm; track %.4g mm; clearance %.4g mm; via %.4g/%.4g mm",
                nc->GetWireWidth()/10000.0, nc->GetBusWidth()/10000.0, nc->GetTrackWidth()/1000000.0,
                nc->GetClearance()/1000000.0, nc->GetViaDiameter()/1000000.0, nc->GetViaDrill()/1000000.0 ) );
        for( const auto& [pattern, name] : ns->GetNetclassPatternAssignments() )
            fields.emplace_back( "Pattern " + pattern->GetPattern(), name );
    }
    else if( kind == 15 )
    {
        fields = { { "Operation", "save" }, { "Existing chain name", "" }, { "New name (empty = unchanged)", "" },
                   { "Chain class", "" }, { "Netclass override", "" }, { "Color (#RRGGBB, empty = default)", "" } };
        for( const auto& chain : schematic.NetChains().GetCommittedNetChains() )
            fields.emplace_back( chain->GetName(), setupJoined( chain->GetNets() ) + " | netclass: " + chain->GetNetClass()
                + " | class: " + schematic.Project().GetProjectFile().NetSettings()->GetNetChainClass(chain->GetName()) );
    }
    else if( kind == 16 )
    {
        fields = { { "Chain name", "" }, { "Member nets (comma separated)", "" }, { "Netclass override", "" }, { "Chain class", "" },
                   { "From reference", "" }, { "From pin number", "" }, { "To reference", "" }, { "To pin number", "" } };
        for( const auto& net : schematic.Connectivity().NetNames() ) fields.emplace_back( "Available net", net );
    }
    else if( kind == 17 )
        fields = { { "Scope", "all" }, { "Targets", "both" }, { "Text size (mm, empty = unchanged)", "" },
                   { "Line width (mm, empty = unchanged)", "" }, { "Bold", "keep" }, { "Italic", "keep" } };
    else if( kind == 18 )
        fields = { { "Source KiCad project (.kicad_pro)", "" }, { "Schematic settings and ERC", "yes" },
                   { "Netclasses and assignments", "yes" }, { "Bus aliases", "yes" } };
    else if( kind == 19 )
    {
        const auto groups=migrationGroups(schematic);
        fields.emplace_back("Migration status", groups.empty() ? "No conflicting legacy bus labels found" : "Choose one vector label for each connected bus");
        for(const auto& group:groups) {fields.emplace_back("Bus identity",group.identity);fields.emplace_back(group.description,group.proposal);}
    }
    else if( kind == 20 )
        fields = { {"Operation","preview"}, {"KiCad PCB file or PCB netlist",""},
                   {"Match by reference","no"}, {"Update footprints","yes"}, {"Update values","yes"},
                   {"Update references","no"}, {"Update net names","no"}, {"Update attributes","yes"},
                   {"Update other fields","no"}, {"Prefer unit swaps","yes"}, {"Prefer pin swaps","yes"},
                   {"Last report",wxString::FromUTF8(session->m_DocumentReport)} };
    else if( kind == 21 )
    {
        const auto conflicts=fieldCaseConflicts(schematic);
        fields={{"Status",conflicts.empty()?wxString("No conflicting user field names"):wxString::Format("%zu field-name conflicts",conflicts.size())},{"Join separator","; "}};
        for(const auto& conflict:conflicts)
        {fields.emplace_back("Conflict identity",conflict.identity);fields.emplace_back("Fields",conflict.description);fields.emplace_back("Resolution","first");}
    }
    else if(kind==22)
    {
        fields={{"Operation","refresh"},{"Data-source archive (.zip)",""},{"Installed package identifier",""},{"Replace existing version","no"},{"Last report",wxString::FromUTF8(session->m_DocumentReport)}};
        auto pcm=std::make_shared<PLUGIN_CONTENT_MANAGER>([](int){});
        for(const auto& entry:pcm->GetInstalledPackages())
            if(entry.package.type==PT_DATASOURCE)
                fields.emplace_back(entry.package.identifier,entry.package.name+" / "+entry.current_version+" / "+entry.repository_name);
        if(fields.size()==5)fields.emplace_back("Installed sources","No schematic data sources installed");
    }
    return fields;
}

ksch_status applySetupDocument( ksch_session* session, uint32_t kind, const std::vector<wxString>& v )
{
    if( kind == 23 ) return importDocumentSheet( session, v );
    auto& host = session->m_Host;
    auto& schematic = *host.GetSchematic();
    auto& project = schematic.Project();
    auto ns = project.GetProjectFile().NetSettings();
    auto invalid = [&]( const wxString& message ) { setError( session, message ); return KSCH_ERR_INVALID_ARG; };
    auto saveProject = [&]() {
        schematic.Settings().SaveToFile();
        schematic.ErcSettings().SaveToFile();
        ns->SaveToFile();
        return project.GetProjectFile().SaveToFile( project.GetProjectDirectory(), true );
    };
    if( (kind == 13 || kind == 14 || kind == 15 || kind == 16 || kind == 18) && project.GetProjectDirectory().empty() )
        return invalid( "Open a saved KiCad project before changing project setup" );
    if( kind == 13 )
    {
        if( v[0] != "save" && v[0] != "delete" ) return invalid( "Choose Save or Delete" );
        if( v[1].empty() || v[1].find_first_of( " \t\r\n{},[]" ) != wxString::npos )
            return invalid( "Bus alias name cannot be empty or contain whitespace, braces, brackets or commas" );
        auto aliases = schematic.GetAllBusAliases();
        const auto original = aliases;
        auto found = std::find_if( aliases.begin(), aliases.end(), [&]( const auto& alias ) { return alias->GetName() == v[1]; } );
        if( v[0] == "delete" )
        {
            if( found == aliases.end() ) return invalid( "Bus alias not found" );
            aliases.erase( found );
        }
        else
        {
            auto alias = std::make_shared<BUS_ALIAS>(); alias->SetName( v[1] );
            wxStringTokenizer members( v[2], ",", wxTOKEN_RET_EMPTY );
            NETNAME_VALIDATOR validator;
            while( members.HasMoreTokens() )
            {
                auto member = members.GetNextToken().Trim().Trim( false );
                if( member.empty() || member == v[1] || !validator.IsValid( member ).empty() )
                    return invalid( "Members must be valid net names and cannot reference the alias itself" );
                alias->AddMember( member );
            }
            if( alias->Members().empty() ) return invalid( "Add at least one alias member" );
            if( found == aliases.end() ) aliases.push_back( alias ); else *found = alias;
            // Alias nesting must form an acyclic graph.
            std::map<wxString, std::vector<wxString>> graph;
            for( const auto& entry : aliases ) graph[entry->GetName()] = entry->Members();
            std::set<wxString> active, complete;
            std::function<bool(const wxString&)> visit = [&]( const wxString& name ) {
                if( active.count( name ) ) return false;
                if( complete.count( name ) || !graph.count( name ) ) return true;
                active.insert( name );
                for( const auto& child : graph[name] ) if( !visit( child ) ) return false;
                active.erase( name ); complete.insert( name ); return true;
            };
            for( const auto& entry : graph ) if( !visit( entry.first ) ) return invalid( "Bus aliases cannot contain cycles" );
        }
        schematic.SetBusAliases( aliases );
        if( !saveProject() ) { schematic.SetBusAliases( original ); return invalid( "Cannot save project bus aliases" ); }
        schematic.Connectivity().Recalculate( schematic, true );
    }
    else if( kind == 14 )
    {
        if( v[0] != "save" && v[0] != "delete" && v[0] != "remove-pattern" ) return invalid( "Choose Save or Delete" );
        if( v[1].empty() || v[1].find_first_of( "\r\n\t" ) != wxString::npos ) return invalid( "Enter a netclass name" );
        double sizes[6];
        for( size_t i = 0; i < 6; ++i )
            if( !v[i+2].ToDouble( &sizes[i] ) || !std::isfinite( sizes[i] ) || sizes[i] <= 0 || sizes[i] > 100 )
                return invalid( "Widths and clearances must be positive millimeters (maximum 100)" );
        if( sizes[5] > sizes[4] ) return invalid( "Via drill cannot exceed diameter" );
        NET_SETTINGS backup( nullptr, "" ); backup.CopyFrom( *ns );
        auto classes = ns->GetNetclasses();
        if( v[0] == "remove-pattern" )
        {
            auto& patterns = ns->GetNetclassPatternAssignments();
            const auto oldSize = patterns.size();
            std::erase_if( patterns, [&]( const auto& entry ) { return entry.first->GetPattern() == v[8] && entry.second == v[1]; } );
            if( patterns.size() == oldSize ) return invalid( "Pattern assignment not found" );
        }
        else if( v[0] == "delete" )
        {
            if( v[1] == "Default" || !classes.erase( v[1] ) ) return invalid( "Cannot delete the default or a missing netclass" );
            for( const auto& [pattern, name] : ns->GetNetclassPatternAssignments() )
                if( name == v[1] ) return invalid( "Remove this netclass's pattern assignments before deleting it" );
            ns->SetNetclasses( classes );
        }
        else
        {
            auto nc = v[1] == "Default" ? ns->GetDefaultNetclass() : classes.count( v[1] ) ? classes[v[1]] : std::make_shared<NETCLASS>( v[1] );
            nc->SetWireWidth( std::lround( sizes[0]*10000 ) ); nc->SetBusWidth( std::lround( sizes[1]*10000 ) );
            nc->SetTrackWidth( std::lround( sizes[2]*1000000 ) ); nc->SetClearance( std::lround( sizes[3]*1000000 ) );
            nc->SetViaDiameter( std::lround( sizes[4]*1000000 ) ); nc->SetViaDrill( std::lround( sizes[5]*1000000 ) );
            if( v[1] == "Default" ) ns->SetDefaultNetclass( nc ); else ns->SetNetclass( v[1], nc );
            if( !v[8].empty() ) ns->SetNetclassPatternAssignment( v[8], v[1] );
        }
        ns->ClearAllCaches();
        if( !saveProject() ) { ns->CopyFrom( backup ); return invalid( "Cannot save project netclasses" ); }
        schematic.Connectivity().Recalculate( schematic, true );
    }
    else if( kind == 15 || kind == 16 )
    {
        auto& manager = schematic.NetChains();
        NET_SETTINGS previousSettings( nullptr, "" );
        previousSettings.CopyFrom( *ns );
        std::unique_ptr<SCH_NETCHAIN> previousChain;
        const auto previousNetclasses = manager.GetNetChainNetClassOverrides();
        const auto previousColors = manager.GetNetChainColorOverrides();
        const auto previousTerminals = manager.GetNetChainTerminalRefOverrides();
        const auto previousMembers = manager.GetNetChainMemberNetOverrides();
        if( kind == 16 )
        {
            if( !SCH_NETCHAIN::IsValidName( v[0] ) || manager.GetNetChainByName( v[0] ) ) return invalid( "Enter a unique valid chain name" );
            if( !v[2].empty() && !ns->HasNetclass( v[2] ) ) return invalid( "Unknown netclass" );
            std::set<wxString> nets;
            wxStringTokenizer members( v[1], ",", wxTOKEN_RET_EMPTY );
            while( members.HasMoreTokens() )
            {
                auto net = members.GetNextToken().Trim().Trim( false );
                if( !schematic.Connectivity().NetByName( net ) ) return invalid( "Unknown net: " + net );
                nets.insert( net );
            }
            if( nets.size() < 2 ) return invalid( "A net chain needs at least two named nets" );
            for( size_t i : {4u,5u,6u,7u} ) if( v[i].empty() ) return invalid( "Specify both terminal references and pin numbers" );
            auto* chain = manager.CreateManualNetChain( v[0], {}, nets, niluuid, niluuid, v[4], v[5], v[6], v[7] );
            if( !chain ) return invalid( "Terminal pins could not be resolved, or a net already belongs to another chain" );
            chain->SetNetClass( v[2] ); ns->SetNetChainClass( v[0], v[3] );
        }
        else
        {
            auto* chain = manager.GetNetChainByName( v[1] );
            if( !chain ) return invalid( "Net chain not found" );
            previousChain = std::make_unique<SCH_NETCHAIN>( *chain );
            if( v[0] == "delete" ) manager.DeleteCommittedNetChain( v[1] );
            else if( v[0] == "save" )
            {
                const auto name = v[2].empty() ? v[1] : v[2];
                if( !SCH_NETCHAIN::IsValidName( name ) || (name != v[1] && manager.GetNetChainByName( name )) ) return invalid( "Enter a unique valid chain name" );
                if( !v[4].empty() && !ns->HasNetclass( v[4] ) ) return invalid( "Unknown netclass" );
                auto color = KIGFX::COLOR4D::UNSPECIFIED;
                if( !v[5].empty() && !color.SetFromHexString( v[5] ) ) return invalid( "Use a hex color such as #0080FF" );
                if( name != v[1] ) manager.RenameCommittedNetChain( v[1], name );
                chain->SetNetClass( v[4] ); chain->SetColor( color ); ns->SetNetChainClass( name, v[3] );
            }
            else return invalid( "Choose Save or Delete" );
        }
        manager.ApplyNetChainNetclasses();
        if( !saveProject() )
        {
            if( kind == 16 ) manager.DeleteCommittedNetChain( v[0] );
            else if( previousChain )
            {
                const auto& old = *previousChain;
                if( v[0] == "delete" )
                {
                    if( auto* restored = manager.CreateManualNetChain( old.GetName(), old.GetSymbols(), old.GetNets(),
                            old.GetTerminalPinA(), old.GetTerminalPinB(), old.GetTerminalRef(0), old.GetTerminalPinNum(0),
                            old.GetTerminalRef(1), old.GetTerminalPinNum(1) ) ) *restored = old;
                }
                else
                {
                    const auto currentName = v[2].empty() ? v[1] : v[2];
                    if( currentName != old.GetName() ) manager.RenameCommittedNetChain( currentName, old.GetName() );
                    if( auto* restored = manager.GetNetChainByName( old.GetName() ) ) *restored = old;
                }
            }
            manager.SetNetChainNetClassOverrides( previousNetclasses );
            manager.SetNetChainColorOverrides( previousColors );
            manager.SetNetChainTerminalRefOverrides( previousTerminals );
            manager.SetNetChainMemberNetOverrides( previousMembers );
            ns->CopyFrom( previousSettings );
            ns->SaveToFile();
            manager.ApplyNetChainNetclasses();
            return invalid( "Could not save net chain project settings" );
        }
        host.OnModify();
    }
    else if( kind == 17 )
    {
        if( v[0] != "all" && v[0] != "sheet" && v[0] != "selection" ) return invalid( "Choose a valid scope" );
        if( v[1] != "text" && v[1] != "graphics" && v[1] != "both" ) return invalid( "Choose text, graphics or both" );
        double textSize = 0, lineWidth = 0;
        if( !v[2].empty() && (!v[2].ToDouble( &textSize ) || !std::isfinite( textSize ) || textSize <= 0 || textSize > 100) ) return invalid( "Invalid text size" );
        if( !v[3].empty() && (!v[3].ToDouble( &lineWidth ) || !std::isfinite( lineWidth ) || lineWidth <= 0 || lineWidth > 100) ) return invalid( "Invalid line width" );
        for( size_t i : {4u,5u} ) if( v[i] != "keep" && v[i] != "yes" && v[i] != "no" ) return invalid( "Choose unchanged, on or off" );
        SCH_COMMIT commit( host.GetToolManager() );
        std::set<SCH_SCREEN*> visited;
        for( const auto& sheet : schematic.Hierarchy() )
        {
            if( v[0] != "all" && sheet != host.GetCurrentSheet() ) continue;
            auto* screen = sheet.LastScreen(); if( !visited.insert( screen ).second ) continue;
            for( SCH_ITEM* item : screen->Items() )
            {
                if( v[0] == "selection" && !item->IsSelected() ) continue;
                auto* text = dynamic_cast<EDA_TEXT*>( item );
                auto* symbol = dynamic_cast<SCH_SYMBOL*>( item );
                auto* line = dynamic_cast<SCH_LINE*>( item );
                auto* shape = dynamic_cast<SCH_SHAPE*>( item );
                bool editText = v[1] != "graphics" && (text || symbol) && (textSize > 0 || v[4] != "keep" || v[5] != "keep");
                bool editLine = v[1] != "text" && (line || shape) && lineWidth > 0;
                if( !editText && !editLine ) continue;
                commit.Modify( item, screen, RECURSE_MODE::NO_RECURSE );
                auto applyText = [&]( EDA_TEXT& t ) {
                    if( textSize > 0 ) t.SetTextSize( VECTOR2I( std::lround(textSize*10000), std::lround(textSize*10000) ) );
                    if( v[4] != "keep" ) t.SetBold( v[4] == "yes" );
                    if( v[5] != "keep" ) t.SetItalic( v[5] == "yes" );
                };
                if( editText ) { if( text ) applyText( *text ); if( symbol ) for( auto& field : symbol->GetFields() ) applyText( field ); }
                if( editLine ) { if( line ) line->SetLineWidth( std::lround(lineWidth*10000) ); if( shape ) { auto stroke=shape->GetStroke(); stroke.SetWidth(std::lround(lineWidth*10000)); shape->SetStroke(stroke); } }
            }
        }
        commit.Push( "Edit Text and Graphics" );
    }
    else if( kind == 18 )
    {
        for( size_t i : {1u,2u,3u} ) if( v[i] != "yes" && v[i] != "no" ) return invalid( "Choose import categories" );
        if( !wxFileName::FileExists( v[0] ) || wxFileName( v[0] ).GetExt() != "kicad_pro" ) return invalid( "Choose an existing .kicad_pro file" );
        PROJECT_FILE source( v[0] );
        SCHEMATIC_SETTINGS imported( &source, "schematic" );
        ERC_SETTINGS importedErc( &source, "erc" );
        if( !source.LoadFromFile() ) return invalid( "Could not load source project settings" );
        imported.LoadFromFile();
        importedErc.LoadFromFile();
        NET_SETTINGS oldNet( nullptr, "" ); oldNet.CopyFrom( *ns );
        const auto oldAliases = schematic.GetAllBusAliases();
        auto& target = schematic.Settings();
        // Keep the native nested-settings object attached to its existing project.
        const auto targetFields = hostAllSetupFields( target, schematic.ErcSettings() );
        const auto sourceFields = hostAllSetupFields( imported, importedErc );
        std::vector<wxString> oldValues, newValues;
        for( size_t i = 0; i < targetFields.size(); ++i )
        {
            oldValues.push_back( targetFields[i].read() );
            newValues.push_back( sourceFields[i].read() );
            if( v[1] == "yes" && !targetFields[i].write( newValues.back(), false ) )
                return invalid( "Source project has invalid formatting settings" );
        }
        if( v[1] == "yes" )
            for( size_t i = 0; i < targetFields.size(); ++i ) targetFields[i].write( newValues[i], true );
        if( v[2] == "yes" ) ns->CopyFrom( *source.NetSettings() );
        if( v[3] == "yes" )
        {
            std::vector<std::shared_ptr<BUS_ALIAS>> aliases;
            for( const auto& [name,members] : source.m_BusAliases ) { auto alias=std::make_shared<BUS_ALIAS>(); alias->SetName(name); alias->SetMembers(members); aliases.push_back(alias); }
            schematic.SetBusAliases( aliases );
        }
        if( !saveProject() )
        {
            ns->CopyFrom( oldNet ); schematic.SetBusAliases( oldAliases );
            for( size_t i = 0; i < targetFields.size(); ++i ) targetFields[i].write( oldValues[i], true );
            target.SaveToFile(); schematic.ErcSettings().SaveToFile(); ns->SaveToFile();
            return invalid( "Could not save imported project settings" );
        }
        schematic.Connectivity().Recalculate( schematic, true );
    }
    else if( kind == 19 )
    {
        auto groups=migrationGroups(schematic);
        if(v.size()!=1+groups.size()*2) return invalid("Bus connectivity changed; reopen migration");
        for(size_t i=0;i<groups.size();++i)
        {
            if(v[1+i*2]!=groups[i].identity) return invalid("Bus connectivity changed; reopen migration");
            SCH_CONNECTION connection(schematic.ConnectionGraph()); connection.ConfigureFromLabel(v[2+i*2]);
            if(connection.Type()!=CONNECTION_TYPE::BUS) return invalid("Use a vector bus label such as DATA[0..7]");
        }
        SCH_COMMIT commit(host.GetToolManager());
        for(size_t i=0;i<groups.size();++i)
            for(auto* item:groups[i].labels)
            {
                commit.Modify(item,groups[i].screen,RECURSE_MODE::NO_RECURSE);
                static_cast<SCH_LABEL_BASE*>(item)->SetText(v[2+i*2]); item->SetConnectivityDirty();
            }
        commit.Push("Migrate Legacy Bus Labels");
    }
    else if( kind == 20 )
    {
        if(v[0]!="preview" && v[0]!="apply")return invalid("Choose Preview or Apply");
        for(size_t i=2;i<=10;++i)if(v[i]!="yes" && v[i]!="no")return invalid("Choose valid backannotation options");
        const auto payload=pcbBackannotationPayload(v[1]);
        WX_STRING_REPORTER reporter;
        BACK_ANNOTATE backannotation(&host,host.GetToolManager(),reporter,v[2]=="yes",v[3]=="yes",v[4]=="yes",
                                     v[5]=="yes",v[6]=="yes",v[7]=="yes",v[8]=="yes",v[9]=="yes",v[10]=="yes",v[0]=="preview");
        const bool success=backannotation.BackAnnotateSymbols(payload);
        session->m_DocumentReport=reporter.GetMessages().utf8_string();
        if(!success)return invalid(reporter.GetMessages());
    }
    else if(kind==21)
    {
        if(!schematic.GetCurrentVariant().empty())return invalid("Resolve field names in the base variant");
        const auto conflicts=fieldCaseConflicts(schematic);
        if(v.size()!=2+conflicts.size()*3)return invalid("Fields changed; reopen conflict resolution");
        for(size_t i=0;i<conflicts.size();++i)
        {
            if(v[2+i*3]!=conflicts[i].identity)return invalid("Fields changed; reopen conflict resolution");
            const auto& action=v[4+i*3];
            if(action!="first" && action!="last" && action!="join")return invalid("Choose a field resolution");
        }
        SCH_COMMIT commit(host.GetToolManager());
        for(size_t i=0;i<conflicts.size();++i)
        {
            const auto& conflict=conflicts[i]; const auto& action=v[4+i*3];
            const size_t winner=action=="last"?conflict.fields.size()-1:0;
            wxString value=conflict.fields[winner].second;
            if(action=="join")
            {
                value.clear();
                for(const auto& field:conflict.fields)if(!field.second.empty()){if(!value.empty())value+=v[1];value+=field.second;}
            }
            else if(value.empty())for(const auto& field:conflict.fields)if(!field.second.empty()){value=field.second;break;}
            commit.Modify(conflict.symbol,conflict.screen,RECURSE_MODE::NO_RECURSE);
            conflict.symbol->GetField(conflict.fields[winner].first)->SetText(value);
            for(size_t j=0;j<conflict.fields.size();++j)if(j!=winner)conflict.symbol->RemoveField(conflict.fields[j].first);
        }
        commit.Push("Resolve Field Name Case Conflicts");
    }
    else if(kind==22)
    {
        auto pcm=std::make_shared<PLUGIN_CONTENT_MANAGER>([](int){});
        WX_STRING_REPORTER reporter; PCM_TASK_MANAGER tasks(pcm);
        auto status=PCM_TASK_MANAGER::STATUS::SUCCESS;
        if(v[0]=="install")
        {
            if(v[3]!="yes" && v[3]!="no")return invalid("Choose whether to replace an installed version");
            status=tasks.InstallDataSourceFromFile(v[1],v[3]=="yes",reporter);
        }
        else if(v[0]=="uninstall")
        {
            const auto installed=pcm->GetInstalledPackages();
            const auto found=std::find_if(installed.begin(),installed.end(),[&](const auto& entry){return entry.package.identifier==v[2] && entry.package.type==PT_DATASOURCE;});
            if(found==installed.end())return invalid("Installed data-source identifier not found");
            status=tasks.UninstallDataSource(found->package,reporter);
        }
        else if(v[0]=="refresh") reporter.Report("Installed data-source list refreshed",RPT_SEVERITY_INFO);
        else return invalid("Choose Refresh, Install archive or Uninstall");
        session->m_DocumentReport=reporter.GetMessages().utf8_string();
        if(status!=PCM_TASK_MANAGER::STATUS::SUCCESS)return invalid(reporter.GetMessages());
    }
    host.View().UpdateAllItems( KIGFX::ALL ); host.RefreshCanvas();
    return KSCH_OK;
}
}
