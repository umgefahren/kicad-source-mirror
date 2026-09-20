// SPDX-License-Identifier: GPL-3.0-or-later
// Detached native loader and a single import commit; no dialog or frame dependency.
#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <sch_group.h>

namespace
{
ksch_status importDocumentSheet( ksch_session* session, const std::vector<wxString>& v )
{
    auto& host = session->m_Host;
    auto& schematic = *host.GetSchematic();
    auto current = host.GetCurrentSheet();
    auto invalid = [&](const wxString& text){setError(session,text);return KSCH_ERR_INVALID_ARG;};
    if(v[0]!="sheet" && v[0]!="append")return invalid("Choose hierarchical sheet or append content");
    for(size_t i:{5u,6u})if(v[i]!="yes" && v[i]!="no")return invalid("Choose valid import options");
    double x,y;
    if(!v[3].ToDouble(&x)||!v[4].ToDouble(&y)||!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>100000||std::abs(y)>100000)
        return invalid("Use finite coordinates within 100000 mm");
    wxFileName source(v[1]);
    if(!source.Normalize(wxPATH_NORM_DOTS|wxPATH_NORM_ABSOLUTE|wxPATH_NORM_TILDE,
                         wxFileName(current.LastScreen()->GetFileName()).GetPath()) || !source.FileExists() || source.GetExt()!="kicad_sch")
        return invalid("Choose an existing .kicad_sch file");
    if(v[0]=="sheet" && v[2].empty())return invalid("Enter a sheet name");
    if(v[0]=="sheet")
        for(auto* item:current.LastScreen()->Items().OfType(SCH_SHEET_T))
            if(static_cast<SCH_SHEET*>(item)->GetName()==v[2])return invalid("A sheet already has that name");
    std::unique_ptr<SCH_SHEET> loaded=std::make_unique<SCH_SHEET>(&schematic);
    loaded->SetFileName(source.GetFullPath());
    std::unique_ptr<SCH_IO> io(SCH_IO_MGR::FindPlugin(SCH_IO_MGR::SCH_KICAD));
    std::map<std::string,UTF8> options{{"hierarchical_sheet_load","1"}};
    io->LoadSchematicFile(source.GetFullPath(),&schematic,loaded.get(),&options);
    if(!io->GetError().empty() || !loaded->GetScreen())return invalid("Cannot import incomplete schematic: "+io->GetError());
    auto hierarchy=schematic.Hierarchy();
    SCH_SHEET_LIST loadedPaths(loaded.get());
    if(hierarchy.TestForRecursion(loadedPaths,current.LastScreen()->GetFileName()))return invalid("Import would create a recursive hierarchy");
    if(v[0]=="append" && loadedPaths.size()>1)return invalid("Use hierarchical sheet mode to import a schematic containing nested sheets");
    if(v[0]=="sheet")
        for(const auto& incoming:loadedPaths)
            for(const auto& existing:hierarchy)
                if(incoming.LastScreen()->GetFileName()==existing.LastScreen()->GetFileName())
                    return invalid("This file is already in the hierarchy; append its flat content or choose another file");
    const VECTOR2I offset(std::lround(x*10000),std::lround(y*10000));
    std::vector<std::unique_ptr<SCH_ITEM>> prepared;
    if(v[0]=="append")
    {
        for(auto* item:loaded->GetScreen()->Items())
        {
            if(item->Type()==SCH_GROUP_T || item->Type()==SCH_MARKER_T)continue;
            std::unique_ptr<SCH_ITEM> copy(item->Duplicate(false));
            copy->SetParentGroup(nullptr);copy->SetParent(current.LastScreen());copy->Move(offset);
            if(auto* symbol=dynamic_cast<SCH_SYMBOL*>(copy.get()))
            {
                const auto reference=static_cast<SCH_SYMBOL*>(item)->GetRef(&loadedPaths.front());
                symbol->SetNetChainName(wxEmptyString);
                symbol->SetRef(&current,reference);
                if(v[5]=="no")symbol->ClearAnnotation(&current,false);
            }
            prepared.push_back(std::move(copy));
        }
        if(prepared.empty())return invalid("The source contains no importable drawing items");
    }
    else
    {
        // Imported copies need fresh identities; instance creation below uses the loaded
        // references as its fallback when paths change. Existing document IDs are untouched.
        std::set<SCH_SCREEN*> screens;
        for(const auto& path:loadedPaths)
            if(screens.insert(path.LastScreen()).second)
                for(auto* item:path.LastScreen()->Items())
                {
                    const_cast<KIID&>(item->m_Uuid)=KIID();
                    item->RunOnChildren([](SCH_ITEM* child){const_cast<KIID&>(child->m_Uuid)=KIID();},RECURSE_MODE::NO_RECURSE);
                }
        loadedPaths=SCH_SHEET_LIST(loaded.get());
        wxString projectName=schematic.Project().GetProjectName();
        if(projectName.empty())projectName=wxFileName(schematic.Root().GetScreen()->GetFileName()).GetName();
        if(projectName.empty())projectName="Untitled";
        loadedPaths.AddNewSymbolInstances(current,projectName);
        loadedPaths.AddNewSheetInstances(current,hierarchy.GetLastVirtualPageNumber());
        if(v[5]=="no")for(const auto& path:loadedPaths)
        {
            SCH_SHEET_PATH full=current+path;
            path.LastScreen()->ClearAnnotation(&full,false);
        }
        source.MakeRelativeTo(wxFileName(current.LastScreen()->GetFileName()).GetPath());
        loaded->SetFileName(source.GetFullPath(wxPATH_UNIX));loaded->SetName(v[2]);
        loaded->SetPosition(offset);loaded->SetSize(VECTOR2I(500000,300000));
        loaded->SetParent(current.LastScreen());
        prepared.emplace_back(loaded.release());
    }
    host.GetToolManager()->RunAction(ACTIONS::cancelInteractive);
    host.GetToolManager()->RunAction(ACTIONS::selectionClear);
    SCH_COMMIT commit(host.GetToolManager());
    auto* selection=host.GetToolManager()->GetTool<SCH_SELECTION_TOOL>();
    std::unique_ptr<SCH_GROUP> group;
    if(v[0]=="append" && v[6]=="yes")group=std::make_unique<SCH_GROUP>(current.LastScreen());
    for(auto& item:prepared)
    {
        auto* raw=item.release();commit.Add(raw,current.LastScreen());
        if(group)group->AddItem(raw);else selection->AddItemToSel(raw);
    }
    if(group){auto* raw=group.release();commit.Add(raw,current.LastScreen());selection->AddItemToSel(raw);}
    commit.Push("Import Schematic");
    schematic.RefreshHierarchy();
    host.RecalculateConnections(nullptr,GLOBAL_CLEANUP);
    return KSCH_OK;
}
}
