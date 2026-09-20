// SPDX-License-Identifier: GPL-3.0-or-later
extern "C" ksch_status ksch_session_configure_library( ksch_session* session, uint32_t global,
    const char* nickname, ksch_document_visitor visitor, void* context )
{
    if(!session || global>1 || !nickname || !visitor) return KSCH_ERR_INVALID_ARG;
    return guard(session,[&]() -> ksch_status {
        if(!session->m_Host.IsLoaded()) return KSCH_ERR_NO_DOCUMENT;
        symbolLibraries(session);
        const auto row=session->m_SymbolLibraries->GetRow(LIBRARY_TABLE_TYPE::SYMBOL, wxString::FromUTF8(nickname),global?LIBRARY_TABLE_SCOPE::GLOBAL:LIBRARY_TABLE_SCOPE::PROJECT);
        if(!row) { session->m_Error="Save the library table before opening its connection settings"; return KSCH_ERR_INVALID_ARG; }
        const auto type=(*row)->Type();
        if(type!="Database" && type!="HTTP") { session->m_Error="Connection settings apply to Database and HTTP library types"; return KSCH_ERR_INVALID_ARG; }
        auto uri=(*row)->URI();
        if(session->m_Host.GetSchematic()->Project().GetProjectPath().empty())
            uri.Replace("${KIPRJMOD}",wxFileName(session->m_Host.GetSchematic()->RootScreen()->GetFileName()).GetPath());
        auto path=LIBRARY_MANAGER::ExpandURI(uri,session->m_Host.GetSchematic()->Project());
        auto send=[&](const std::string& label,const std::string& value){ visitor(context,label.c_str(),value.c_str()); };
        send("Configuration file",path.utf8_string()); send("Library type",type.utf8_string());
        if(type=="Database") {
            DATABASE_LIB_SETTINGS config(path.utf8_string());
            if(!config.LoadFromFile()) { session->m_Error="Could not read database library configuration"; return KSCH_ERR_INVALID_ARG; }
            send("Connection string",config.m_Source.connection_string); send("DSN",config.m_Source.dsn);
            send("Username",config.m_Source.username); send("Password",config.m_Source.password);
            send("Connection timeout (seconds)",std::to_string(config.m_Source.timeout));
            send("Cache rows",std::to_string(config.m_Cache.max_size)); send("Cache lifetime (seconds)",std::to_string(config.m_Cache.max_age));
        } else {
            HTTP_LIB_SETTINGS config(path.utf8_string());
            if(!config.LoadFromFile()) { session->m_Error="Could not read HTTP library configuration"; return KSCH_ERR_INVALID_ARG; }
            send("Root URL",config.m_Source.root_url); send("API version",config.m_Source.api_version); send("Access token",config.m_Source.token);
            send("Parts cache lifetime (seconds)",std::to_string(config.m_Source.timeout_parts)); send("Categories cache lifetime (seconds)",std::to_string(config.m_Source.timeout_categories));
        }
        return KSCH_OK;
    });
}
namespace {
ksch_status applyLibraryConfig(ksch_session* session,const char* const* values,uint32_t count)
{
    if(count<2) return KSCH_ERR_INVALID_ARG;
    auto invalid=[&](){session->m_Error="Invalid connection settings; configuration was not saved";return KSCH_ERR_INVALID_ARG;};
    const std::string path(values[0]),type(values[1]);
    if(path.empty()) return invalid();
    if(type=="Database" && count==9) {
        long timeout=0,rows=0,age=0;
        if(!wxString::FromUTF8(values[6]).ToLong(&timeout) || timeout<0 || timeout>3600
           || !wxString::FromUTF8(values[7]).ToLong(&rows) || rows<0 || rows>1000000
           || !wxString::FromUTF8(values[8]).ToLong(&age) || age<0 || age>31536000) return invalid();
        if(!*values[2] && !*values[3]) return invalid();
        DATABASE_LIB_SETTINGS config(path); if(!config.LoadFromFile()) return invalid();
        config.m_Source.connection_string=values[2];
        config.m_Source.dsn=*values[2]?"":values[3]; config.m_Source.username=*values[2]?"":values[4]; config.m_Source.password=*values[2]?"":values[5];
        config.m_Source.timeout=timeout; config.m_Cache.max_size=rows; config.m_Cache.max_age=age;
        config.SetReadOnly(false); if(!config.SaveToFile(wxEmptyString,true)) return invalid();
    } else if(type=="HTTP" && count==7) {
        long parts=0,categories=0;
        const wxString url=wxString::FromUTF8(values[2]);
        if((!url.StartsWith("https://") && !url.StartsWith("http://")) || !*values[3]
           || !wxString::FromUTF8(values[5]).ToLong(&parts) || parts<0 || parts>31536000
           || !wxString::FromUTF8(values[6]).ToLong(&categories) || categories<0 || categories>31536000) return invalid();
        HTTP_LIB_SETTINGS config(path); if(!config.LoadFromFile()) return invalid();
        config.m_Source.root_url=values[2]; config.m_Source.api_version=values[3]; config.m_Source.token=values[4];
        config.m_Source.timeout_parts=parts; config.m_Source.timeout_categories=categories;
        config.SetReadOnly(false); if(!config.SaveToFile(wxEmptyString,true)) return invalid();
    } else return invalid();
    session->m_SymbolLibraries.reset(); return KSCH_OK;
}
}
