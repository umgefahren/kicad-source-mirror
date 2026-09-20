// SPDX-License-Identifier: GPL-3.0-or-later
// Result vectors and user expressions use the same native ngspice instance as analyses.
namespace {
void refreshSimulationSignals(ksch_session* session)
{
    if(!session->m_SimSignalsPending || !session->m_Simulator || activeSimulationSession!=session || session->m_Simulator->IsRunning())return;
    session->m_SimSignalsPending=false;
    for(const auto& [name,expression]:session->m_SimUserSignals)
        if(!session->m_Simulator->Command("let "+name+" = "+expression)) THROW_IO_ERROR("Could not refresh user signal: "+name);
}

ksch_status readSimulationResults(ksch_session* session,uint32_t kind,ksch_document_visitor visitor,void* context)
{
    auto send=[&](const std::string& name,const std::string& value){visitor(context,name.c_str(),value.c_str());};
    if(kind==15) {
        auto* settings=GetAppSettings<EESCHEMA_SETTINGS>("eeschema");if(!settings)return KSCH_ERR_INVALID_ARG;
        const auto& a=settings->m_Simulator.preferences.mouse_wheel_actions;
        for(const auto& [name,action]:std::vector<std::pair<std::string,SIM_MOUSE_WHEEL_ACTION>>{
            {"Vertical wheel",a.vertical_unmodified},{"Control + wheel",a.vertical_with_ctrl},
            {"Shift + wheel",a.vertical_with_shift},{"Alt + wheel",a.vertical_with_alt},{"Horizontal wheel",a.horizontal}})
            send(name,std::to_string(static_cast<int>(action)));
        return KSCH_OK;
    }
    if(kind==14) {
        for(const auto& [name,expression]:session->m_SimUserSignals){send("Signal name",name);send("Expression",expression);}
        send("Signal name","");send("Expression","");return KSCH_OK;
    }
    if(!session->m_Simulator || activeSimulationSession!=session || session->m_Simulator->IsRunning())
        THROW_IO_ERROR("Run an analysis and wait for it to finish before plotting");
    refreshSimulationSignals(session);
    auto& engine=*session->m_Simulator;
    const auto vectors=engine.AllVectors();
    if(vectors.empty()) THROW_IO_ERROR("The analysis produced no result vectors");
    if(std::find(vectors.begin(),vectors.end(),session->m_SimVector)==vectors.end()) session->m_SimVector=vectors.front();
    send("Vector",session->m_SimVector);
    if(auto* settings=GetAppSettings<EESCHEMA_SETTINGS>("eeschema")) {
        const auto& a=settings->m_Simulator.preferences.mouse_wheel_actions;
        std::string actions;
        for(auto action:{a.vertical_unmodified,a.vertical_with_ctrl,a.vertical_with_shift,a.vertical_with_alt,a.horizontal}){
            if(!actions.empty())actions+=',';actions+=std::to_string(static_cast<int>(action));
        }
        send("Wheel actions",actions);
    }
    for(const auto& name:vectors) send("Available vector",name);
    const auto values=engine.GetComplexVector(session->m_SimVector);
    const auto xaxis=engine.GetXAxis(SPICE_CIRCUIT_MODEL::CommandToSimType(session->m_SimCommand));
    const auto x=engine.GetRealVector(xaxis.utf8_string());
    send("X axis",xaxis.utf8_string());
    // Bound transfer and paint work while preserving numeric abscissas and both endpoints.
    const size_t samples=std::min<size_t>(4096,values.size());
    for(size_t i=0;i<samples;++i) {
        const size_t index=samples>1?i*(values.size()-1)/(samples-1):0;
        const double xv=index<x.size()?x[index]:static_cast<double>(index);
        std::ostringstream row;row<<std::setprecision(17)<<xv<<','<<values[index].real()<<','<<values[index].imag();
        send("Sample",row.str());
    }
    return KSCH_OK;
}
ksch_status applySimulationResults(ksch_session* session,uint32_t kind,const char* const* values,uint32_t count)
{
    if(kind==15) {
        if(count!=5)return KSCH_ERR_INVALID_ARG;
        auto* settings=GetAppSettings<EESCHEMA_SETTINGS>("eeschema");if(!settings)return KSCH_ERR_INVALID_ARG;
        auto& a=settings->m_Simulator.preferences.mouse_wheel_actions;const auto before=a;
        std::array<SIM_MOUSE_WHEEL_ACTION,5> parsed;
        for(size_t i=0;i<5;++i){long n=0;if(!wxString::FromUTF8(values[i]).ToLong(&n)||n<0||n>=7)return KSCH_ERR_INVALID_ARG;parsed[i]=static_cast<SIM_MOUSE_WHEEL_ACTION>(n);}
        a.vertical_unmodified=parsed[0];a.vertical_with_ctrl=parsed[1];a.vertical_with_shift=parsed[2];a.vertical_with_alt=parsed[3];a.horizontal=parsed[4];
        if(!settings->SaveToFile(Pgm().GetSettingsManager().GetPathForSettingsFile(settings),true)){a=before;THROW_IO_ERROR("Simulator preferences could not be saved");}
        return KSCH_OK;
    }
    if(!session->m_Simulator || activeSimulationSession!=session || session->m_Simulator->IsRunning())
        THROW_IO_ERROR("Run an analysis and wait for it to finish first");
    refreshSimulationSignals(session);
    auto& engine=*session->m_Simulator;
    if(kind==17) {
        if(count<1)return KSCH_ERR_INVALID_ARG;
        const auto vectors=engine.AllVectors();
        if(std::find(vectors.begin(),vectors.end(),values[0])==vectors.end())return KSCH_ERR_INVALID_ARG;
        session->m_SimVector=values[0];return KSCH_OK;
    }
    if(count%2)return KSCH_ERR_INVALID_ARG;
    std::map<std::string,std::string> replacement;
    const auto vectors=engine.AllVectors();
    for(size_t i=0;i<count;i+=2) {
        const std::string name=values[i],expression=values[i+1];
        if(name.empty() && expression.empty())continue;
        if(name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0]=='_')
           || !std::all_of(name.begin(),name.end(),[](unsigned char c){return std::isalnum(c)||c=='_';})
           || expression.empty() || expression.find_first_of("\r\n;`$")!=std::string::npos || replacement.contains(name)) return KSCH_ERR_INVALID_ARG;
        if(std::find(vectors.begin(),vectors.end(),name)!=vectors.end() && !session->m_SimUserSignals.contains(name))
            THROW_IO_ERROR("Choose a name that does not overwrite an analysis vector");
        replacement[name]=expression;
    }
    static uint64_t expressionSerial=0;
    auto temporaryName=[&]() {
        const auto current=engine.AllVectors();std::string name;
        do{name="gpui_expression_"+std::to_string(++expressionSerial);}
        while(std::find(current.begin(),current.end(),name)!=current.end());
        return name;
    };
    std::map<std::string,std::string> backups;
    std::vector<std::string> temporaryVectors;
    const auto oldSelection=session->m_SimVector;
    auto cleanup=[&](){for(const auto& temporary:temporaryVectors)engine.Command("unlet "+temporary);};
    try {
        for(const auto& [name,expression]:replacement) {
            if(std::find(vectors.begin(),vectors.end(),name)!=vectors.end()) {
                const auto backup=temporaryName();temporaryVectors.push_back(backup);
                if(!engine.Command("let "+backup+" = "+name) || engine.GetComplexVector(backup).empty())
                    THROW_IO_ERROR("Could not preserve the previous signal: "+name);
                backups[name]=backup;
            } else backups[name]="";
        }
        for(const auto& [name,expression]:replacement) {
            // Fresh names distinguish failed edits from still-existing old vectors.
            const auto temporary=temporaryName();temporaryVectors.push_back(temporary);
            if(!engine.Command("let "+temporary+" = "+expression) || engine.GetComplexVector(temporary).empty())
                THROW_IO_ERROR("The expression produced no samples: "+name);
            if(!engine.Command("let "+name+" = "+temporary)) THROW_IO_ERROR("Could not assign the user signal: "+name);
            session->m_SimVector=name;
        }
    } catch(...) {
        for(const auto& [name,backup]:backups) {
            if(backup.empty())engine.Command("unlet "+name);
            else engine.Command("let "+name+" = "+backup);
        }
        session->m_SimVector=oldSelection;cleanup();throw;
    }
    cleanup();
    for(const auto& [name,expression]:session->m_SimUserSignals) if(!replacement.contains(name))engine.Command("unlet "+name);
    session->m_SimUserSignals=std::move(replacement);
    return KSCH_OK;
}
}
