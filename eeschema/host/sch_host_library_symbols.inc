// SPDX-License-Identifier: GPL-3.0-or-later
// Library model editing without a symbol-editor wx frame.
namespace {
LIB_SYMBOL* hostLibrarySymbol( ksch_session* session, const wxString& text )
{
    LIB_ID id;
    if( id.Parse( text ) >= 0 ) THROW_IO_ERROR( "Enter a valid Library:Symbol ID" );
    auto* adapter = symbolLibraries( session );
    adapter->LoadOne( id.GetLibNickname() );
    auto* symbol = adapter->LoadSymbol( id );
    if( !symbol ) THROW_IO_ERROR( "Library symbol was not found" );
    return symbol;
}
ksch_status readLibrarySymbol( ksch_session* session, uint32_t kind, ksch_document_visitor visitor, void* context )
{
    auto send = [&]( const wxString& name, const wxString& value ) { visitor( context, name.utf8_string().c_str(), value.utf8_string().c_str() ); };
    auto* selected = dynamic_cast<SCH_SYMBOL*>( propertyItem( session ) );
    const wxString selectedId = selected ? wxString( selected->GetLibId().Format() ) : wxString();
    if( kind == 4 )
    {
        send( "New library ID", "" ); send( "Reference prefix", "U" ); send( "Units", "1" );
        send( "Copy from library ID (optional)", "" ); return KSCH_OK;
    }
    if( kind == 6 )
    {
        send( "Source library ID", selectedId ); send( "Destination library ID", "" ); return KSCH_OK;
    }
    if( selectedId.empty() ) { session->m_Error = "Select a symbol to open its library definition"; return KSCH_ERR_INVALID_ARG; }
    auto* symbol = hostLibrarySymbol( session, selectedId );
    wxString targetId=selectedId;
    if(kind==5 && symbol->IsDerived()) {
        symbol=symbol->GetRootSymbol().get();
        if(!symbol) THROW_IO_ERROR("The inherited symbol's root definition is unavailable");
        targetId=LIB_ID(selected->GetLibId().GetLibNickname(),symbol->GetName()).Format();
    }
    send( "Library ID", targetId );
    if( kind == 16 )
    {
        if(!symbol->IsDerived()) THROW_IO_ERROR("Select a derived library symbol to update fields from its parent");
        for(const auto* label:{"Remove extra fields","Reset visibility","Reset text effects","Reset positions","Reset nonempty text","Reset empty text"}) send(label,"false");
        std::set<wxString> names;
        std::vector<SCH_FIELD*> fields; symbol->GetFields(fields);
        for(const auto* field:fields) names.insert(field->GetName());
        fields.clear(); const auto parent=symbol->GetParent().lock();
        if(!parent) THROW_IO_ERROR("The inherited symbol parent is unavailable");
        parent->GetFields(fields);
        for(const auto* field:fields) names.insert(field->GetName());
        wxString schema;
        for(const auto& name:names) {send("Update field: "+name,"true");schema+=wxString::Format("%zu:",name.length())+name;}
        send("Field schema",schema);
    }
    else if( kind == 3 )
    {
        auto structure=[&](const wxString& name,const wxString& value){send(symbol->IsDerived()?"Inherited: "+name:name,value);};
        send( "Description", symbol->GetDescription() ); send( "Keywords", symbol->GetKeyWords() );
        structure( "Units", wxString::Format( "%d", symbol->GetUnitCount() ) );
        structure( "Power symbol", symbol->IsPower() ? "true" : "false" );
        structure( "Local power", symbol->IsLocalPower() ? "true" : "false" );
        structure( "Show pin names", symbol->GetShowPinNames() ? "true" : "false" );
        structure( "Show pin numbers", symbol->GetShowPinNumbers() ? "true" : "false" );
        structure( "Pin name offset (mm)", wxString::Format( "%.6f", schIUScale.IUTomm( symbol->GetPinNameOffset() ) ) );
        structure( "Units interchangeable", symbol->GetUnitsInterchangeableProp() ? "true" : "false" );
        std::vector<SCH_FIELD*> fields; symbol->GetFields( fields );
        std::erase_if(fields, [](const auto* field) { return field->GetId() == FIELD_T::DESCRIPTION; });
        for( const auto* field : fields ) {
            send(field->IsMandatory()?"Field name (required)":"Field name",field->GetName());
            send("Field value",field->GetText());
        }
    }
    else
    {
        const auto pins = symbol->GetPins();
        for( size_t i = 0; i < pins.size(); ++i )
        {
            auto* pin = pins[i]; const auto prefix = wxString::Format( "Pin %zu / ", i + 1 );
            send( prefix + "Number", pin->GetNumber() ); send( prefix + "Name", pin->GetName() );
            send( prefix + "Electrical type", PinTypeNames()[static_cast<int>( pin->GetType() )] );
            send( prefix + "Shape", PinShapeNames()[static_cast<int>( pin->GetShape() )] );
            send( prefix + "Orientation", PinOrientationName( pin->GetOrientation() ) );
            for( const auto& [name, value] : std::vector<std::pair<wxString, int>>{
                {"Length",pin->GetLength()}, {"X",pin->GetPosition().x}, {"Y",pin->GetPosition().y},
                {"Name size",pin->GetNameTextSize()}, {"Number size",pin->GetNumberTextSize()} } )
                send( prefix + name + " (mm)", wxString::Format( "%.6f", schIUScale.IUTomm( value ) ) );
            send( prefix + "Unit", wxString::Format( "%d", pin->GetUnit() ) );
            send( prefix + "Body style", wxString::Format( "%d", pin->GetBodyStyle() ) );
            send( prefix + "Visible", pin->IsVisible() ? "true" : "false" );
            send( prefix + "Identity",pin->m_Uuid.AsString());
        }
    }
    return KSCH_OK;
}
ksch_status applyLibrarySymbol( ksch_session* session, uint32_t kind, const char* const* values, uint32_t count )
{
    if( !count || (kind == 6 && count != 2) || (kind == 4 && count != 4) ) return KSCH_ERR_INVALID_ARG;
    std::vector<wxString> v; for( uint32_t i = 0; i < count; ++i ) v.push_back( wxString::FromUTF8( values[i] ) );
    auto invalid = [&]() { session->m_Error = "Invalid library symbol properties; no library was changed"; return KSCH_ERR_INVALID_ARG; };
    auto boolean = []( const wxString& value ) { return value == "true" || value == "false"; };
    LIB_ID id; if( id.Parse( v[kind == 6 ? 1 : 0] ) >= 0 || id.GetLibNickname().empty() || id.GetLibItemName().empty() ) return invalid();
    std::unique_ptr<LIB_SYMBOL> edited;
    bool overwrite = kind != 4 && kind != 6;
    if( kind == 4 || kind == 6 )
    {
        if( (kind == 4 && count != 4) || (kind == 6 && count != 2) ) return invalid();
        const auto source = kind == 4 ? v[3] : v[0];
        edited = source.empty() ? std::make_unique<LIB_SYMBOL>( id.GetLibItemName() )
                               : hostLibrarySymbol( session, source )->Flatten();
        edited->SetName( id.GetLibItemName() ); edited->SetLibId( id );
        edited->GetValueField().SetText(id.GetLibItemName());
        if( kind == 4 )
        {
            long units = 0; if( !v[2].ToLong( &units ) || units < 1 || units > 64 || v[1].empty() ) return invalid();
            edited->SetUnitCount( units, true ); edited->GetReferenceField().SetText( v[1] );
        }
    }
    else
    {
        edited = std::make_unique<LIB_SYMBOL>( *hostLibrarySymbol( session, v[0] ) );
        if( kind == 16 )
        {
            if(!edited->IsDerived() || count<8) return invalid();
            for(size_t i=1;i+1<v.size();++i) if(!boolean(v[i])) return invalid();
            std::vector<std::pair<std::string,std::string>> rows;
            auto collect=[](void* context,const char* name,const char* value){
                static_cast<std::vector<std::pair<std::string,std::string>>*>(context)->emplace_back(name,value);
            };
            if(readLibrarySymbol(session,16,collect,&rows)!=KSCH_OK || rows.size()!=count || rows[0].second!=values[0] || rows.back().second!=values[count-1]) return invalid();
            LIB_FIELD_SYNC_OPTIONS options; options.m_updateAllFields=false;
            options.m_removeExtraFields=v[1]=="true";options.m_resetVisibility=v[2]=="true";
            options.m_resetEffects=v[3]=="true";options.m_resetPositions=v[4]=="true";
            options.m_resetText=v[5]=="true";options.m_resetEmptyText=v[6]=="true";
            for(size_t i=7;i+1<v.size();++i) if(v[i]=="true") options.m_updateFields.insert(wxString::FromUTF8(rows[i].first.substr(14)));
            edited->SyncFieldsFromParent(options);
        }
        else if( kind == 3 )
        {
            std::vector<SCH_FIELD*> fields; edited->GetFields( fields );
            std::erase_if(fields, [](const auto* field) { return field->GetId() == FIELD_T::DESCRIPTION; });
            if( count < 10 || (count-10)%2 ) return invalid();
            long units = 0; double offset = 0;
            if( !v[3].ToLong( &units ) || units < 1 || units > 64 || !v[8].ToDouble( &offset ) || !std::isfinite(offset) || offset < 0 || offset > 1000 ) return invalid();
            for( auto index : {4,5,6,7,9} ) if( !boolean(v[index]) ) return invalid();
            if(edited->IsDerived() && (units!=edited->GetUnitCount() || (v[4]=="true")!=edited->IsPower()
                || (v[5]=="true")!=edited->IsLocalPower() || (v[6]=="true")!=edited->GetShowPinNames()
                || (v[7]=="true")!=edited->GetShowPinNumbers() || schIUScale.mmToIU(offset)!=edited->GetPinNameOffset()
                || (v[9]=="true")!=edited->GetUnitsInterchangeableProp())) {
                session->m_Error="Structural properties are inherited; edit the root library symbol";return KSCH_ERR_INVALID_ARG;
            }
            std::set<wxString> fieldNames;
            for(size_t i=10;i<v.size();i+=2) {
                if(v[i].empty() || v[i].Contains("\n") || !fieldNames.insert(v[i].Lower()).second) return invalid();
                auto* field=edited->GetField(v[i]);
                if(field && field->GetId()==FIELD_T::DESCRIPTION) return invalid();
                const auto type=field?field->GetId():FIELD_T::USER;
                if(type==FIELD_T::VALUE && (v[4]=="true" || v[5]=="true") && v[i+1]!=edited->GetName()) {
                    session->m_Error="A power symbol's value must match its library name";return KSCH_ERR_INVALID_ARG;
                }
                const auto error=GetFieldValidationErrorMessage(type,v[i+1]);
                if(!error.empty()){session->m_Error=error.utf8_string();return KSCH_ERR_INVALID_ARG;}
            }
            for(auto* field:fields) if(field->IsMandatory() && !fieldNames.contains(field->GetName().Lower())) {
                session->m_Error="Required symbol fields cannot be removed or renamed";return KSCH_ERR_INVALID_ARG;
            }
            edited->SetDescription(v[1]); edited->SetKeyWords(v[2]);
            if(!edited->IsDerived()) {
            edited->SetUnitCount(units,true);
            edited->SetPowerSymbolProp(v[4]=="true"); edited->SetLocalPowerSymbolProp(v[5]=="true");
            edited->SetShowPinNames(v[6]=="true"); edited->SetShowPinNumbers(v[7]=="true");
            edited->SetPinNameOffset(schIUScale.mmToIU(offset)); edited->SetUnitsInterchangeableProp(v[9]=="true");
            }
            for(auto* field:fields) if(!fieldNames.contains(field->GetName().Lower())) edited->RemoveField(field);
            for(size_t i=10;i<v.size();i+=2) {
                auto* field=edited->GetField(v[i]);
                if(!field) { field=new SCH_FIELD(edited.get(),FIELD_T::USER,v[i]);field->SetVisible(false);edited->AddDrawItem(field); }
                field->SetText(v[i+1]);
            }
        }
        else
        {
            if(edited->IsDerived()){session->m_Error="Inherited pins must be edited on the root library symbol";return KSCH_ERR_INVALID_ARG;}
            auto pins=edited->GetPins(); if( (count-1)%14 ) return invalid();
            std::set<SCH_PIN*> retained;
            for( size_t i=0;i<(count-1)/14;++i )
            {
                const size_t n=1+i*14; SCH_PIN* pin=nullptr;
                if(v[n+13].empty()) {pin=new SCH_PIN(edited.get());edited->AddDrawItem(pin);}
                else for(auto* existing:pins) if(existing->m_Uuid.AsString()==v[n+13]) {pin=existing;break;}
                if(!pin || !retained.insert(pin).second) {session->m_Error="Pin identities changed; reopen the pin table";return KSCH_ERR_INVALID_ARG;}
                const int type=PinTypeNames().Index(v[n+2]), shape=PinShapeNames().Index(v[n+3]), orientation=PinOrientationNames().Index(v[n+4]);
                if(v[n].empty() || type==wxNOT_FOUND || shape==wxNOT_FOUND || orientation==wxNOT_FOUND || !boolean(v[n+12])) return invalid();
                double dimensions[5]; for(size_t j=0;j<5;++j) if(!v[n+5+j].ToDouble(&dimensions[j]) || !std::isfinite(dimensions[j]) || std::abs(dimensions[j])>1000) return invalid();
                if(dimensions[0]<0 || dimensions[3]<=0 || dimensions[4]<=0) return invalid();
                long unit=0,body=0; if(!v[n+10].ToLong(&unit) || unit<0 || unit>edited->GetUnitCount() || !v[n+11].ToLong(&body) || body<0 || body>edited->GetBodyStyleCount()) return invalid();
                pin->SetNumber(v[n]); pin->SetName(v[n+1]); pin->SetType(static_cast<ELECTRICAL_PINTYPE>(type)); pin->SetShape(static_cast<GRAPHIC_PINSHAPE>(shape)); pin->SetOrientation(PinOrientationCode(orientation));
                pin->SetLength(schIUScale.mmToIU(dimensions[0])); pin->SetPosition({schIUScale.mmToIU(dimensions[1]),schIUScale.mmToIU(dimensions[2])});
                pin->SetNameTextSize(schIUScale.mmToIU(dimensions[3])); pin->SetNumberTextSize(schIUScale.mmToIU(dimensions[4])); pin->SetUnit(unit); pin->SetBodyStyle(body); pin->SetVisible(v[n+12]=="true");
            }
            for(auto* pin:pins) if(!retained.contains(pin)) edited->RemoveDrawItem(pin);
        }
    }
    auto* adapter=symbolLibraries(session);
    if(!adapter->IsSymbolLibWritable(id.GetLibNickname())){session->m_Error="The library is not writable";return KSCH_ERR_INVALID_ARG;}
    const auto result=adapter->SaveSymbol(id.GetLibNickname(), std::move(edited), overwrite);
    if(result != SYMBOL_LIBRARY_ADAPTER::SAVE_OK) { session->m_Error=overwrite?"Library symbol could not be saved":"Destination already contains this symbol, or the library could not be saved"; return KSCH_ERR_INVALID_ARG; }
    adapter->ReloadLibraryEntry(id.GetLibNickname());
    return KSCH_OK;
}
}

namespace {
struct HOST_LIBRARY_FIELDS {
    std::vector<std::unique_ptr<LIB_SYMBOL>> symbols;
    std::vector<std::pair<std::string, SCH_FIELD*>> fields;
    std::string fingerprint;
};
HOST_LIBRARY_FIELDS libraryFieldSnapshot( ksch_session* session, const wxString& locator )
{
    HOST_LIBRARY_FIELDS result;
    const auto nickname=locator.BeforeFirst(':');
    const auto relatedRoot=locator.Contains(":")?locator.AfterFirst(':'):wxString();
    auto* adapter=symbolLibraries(session); adapter->LoadOne(nickname);
    std::set<wxString> names;
    for(const auto& name:adapter->GetSymbolNames(nickname)) names.insert(name);
    std::string shape;
    for(const auto& name:names) {
        auto* original=adapter->LoadSymbol(LIB_ID(nickname,name));
        if(!original) THROW_IO_ERROR("A symbol could not be loaded for the field table");
        if(!relatedRoot.empty() && original->GetRootSymbol()->GetName()!=relatedRoot) continue;
        result.symbols.push_back(std::make_unique<LIB_SYMBOL>(*original));
        std::vector<SCH_FIELD*> fields; result.symbols.back()->GetFields(fields);
        for(auto* field:fields) {
            const std::string label=name.utf8_string()+" / "+field->GetName().utf8_string();
            shape+=label+'\n'; result.fields.emplace_back(label,field);
        }
    }
    result.fingerprint=std::to_string(std::hash<std::string>{}(shape));
    return result;
}
ksch_status readLibraryFields(ksch_session* session,bool related,ksch_document_visitor visitor,void* context)
{
    auto* selected=dynamic_cast<SCH_SYMBOL*>(propertyItem(session));
    if(!selected) {session->m_Error="Select a symbol from the library to edit its field table";return KSCH_ERR_INVALID_ARG;}
    const wxString nickname=selected->GetLibId().GetLibNickname();
    const auto locator=related ? nickname+":"+hostLibrarySymbol(session,wxString(selected->GetLibId().Format()))->GetRootSymbol()->GetName() : nickname;
    auto snapshot=libraryFieldSnapshot(session,locator);
    visitor(context,"Library scope",locator.utf8_string().c_str());
    visitor(context,"Table revision",snapshot.fingerprint.c_str());
    for(const auto& [label,field]:snapshot.fields) visitor(context,label.c_str(),field->GetText().utf8_string().c_str());
    return KSCH_OK;
}
ksch_status applyLibraryFields(ksch_session* session,const char* const* values,uint32_t count)
{
    if(count<2)return KSCH_ERR_INVALID_ARG;
    const auto locator=wxString::FromUTF8(values[0]);
    const auto nickname=locator.BeforeFirst(':');
    auto snapshot=libraryFieldSnapshot(session,locator);
    if(snapshot.fingerprint!=values[1] || count!=snapshot.fields.size()+2) {
        session->m_Error="Library table structure changed; reopen the field table";return KSCH_ERR_INVALID_ARG;
    }
    for(size_t i=0;i<snapshot.fields.size();++i) {
        auto* field=snapshot.fields[i].second;
        auto* parent=static_cast<LIB_SYMBOL*>(field->GetParent());
        if(field->GetId()==FIELD_T::VALUE && parent->IsPower() && wxString::FromUTF8(values[i+2])!=parent->GetName()) {
            session->m_Error="A power symbol's value must match its library name";return KSCH_ERR_INVALID_ARG;
        }
        const auto error=GetFieldValidationErrorMessage(field->GetId(),wxString::FromUTF8(values[i+2]));
        if(!error.empty()){session->m_Error=error.utf8_string();return KSCH_ERR_INVALID_ARG;}
    }
    std::set<LIB_SYMBOL*> changed;
    for(size_t i=0;i<snapshot.fields.size();++i) {
        auto* field=snapshot.fields[i].second;
        if(field->GetText()!=wxString::FromUTF8(values[i+2])) {
            field->SetText(wxString::FromUTF8(values[i+2]));
            changed.insert(static_cast<LIB_SYMBOL*>(field->GetParent()));
        }
    }
    auto* adapter=symbolLibraries(session);
    if(!changed.empty() && !adapter->IsSymbolLibWritable(nickname)){session->m_Error="The library is not writable";return KSCH_ERR_INVALID_ARG;}
    size_t saved=0;
    for(auto& symbol:snapshot.symbols) if(changed.contains(symbol.get())) {
        if(adapter->SaveSymbol(nickname,std::move(symbol))!=SYMBOL_LIBRARY_ADAPTER::SAVE_OK) {
            session->m_Error="Could not save library field table ("+std::to_string(saved)+" symbols saved)";
            adapter->ReloadLibraryEntry(nickname);return KSCH_ERR_INVALID_ARG;
        }
        ++saved;
    }
    adapter->ReloadLibraryEntry(nickname);
    return KSCH_OK;
}
}
