// SPDX-License-Identifier: GPL-3.0-or-later
// The synchronization comparison and transaction are model services; GPUI owns the editor.
#include <sch_sheet_pin.h>
#include <schematic_settings.h>

namespace
{
struct SHEET_PIN_CHANGE
{
    SCH_SHEET* sheet;
    SCH_SCREEN* parent;
    SCH_SHEET_PIN* pin;
    SCH_HIERLABEL* label;
    wxString caption;
    std::vector<std::string> actions;
};

std::vector<SHEET_PIN_CHANGE> sheetPinChanges( ksch_session* session, bool all, std::string& identity )
{
    std::vector<std::pair<SCH_SHEET*, SCH_SCREEN*>> sheets;
    if( all )
    {
        std::set<SCH_SHEET*> seen;
        for( const auto& path : session->m_Host.GetSchematic()->Hierarchy() )
            if( path.size() > 1 && seen.insert( path.Last() ).second )
                sheets.emplace_back( path.Last(), path.at( path.size() - 2 )->GetScreen() );
    }
    else if( auto* sheet = dynamic_cast<SCH_SHEET*>( propertyItem( session ) ) )
        sheets.emplace_back( sheet, session->m_Host.GetScreen() );
    else throw std::invalid_argument( "Select a hierarchical sheet to synchronize" );
    std::vector<SHEET_PIN_CHANGE> changes;
    identity = all ? "all:" : "selected:";
    for( auto [sheet, parent] : sheets )
    {
        if( !sheet->GetScreen() ) continue;
        std::set<SCH_HIERLABEL*> matched;
        std::vector<SCH_HIERLABEL*> labels;
        std::set<wxString> labelNames;
        for( auto* item : sheet->GetScreen()->Items().OfType( SCH_HIER_LABEL_T ) )
        {
            auto* label = static_cast<SCH_HIERLABEL*>( item );
            if( labelNames.insert( label->GetText() ).second ) labels.push_back( label );
        }
        const auto row = [&]( SCH_SHEET_PIN* pin, SCH_HIERLABEL* label )
        {
            const auto* text = pin ? static_cast<SCH_LABEL_BASE*>( pin ) : label;
            wxString status = !pin ? "missing pin" : !label ? "missing label" : "electrical type differs";
            std::vector<std::string> actions{ "Keep" };
            if( !pin ) actions.insert( actions.end(), { "Add pin", "Remove label" } );
            else if( !label ) actions.insert( actions.end(), { "Add label", "Remove pin" } );
            else actions.insert( actions.end(), { "Match pin to label", "Match label to pin" } );
            changes.push_back( { sheet, parent, pin, label,
                sheet->GetName() + " / " + text->GetText() + " (" + status + ")", std::move( actions ) } );
        };
        identity += sheet->m_Uuid.AsString().utf8_string();
        for( auto* pin : sheet->GetPins() )
        {
            identity += pin->m_Uuid.AsString().utf8_string() + pin->GetText().utf8_string()
                        + std::to_string( static_cast<int>( pin->GetShape() ) );
            auto found = std::find_if( labels.begin(), labels.end(), [&]( auto* label )
                                      { return label->GetText() == pin->GetText(); } );
            if( found == labels.end() ) row( pin, nullptr );
            else
            {
                matched.insert( *found );
                if( pin->GetShape() != (*found)->GetShape() ) row( pin, *found );
            }
        }
        for( auto* label : labels )
        {
            identity += label->m_Uuid.AsString().utf8_string() + label->GetText().utf8_string()
                        + std::to_string( static_cast<int>( label->GetShape() ) );
            if( !matched.contains( label ) ) row( nullptr, label );
        }
    }
    return changes;
}
}

extern "C" ksch_status ksch_session_sheet_pin_properties( ksch_session* session, int all,
        ksch_property_visitor visitor, void* context )
{
    if( !session || !visitor ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        std::string identity;
        auto changes = sheetPinChanges( session, all != 0, identity );
        // An empty result is still a useful reviewed result, with a stable snapshot identity.
        for( const auto& change : changes )
        {
            std::vector<const char*> choices;
            for( const auto& action : change.actions ) choices.push_back( action.c_str() );
            visitor( context, identity.c_str(), change.caption.utf8_string().c_str(), "Keep",
                     KSCH_PROPERTY_CHOICE, choices.data(), choices.size() );
        }
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_sheet_pin_properties( ksch_session* session, int all,
        const char* expected, const char* const* values, uint32_t count )
{
    if( !session || !expected || ( count && !values ) ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        std::string identity;
        auto changes = sheetPinChanges( session, all != 0, identity );
        if( changes.empty() && count == 0 ) return KSCH_OK;
        if( identity != expected || count != changes.size() )
        { setError( session, "Sheet pins changed; reopen synchronization" ); return KSCH_ERR_INVALID_ARG; }
        bool modified = false;
        for( size_t i = 0; i < changes.size(); ++i )
        {
            if( !values[i] || std::find( changes[i].actions.begin(), changes[i].actions.end(), values[i] ) == changes[i].actions.end() )
                return KSCH_ERR_INVALID_ARG;
            modified |= std::string( values[i] ) != "Keep";
        }
        if( !modified ) return KSCH_OK;
        std::map<SCH_HIERLABEL*, std::string> labelEdits;
        for( size_t i = 0; i < changes.size(); ++i )
        {
            const std::string action = values[i];
            if( action == "Remove label" || action == "Match label to pin" )
            {
                const std::string desired = action == "Remove label" ? "remove" :
                    std::to_string( static_cast<int>( changes[i].pin->GetShape() ) );
                auto [position, inserted] = labelEdits.emplace( changes[i].label, desired );
                if( !inserted && position->second != desired )
                { setError( session, "Conflicting changes to a label shared by multiple sheets" ); return KSCH_ERR_INVALID_ARG; }
            }
        }
        std::set<SCH_HIERLABEL*> changedLabels;
        std::set<std::pair<SCH_SCREEN*, wxString>> addedLabels;
        SCH_COMMIT commit( session->m_Host.GetToolManager() );
        for( size_t i = 0; i < changes.size(); ++i )
        {
            auto& change = changes[i];
            const std::string action = values[i];
            if( action == "Keep" ) continue;
            auto* child = change.sheet->GetScreen();
            if( ( action == "Remove label" || action == "Match label to pin" )
                && !changedLabels.insert( change.label ).second ) continue;
            if( action == "Add label" && !addedLabels.emplace( child, change.pin->GetText() ).second ) continue;
            if( action == "Add pin" || action == "Remove pin" || action == "Match pin to label" )
                commit.Modify( change.sheet, change.parent, RECURSE_MODE::NO_RECURSE );
            if( action == "Add pin" )
            {
                auto* pin = new SCH_SHEET_PIN( change.sheet );
                pin->SetText( change.label->GetText() );
                pin->SetShape( change.label->GetShape() );
                pin->SetTextSize( change.label->GetTextSize() );
                const int pitch = schIUScale.MilsToIU( 100 );
                const int offset = pitch * ( static_cast<int>( change.sheet->GetPins().size() ) + 1 );
                auto size = change.sheet->GetSize();
                if( size.y < offset + pitch ) { size.y = offset + pitch; change.sheet->Resize( size ); }
                pin->ConstrainOnEdge( change.sheet->GetPosition() + VECTOR2I( 0, offset ), true );
                change.sheet->AddPin( pin );
            }
            else if( action == "Remove pin" )
            { change.sheet->RemovePin( change.pin ); delete change.pin; }
            else if( action == "Match pin to label" ) change.pin->SetShape( change.label->GetShape() );
            else if( action == "Add label" )
            {
                auto* label = new SCH_HIERLABEL( VECTOR2I( schIUScale.MilsToIU( 1000 ),
                    schIUScale.MilsToIU( 1000 + static_cast<int>( i ) * 100 ) ) );
                label->SetParent( session->m_Host.GetSchematic() );
                label->SetText( change.pin->GetText() );
                label->SetShape( change.pin->GetShape() );
                label->SetTextSize( change.pin->GetTextSize() );
                commit.Add( label, child );
            }
            else if( action == "Remove label" ) commit.Remove( change.label, child );
            else if( action == "Match label to pin" )
            { commit.Modify( change.label, child ); change.label->SetShape( change.pin->GetShape() ); }
        }
        commit.Push( "Synchronize Sheet Pins" );
        return KSCH_OK;
    } );
}
