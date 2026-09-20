// SPDX-License-Identifier: GPL-3.0-or-later
namespace {
std::vector<wxString> hostPinMapPins(const LIB_SYMBOL& symbol)
{
    std::set<wxString> unique;for(const auto* pin:symbol.GetPins())unique.insert(pin->GetNumber());
    return {unique.begin(),unique.end()};
}
std::string hostPinMapSchema(const std::vector<wxString>& pins)
{
    std::string schema;for(const auto& pin:pins){const auto text=pin.utf8_string();schema+=std::to_string(text.size())+":"+text;}return schema;
}
ksch_status readLibraryPinMaps(ksch_session* session,uint32_t kind,ksch_document_visitor visitor,void* context)
{
    auto* selected=dynamic_cast<SCH_SYMBOL*>(propertyItem(session));if(!selected)THROW_IO_ERROR("Select a symbol to edit library pin maps");
    auto* symbol=kind==22?selected->GetLibSymbolRef().get():hostLibrarySymbol(session,selected->GetLibId().Format());
    if(!symbol)THROW_IO_ERROR("The symbol definition is unavailable");
    const auto pins=hostPinMapPins(*symbol);
    auto send=[&](const std::string& name,const std::string& value){visitor(context,name.c_str(),value.c_str());};
    send(kind==22?"Symbol UUID":"Library ID",kind==22?selected->m_Uuid.AsString().utf8_string():std::string(selected->GetLibId().Format()));send("Pin map schema",hostPinMapSchema(pins));send("Pin count",std::to_string(pins.size()));
    for(const auto& pin:pins)send("Pin definition",pin.utf8_string());
    for(const auto& map:symbol->GetPinMaps().GetAll()) {
        send("Map name",map.GetName().utf8_string());std::string footprints;
        for(const auto& association:symbol->GetAssociatedFootprints())if(association.m_MapName==map.GetName()){
            if(!footprints.empty())footprints+=';';footprints+=association.m_FootprintLibId.Format();
        }
        send("Associated footprints (; separated)",footprints);
        for(const auto& pin:pins)send("Pin "+pin.utf8_string()+" → pad",(map.HasEntry(pin)?map.GetPadNumber(pin):pin).utf8_string());
    }
    return KSCH_OK;
}
ksch_status applyLibraryPinMaps(ksch_session* session,uint32_t kind,const char* const* values,uint32_t count)
{
    if(count<3)return KSCH_ERR_INVALID_ARG;
    auto* selected=dynamic_cast<SCH_SYMBOL*>(propertyItem(session));
    if(kind==22 && (!selected||selected->m_Uuid.AsString()!=wxString::FromUTF8(values[0])))THROW_IO_ERROR("Selection changed; reopen Pin Maps");
    auto* original=kind==22?selected->GetLibSymbolRef().get():hostLibrarySymbol(session,wxString::FromUTF8(values[0]));
    if(!original)THROW_IO_ERROR("The symbol definition is unavailable");const auto pins=hostPinMapPins(*original);
    unsigned long pinCount=0;if(!wxString::FromUTF8(values[2]).ToULong(&pinCount)||pinCount!=pins.size()
        ||values[1]!=hostPinMapSchema(pins)||count<3+pins.size()||(count-3-pins.size())%(2+pins.size()))
        THROW_IO_ERROR("Pin definitions changed; reopen Pin Maps before saving");
    for(size_t i=0;i<pins.size();++i)if(wxString::FromUTF8(values[3+i])!=pins[i])return KSCH_ERR_INVALID_ARG;
    auto edited=std::make_unique<LIB_SYMBOL>(*original);PIN_MAP_SET maps;std::vector<ASSOCIATED_FOOTPRINT> associations;
    // Unmapped identity associations have no column and must survive map edits.
    for(const auto& association:original->GetAssociatedFootprints())if(association.m_MapName.empty())associations.push_back(association);
    std::set<wxString> names,footprintIds;
    for(const auto& association:associations)footprintIds.insert(association.m_FootprintLibId.Format());
    for(size_t start=3+pins.size();start<count;start+=2+pins.size()) {
        const auto name=wxString::FromUTF8(values[start]);if(name.empty()||!names.insert(name).second)THROW_IO_ERROR("Pin map names must be nonempty and unique");
        PIN_MAP map(name);
        for(size_t i=0;i<pins.size();++i){const auto pad=wxString::FromUTF8(values[start+2+i]);
            if(pad.Find('\n')!=wxNOT_FOUND||pad.Find('\r')!=wxNOT_FOUND)THROW_IO_ERROR("Pad numbers cannot contain line breaks");
            bool valid=true;if(!pad.empty())ExpandStackedPinNotation(pad,&valid);if(!valid)THROW_IO_ERROR("Malformed stacked pad notation");
            if(!pad.empty()&&pad!=pins[i])map.SetEntry(pins[i],pad);
        }
        maps.AddOrReplace(std::move(map));
        wxStringTokenizer tokens(wxString::FromUTF8(values[start+1]),";");
        while(tokens.HasMoreTokens()){auto text=tokens.GetNextToken();text.Trim().Trim(false);LIB_ID id;
            if(id.Parse(text)>=0||id.GetLibNickname().empty()||id.GetLibItemName().empty()||!footprintIds.insert(text).second)THROW_IO_ERROR("Footprints need unique Library:Footprint IDs");
            associations.push_back({id,name});
        }
    }
    edited->SetPinMaps(maps);edited->SetAssociatedFootprints(std::move(associations));
    if(kind==22){
        SCH_COMMIT commit(session->m_Host.GetToolManager());
        session->m_Host.GetSchematic()->SyncLibSymbolPinMaps(selected->GetSchSymbolLibraryName(),*edited,&commit);
        commit.Push("Edit Schematic Symbol Pin Maps");return KSCH_OK;
    }
    LIB_ID targetId;targetId.Parse(wxString::FromUTF8(values[0]));const auto nickname=targetId.GetLibNickname();auto* adapter=symbolLibraries(session);
    if(!adapter->IsSymbolLibWritable(nickname)||adapter->SaveSymbol(nickname,std::move(edited),true)!=SYMBOL_LIBRARY_ADAPTER::SAVE_OK)THROW_IO_ERROR("The library pin maps could not be saved");
    adapter->ReloadLibraryEntry(nickname);return KSCH_OK;
}
}
