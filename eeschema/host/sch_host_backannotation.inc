// SPDX-License-Identifier: GPL-3.0-or-later
// Board-file transport into the shared native backannotation engine.
#include <tools/backannotate.h>
#include <dsnlexer.h>
#include <ptree.h>
#include <boost/property_tree/ptree.hpp>
#include <richio.h>

namespace
{
std::string pcbBackannotationPayload( const wxString& path )
{
    std::ifstream input(path.utf8_string(),std::ios::binary);
    if(!input) throw std::runtime_error("Could not open the PCB file");
    const std::string source((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
    DSNLEXER lexer(source,path); PTREE document; Scan(&document,&lexer);
    if(document.get_child_optional("pcb_netlist")) return source;
    auto board=document.get_child_optional("kicad_pcb");
    if(!board) throw std::runtime_error("Expected a KiCad board or PCB backannotation netlist");
    STRING_FORMATTER output;
    auto quote=[&](const std::string& value){return output.Quotew(wxString::FromUTF8(value));};
    auto scalars=[](const PTREE& tree){std::vector<std::string> result;for(const auto& child:tree)if(child.second.empty())result.push_back(child.first);return result;};
    output.Print(0,"(pcb_netlist\n");
    for(const auto& item:*board)
    {
        if(item.first!="footprint" && item.first!="module") continue;
        const auto& footprint=item.second;
        const auto tokens=scalars(footprint);
        if(tokens.empty()) throw std::runtime_error("A footprint has no library identifier");
        std::string ref,value,timestamp;
        std::map<std::string,std::string> fields,pinNets;
        std::set<std::string> attributes;
        for(const auto& property:footprint)
        {
            const auto data=scalars(property.second);
            if(property.first=="property" && data.size()>=2)
            {
                fields[data[0]]=data[1];
                if(data[0]=="Reference")ref=data[1];
                if(data[0]=="Value")value=data[1];
            }
            else if(property.first=="fp_text" && data.size()>=2)
            {
                if(data[0]=="reference")ref=data[1];
                if(data[0]=="value")value=data[1];
            }
            else if(property.first=="path" && !data.empty()) timestamp=data[0];
            else if(property.first=="attr") attributes.insert(data.begin(),data.end());
            else if(property.first=="pad" && !data.empty())
            {
                auto net=property.second.get_child_optional("net");
                if(net) {const auto netTokens=scalars(*net); if(netTokens.size()>=2)pinNets[data[0]]=netTokens.back();}
            }
        }
        if(attributes.count("board_only"))continue;
        if(ref.empty())throw std::runtime_error("A footprint is missing its reference");
        output.Print(1,"(ref %s (fpid %s) (value %s) (timestamp %s)\n",quote(ref).c_str(),quote(tokens.front()).c_str(),quote(value).c_str(),quote(timestamp).c_str());
        output.Print(2,"(fields\n");
        for(const auto& [name,text]:fields)output.Print(3,"(field (name %s) %s)\n",quote(name).c_str(),quote(text).c_str());
        output.Print(2,")\n(nets\n");
        for(const auto& [pin,net]:pinNets)output.Print(3,"(pin_net %s %s)\n",quote(pin).c_str(),quote(net).c_str());
        output.Print(2,")\n");
        for(const auto& attr:attributes)
            if(attr=="dnp" || attr=="exclude_from_bom" || attr=="exclude_from_sim" || attr=="exclude_from_pos_files")
                output.Print(2,"(property (name %s))\n",quote(attr).c_str());
        output.Print(1,")\n");
    }
    output.Print(0,")\n");
    return output.GetString();
}
}
