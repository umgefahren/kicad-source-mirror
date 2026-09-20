// SPDX-License-Identifier: GPL-3.0-or-later
#include <eeschema_settings.h>
#include <settings/settings_manager.h>
#include <remote_provider_client.h>
#include <oauth/secure_token_store.h>
namespace {
ksch_status readRemoteProviders(ksch_session* session,ksch_document_visitor visitor,void* context)
{
    auto* settings=GetAppSettings<EESCHEMA_SETTINGS>("eeschema");
    if(!settings){session->m_Error="Schematic application settings are unavailable";return KSCH_ERR_INVALID_ARG;}
    const auto& remote=settings->m_RemoteSymbol;
    auto send=[&](const wxString& name,const wxString& value){visitor(context,name.utf8_string().c_str(),value.utf8_string().c_str());};
    send("Destination directory",remote.destination_dir);send("Library prefix",remote.library_prefix);
    send("Add to global library table",remote.add_to_global_table?"true":"false");
    for(size_t i=0;i<remote.providers.size();++i){
        const auto prefix=wxString::Format("Provider %zu / ",i+1);const auto& provider=remote.providers[i];
        send(prefix+"Metadata URL",provider.metadata_url);send(prefix+"Display name",provider.display_name_override);
        send(prefix+"Account",provider.last_account_label);send(prefix+"Authentication",provider.last_auth_status);
    }
    return KSCH_OK;
}
ksch_status applyRemoteProviders(ksch_session* session,uint32_t kind,const char* const* values,uint32_t count)
{
    auto* settings=GetAppSettings<EESCHEMA_SETTINGS>("eeschema");
    if(!settings)return KSCH_ERR_INVALID_ARG;
    auto replacement=settings->m_RemoteSymbol;
    if(kind==10){
        if(count<3 || (count-3)%4 || !*values[0] || !*values[1]
            || (std::string(values[2])!="true" && std::string(values[2])!="false"))return KSCH_ERR_INVALID_ARG;
        std::set<wxString> urls;std::vector<REMOTE_PROVIDER_ENTRY> providers;
        for(uint32_t i=3;i<count;i+=4){
            const auto url=wxString::FromUTF8(values[i]);
            if((!url.StartsWith("https://") && !url.StartsWith("http://")) || !urls.insert(url).second){session->m_Error="Provider metadata URLs must be unique HTTP or HTTPS URLs";return KSCH_ERR_INVALID_ARG;}
            auto provider=replacement.UpsertProvider(url);provider.display_name_override=wxString::FromUTF8(values[i+1]);
            providers.push_back(std::move(provider));
        }
        replacement.providers=std::move(providers);replacement.destination_dir=wxString::FromUTF8(values[0]);replacement.library_prefix=wxString::FromUTF8(values[1]);replacement.add_to_global_table=std::string(values[2])=="true";
        if(!replacement.FindProviderById(replacement.last_used_provider_id))replacement.last_used_provider_id.clear();
    }else{
        if(count!=1)return KSCH_ERR_INVALID_ARG;
        auto* provider=replacement.FindProviderByMetadataUrl(wxString::FromUTF8(values[0]));
        if(!provider){session->m_Error="Save this provider before refreshing or signing out";return KSCH_ERR_INVALID_ARG;}
        if(kind==11){
            REMOTE_PROVIDER_CLIENT client;REMOTE_PROVIDER_METADATA metadata;REMOTE_PROVIDER_ERROR error;
            if(!client.DiscoverProvider(provider->metadata_url,metadata,error)){session->m_Error=error.message.utf8_string();return KSCH_ERR_INVALID_ARG;}
            if(provider->display_name_override.empty())provider->display_name_override=metadata.provider_name;
        }else{
            SECURE_TOKEN_STORE store;
            if(!store.DeleteTokens(provider->provider_id,"default")){session->m_Error="Credentials could not be deleted from the operating system store";return KSCH_ERR_INVALID_ARG;}
            provider->last_account_label.clear();provider->last_auth_status="signed_out";
        }
    }
    const auto before=settings->m_RemoteSymbol;
    settings->m_RemoteSymbol=std::move(replacement);
    if(!settings->SaveToFile(Pgm().GetSettingsManager().GetPathForSettingsFile(settings),true)) {
        settings->m_RemoteSymbol=before;session->m_Error="Remote provider settings could not be saved";return KSCH_ERR_INVALID_ARG;
    }
    return KSCH_OK;
}
}
