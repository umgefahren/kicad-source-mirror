// SPDX-License-Identifier: GPL-3.0-or-later
#include <sim/sim_model_ibis.h>
#include <sim/sim_library_ibis.h>
namespace {
std::string simulationFieldShape(const std::vector<SCH_FIELD>& fields)
{
    std::string result;for(const auto& field:fields){const auto name=field.GetName().utf8_string(),value=field.GetText().utf8_string();result+=std::to_string(name.size())+":"+name+std::to_string(value.size())+":"+value;}return result;
}
SCH_SYMBOL* simulationDraft(ksch_session* session,bool refresh=false)
{
    auto* symbol=dynamic_cast<SCH_SYMBOL*>(propertyItem(session));if(!symbol)THROW_IO_ERROR("Select a symbol to edit its simulation model");
    const auto uuid=symbol->m_Uuid.AsString().utf8_string();
    if(session->m_SimModelDraftUuid!=uuid || (refresh && session->m_SimModelBaseline!=simulationFieldShape(symbol->GetFields()))){session->m_SimModelDraftUuid=uuid;session->m_SimModelDraft=symbol->GetFields();session->m_SimModelBaseline=simulationFieldShape(symbol->GetFields());}
    return symbol;
}
std::string detailedModelSchema(const SIM_MODEL& model)
{
    std::string schema=std::to_string(static_cast<int>(model.GetType()))+":";
    for(int i=0;i<model.GetParamCount();++i){const auto& name=model.GetParam(i).info.name;schema+=std::to_string(name.size())+":"+name;}
    schema+='|';for(const auto& pin:model.GetPins()){const auto& name=pin.get().modelPinName;schema+=std::to_string(name.size())+":"+name;}return schema;
}
void modelReport(ksch_session* session,const WX_STRING_REPORTER& reporter)
{
    session->m_SimParserLog=reporter.GetMessages().utf8_string();
}
ksch_status readSimulationModelEditor(ksch_session* session,uint32_t kind,ksch_document_visitor visitor,void* context)
{
    auto send=[&](const std::string& name,const std::string& value){visitor(context,name.c_str(),value.c_str());};
    if(kind==21){send("Parser report",session->m_SimParserLog.empty()?"No parser messages":session->m_SimParserLog);return KSCH_OK;}
    auto* symbol=simulationDraft(session,true);auto& fields=session->m_SimModelDraft;
    SIM_LIB_MGR manager(&session->m_Host.GetSchematic()->Project());manager.SetForceFullParse();WX_STRING_REPORTER reporter;
    const auto pins=SIM_MODEL::PinNumbers(static_cast<const SCH_SYMBOL*>(symbol)->GetPins(&session->m_Host.GetCurrentSheet()));
    send("Symbol UUID",session->m_SimModelDraftUuid);
    if(kind==20){
        const auto path=GetFieldValue(&fields,SIM_LIBRARY_FIELD,false,0),name=GetFieldValue(&fields,SIM_NAME_FIELD,false,0);
        send("Model library path",path);send("Model / IBIS component",name);
        send("IBIS pin",GetFieldValue(&fields,SIM_LIBRARY_IBIS::PIN_FIELD,false,0));
        send("IBIS pin model",GetFieldValue(&fields,SIM_LIBRARY_IBIS::MODEL_FIELD,false,0));
        send("Differential",GetFieldValue(&fields,SIM_LIBRARY_IBIS::DIFF_FIELD,false,0)=="1"?"true":"false");
        send("IBIS waveform",GetFieldValue(&fields,SIM_DEVICE_SUBTYPE_FIELD,false,0));
        if(!path.empty()){
            manager.SetLibrary(wxString::FromUTF8(path),reporter);modelReport(session,reporter);
            for(const auto& [_,library]:manager.GetLibraries())for(const auto& model:library.get().GetModels())send("Choice 2",model.name);
            try {
                auto loaded=manager.CreateModel(fields,false,0,pins,reporter);modelReport(session,reporter);
                if(auto* ibis=dynamic_cast<SIM_MODEL_IBIS*>(&loaded.model)){
                    for(const auto& pin:ibis->GetIbisPins())send("Choice 3",pin.first);
                    for(const auto& model:ibis->GetIbisModels())send("Choice 4",model);
                    for(auto type:{SIM_MODEL::TYPE::KIBIS_DEVICE,SIM_MODEL::TYPE::KIBIS_DRIVER_DC,SIM_MODEL::TYPE::KIBIS_DRIVER_RECT,SIM_MODEL::TYPE::KIBIS_DRIVER_PRBS})send("Choice 6",SIM_MODEL::TypeInfo(type).fieldValue);
                }
            }catch(const std::exception& error){session->m_SimParserLog+=error.what();}
        }
        return KSCH_OK;
    }
    auto loaded=manager.CreateModel(fields,false,0,pins,reporter);modelReport(session,reporter);if(reporter.HasMessageOfSeverity(RPT_SEVERITY_ERROR))THROW_IO_ERROR("Model parsing failed; inspect Parser Report");auto& model=loaded.model;
    send("Model schema",detailedModelSchema(model));
    for(int i=0;i<model.GetParamCount();++i){const auto& param=model.GetParam(i);send("Parameter "+(param.info.description.empty()?param.info.name:param.info.description+" ("+param.info.name+")")+(param.info.unit.empty()?"":" ["+param.info.unit+"]"),param.value);
        if(param.info.type==SIM_VALUE::TYPE_BOOL){send("Choice "+std::to_string(2+i),"0");send("Choice "+std::to_string(2+i),"1");}
        for(const auto& choice:param.info.enumValues)send("Choice "+std::to_string(2+i),choice);
    }
    for(int i=0;i<model.GetPinCount();++i){const auto& pin=model.GetPin(i);send("Model pin "+pin.modelPinName,pin.symbolPinNumber.utf8_string());
        send("Choice "+std::to_string(2+model.GetParamCount()+i),"");
        for(const auto& symbolPin:pins)send("Choice "+std::to_string(2+model.GetParamCount()+i),symbolPin.utf8_string());
    }
    return KSCH_OK;
}
ksch_status applySimulationModelEditor(ksch_session* session,uint32_t kind,const char* const* values,uint32_t count)
{
    auto* symbol=simulationDraft(session);if(!count||session->m_SimModelDraftUuid!=values[0])THROW_IO_ERROR("Selection changed; reopen the model editor");
    if(!session->m_Host.GetSchematic()->GetCurrentVariant().empty())THROW_IO_ERROR("Edit simulation models in the base variant");
    auto fields=session->m_SimModelDraft;
    SIM_LIB_MGR manager(&session->m_Host.GetSchematic()->Project());manager.SetForceFullParse();WX_STRING_REPORTER reporter;
    const auto pins=SIM_MODEL::PinNumbers(static_cast<const SCH_SYMBOL*>(symbol)->GetPins(&session->m_Host.GetCurrentSheet()));
    if(kind==20){
        if(count!=7)return KSCH_ERR_INVALID_ARG;
        manager.SetLibrary(wxString::FromUTF8(values[1]),reporter);modelReport(session,reporter);
        std::vector<SIM_LIBRARY::MODEL> models;const SIM_LIBRARY_IBIS* ibisLibrary=nullptr;
        for(const auto& [_,library]:manager.GetLibraries()){const auto entries=library.get().GetModels();for(const auto& entry:entries)models.push_back(entry);if(auto* ibis=dynamic_cast<const SIM_LIBRARY_IBIS*>(&library.get()))ibisLibrary=ibis;}
        if(models.empty())THROW_IO_ERROR("The model library has no loadable models; inspect the parser report");
        auto selected=std::find_if(models.begin(),models.end(),[&](const auto& model){return model.name==values[2];});if(selected==models.end())selected=models.begin();
        const bool changed=GetFieldValue(&fields,SIM_LIBRARY_FIELD,false,0)!=values[1]||GetFieldValue(&fields,SIM_NAME_FIELD,false,0)!=selected->name;
        SetFieldValue(fields,SIM_LIBRARY_FIELD,values[1]);SetFieldValue(fields,SIM_NAME_FIELD,selected->name);
        SetFieldValue(fields,SIM_DEVICE_FIELD,selected->model.GetDeviceInfo().fieldValue);SetFieldValue(fields,SIM_DEVICE_SUBTYPE_FIELD,selected->model.GetTypeInfo().fieldValue);
        if(changed){SetFieldValue(fields,SIM_PARAMS_FIELD,"");SetFieldValue(fields,SIM_PINS_FIELD,"");}
        if(auto* ibis=dynamic_cast<SIM_MODEL_IBIS*>(&selected->model)){
            const auto availablePins=ibis->GetIbisPins();if(availablePins.empty()||!ibisLibrary)THROW_IO_ERROR("This IBIS component has no pins");
            auto pin=std::find_if(availablePins.begin(),availablePins.end(),[&](const auto& pin){return pin.first==values[3];});if(pin==availablePins.end())pin=availablePins.begin();
            if(!ibis->ChangePin(*ibisLibrary,pin->first))THROW_IO_ERROR("Could not load the selected IBIS pin");
            const auto availableModels=ibis->GetIbisModels();if(availableModels.empty())THROW_IO_ERROR("The IBIS pin has no models");
            auto model=std::find(availableModels.begin(),availableModels.end(),values[4]);if(model==availableModels.end())model=availableModels.begin();
            if(!ibis->SetIbisModel(*ibisLibrary,pin->first,*model))THROW_IO_ERROR("Could not bind the IBIS pin model");
            if(std::string(values[5])!="true"&&std::string(values[5])!="false")return KSCH_ERR_INVALID_ARG;
            if(std::string(values[5])=="true"&&!ibis->CanDifferential())THROW_IO_ERROR("This IBIS model cannot use differential mode");
            SetFieldValue(fields,SIM_LIBRARY_IBIS::PIN_FIELD,pin->first);SetFieldValue(fields,SIM_LIBRARY_IBIS::MODEL_FIELD,*model);SetFieldValue(fields,SIM_LIBRARY_IBIS::DIFF_FIELD,std::string(values[5])=="true"?"1":"");
            for(auto type:{SIM_MODEL::TYPE::KIBIS_DEVICE,SIM_MODEL::TYPE::KIBIS_DRIVER_DC,SIM_MODEL::TYPE::KIBIS_DRIVER_RECT,SIM_MODEL::TYPE::KIBIS_DRIVER_PRBS})if(SIM_MODEL::TypeInfo(type).fieldValue==values[6])SetFieldValue(fields,SIM_DEVICE_SUBTYPE_FIELD,values[6]);
        }
        auto loaded=manager.CreateModel(fields,false,0,pins,reporter);modelReport(session,reporter);if(reporter.HasMessageOfSeverity(RPT_SEVERITY_ERROR))THROW_IO_ERROR("Model parsing failed; inspect Parser Report");loaded.model.WriteFields(fields);
        session->m_SimModelDraft=std::move(fields);return KSCH_OK;
    }
    auto loaded=manager.CreateModel(fields,false,0,pins,reporter);modelReport(session,reporter);if(reporter.HasMessageOfSeverity(RPT_SEVERITY_ERROR))THROW_IO_ERROR("Model parsing failed; inspect Parser Report");auto& model=loaded.model;
    if(count!=static_cast<uint32_t>(2+model.GetParamCount()+model.GetPinCount())||values[1]!=detailedModelSchema(model))THROW_IO_ERROR("Model definition changed; reopen Parameters and Pins");
    for(int i=0;i<model.GetParamCount();++i){const auto& info=model.GetParam(i).info;const std::string value=values[2+i];
        if(value.find_first_of("\r\n;`$")!=std::string::npos || (!value.empty() && (((info.type==SIM_VALUE::TYPE_INT||info.type==SIM_VALUE::TYPE_FLOAT)&&!SIM_VALUE_GRAMMAR::IsValid(value,info.type))
            || (info.type==SIM_VALUE::TYPE_BOOL && value!="0" && value!="1")
            || (!info.enumValues.empty() && std::find(info.enumValues.begin(),info.enumValues.end(),value)==info.enumValues.end()))))THROW_IO_ERROR("Invalid model parameter: "+info.name);
        model.SetParamValue(i,value);
    }
    for(int i=0;i<model.GetPinCount();++i){const auto pin=wxString::FromUTF8(values[2+model.GetParamCount()+i]);if(!pin.empty()&&std::find(pins.begin(),pins.end(),pin)==pins.end())THROW_IO_ERROR("Select an existing symbol pin");model.AssignSymbolPinNumberToModelPin(i,pin);}
    model.WriteFields(fields);
    if(simulationFieldShape(symbol->GetFields())!=session->m_SimModelBaseline)THROW_IO_ERROR("Symbol fields changed while the model editor was open; close and reopen it");
    SCH_COMMIT commit(session->m_Host.GetToolManager());commit.Modify(symbol,session->m_Host.GetScreen(),RECURSE_MODE::NO_RECURSE);
    symbol->GetFields()=fields;for(auto& field:symbol->GetFields())field.SetParent(symbol);
    symbol->SyncOtherUnits(session->m_Host.GetCurrentSheet(),commit,nullptr,wxEmptyString);commit.Push("Edit Simulation Parameters and Pins");
    session->m_SimModelDraft=std::move(fields);session->m_SimModelBaseline=simulationFieldShape(symbol->GetFields());return KSCH_OK;
}
}
