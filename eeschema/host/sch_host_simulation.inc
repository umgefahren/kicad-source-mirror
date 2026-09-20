// SPDX-License-Identifier: GPL-3.0-or-later
// Native simulation services, included by the ABI translation unit. No windows.
namespace {
const std::array<const char*, 9> simulationFields = { "Sim.Device", "Sim.Type", "Sim.Params", "Sim.Pins",
    "Sim.Library", "Sim.Name", "Sim.NodesFormat", "Sim.Enable", "Sim.Decomposition" };
}
extern "C" ksch_status ksch_session_simulation_workflow( ksch_session* session, uint32_t kind,
                                                        ksch_document_visitor visitor, void* context )
{
    if( !session || !visitor || kind > 22 || kind == 7 || kind == 11 || kind == 12 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        if(kind==18||kind==22)return readLibraryPinMaps(session,kind,visitor,context);
        if(kind>=19)return readSimulationModelEditor(session,kind,visitor,context);
        if( kind == 14 || kind == 15 || kind == 17 ) return readSimulationResults(session,kind,visitor,context);
        if( kind == 16 ) return readLibrarySymbol(session,kind,visitor,context);
        if( kind == 13 ) {
            const auto precision=std::to_string(session->m_SimFormat.Precision);
            visitor(context,"Significant digits",precision.c_str());
            visitor(context,"SI range (~ for automatic)",session->m_SimFormat.Range.utf8_string().c_str());
            return KSCH_OK;
        }
        if( kind == 10 ) return readRemoteProviders( session, visitor, context );
        if( kind == 8 || kind == 9 ) return readLibraryFields( session, kind == 9, visitor, context );
        if( kind >= 3 ) return readLibrarySymbol( session, kind, visitor, context );
        auto send = [&]( const std::string& name, const std::string& value )
        { visitor( context, name.c_str(), value.c_str() ); };
        if( kind == 0 )
        {
            auto* symbol = dynamic_cast<SCH_SYMBOL*>( propertyItem( session ) );
            if( !symbol ) { session->m_Error = "Select one symbol to edit its simulation model"; return KSCH_ERR_INVALID_ARG; }
            send( "Symbol UUID", symbol->m_Uuid.AsString().utf8_string() );
            for( const auto* name : simulationFields )
                send( name, GetFieldValue( &symbol->GetFields(), wxString::FromUTF8( name ), false, 0 ) );
            std::set<std::string> devices,types;
            for(auto type:SIM_MODEL::TYPE_ITERATOR()) {
                const auto info=SIM_MODEL::TypeInfo(type);
                devices.insert(SIM_MODEL::DeviceInfo(info.deviceType).fieldValue);
                types.insert(info.fieldValue);
            }
            for(const auto& device:devices) send("Device choice",device);
            for(const auto& type:types) send("Type choice",type);
        }
        else if( kind == 1 )
        {
            send( "Analysis command", session->m_SimCommand );
            send( "Netlist option flags", std::to_string( session->m_SimOptions ) );
            send( "Compatibility (0=user, 1=ngspice, 2=PSpice, 3=LTspice, 4=both, 5=HSpice)",
                std::to_string( session->m_SimSettings ? static_cast<int>( session->m_SimSettings->GetCompatibilityMode() ) : 1 ) );
        }
        else
        {
            if( !session->m_Simulator || activeSimulationSession != session ) { send( "Status", "No analysis has been started" ); return KSCH_OK; }
            refreshSimulationSignals(session);
            auto& simulator = *session->m_Simulator;
            send( "Status", simulator.IsRunning() ? "Running" : "Finished" );
            if( !simulator.IsRunning() )
                for( const auto& vector : simulator.AllVectors() )
                {
                    const auto values = simulator.GetComplexVector( vector );
                    std::string summary = std::to_string( values.size() ) + " samples";
                    if( !values.empty() ) {
                        auto format=[&](double value) { return std::isfinite(value)
                            ? SPICE_VALUE(value).ToString(session->m_SimFormat).utf8_string()
                            : wxString::Format("%g",value).utf8_string(); };
                        summary += "; last = " + format(values.back().real()) + " + j" + format(values.back().imag());
                    }
                    send( vector, summary );
                }
        }
        return KSCH_OK;
    } );
}
extern "C" ksch_status ksch_session_apply_simulation_workflow( ksch_session* session, uint32_t kind,
                                                              const char* const* values, uint32_t count )
{
    if( !session || kind > 22 || ( count && !values ) ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        for( uint32_t i = 0; i < count; ++i ) if( !values[i] ) return KSCH_ERR_INVALID_ARG;
        if(kind==18||kind==22)return applyLibraryPinMaps(session,kind,values,count);
        if(kind==19||kind==20)return applySimulationModelEditor(session,kind,values,count);
        if(kind==21)return KSCH_ERR_INVALID_ARG;
        if( kind == 14 || kind == 15 || kind == 17 ) return applySimulationResults(session,kind,values,count);
        if( kind == 16 ) return applyLibrarySymbol(session,kind,values,count);
        if( kind == 13 ) {
            long precision=0;
            if(count!=2 || !wxString::FromUTF8(values[0]).ToLong(&precision) || precision<1 || precision>9
                || !*values[1] || std::string(values[1]).size()>32) return KSCH_ERR_INVALID_ARG;
            session->m_SimFormat.Precision=precision;session->m_SimFormat.Range=wxString::FromUTF8(values[1]);return KSCH_OK;
        }
        if( kind >= 10 ) return applyRemoteProviders( session, kind, values, count );
        if( kind == 8 || kind == 9 ) return applyLibraryFields( session, values, count );
        if( kind == 7 ) return applyLibraryConfig( session, values, count );
        if( kind >= 3 ) return applyLibrarySymbol( session, kind, values, count );
        auto& host = session->m_Host;
        if( kind == 0 )
        {
            if( count != simulationFields.size() + 1 ) return KSCH_ERR_INVALID_ARG;
            auto* symbol = dynamic_cast<SCH_SYMBOL*>( propertyItem( session ) );
            if( !symbol || symbol->m_Uuid.AsString() != wxString::FromUTF8( values[0] ) )
            { session->m_Error = "Selection changed; reopen Simulation Model"; return KSCH_ERR_INVALID_ARG; }
            if( !host.GetSchematic()->GetCurrentVariant().empty() )
            { session->m_Error = "Simulation models must be edited in the base variant"; return KSCH_ERR_INVALID_ARG; }
            auto fields = symbol->GetFields();
            bool changed = false;
            for( size_t i = 0; i < simulationFields.size(); ++i )
            {
                const auto name = wxString::FromUTF8( simulationFields[i] );
                changed |= GetFieldValue( &fields, name, false, 0 ) != values[i + 1];
                SetFieldValue( fields, name, values[i + 1], false );
            }
            if( !changed ) return KSCH_OK;
            SIM_LIB_MGR models( &host.GetSchematic()->Project() );
            WX_STRING_REPORTER reporter;
            auto pins = SIM_MODEL::PinNumbers( static_cast<const SCH_SYMBOL*>( symbol )->GetPins( &host.GetCurrentSheet() ) );
            auto model = models.CreateModel( fields, false, 0, pins, reporter );
            if( !reporter.GetMessages().empty() )
            { session->m_Error = reporter.GetMessages().utf8_string(); return KSCH_ERR_INVALID_ARG; }
            model.model.WriteFields( fields );
            SCH_COMMIT commit( host.GetToolManager() );
            commit.Modify( symbol, host.GetScreen(), RECURSE_MODE::NO_RECURSE );
            symbol->GetFields() = std::move( fields );
            for( auto& field : symbol->GetFields() ) field.SetParent( symbol );
            symbol->SyncOtherUnits( host.GetCurrentSheet(), commit, nullptr, wxEmptyString );
            commit.Push( "Edit Simulation Model" );
            session->m_SimModelDraftUuid.clear();
            return KSCH_OK;
        }
        if( kind == 2 )
        {
            if( session->m_Simulator && activeSimulationSession == session && !session->m_Simulator->Stop() )
            { session->m_Error = "Could not stop simulation"; return KSCH_ERR_INVALID_ARG; }
            return KSCH_OK;
        }
        if( count != 3 ) return KSCH_ERR_INVALID_ARG;
        const wxString command = wxString::FromUTF8( values[0] );
        unsigned long options = 0, mode = 0;
        if( std::string(values[0]).find_first_of("\r\n;`$") != std::string::npos
            || !SPICE_CIRCUIT_MODEL::IsSimCommand( command )
            || !wxString::FromUTF8( values[1] ).ToULong( &options ) || options > 0xfff
            || !wxString::FromUTF8( values[2] ).ToULong( &mode ) || mode > 5 )
        { session->m_Error = "Enter a SPICE analysis command, valid flags, and compatibility mode 0–5"; return KSCH_ERR_INVALID_ARG; }
        WX_STRING_REPORTER reporter;
        auto circuit = std::make_shared<SPICE_CIRCUIT_MODEL>( host.GetSchematic() );
        STRING_FORMATTER netlist;
        // The circuit writer appends the requested analysis itself. Keeping the
        // exporter flag would also retain schematic analyses and run both.
        if( !circuit->GetNetlist( command, options & ~NETLIST_EXPORTER_SPICE::OPTION_SIM_COMMAND, &netlist, reporter ) )
        { session->m_Error = reporter.GetMessages().utf8_string(); return KSCH_ERR_INVALID_ARG; }
        // Avoid SIMULATOR::CreateInstance: its failure path presents a wx message box.
        static std::shared_ptr<SPICE_SIMULATOR> engine;
        if( !engine ) engine = std::make_shared<NGSPICE>();
        if( engine->IsRunning() )
        { session->m_Error = "Stop the active simulation before starting another"; return KSCH_ERR_INVALID_ARG; }
        session->m_Simulator = engine;
        activeSimulationSession = session;
        session->m_SimSettings = std::make_shared<NGSPICE_SETTINGS>( nullptr, "" );
        session->m_SimSettings->SetCompatibilityMode( static_cast<NGSPICE_COMPATIBILITY_MODE>( mode ) );
        engine->Settings() = session->m_SimSettings;
        engine->Init();
        if( !engine->LoadNetlist( netlist.GetString() ) || !engine->Run() )
        { session->m_Error = "Ngspice rejected the netlist or analysis command"; return KSCH_ERR_INVALID_ARG; }
        session->m_SimCommand = values[0]; session->m_SimOptions = options;
        session->m_SimSignalsPending=true;
        return KSCH_OK;
    } );
}
