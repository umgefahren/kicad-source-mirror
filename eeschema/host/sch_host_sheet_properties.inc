// SPDX-License-Identifier: GPL-3.0-or-later
// Model-only hierarchical file relinking for the GPUI properties editor.
#include <sch_io/sch_io.h>
#include <sch_io/sch_io_mgr.h>
#include <common.h>

extern "C" ksch_status ksch_session_relink_sheet( ksch_session* session,
        const char* item_id, const char* path_utf8 )
{
    if( !session || !item_id || !path_utf8 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto* sheet = dynamic_cast<SCH_SHEET*>( propertyItem( session ) );
        if( !sheet || sheet->m_Uuid.AsString() != wxString::FromUTF8( item_id ) )
        { setError( session, "Select the same hierarchical sheet before relinking" ); return KSCH_ERR_INVALID_ARG; }
        const bool preview = sheet->HasFlag( IS_NEW ) && sheet->HasFlag( IS_MOVING );
        auto* schematic = session->m_Host.GetSchematic();
        const auto& current = session->m_Host.GetCurrentSheet();
        const wxString rawPath = wxString::FromUTF8( path_utf8 );
        wxString error = GetFieldValidationErrorMessage( FIELD_T::SHEET_FILENAME, rawPath );
        if( !error.empty() ) { setError( session, error ); return KSCH_ERR_INVALID_ARG; }
        wxFileName filename( ExpandTextVars( rawPath, &schematic->Project(), INTERNAL ) );
        if( filename.GetExt().empty() ) filename.SetExt( "kicad_sch" );
        if( filename.GetExt() != "kicad_sch" )
        { setError( session, "Choose a .kicad_sch file" ); return KSCH_ERR_INVALID_ARG; }
        if( !filename.Normalize( wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE,
                                 wxFileName( current.LastScreen()->GetFileName() ).GetPath() ) )
        { setError( session, "Invalid sheet filename" ); return KSCH_ERR_INVALID_ARG; }
        const wxString absolute = filename.GetFullPath();
        if( sheet->GetScreen() && absolute == sheet->GetScreen()->GetFileName() ) return KSCH_OK;
        // Prepare the complete replacement before touching the current sheet or undo history.
        std::unique_ptr<SCH_SHEET> replacement = std::make_unique<SCH_SHEET>( schematic );
        const_cast<KIID&>( replacement->m_Uuid ) = sheet->m_Uuid;
        replacement->SetFileName( absolute );
        SCH_SCREEN* existing = nullptr;
        schematic->Root().SearchHierarchy( absolute, &existing );
        if( existing ) replacement->SetScreen( existing );
        else
        {
            std::unique_ptr<SCH_IO> io( SCH_IO_MGR::FindPlugin( SCH_IO_MGR::SCH_KICAD ) );
            if( !filename.FileExists() )
            {
                if( !sheet->GetScreen() )
                {
                    replacement->SetScreen( new SCH_SCREEN( schematic ) );
                    replacement->GetScreen()->SetFileName( absolute );
                    io->SaveSchematicFile( absolute, replacement.get(), schematic );
                    replacement->SetScreen( nullptr );
                }
                else io->SaveSchematicFile( absolute, sheet, schematic );
            }
            std::map<std::string, UTF8> options{ { "hierarchical_sheet_load", "1" } };
            io->LoadSchematicFile( absolute, schematic, replacement.get(), &options );
            if( !io->GetError().empty() )
            { setError( session, io->GetError() ); return KSCH_ERR_IO; }
        }
        if( !replacement->GetScreen() ) return KSCH_ERR_IO;
        auto hierarchy = schematic->Hierarchy();
        SCH_SHEET_LIST loaded( replacement.get() );
        if( hierarchy.TestForRecursion( loaded, current.LastScreen()->GetFileName() ) )
        { setError( session, "The sheet link would create a recursive hierarchy" ); return KSCH_ERR_INVALID_ARG; }
        wxString projectName = schematic->Project().GetProjectName();
        if( projectName.empty() ) projectName = wxFileName( schematic->Root().GetScreen()->GetFileName() ).GetName();
        if( projectName.empty() ) projectName = "Untitled";
        loaded.AddNewSymbolInstances( current, projectName );
        loaded.AddNewSheetInstances( current, hierarchy.GetLastVirtualPageNumber() );
        // Native sheet-screen relinks are not undoable: retain that explicit contract.
        if( !preview )
        {
            session->m_Host.ClearUndoORRedoList( UNDO_REDO_HOLDER::UNDO_LIST );
            session->m_Host.ClearUndoORRedoList( UNDO_REDO_HOLDER::REDO_LIST );
        }
        sheet->SetScreen( replacement->GetScreen() );
        filename.MakeRelativeTo( wxFileName( current.LastScreen()->GetFileName() ).GetPath() );
        sheet->SetFileName( filename.GetFullPath( wxPATH_UNIX ) );
        if( preview ) session->m_Host.GetToolManager()->RunAction( ACTIONS::refreshPreview );
        else
        {
            schematic->RefreshHierarchy();
            SCH_SHEET_LIST repaired;
            repaired.BuildSheetList( &schematic->Root(), true );
            session->m_Host.RecalculateConnections( nullptr, GLOBAL_CLEANUP );
            session->m_Host.OnModify();
        }
        return KSCH_OK;
    } );
}
