/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * @file sch_host_abi.cpp
 * @brief Implementation of the C ABI declared in `include/sch_host/sch_host_abi.h`.
 *
 * Everything here exists to keep two promises the header makes: no C++ type
 * crosses the boundary, and no exception does either. The second is enforced by
 * routing every entry point through guard(), which catches everything, records
 * a message on the session and turns it into a ::ksch_status.
 */

#include <iomanip>
#include <sstream>
#include <exception>
#include <sim/sim_lib_mgr.h>
#include <sim/spice_circuit_model.h>
#include <sim/ngspice.h>
#include <reporter.h>
#include <string_utils.h>
#include <richio.h>
#include <database/database_lib_settings.h>
#include <http_lib/http_lib_settings.h>
#include <cmath>
#include <limits>
#include <inspectable_impl.h>
#include <properties/property.h>
#include <properties/property_mgr.h>
#include <units_provider.h>
#include <project/project_file.h>
#include <project/net_settings.h>
#include <fstream>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include <bitmaps/bitmap_info.h>
#include <bitmaps/bitmaps_list.h>
#include <gal/recording/draw_stream.h>
#include <gal/recording/recording_gal.h>
#include <sch_painter.h>
#include <sch_render_settings.h>
#include <hotkeys_basic.h>
#include <ki_exception.h>
#include <locale_io.h>
#include <pgm_base.h>
#include <sch_host/sch_host_abi.h>
#include <tool/action_manager.h>
#include <tool/actions.h>
#include <tool/host_tool_dispatcher.h>
#include <tool/tool_action.h>
#include <view/host_view_controls.h>
#include <wx/filename.h>
#include <wx/string.h>

#include "sch_host.h"
#include <schematic.h>
#include <sch_marker.h>
#include <sch_screen.h>
#include <tools/sch_find_replace_tool.h>
#include <tool/tool_manager.h>
#include <sch_item.h>
#include <sch_symbol.h>
#include <lib_symbol.h>
#include <project_sch.h>
#include <libraries/symbol_library_adapter.h>
#include <tools/sch_actions.h>
#include <set>
#include <sch_field.h>
#include <sch_text.h>
#include <sch_label.h>
#include <sch_sheet.h>
#include <sch_table.h>
#include <sch_tablecell.h>
#include <sch_bitmap.h>
#include <validators.h>
#include <sch_commit.h>
#include <tools/sch_selection_tool.h>


/**
 * The session as C sees it.
 *
 * A thin shell around SCH_HOST holding the two things the ABI needs but the
 * C++ class does not: the error string a C caller reads back, and storage that
 * keeps the strings in a ::ksch_sheet_info alive across the return.
 */
namespace { ksch_session* activeSimulationSession = nullptr; }

struct ksch_session
{
    SCH_HOST m_Host;
    std::unique_ptr<KIGFX::RECORDING_GAL> m_SymbolPreview;
    // Ngspice is process-global. The host retains its adapter for the process lifetime;
    // it never constructs a simulator frame or the factory's wx error dialogs.
    std::shared_ptr<SPICE_SIMULATOR> m_Simulator;
    std::shared_ptr<NGSPICE_SETTINGS> m_SimSettings;
    std::string m_SimCommand = ".op";
    std::string m_SimVector;
    std::map<std::string,std::string> m_SimUserSignals;
    bool m_SimSignalsPending=false;
    std::vector<SCH_FIELD> m_SimModelDraft;
    std::string m_SimModelDraftUuid,m_SimModelBaseline,m_SimParserLog;
    SPICE_VALUE_FORMAT m_SimFormat{ 6, "~" };
    uint32_t m_SimOptions = NETLIST_EXPORTER_SPICE::OPTION_DEFAULT_FLAGS;
    ~ksch_session() { if( m_Simulator && activeSimulationSession == this ) { m_Simulator->Stop(); m_Simulator->SetReporter( nullptr ); activeSimulationSession = nullptr; } }


    // Bind libraries to this document, not the process-wide active GUI project.
    std::unique_ptr<LIBRARY_MANAGER> m_SymbolLibraries;

    /// Last error, as UTF-8. Owned here so the returned pointer outlives the call.
    std::string m_Error;

    /// Backing store for the strings of the most recent ksch_session_sheet_info().
    std::string m_SheetName;
    std::string m_SheetPath;
    std::string m_SheetPage;

    /// Backing store for the strings of the most recent ksch_session_editor_state().
    std::string m_SymbolIds;
    std::string m_ToolName;
    std::string m_StatusText;
    std::string m_DocumentReport;
};


namespace
{

SYMBOL_LIBRARY_ADAPTER* symbolLibraries( ksch_session* session )
{
    if( !session->m_SymbolLibraries )
    {
        auto manager = std::make_unique<LIBRARY_MANAGER>( &session->m_Host.GetSchematic()->Project() );
        manager->RegisterAdapter( LIBRARY_TABLE_TYPE::SYMBOL,
                                  std::make_unique<SYMBOL_LIBRARY_ADAPTER>( *manager ) );
        manager->LoadGlobalTables( { LIBRARY_TABLE_TYPE::SYMBOL } );
        const auto projectPath = session->m_Host.GetSchematic()->Project().GetProjectPath();
        manager->LoadProjectTables( projectPath.empty()
            ? wxFileName( session->m_Host.GetSchematic()->RootScreen()->GetFileName() ).GetPath()
            : projectPath, { LIBRARY_TABLE_TYPE::SYMBOL } );
        session->m_SymbolLibraries = std::move( manager );
    }
    return static_cast<SYMBOL_LIBRARY_ADAPTER*>(
            session->m_SymbolLibraries->Adapter( LIBRARY_TABLE_TYPE::SYMBOL ).value() );
}

/// Returned by ksch_session_last_error() and friends so they never yield null.
const char* const EMPTY_STRING = "";


/// Error text for the calls that have no session to record it on.
std::string& globalError()
{
    static std::string error;
    return error;
}


void setError( ksch_session* aSession, const wxString& aMessage )
{
    if( aSession )
        aSession->m_Error = aMessage.utf8_string();
    else
        globalError() = aMessage.utf8_string();
}


/**
 * Run @p aBody, converting anything it throws into a status code.
 *
 * KiCad's reader throws IO_ERROR and PARSE_ERROR on malformed input, and the
 * document model is free to throw std::bad_alloc or anything else. None of that
 * may unwind past this file: a Rust caller has no handler and the process would
 * abort. So the net is deliberately total, including the bare `...` case.
 */
template <typename FUNC>
ksch_status guard( ksch_session* aSession, FUNC aBody )
{
    try
    {
        return aBody();
    }
    catch( const IO_ERROR& e )
    {
        setError( aSession, e.What() );
        return KSCH_ERR_LOAD_FAILED;
    }
    catch( const std::bad_alloc& )
    {
        setError( aSession, wxT( "Out of memory." ) );
        return KSCH_ERR_OUT_OF_MEMORY;
    }
    catch( const std::exception& e )
    {
        setError( aSession, wxString::FromUTF8( e.what() ) );
        return KSCH_ERR_INTERNAL;
    }
    catch( ... )
    {
        setError( aSession, wxT( "Unknown exception crossing the host ABI boundary." ) );
        return KSCH_ERR_INTERNAL;
    }
}


/* ------------------------------------------------------------------ input */

/**
 * Translate a ::ksch_input_event into the C++ struct the dispatcher takes.
 *
 * Three vocabularies meet here and nowhere else: the ABI's button ordinals become
 * KiCad's `BUT_*` bits, the ABI's modifier bits become `MD_*` bits, and the UI's
 * key *name* becomes a `WXK_*` code. Each of those is a place where a wrong number
 * is a feature that silently does not work, which is why they are all on this side
 * of the boundary, where a compiler reads the real values out of KiCad's headers.
 *
 * @return false if the event's type is not one this ABI defines.
 */
bool toHostInput( const ksch_input_event& aEvent, HOST_INPUT_EVENT& aOut )
{
    switch( aEvent.type )
    {
    case KSCH_INPUT_POINTER_MOTION: aOut.type = HOST_INPUT_TYPE::POINTER_MOTION; break;
    case KSCH_INPUT_POINTER_DOWN: aOut.type = HOST_INPUT_TYPE::POINTER_DOWN; break;
    case KSCH_INPUT_POINTER_UP: aOut.type = HOST_INPUT_TYPE::POINTER_UP; break;
    case KSCH_INPUT_POINTER_DBLCLICK: aOut.type = HOST_INPUT_TYPE::POINTER_DBLCLICK; break;
    case KSCH_INPUT_POINTER_LEAVE: aOut.type = HOST_INPUT_TYPE::POINTER_LEAVE; break;
    case KSCH_INPUT_SCROLL: aOut.type = HOST_INPUT_TYPE::SCROLL; break;
    case KSCH_INPUT_KEY_DOWN: aOut.type = HOST_INPUT_TYPE::KEY_DOWN; break;
    case KSCH_INPUT_KEY_UP: aOut.type = HOST_INPUT_TYPE::KEY_UP; break;
    case KSCH_INPUT_CANCEL: aOut.type = HOST_INPUT_TYPE::CANCEL; break;
    default: return false;
    }

    switch( aEvent.button )
    {
    case KSCH_BUTTON_LEFT: aOut.button = BUT_LEFT; break;
    case KSCH_BUTTON_RIGHT: aOut.button = BUT_RIGHT; break;
    case KSCH_BUTTON_MIDDLE: aOut.button = BUT_MIDDLE; break;
    case KSCH_BUTTON_BACK: aOut.button = BUT_AUX1; break;
    case KSCH_BUTTON_FORWARD: aOut.button = BUT_AUX2; break;
    default: aOut.button = BUT_NONE; break;
    }

    aOut.modifiers = 0;

    if( aEvent.modifiers & KSCH_MOD_SHIFT )
        aOut.modifiers |= MD_SHIFT;

    if( aEvent.modifiers & KSCH_MOD_ALT )
        aOut.modifiers |= MD_ALT;

    // The Command/Control question, which has exactly one right answer and it is not
    // the obvious one.
    //
    // KiCad's hotkey table is written in MD_* bits — `.DefaultHotkey( MD_CTRL + 'Z' )`
    // — and on macOS those are reached with **Command**, not Control, because
    // `wxMOD_CMD == wxMOD_CONTROL` there (`wx/defs.h`) and `decodeModifiers` tests
    // `wxMOD_CONTROL`. Physical Control arrives as `wxMOD_RAW_CONTROL`, which
    // `decodeModifiers` does not look at, so in the wx editor on macOS it produces no
    // modifier bit at all.
    //
    // Mapping the UI's "meta" (Command on macOS) to MD_META instead would be the
    // literal translation and would leave every Ctrl-keyed shortcut in the tree
    // unreachable, while physical Control would fire them — inverted from the wx app
    // on the same machine, and silently, because an unmatched hotkey does nothing.
#ifdef __APPLE__
    if( aEvent.modifiers & KSCH_MOD_META )
        aOut.modifiers |= MD_CTRL;
#else
    if( aEvent.modifiers & KSCH_MOD_CTRL )
        aOut.modifiers |= MD_CTRL;

    if( aEvent.modifiers & KSCH_MOD_META )
        aOut.modifiers |= MD_META;
#endif

    aOut.keyCode = aEvent.key ? HOST_TOOL_DISPATCHER::KeyCodeFromName( aEvent.key ) : 0;
    aOut.isAutoRepeat = ( aEvent.flags & KSCH_INPUT_FLAG_AUTOREPEAT ) != 0;

    aOut.position = VECTOR2D( aEvent.x, aEvent.y );
    aOut.scrollDelta = VECTOR2D( aEvent.scroll_x, aEvent.scroll_y );

    return true;
}


/* ------------------------------------------------------- action registry */

/**
 * One action, with its strings owned so the ABI can hand out stable pointers.
 *
 * TOOL_ACTION's getters return wxString by value, so a pointer into one would
 * dangle the moment the temporary died. The table below converts each action
 * once and keeps the result for the lifetime of the process, which is also what
 * the header promises the caller.
 */
struct ACTION_RECORD
{
    std::string m_Name;
    std::string m_FriendlyName;
    std::string m_MenuLabel;
    std::string m_Tooltip;
    std::string m_Description;
    std::string m_IconName;
    std::string m_ToolName;

    ksch_action m_Abi;
    std::string m_HotkeyName;
    std::string m_HotkeyAltName;
};


/**
 * Map every BITMAPS enumerator that has an image to its base name.
 *
 * The enumerator name is the SVG file name, but the enum carries no names at
 * runtime. The generated table in `common/bitmap_info.cpp` does carry them,
 * indirectly: for the light theme the CMake generator writes the file name as
 * `<name>[_<height>].png`, so stripping the extension and the height suffix
 * recovers exactly the enumerator name, which is what the Rust side needs in
 * order to find `sources/{light,dark}/<name>.svg`.
 */
const std::unordered_map<BITMAPS, std::string>& iconNames()
{
    static const std::unordered_map<BITMAPS, std::string> names = []
    {
        std::unordered_map<BITMAPS, std::vector<BITMAP_INFO>> cache;
        BuildBitmapInfo( cache );

        std::unordered_map<BITMAPS, std::string> result;

        for( const auto& [id, infos] : cache )
        {
            for( const BITMAP_INFO& info : infos )
            {
                // Only the light theme leaves the base name untouched; the dark
                // variants carry a "_dark" tag that would have to be stripped too.
                if( info.theme != BITMAP_INFO::THEME::LIGHT )
                    continue;

                std::string base = info.filename;

                const std::string ext = ".png";

                if( base.size() > ext.size() && base.compare( base.size() - ext.size(),
                                                              ext.size(), ext ) == 0 )
                {
                    base.erase( base.size() - ext.size() );
                }

                // A height of -1 means the generator emitted no size suffix.
                if( info.height >= 0 )
                {
                    const std::string suffix = "_" + std::to_string( info.height );

                    if( base.size() > suffix.size()
                        && base.compare( base.size() - suffix.size(), suffix.size(), suffix )
                                   == 0 )
                    {
                        base.erase( base.size() - suffix.size() );
                    }
                }

                result.emplace( id, base );
                break;
            }
        }

        return result;
    }();

    return names;
}


/**
 * The action table, built once on first use.
 *
 * ACTION_MANAGER::GetActionList() is a function-local static that every
 * TOOL_ACTION constructor pushes itself onto, so it is already complete when
 * this runs — no frame, no window and no wxApp required. That is the whole
 * reason the registry is exposable at all.
 */
const std::vector<ACTION_RECORD>& actionTable()
{
    static const std::vector<ACTION_RECORD> table = []
    {
        std::vector<ACTION_RECORD> records;

        const std::list<TOOL_ACTION*>& actions = ACTION_MANAGER::GetActionList();
        records.reserve( actions.size() );

        for( TOOL_ACTION* action : actions )
        {
            if( !action )
                continue;

            ACTION_RECORD record;

            record.m_Name = action->GetName();
            record.m_FriendlyName = action->GetFriendlyName().utf8_string();
            record.m_MenuLabel = action->GetMenuLabel().utf8_string();
            record.m_Tooltip = action->GetTooltip().utf8_string();
            record.m_Description = action->GetDescription().utf8_string();
            record.m_ToolName = action->GetToolName();

            BITMAPS icon = action->GetIcon();

            if( icon != BITMAPS::INVALID_BITMAP )
            {
                auto it = iconNames().find( icon );

                if( it != iconNames().end() )
                    record.m_IconName = it->second;
            }

            record.m_Abi.id = action->GetId();
            record.m_Abi.ui_id = action->GetUIId();
            record.m_Abi.default_hotkey = action->GetDefaultHotKey();
            record.m_Abi.default_hotkey_alt = action->GetDefaultHotKeyAlt();
            record.m_Abi.hotkey = action->GetHotKey();
            record.m_Abi.hotkey_alt = action->GetHotKeyAlt();
            if( record.m_Abi.hotkey )
                record.m_HotkeyName = KeyNameFromKeyCode( record.m_Abi.hotkey ).utf8_string();

            if( record.m_Abi.hotkey_alt )
                record.m_HotkeyAltName = KeyNameFromKeyCode( record.m_Abi.hotkey_alt ).utf8_string();
            record.m_Abi.scope = static_cast<std::int32_t>( action->GetScope() );

            record.m_Abi.flags = 0;

            if( action->IsActivation() )
                record.m_Abi.flags |= KSCH_ACTION_FLAG_ACTIVATION;

            if( action->IsNotification() )
                record.m_Abi.flags |= KSCH_ACTION_FLAG_NOTIFICATION;

            records.push_back( std::move( record ) );
        }

        // The string pointers must be filled in only after the vector has stopped
        // reallocating, or every record written before a growth would point into
        // freed storage.
        for( ACTION_RECORD& record : records )
        {
            record.m_Abi.name = record.m_Name.c_str();
            record.m_Abi.friendly_name = record.m_FriendlyName.c_str();
            record.m_Abi.menu_label = record.m_MenuLabel.c_str();
            record.m_Abi.tooltip = record.m_Tooltip.c_str();
            record.m_Abi.description = record.m_Description.c_str();
            record.m_Abi.icon_name = record.m_IconName.c_str();
            record.m_Abi.tool_name = record.m_ToolName.c_str();
        }

        return records;
    }();

    return table;
}

} // namespace


/* ------------------------------------------------------------ diagnostics */

extern "C" const char* ksch_status_name( ksch_status aStatus )
{
    switch( aStatus )
    {
    case KSCH_OK:                 return "KSCH_OK";
    case KSCH_ERR_INVALID_ARG:    return "KSCH_ERR_INVALID_ARG";
    case KSCH_ERR_NO_DOCUMENT:    return "KSCH_ERR_NO_DOCUMENT";
    case KSCH_ERR_FILE_NOT_FOUND: return "KSCH_ERR_FILE_NOT_FOUND";
    case KSCH_ERR_LOAD_FAILED:    return "KSCH_ERR_LOAD_FAILED";
    case KSCH_ERR_OUT_OF_RANGE:   return "KSCH_ERR_OUT_OF_RANGE";
    case KSCH_ERR_IO:             return "KSCH_ERR_IO";
    case KSCH_ERR_OUT_OF_MEMORY:  return "KSCH_ERR_OUT_OF_MEMORY";
    case KSCH_ERR_INTERNAL:       return "KSCH_ERR_INTERNAL";
    }

    return "KSCH_ERR_UNKNOWN";
}


extern "C" uint32_t ksch_abi_version( void )
{
    return KSCH_ABI_VERSION;
}


/* ---------------------------------------------------------------- session */

extern "C" ksch_session* ksch_session_create( void )
{
    // Without a PGM_BASE there is no settings manager, and the first thing a
    // load does is ask for one. Failing here, with a message that names the fix,
    // beats a null dereference several frames into the document model.
    if( !PgmOrNull() )
    {
        globalError() = "The process singletons are not standing: call ksch_runtime_init() "
                        "(or install a PGM_BASE) before creating a session.";
        return nullptr;
    }

    try
    {
        return new ksch_session();
    }
    catch( const std::exception& e )
    {
        globalError() = e.what();
    }
    catch( ... )
    {
        globalError() = "Unknown exception while creating a session.";
    }

    return nullptr;
}


extern "C" void ksch_session_destroy( ksch_session* aSession )
{
    // Teardown runs SCH_HOST's destructor, which tears down a VIEW and a
    // SCHEMATIC. Neither should throw, but a destructor that escaped into C
    // would terminate the process, so the net stays up here too.
    try
    {
        delete aSession;
    }
    catch( ... )
    {
    }
}


extern "C" const char* ksch_session_last_error( const ksch_session* aSession )
{
    if( !aSession )
        return EMPTY_STRING;

    return aSession->m_Error.c_str();
}


extern "C" const char* ksch_last_global_error( void )
{
    return globalError().c_str();
}


/* --------------------------------------------------------------- document */

extern "C" ksch_status ksch_session_load_file( ksch_session* aSession, const char* aPathUtf8 )
{
    if( !aSession || !aPathUtf8 )
        return KSCH_ERR_INVALID_ARG;

    aSession->m_Error.clear();

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      // Number formatting in the s-expression reader is locale
                      // sensitive: a decimal comma turns every coordinate in the
                      // file into a parse error. Callers used to have to know
                      // that — kicad-sch-dump holds one for its whole run — but
                      // an ABI that silently misreads a file in a French locale
                      // is not one, so the guard belongs here. It counts, so a
                      // caller that already holds one loses nothing.
                      LOCALE_IO localeGuard;

                      const wxString path = wxString::FromUTF8( aPathUtf8 );

                      if( !wxFileName::FileExists( path ) )
                      {
                          setError( aSession,
                                    wxString::Format( wxT( "Schematic file '%s' does not exist." ),
                                                      path ) );
                          return KSCH_ERR_FILE_NOT_FOUND;
                      }

                      aSession->m_SimModelDraft.clear();
                      aSession->m_SimModelDraftUuid.clear();
                      aSession->m_SimModelBaseline.clear();
                      aSession->m_SimParserLog.clear();
                      aSession->m_SymbolLibraries.reset();
                      if( !aSession->m_Host.LoadFile( path ) )
                      {
                          setError( aSession, aSession->m_Host.GetLastError() );
                          return KSCH_ERR_LOAD_FAILED;
                      }

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_unload( ksch_session* aSession )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      aSession->m_SimModelDraft.clear();
                      aSession->m_SimModelDraftUuid.clear();
                      aSession->m_SimModelBaseline.clear();
                      aSession->m_SimParserLog.clear();
                      aSession->m_SymbolLibraries.reset();
                      aSession->m_Host.Unload();
                      return KSCH_OK;
                  } );
}


extern "C" int ksch_session_is_loaded( const ksch_session* aSession )
{
    return ( aSession && aSession->m_Host.IsLoaded() ) ? 1 : 0;
}


extern "C" ksch_status ksch_session_document_info( const ksch_session*  aSession,
                                                   ksch_document_info* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    ksch_session* session = const_cast<ksch_session*>( aSession );

    return guard( session,
                  [&]() -> ksch_status
                  {
                      const SCH_HOST& host = aSession->m_Host;

                      if( !host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      aOut->sheet_count =
                              static_cast<std::uint32_t>( host.GetSheetHierarchy().size() );
                      aOut->current_sheet =
                              static_cast<std::uint32_t>( host.GetCurrentSheetIndex() );
                      aOut->item_count = static_cast<std::uint64_t>( host.GetItemCount() );
                      aOut->modified = host.IsModified() ? 1u : 0u;
                      aOut->reserved = 0;

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_bbox( const ksch_session* aSession, int aIncludeAllVisible,
                                          ksch_bbox* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    ksch_session* session = const_cast<ksch_session*>( aSession );

    return guard( session,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      BOX2I box = aSession->m_Host.GetDocumentBBox( aIncludeAllVisible != 0 );

                      aOut->x = static_cast<double>( box.GetX() );
                      aOut->y = static_cast<double>( box.GetY() );
                      aOut->width = static_cast<double>( box.GetWidth() );
                      aOut->height = static_cast<double>( box.GetHeight() );

                      return KSCH_OK;
                  } );
}


/* ----------------------------------------------------------------- sheets */

extern "C" ksch_status ksch_session_sheet_count( const ksch_session* aSession, uint32_t* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    ksch_session* session = const_cast<ksch_session*>( aSession );

    return guard( session,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      *aOut = static_cast<std::uint32_t>(
                              aSession->m_Host.GetSheetHierarchy().size() );

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_sheet_info( const ksch_session* aSession, uint32_t aIndex,
                                                ksch_sheet_info* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    ksch_session* session = const_cast<ksch_session*>( aSession );

    return guard( session,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      const std::vector<SCH_HOST_SHEET_INFO>& sheets =
                              aSession->m_Host.GetSheetHierarchy();

                      if( aIndex >= sheets.size() )
                          return KSCH_ERR_OUT_OF_RANGE;

                      const SCH_HOST_SHEET_INFO& info = sheets[aIndex];

                      // The returned pointers must outlive the call, so the UTF-8
                      // conversions are parked on the session rather than on a temporary.
                      session->m_SheetName = info.m_Name.utf8_string();
                      session->m_SheetPath = info.m_Path.utf8_string();
                      session->m_SheetPage = info.m_PageNumber.utf8_string();

                      aOut->name = session->m_SheetName.c_str();
                      aOut->path = session->m_SheetPath.c_str();
                      aOut->page_number = session->m_SheetPage.c_str();
                      aOut->item_count = static_cast<std::uint64_t>( info.m_ItemCount );

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_set_sheet( ksch_session* aSession, uint32_t aIndex )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      if( !aSession->m_Host.SetCurrentSheetIndex( aIndex ) )
                          return KSCH_ERR_OUT_OF_RANGE;

                      return KSCH_OK;
                  } );
}


/* --------------------------------------------------------------- viewport */

extern "C" ksch_status ksch_session_set_viewport( ksch_session*        aSession,
                                                  const ksch_viewport* aViewport )
{
    if( !aSession || !aViewport )
        return KSCH_ERR_INVALID_ARG;

    if( aViewport->width_px == 0 || aViewport->height_px == 0 || !( aViewport->scale > 0.0 ) )
        return KSCH_ERR_INVALID_ARG;

    // A viewport wider than this would overflow the int the GAL stores it in, and
    // nothing legitimate asks for it.
    if( aViewport->width_px > 65535u || aViewport->height_px > 65535u )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      aSession->m_Host.SetViewport(
                              static_cast<int>( aViewport->width_px ),
                              static_cast<int>( aViewport->height_px ),
                              VECTOR2D( aViewport->center_x, aViewport->center_y ),
                              aViewport->scale );

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_get_viewport( const ksch_session* aSession,
                                                  ksch_viewport*     aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    ksch_session* session = const_cast<ksch_session*>( aSession );

    return guard( session,
                  [&]() -> ksch_status
                  {
                      const SCH_HOST& host = aSession->m_Host;
                      const VECTOR2I  size = host.GetViewportSize();
                      const VECTOR2D  centre = host.GetViewCenter();

                      aOut->width_px = static_cast<std::uint32_t>( size.x );
                      aOut->height_px = static_cast<std::uint32_t>( size.y );
                      aOut->center_x = centre.x;
                      aOut->center_y = centre.y;
                      aOut->scale = host.GetViewScale();

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_zoom_to_fit( ksch_session* aSession )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      aSession->m_Host.ZoomToFit();
                      return KSCH_OK;
                  } );
}


/* ----------------------------------------------------------------- render */

extern "C" ksch_status ksch_session_render( ksch_session* aSession, kgds_stream_view* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      *aOut = aSession->m_Host.Render();
                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_publish( const ksch_session* aSession, kgds_stream_view* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    ksch_session* session = const_cast<ksch_session*>( aSession );

    return guard( session,
                  [&]() -> ksch_status
                  {
                      *aOut = aSession->m_Host.PublishLastFrame();
                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_write_stream( ksch_session* aSession, const char* aPathUtf8 )
{
    if( !aSession || !aPathUtf8 )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      std::ofstream out( aPathUtf8, std::ios::binary );

                      if( !out.is_open() )
                      {
                          setError( aSession,
                                    wxString::Format( wxT( "Cannot open '%s' for writing." ),
                                                      wxString::FromUTF8( aPathUtf8 ) ) );
                          return KSCH_ERR_IO;
                      }

                      if( !aSession->m_Host.Gal().Stream().Serialize( out ) )
                      {
                          setError( aSession, wxT( "Failed to serialise the draw stream." ) );
                          return KSCH_ERR_IO;
                      }

                      out.flush();

                      if( !out )
                      {
                          setError( aSession, wxT( "Write error while serialising the stream." ) );
                          return KSCH_ERR_IO;
                      }

                      return KSCH_OK;
                  } );
}


/* -------------------------------------------------------- action registry */

/* ------------------------------------------------------------------ input */

extern "C" ksch_status ksch_session_dispatch_input( ksch_session*           aSession,
                                                    const ksch_input_event* aEvent,
                                                    uint32_t*               aOutFlags )
{
    if( !aSession || !aEvent )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      HOST_INPUT_EVENT event;

                      if( !toHostInput( *aEvent, event ) )
                      {
                          setError( aSession,
                                    wxString::Format( wxT( "Unknown input event type %d." ),
                                                      aEvent->type ) );
                          return KSCH_ERR_INVALID_ARG;
                      }

                      const bool handled = aSession->m_Host.DispatchInput( event );

                      if( aOutFlags )
                      {
                          uint32_t flags = 0;

                          if( handled )
                              flags |= KSCH_INPUT_HANDLED;

                          if( aSession->m_Host.TakeRedrawRequest() )
                              flags |= KSCH_INPUT_REDRAW;

                          *aOutFlags = flags;
                      }

                      // With no out-parameter the request is deliberately *left
                      // pending* rather than consumed. It is the only way a caller can
                      // learn that a tool asked for a repaint, so dropping it loses a
                      // frame the UI needed; carrying it to the next call that does ask
                      // attributes it to the wrong event, which costs one redundant
                      // re-record and nothing else. A lost repaint is worse than a late
                      // one.

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_reset_input( ksch_session* aSession )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      aSession->m_Host.ResetInputState();
                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_run_action( ksch_session* aSession, const char* aNameUtf8,
                                                uint32_t* aOutFlags )
{
    if( !aSession || !aNameUtf8 )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      const bool handled = aSession->m_Host.RunActionByName( aNameUtf8, true );

                      if( aOutFlags )
                      {
                          // Same rule as ksch_session_dispatch_input: with no
                          // out-parameter the redraw request stays pending rather than
                          // being consumed and lost.
                          *aOutFlags = ( handled ? KSCH_INPUT_HANDLED : 0u )
                                       | ( aSession->m_Host.TakeRedrawRequest()
                                                   ? KSCH_INPUT_REDRAW
                                                   : 0u );
                      }

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_editor_state( ksch_session*      aSession,
                                                  ksch_editor_state* aOut )
{
    if( !aSession || !aOut )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      SCH_HOST& host = aSession->m_Host;

                      // Parked on the session rather than on a temporary, which is the
                      // only reason these pointers are safe to return at all.
                      //
                      // `TOOLS_HOLDER::CurrentToolName()` answers with the selection
                      // tool's name when the stack is empty rather than with nothing,
                      // which is a sensible default for a status bar that always has
                      // a tool and a wrong answer for an ABI that promises "" for
                      // none — a UI would show a tool that is not even registered.
                      aSession->m_ToolName =
                              host.ToolStackIsEmpty() ? std::string() : host.CurrentToolName();
                      aSession->m_StatusText = host.GetToolMessage().utf8_string();

                      const VECTOR2D cursor = host.GetCursorPosition();

                      aOut->cursor_x = cursor.x;
                      aOut->cursor_y = cursor.y;
                      aOut->selection_count = static_cast<uint32_t>( host.GetSelectionCount() );
                      aOut->undo_count = static_cast<uint32_t>( host.GetUndoCommandCount() );
                      aOut->redo_count = static_cast<uint32_t>( host.GetRedoCommandCount() );

                      aOut->flags = 0;

                      if( host.ViewControls().PointerIsOverCanvas() )
                          aOut->flags |= KSCH_EDITOR_POINTER_OVER_CANVAS;

                      if( host.IsModified() )
                          aOut->flags |= KSCH_EDITOR_MODIFIED;

                      aOut->tool_name = aSession->m_ToolName.c_str();
                      aOut->status_text = aSession->m_StatusText.c_str();

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_undo( ksch_session* aSession, int* aOutUndone )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      const bool undone = aSession->m_Host.Undo();

                      if( aOutUndone )
                          *aOutUndone = undone ? 1 : 0;

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_redo( ksch_session* aSession, int* aOutRedone )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      const bool redone = aSession->m_Host.Redo();

                      if( aOutRedone )
                          *aOutRedone = redone ? 1 : 0;

                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_session_save( ksch_session* aSession )
{
    if( !aSession )
        return KSCH_ERR_INVALID_ARG;

    return guard( aSession,
                  [&]() -> ksch_status
                  {
                      if( !aSession->m_Host.IsLoaded() )
                          return KSCH_ERR_NO_DOCUMENT;

                      // The writer throws IO_ERROR on a path it cannot write; guard()
                      // catches it and records the message, which is what a UI shows.
                      return aSession->m_Host.Save() ? KSCH_OK : KSCH_ERR_IO;
                  } );
}


extern "C" uint32_t ksch_action_count( void )
{
    try
    {
        return static_cast<std::uint32_t>( actionTable().size() );
    }
    catch( ... )
    {
        return 0;
    }
}


extern "C" ksch_status ksch_action_at( uint32_t aIndex, ksch_action* aOut )
{
    if( !aOut )
        return KSCH_ERR_INVALID_ARG;

    return guard( nullptr,
                  [&]() -> ksch_status
                  {
                      const std::vector<ACTION_RECORD>& table = actionTable();

                      if( aIndex >= table.size() )
                          return KSCH_ERR_OUT_OF_RANGE;

                      *aOut = table[aIndex].m_Abi;
                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_action_hotkey_names( uint32_t aIndex, const char** aPrimary,
                                                  const char** aAlternate )
{
    if( !aPrimary || !aAlternate )
        return KSCH_ERR_INVALID_ARG;

    return guard( nullptr,
                  [&]() -> ksch_status
                  {
                      const std::vector<ACTION_RECORD>& table = actionTable();

                      if( aIndex >= table.size() )
                          return KSCH_ERR_OUT_OF_RANGE;

                      *aPrimary = table[aIndex].m_HotkeyName.c_str();
                      *aAlternate = table[aIndex].m_HotkeyAltName.c_str();
                      return KSCH_OK;
                  } );
}


extern "C" ksch_status ksch_action_find( const char* aNameUtf8, ksch_action* aOut )
{
    if( !aNameUtf8 || !aOut )
        return KSCH_ERR_INVALID_ARG;

    return guard( nullptr,
                  [&]() -> ksch_status
                  {
                      for( const ACTION_RECORD& record : actionTable() )
                      {
                          if( record.m_Name == aNameUtf8 )
                          {
                              *aOut = record.m_Abi;
                              return KSCH_OK;
                          }
                      }

                      return KSCH_ERR_OUT_OF_RANGE;
                  } );
}

extern "C" ksch_status ksch_session_set_search_data( ksch_session* session,
                                                       const ksch_search_data* data )
{
    if( !session || !data || !data->find || !data->replace )
        return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() )
            return KSCH_ERR_NO_DOCUMENT;
        SCH_SEARCH_DATA terms;
        terms.findString = wxString::FromUTF8( data->find );
        terms.replaceString = wxString::FromUTF8( data->replace );
        terms.matchCase = data->match_case != 0;
        terms.matchMode = data->whole_word ? EDA_SEARCH_MATCH_MODE::WHOLEWORD
                                          : EDA_SEARCH_MATCH_MODE::PLAIN;
        terms.searchCurrentSheetOnly = data->current_sheet_only != 0;
        terms.searchSelectedOnly = data->selected_only != 0;
        terms.replaceReferences = data->replace_references != 0;
        terms.searchAllFields = data->search_all_fields != 0;
        terms.searchAllPins = data->search_all_pins != 0;
        terms.searchAndReplace = data->replace_mode != 0;
        session->m_Host.SetSearchData( terms, data->active != 0 );
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_search_result( ksch_session* session,
                                                     ksch_search_result* result )
{
    if( !session || !result )
        return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() )
            return KSCH_ERR_NO_DOCUMENT;
        *result = {};
        auto* tool = session->m_Host.GetToolManager()->GetTool<SCH_FIND_REPLACE_TOOL>();
        result->wrapped = tool->Wrapped();
        result->replaced = tool->Replaced();
        if( tool->GetLastFoundItem() )
        {
            result->found = 1;
            const VECTOR2I center = tool->LastFoundCenter();
            result->center_x = center.x;
            result->center_y = center.y;
        }
        return KSCH_OK;
    } );
}

// Properties retain no model pointers across calls: selection and UUID are checked again on apply.
namespace
{
// Values are transported as UTF-8, with explicit editor metadata.  Property-manager
// descriptors contain model accessors only: this path never creates a wx control.
struct HOST_PROPERTY
{
    SCH_ITEM* target;
    PROPERTY_BASE* property;
    wxString name, value;
    uint32_t kind;
    wxPGChoices choices;
};

bool describeProperty( HOST_PROPERTY& result )
{
    wxAny value = result.target->Get( result.property );
    result.choices = result.property->GetChoices( result.target );
    if( !result.choices.GetCount() ) result.choices = result.property->Choices();
    if( result.choices.GetCount() )
    {
        result.kind = KSCH_PROPERTY_CHOICE;
        int number;
        wxString label;
        if( value.GetAs<int>( &number ) )
        {
            for( unsigned i = 0; i < result.choices.GetCount(); ++i )
                if( result.choices.GetValue( i ) == number )
                { result.value = result.choices.GetLabel( i ); return true; }
        }
        if( value.GetAs<wxString>( &label ) )
        { result.value = label; return true; }
        return false;
    }
    if( value.CheckType<wxString>() )
    { result.kind = result.property->Name() == "Text" && !dynamic_cast<SCH_FIELD*>( result.target )
          && !dynamic_cast<SCH_LABEL_BASE*>( result.target ) ? KSCH_PROPERTY_MULTILINE : KSCH_PROPERTY_TEXT;
      result.value = value.As<wxString>(); }
    else if( value.CheckType<bool>() )
    { result.kind = KSCH_PROPERTY_BOOL; result.value = value.As<bool>() ? "true" : "false"; }
    else if( value.CheckType<int>() || value.CheckType<unsigned>() || value.CheckType<double>() )
    {
        double number = 0;
        value.GetAs<double>( &number );
        result.kind = value.CheckType<double>() ? KSCH_PROPERTY_NUMBER : KSCH_PROPERTY_INTEGER;
        if( result.property->Display() == PT_SIZE || result.property->Display() == PT_COORD )
        { result.kind = KSCH_PROPERTY_DISTANCE; number /= schIUScale.IU_PER_MM; }
        else if( result.property->Display() == PT_DEGREE ) result.kind = KSCH_PROPERTY_ANGLE;
        else if( result.property->Display() == PT_DECIDEGREE )
        { result.kind = KSCH_PROPERTY_ANGLE; number /= 10.; }
        result.value = wxString::Format( "%.12g", number );
    }
    else if( value.CheckType<EDA_ANGLE>() )
    { result.kind = KSCH_PROPERTY_ANGLE; result.value = wxString::Format( "%.12g", value.As<EDA_ANGLE>().AsDegrees() ); }
    else if( value.CheckType<KIGFX::COLOR4D>() )
    { result.kind = KSCH_PROPERTY_COLOR; result.value = value.As<KIGFX::COLOR4D>().ToCSSString(); }
    else return false;
    return true;
}

std::vector<HOST_PROPERTY> modelProperties( SCH_ITEM* item )
{
    std::vector<HOST_PROPERTY> result;
    auto& manager = PROPERTY_MANAGER::Instance();
    auto append = [&]( SCH_ITEM* target, const wxString& prefix )
    {
        for( auto* property : manager.GetProperties( TYPE_HASH( *target ) ) )
        {
            if( property->IsHiddenFromPropertiesManager() || property->IsHiddenFromDesignEditors()
                || !manager.IsAvailableFor( TYPE_HASH( *target ), property, target )
                || !manager.IsWriteableFor( TYPE_HASH( *target ), property, target ) ) continue;
            // These fields need sheet-instance/variant semantics, handled separately below.
            if( dynamic_cast<SCH_SYMBOL*>( target ) && property->Group() == "Fields" ) continue;
            if( dynamic_cast<SCH_SYMBOL*>( target )
                && ( property->Name() == "Reference" || property->Name() == "Value"
                     || property->Name() == "Library Link" || property->Name() == "Library Description"
                     || property->Name() == "Keywords" ) ) continue;
            if( dynamic_cast<EDA_TEXT*>( target ) && property->Name() == "Text"
                && ( target == item || dynamic_cast<SCH_SYMBOL*>( item ) ) ) continue;
            if( auto* field = dynamic_cast<SCH_FIELD*>( target ); field
                && property->Name() == "Text" && field->GetId() == FIELD_T::SHEET_FILENAME ) continue;
            HOST_PROPERTY descriptor{ target, property, prefix + property->Name(), {}, 0, {} };
            if( describeProperty( descriptor ) ) result.push_back( std::move( descriptor ) );
        }
    };
    append( item, {} );
    if( auto* symbol = dynamic_cast<SCH_SYMBOL*>( item ) )
        for( auto& field : symbol->GetFields() ) append( &field, field.GetName() + ": " );
    if( auto* sheet = dynamic_cast<SCH_SHEET*>( item ) )
        for( auto& field : sheet->GetFields() ) append( &field, field.GetName() + ": " );
    if( auto* label = dynamic_cast<SCH_LABEL_BASE*>( item ) )
        for( auto& field : label->GetFields() ) append( &field, field.GetName() + ": " );
    if( auto* table = dynamic_cast<SCH_TABLE*>( item ) )
        for( auto* cell : table->GetCells() )
            append( cell, wxString::Format( "Cell %d,%d: ", cell->GetRow() + 1, cell->GetColumn() + 1 ) );
    return result;
}

bool parseProperty( const HOST_PROPERTY& descriptor, const wxString& text, wxAny& value )
{
    const auto old = descriptor.target->Get( descriptor.property );
    if( descriptor.kind == KSCH_PROPERTY_CHOICE )
    {
        for( unsigned i = 0; i < descriptor.choices.GetCount(); ++i )
            if( text == descriptor.choices.GetLabel( i ) )
            {
                if( old.CheckType<wxString>() ) value = text;
                else value = descriptor.choices.GetValue( i );
                return true;
            }
        return false;
    }
    if( descriptor.kind == KSCH_PROPERTY_TEXT || descriptor.kind == KSCH_PROPERTY_MULTILINE ) { value = text; return true; }
    if( descriptor.kind == KSCH_PROPERTY_BOOL )
    { if( text != "true" && text != "false" ) return false; value = text == "true"; return true; }
    if( descriptor.kind == KSCH_PROPERTY_COLOR )
    {
        KIGFX::COLOR4D color;
        if( !color.SetFromWxString( text ) ) return false;
        value = color; return true;
    }
    double number;
    if( !text.ToDouble( &number ) || !std::isfinite( number ) ) return false;
    if( descriptor.kind == KSCH_PROPERTY_DISTANCE ) number *= schIUScale.IU_PER_MM;
    if( descriptor.property->Display() == PT_DECIDEGREE ) number *= 10.;
    if( old.CheckType<EDA_ANGLE>() ) value = EDA_ANGLE( number, DEGREES_T );
    else if( old.CheckType<int>() )
    {
        if( number < std::numeric_limits<int>::min() || number > std::numeric_limits<int>::max() ) return false;
        if( descriptor.kind == KSCH_PROPERTY_INTEGER && number != std::round( number ) ) return false;
        value = static_cast<int>( std::round( number ) );
    }
    else if( old.CheckType<unsigned>() )
    {
        if( number < 0 || number > std::numeric_limits<unsigned>::max() || number != std::round( number ) ) return false;
        value = static_cast<unsigned>( number );
    }
    else value = number;
    return true;
}

SCH_ITEM* propertyItem( ksch_session* session )
{
    auto& selection = session->m_Host.GetSelectionTool()->GetSelection();
    if( selection.Size() != 1 )
        return nullptr;
    auto* item = dynamic_cast<SCH_ITEM*>( selection.Front() );
    if( item && item->Type() == SCH_FIELD_T )
        item = dynamic_cast<SCH_ITEM*>( item->GetParent() );
    return item;
}

std::vector<EDA_TEXT*> propertyTexts( SCH_ITEM* item )
{
    std::vector<EDA_TEXT*> result;
    if( auto* symbol = dynamic_cast<SCH_SYMBOL*>( item ) )
    {
        for( auto& field : symbol->GetFields() )
            if( !field.IsGeneratedField() ) result.push_back( &field );
    }
    else if( auto* text = dynamic_cast<EDA_TEXT*>( item ) )
        result.push_back( text );
    return result;
}
}

extern "C" ksch_status ksch_session_item_properties( ksch_session* session,
        ksch_property_visitor visitor, void* context )
{
    if( !session || !visitor ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto* item = propertyItem( session );
        if( !item ) { setError( session, wxS( "Select one schematic item" ) ); return KSCH_ERR_INVALID_ARG; }
        const auto texts = propertyTexts( item );
        const auto properties = modelProperties( item );
        if( texts.empty() && properties.empty() ) return KSCH_ERR_INVALID_ARG;
        const auto id = item->m_Uuid.AsString().utf8_string();
        for( auto* text : texts )
        {
            auto* field = dynamic_cast<SCH_FIELD*>( text );
            const auto name = field ? field->GetName().utf8_string() : std::string( "Text" );
            auto value = field ? field->GetText( &session->m_Host.GetCurrentSheet(),
                                                session->m_Host.GetSchematic()->GetCurrentVariant() )
                               : text->GetText();
            if( field && field->GetId() == FIELD_T::REFERENCE )
                value = static_cast<SCH_SYMBOL*>( item )->GetRef( &session->m_Host.GetCurrentSheet() );
            visitor( context, id.c_str(), name.c_str(), value.utf8_string().c_str(), field || dynamic_cast<SCH_LABEL_BASE*>( item ) ? KSCH_PROPERTY_TEXT : KSCH_PROPERTY_MULTILINE, nullptr, 0 );
        }
        for( const auto& property : properties )
        {
            std::vector<std::string> strings;
            for( unsigned i = 0; i < property.choices.GetCount(); ++i )
                strings.push_back( property.choices.GetLabel( i ).utf8_string() );
            std::vector<const char*> choices;
            for( const auto& string : strings ) choices.push_back( string.c_str() );
            visitor( context, id.c_str(), property.name.utf8_string().c_str(),
                     property.value.utf8_string().c_str(), property.kind, choices.data(), choices.size() );
        }
        if( auto* sheet = dynamic_cast<SCH_SHEET*>( item ) )
        {
            auto path = session->m_Host.GetCurrentSheet();
            path.push_back( sheet );
            visitor( context, id.c_str(), "Page number", path.GetPageNumber().utf8_string().c_str(),
                     KSCH_PROPERTY_TEXT, nullptr, 0 );
        }
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_apply_properties( ksch_session* session,
        const char* item_id, const char* const* values, uint32_t count )
{
    if( !session || !item_id || !values ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto* item = propertyItem( session );
        if( !item || item->m_Uuid.AsString() != wxString::FromUTF8( item_id ) )
        { setError( session, wxS( "Selection changed; reopen Properties" ) ); return KSCH_ERR_INVALID_ARG; }
        const auto texts = propertyTexts( item );
        const auto properties = modelProperties( item );
        const bool isSheet = dynamic_cast<SCH_SHEET*>( item ) != nullptr;
        if( texts.size() + properties.size() + ( isSheet ? 1 : 0 ) != count ) return KSCH_ERR_INVALID_ARG;
        SCH_SHEET_PATH pagePath = session->m_Host.GetCurrentSheet();
        wxString pageNumber;
        if( isSheet )
        {
            pagePath.push_back( static_cast<SCH_SHEET*>( item ) );
            if( !values[count - 1] ) return KSCH_ERR_INVALID_ARG;
            pageNumber = wxString::FromUTF8( values[count - 1] );
            if( pageNumber.empty() || std::any_of( pageNumber.begin(), pageNumber.end(), []( wxUniChar c ) { return !wxIsalnum( c ); } ) )
            { setError( session, "Page number must contain letters and numbers only" ); return KSCH_ERR_INVALID_ARG; }
        }
        std::vector<wxAny> parsed( properties.size() );
        UNITS_PROVIDER units( schIUScale, EDA_UNITS::MM );
        for( size_t i = 0; i < properties.size(); ++i )
        {
            if( !values[texts.size() + i] ) return KSCH_ERR_INVALID_ARG;
            if( properties[i].value == wxString::FromUTF8( values[texts.size() + i] ) ) continue;
            if( !parseProperty( properties[i], wxString::FromUTF8( values[texts.size() + i] ), parsed[i] ) )
            { setError( session, "Invalid value for " + properties[i].name ); return KSCH_ERR_INVALID_ARG; }
            // Dialog bounds are stricter than several legacy property-grid setters.
            double number = 0;
            if( parsed[i].GetAs<double>( &number ) )
            {
                const auto& name = properties[i].property->Name();
                const bool positive = name == "Text Size" || name == "Column Width" || name == "Row Height"
                    || ( dynamic_cast<SCH_BITMAP*>( properties[i].target ) && name == "Scale" );
                if( ( positive && number <= 0 )
                    || ( properties[i].property->Display() == PT_SIZE && number < 0 ) )
                { setError( session, "Invalid size for " + properties[i].name ); return KSCH_ERR_INVALID_ARG; }
            }
            if( auto* field = dynamic_cast<SCH_FIELD*>( properties[i].target );
                field && properties[i].property->Name() == "Text" )
            {
                auto message = GetFieldValidationErrorMessage( field->GetId(), parsed[i].As<wxString>() );
                if( !message.empty() ) { setError( session, message ); return KSCH_ERR_INVALID_ARG; }
            }
            auto error = properties[i].property->Validate( wxAny( parsed[i] ), properties[i].target );
            if( error ) { setError( session, (*error)->Format( &units ) ); return KSCH_ERR_INVALID_ARG; }
        }
        bool changed = isSheet && pagePath.GetPageNumber() != pageNumber;
        for( size_t i = 0; i < texts.size(); ++i )
        {
            if( !values[i] ) return KSCH_ERR_INVALID_ARG;
            auto* field = dynamic_cast<SCH_FIELD*>( texts[i] );
            const auto value = wxString::FromUTF8( values[i] );
            wxString error;
            if( field )
                error = GetFieldValidationErrorMessage( field->GetId(), value );
            else if( dynamic_cast<SCH_LABEL_BASE*>( item ) )
            {
                NETNAME_VALIDATOR validator;
                error = value.empty() ? wxS( "Label cannot be empty" ) : validator.IsValid( value );
            }
            if( !error.empty() ) { setError( session, error ); return KSCH_ERR_INVALID_ARG; }
            const auto variant = session->m_Host.GetSchematic()->GetCurrentVariant();
            auto oldValue = field ? field->GetText( &session->m_Host.GetCurrentSheet(), variant )
                                  : texts[i]->GetText();
            if( field && field->GetId() == FIELD_T::REFERENCE )
            {
                oldValue = static_cast<SCH_SYMBOL*>( item )->GetRef( &session->m_Host.GetCurrentSheet() );
                if( !variant.empty() && oldValue != value )
                {
                    setError( session, wxS( "References can only be edited in the base variant" ) );
                    return KSCH_ERR_INVALID_ARG;
                }
            }
            changed |= oldValue != value;
        }
        for( size_t i = 0; i < properties.size(); ++i )
            changed |= properties[i].value != wxString::FromUTF8( values[texts.size() + i] );
        if( !changed ) return KSCH_OK;
        SCH_SHEET_PATH renamedPath = session->m_Host.GetCurrentSheet();
        wxString oldNetPrefix;
        if( auto* sheet = dynamic_cast<SCH_SHEET*>( item ) )
        {
            renamedPath.push_back( sheet );
            oldNetPrefix = renamedPath.PathHumanReadable( true, false, true );
        }
        SCH_COMMIT commit( session->m_Host.GetToolManager() );
        const bool preview = item->HasFlag( IS_NEW ) && item->HasFlag( IS_MOVING );
        if( !preview ) commit.Modify( item, session->m_Host.GetScreen(), RECURSE_MODE::NO_RECURSE );
        try
        {
        if( isSheet ) pagePath.SetPageNumber( pageNumber );
        for( size_t i = 0; i < texts.size(); ++i )
        {
            const auto value = wxString::FromUTF8( values[i] );
            auto* field = dynamic_cast<SCH_FIELD*>( texts[i] );
            if( field && field->GetId() == FIELD_T::REFERENCE )
                static_cast<SCH_SYMBOL*>( item )->SetRef( &session->m_Host.GetCurrentSheet(), value );
            else if( field )
                field->SetText( value, &session->m_Host.GetCurrentSheet(),
                                session->m_Host.GetSchematic()->GetCurrentVariant() );
            else
                texts[i]->SetText( value );
        }
        for( size_t i = 0; i < properties.size(); ++i )
            if( properties[i].value != wxString::FromUTF8( values[texts.size() + i] ) )
                properties[i].target->Set( properties[i].property, parsed[i] );
        // Match the symbol dialog: changing one unit must keep its sibling units' fields in sync.
        if( auto* symbol = dynamic_cast<SCH_SYMBOL*>( item ); symbol && !preview )
            symbol->SyncOtherUnits( session->m_Host.GetCurrentSheet(), commit, nullptr,
                                    session->m_Host.GetSchematic()->GetCurrentVariant() );
        if( !oldNetPrefix.empty() && !preview )
        {
            const wxString newNetPrefix = renamedPath.PathHumanReadable( true, false, true );
            if( oldNetPrefix != newNetPrefix )
                session->m_Host.GetSchematic()->Project().GetProjectFile().NetSettings()->RenameNetPathPrefix( oldNetPrefix, newNetPrefix );
            session->m_Host.GetSchematic()->RefreshHierarchy();
        }
        if( preview ) session->m_Host.GetToolManager()->RunAction( ACTIONS::refreshPreview );
        else commit.Push( wxS( "Edit Properties" ) );
        }
        catch( ... )
        {
            commit.Revert();
            throw;
        }
        return KSCH_OK;
    } );
}


extern "C" ksch_status ksch_session_take_pending_properties( ksch_session* session, int* pending )
{
    if( !session || !pending ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    { *pending = session->m_Host.TakePendingItemProperties(); return KSCH_OK; } );
}

extern "C" ksch_status ksch_session_property_capabilities( ksch_session* session, uint32_t* flags )
{
    if( !session || !flags ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        *flags = 0;
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto* item = propertyItem( session );
        if( dynamic_cast<SCH_SYMBOL*>( item ) || dynamic_cast<SCH_LABEL_BASE*>( item ) ) *flags = 1;
        if( dynamic_cast<SCH_SHEET*>( item ) ) *flags = 3;
        if( item && item->HasFlag( IS_NEW ) && item->HasFlag( IS_MOVING ) ) *flags |= 4;
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_edit_custom_field( ksch_session* session,
        const char* item_id, const char* name_utf8, const char* value_utf8 )
{
    if( !session || !item_id || !name_utf8 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        auto* item = propertyItem( session );
        if( !item || item->m_Uuid.AsString() != wxString::FromUTF8( item_id ) )
        { setError( session, "Selection changed; reopen Properties" ); return KSCH_ERR_INVALID_ARG; }
        std::vector<SCH_FIELD>* fields = nullptr;
        auto* symbol = dynamic_cast<SCH_SYMBOL*>( item );
        auto* sheet = dynamic_cast<SCH_SHEET*>( item );
        auto* label = dynamic_cast<SCH_LABEL_BASE*>( item );
        if( symbol ) fields = &symbol->GetFields();
        else if( sheet ) fields = &sheet->GetFields();
        else if( label ) fields = &label->GetFields();
        if( !fields ) { setError( session, "This item has no custom fields" ); return KSCH_ERR_INVALID_ARG; }
        if( !session->m_Host.GetSchematic()->GetCurrentVariant().empty() )
        { setError( session, "Add or remove fields in the base variant" ); return KSCH_ERR_INVALID_ARG; }
        wxString name = wxString::FromUTF8( name_utf8 );
        if( name.Trim().Trim( false ).empty() || name.find_first_of( "\r\n\t" ) != wxString::npos )
        { setError( session, "Enter a non-empty field name without line breaks" ); return KSCH_ERR_INVALID_ARG; }
        auto found = std::find_if( fields->begin(), fields->end(), [&]( const SCH_FIELD& field )
        { return field.GetName().CmpNoCase( name ) == 0; } );
        if( value_utf8 && found != fields->end() )
        { setError( session, "A field with this name already exists" ); return KSCH_ERR_INVALID_ARG; }
        if( !value_utf8 && ( found == fields->end() || found->IsMandatory() || found->IsGeneratedField() ) )
        { setError( session, "Only existing custom fields can be removed" ); return KSCH_ERR_INVALID_ARG; }
        SCH_COMMIT commit( session->m_Host.GetToolManager() );
        const bool preview = item->HasFlag( IS_NEW ) && item->HasFlag( IS_MOVING );
        if( !preview ) commit.Modify( item, session->m_Host.GetScreen(), RECURSE_MODE::NO_RECURSE );
        if( value_utf8 )
        {
            SCH_FIELD field( item, sheet ? FIELD_T::SHEET_USER : FIELD_T::USER, name );
            field.SetText( wxString::FromUTF8( value_utf8 ) );
            field.SetPosition( item->GetPosition() );
            field.SetVisible( false );
            field.SetOrdinal( fields->size(), sheet ? FIELD_T::SHEET_USER : FIELD_T::USER );
            if( symbol ) symbol->AddField( field );
            else if( sheet ) sheet->AddField( field );
            else label->AddField( field );
        }
        else if( symbol ) symbol->RemoveField( found->GetName() );
        else fields->erase( found );
        if( symbol && !preview ) symbol->SyncOtherUnits( session->m_Host.GetCurrentSheet(), commit, nullptr );
        if( preview ) session->m_Host.GetToolManager()->RunAction( ACTIONS::refreshPreview );
        else commit.Push( value_utf8 ? "Add Custom Field" : "Remove Custom Field" );
        return KSCH_OK;
    } );
}

#include "sch_host_sheet_properties.inc"
#include "sch_host_graphics_import.inc"
#include "sch_host_image_properties.inc"
#include "sch_host_sheet_pin_sync.inc"

struct ksch_erc_result
{
    struct Violation
    {
        std::string message;
        uint32_t severity, sheet;
        double x, y;
        std::string markerId;
    };
    std::vector<Violation> violations;
};

extern "C" ksch_status ksch_session_run_erc( ksch_session* session, ksch_erc_result** result )
{
    if( !session || !result )
        return KSCH_ERR_INVALID_ARG;
    *result = nullptr;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() )
            return KSCH_ERR_NO_DOCUMENT;
        symbolLibraries( session );
        session->m_Host.RunERC( session->m_SymbolLibraries.get() );
        auto snapshot = std::make_unique<ksch_erc_result>();
        auto sheets = session->m_Host.GetSchematic()->BuildSheetListSortedByPageNumbers();
        for( size_t i = 0; i < sheets.size(); ++i )
        {
            for( SCH_ITEM* item : sheets[i].LastScreen()->Items().OfType( SCH_MARKER_T ) )
            {
                auto* marker = static_cast<SCH_MARKER*>( item );
                auto rc = std::static_pointer_cast<ERC_ITEM>( marker->GetRCItem() );
                if( rc->IsSheetSpecific() && rc->GetSpecificSheetPath() != sheets[i] )
                    continue;
                auto pos = marker->GetPosition();
                snapshot->violations.push_back( { rc->GetErrorMessage( true ).utf8_string(),
                    static_cast<uint32_t>( marker->GetSeverity() ), static_cast<uint32_t>( i ),
                    static_cast<double>( pos.x ), static_cast<double>( pos.y ), marker->m_Uuid.AsString().utf8_string() } );
            }
        }
        *result = snapshot.release();
        return KSCH_OK;
    } );
}

extern "C" uint32_t ksch_erc_result_count( const ksch_erc_result* result )
{
    return result ? static_cast<uint32_t>( result->violations.size() ) : 0;
}

extern "C" ksch_status ksch_erc_result_get( const ksch_erc_result* result, uint32_t index,
                                           ksch_erc_violation* violation )
{
    if( !result || !violation || index >= result->violations.size() )
        return KSCH_ERR_INVALID_ARG;
    const auto& v = result->violations[index];
    *violation = { v.message.c_str(), v.severity, v.sheet, v.x, v.y, v.markerId.c_str() };
    return KSCH_OK;
}

extern "C" ksch_status ksch_session_exclude_erc( ksch_session* session, const char* id, uint32_t excluded )
{
    if( !session || !id || !*id || excluded > 1 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        auto& host = session->m_Host;
        if( !host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        for( const auto& sheet : host.GetSchematic()->Hierarchy() )
            for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_MARKER_T ) )
                if( item->m_Uuid.AsString() == wxString::FromUTF8(id) )
                {
                    auto* marker = static_cast<SCH_MARKER*>(item);
                    const bool before = marker->IsExcluded();
                    const wxString comment = marker->GetComment();
                    marker->SetExcluded( excluded != 0 );
                    auto& schematic = *host.GetSchematic();
                    schematic.RecordERCExclusions();
                    schematic.ErcSettings().SaveToFile();
                    auto& project = schematic.Project();
                    if( !project.GetProjectFile().SaveToFile( project.GetProjectDirectory(), true ) )
                    {
                        marker->SetExcluded(before,comment);
                        schematic.RecordERCExclusions();
                        schematic.ErcSettings().SaveToFile();
                        session->m_Error = "Could not save ERC exclusions";
                        return KSCH_ERR_IO;
                    }
                    host.View().Update(item);
                    host.RefreshCanvas();
                    return KSCH_OK;
                }
        session->m_Error = "ERC result changed; run ERC again before changing exclusions";
        return KSCH_ERR_INVALID_ARG;
    } );
}

extern "C" void ksch_erc_result_destroy( ksch_erc_result* result )
{
    delete result;
}

extern "C" ksch_status ksch_session_list_symbols( ksch_session* session, const char** ids )
{
    if( !session || !ids ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        std::set<wxString> names;
        for( const SCH_SHEET_PATH& sheet : session->m_Host.GetSchematic()->Hierarchy() )
            for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
            {
                auto* symbol = static_cast<SCH_SYMBOL*>( item );
                if( symbol->GetLibSymbolRef() ) names.insert( symbol->GetLibId().Format() );
            }
        session->m_SymbolIds.clear();
        for( const auto& name : names ) session->m_SymbolIds += name.utf8_string() + "\n";
        *ids = session->m_SymbolIds.c_str();
        return KSCH_OK;
    } );
}

static LIB_SYMBOL* chooserSymbol( ksch_session* session, const LIB_ID& libId )
{
    auto& host = session->m_Host;
    LIB_SYMBOL* librarySymbol = nullptr;
    for( const SCH_SHEET_PATH& sheet : host.GetSchematic()->Hierarchy() )
        for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
        {
        auto* symbol = static_cast<SCH_SYMBOL*>( item );
        if( symbol->GetLibId() == libId && symbol->GetLibSymbolRef() )
            librarySymbol = symbol->GetLibSymbolRef().get();
        }
    if( !librarySymbol )
    {
        auto* adapter = symbolLibraries( session );
        adapter->LoadOne( libId.GetLibNickname() );
        librarySymbol = adapter->LoadSymbol( libId );
    }
    return librarySymbol;
}

extern "C" ksch_status ksch_session_place_symbol_variant( ksch_session* session, const char* id, uint32_t unit, uint32_t body )
{
    if( !session || !id || !*id ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        auto& host = session->m_Host;
        if( !host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        LIB_ID libId;
        if( libId.Parse( wxString::FromUTF8( id ) ) >= 0 ) return KSCH_ERR_INVALID_ARG;
        LIB_SYMBOL* librarySymbol = chooserSymbol( session, libId );
        if( !librarySymbol )
        {
            session->m_Error = "Symbol was not found in the schematic or configured libraries";
            return KSCH_ERR_INVALID_ARG;
        }
        if( unit < 1 || unit > static_cast<uint32_t>( librarySymbol->GetUnitCount() )
            || body < 1 || body > static_cast<uint32_t>( librarySymbol->GetBodyStyleCount() ) )
            return KSCH_ERR_INVALID_ARG;
        auto* symbol = new SCH_SYMBOL( *librarySymbol, libId, &host.GetCurrentSheet(), unit, body,
                                      VECTOR2I( 0, 0 ), host.GetSchematic() );
        host.GetToolManager()->RunAction( SCH_ACTIONS::placeSymbol,
                                         SCH_ACTIONS::PLACE_SYMBOL_PARAMS{ symbol, true } );
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_place_symbol( ksch_session* session, const char* id )
{
    return ksch_session_place_symbol_variant( session, id, 1, 1 );
}

extern "C" ksch_status ksch_session_preview_symbol( ksch_session* session, const char* id,
        uint32_t unit, uint32_t body, uint32_t* units, uint32_t* bodies, kgds_stream_view* out )
{
    if( !session || !id || !units || !bodies || !out ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        auto& host = session->m_Host;
        if( !host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        LIB_ID libId;
        if( libId.Parse( wxString::FromUTF8( id ) ) >= 0 ) return KSCH_ERR_INVALID_ARG;
        LIB_SYMBOL* librarySymbol = chooserSymbol( session, libId );
        if( !librarySymbol ) return KSCH_ERR_INVALID_ARG;
        *units = librarySymbol->GetUnitCount();
        *bodies = librarySymbol->GetBodyStyleCount();
        if( unit < 1 || unit > *units || body < 1 || body > *bodies ) return KSCH_ERR_INVALID_ARG;
        SCH_SYMBOL symbol( *librarySymbol, libId, &host.GetCurrentSheet(), unit, body,
                           VECTOR2I( 0, 0 ), host.GetSchematic() );
        session->m_SymbolPreview = std::make_unique<KIGFX::RECORDING_GAL>( host.GetGalDisplayOptions() );
        auto& gal = *session->m_SymbolPreview;
        KIGFX::SCH_PAINTER painter( &gal );
        *painter.GetSettings() = host.RenderSettings();
        painter.SetSchematic( host.GetSchematic() );
        // Record into a retained group so the shared renderer can fit its bounds.
        int group = gal.BeginGroup();
        const std::vector<int> layers = symbol.ViewGetLayers();
        for( auto layer = layers.rbegin(); layer != layers.rend(); ++layer )
            if( *layer != LAYER_SELECTION_SHADOWS ) painter.Draw( &symbol, *layer );
        gal.EndGroup();
        gal.BeginDrawing();
        gal.DrawGroup( group );
        gal.EndDrawing();
        *out = gal.Publish();
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_symbol_libraries( ksch_session* session, const char** names )
{
    if( !session || !names ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        symbolLibraries( session );
        session->m_SymbolIds.clear();
        std::set<wxString> nicknames;
        for( const auto* row : session->m_SymbolLibraries->Rows( LIBRARY_TABLE_TYPE::SYMBOL ) )
            if( !row->Disabled() ) nicknames.insert( row->Nickname() );
        for( const auto& name : nicknames )
            session->m_SymbolIds += name.utf8_string() + "\n";
        *names = session->m_SymbolIds.c_str();
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_browse_symbols( ksch_session* session, const char* library,
                                                    uint32_t power_only, const char** ids )
{
    if( !session || !library || !ids || power_only > 1 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        std::set<wxString> names;
        if( !*library )
        {
            for( const SCH_SHEET_PATH& sheet : session->m_Host.GetSchematic()->Hierarchy() )
                for( SCH_ITEM* item : sheet.LastScreen()->Items().OfType( SCH_SYMBOL_T ) )
                {
                    auto* symbol = static_cast<SCH_SYMBOL*>( item );
                    if( symbol->GetLibSymbolRef() && ( !power_only || symbol->GetLibSymbolRef()->IsPower() ) )
                        names.insert( symbol->GetLibId().Format() );
                }
        }
        else
        {
            auto* adapter = symbolLibraries( session );
            const wxString nickname = wxString::FromUTF8( library );
            const auto status = adapter->LoadOne( nickname );
            if( !status || status->load_status != LOAD_STATUS::LOADED )
            {
                session->m_Error = status && status->error ? status->error->message.utf8_string()
                                                         : "Could not load the selected symbol library";
                return KSCH_ERR_INVALID_ARG;
            }
            const auto type = power_only ? SYMBOL_LIBRARY_ADAPTER::SYMBOL_TYPE::POWER_ONLY
                                         : SYMBOL_LIBRARY_ADAPTER::SYMBOL_TYPE::ALL_SYMBOLS;
            for( const auto& name : adapter->GetSymbolNames( nickname, type ) )
                names.insert( LIB_ID( nickname, name ).Format() );
        }
        session->m_SymbolIds.clear();
        for( const auto& name : names ) session->m_SymbolIds += name.utf8_string() + "\n";
        *ids = session->m_SymbolIds.c_str();
        return KSCH_OK;
    } );
}

#include "sch_host_setup.inc"
#include "sch_host_preferences.inc"
#include "sch_host_document.inc"

extern "C" ksch_status ksch_session_library_table( ksch_session* session, uint32_t scope,
                                                   ksch_library_visitor visitor, void* context )
{
    if( !session || !visitor || scope > 1 ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        symbolLibraries( session );
        auto table = session->m_SymbolLibraries->Table( LIBRARY_TABLE_TYPE::SYMBOL,
            scope ? LIBRARY_TABLE_SCOPE::GLOBAL : LIBRARY_TABLE_SCOPE::PROJECT );
        if( !table ) return KSCH_OK;
        for( const auto& row : (*table)->Rows() )
        {
            const auto name = row.Nickname().utf8_string(), kind = row.Type().utf8_string(),
                       uri = row.URI().utf8_string(), options = row.Options().utf8_string(),
                       description = row.Description().utf8_string();
            ksch_library_row value{ name.c_str(), kind.c_str(), uri.c_str(), options.c_str(),
                                    description.c_str(), !row.Disabled(), !row.Hidden() };
            visitor( context, &value );
        }
        return KSCH_OK;
    } );
}

extern "C" ksch_status ksch_session_save_library_table( ksch_session* session, uint32_t scope,
                                                        const ksch_library_row* rows, uint32_t count )
{
    if( !session || scope > 1 || ( count && !rows ) ) return KSCH_ERR_INVALID_ARG;
    return guard( session, [&]() -> ksch_status
    {
        if( !session->m_Host.IsLoaded() ) return KSCH_ERR_NO_DOCUMENT;
        symbolLibraries( session );
        const auto targetScope = scope ? LIBRARY_TABLE_SCOPE::GLOBAL : LIBRARY_TABLE_SCOPE::PROJECT;
        auto current = session->m_SymbolLibraries->Table( LIBRARY_TABLE_TYPE::SYMBOL, targetScope );
        wxString path = current ? (*current)->Path() :
            scope ? LIBRARY_MANAGER::DefaultGlobalTablePath( LIBRARY_TABLE_TYPE::SYMBOL ) :
            wxFileName( session->m_Host.GetSchematic()->Project().GetProjectPath().empty()
                ? wxFileName( session->m_Host.GetSchematic()->RootScreen()->GetFileName() ).GetPath()
                : session->m_Host.GetSchematic()->Project().GetProjectPath(), "sym-lib-table" ).GetFullPath();
        LIBRARY_TABLE replacement( true, "(sym_lib_table)", targetScope );
        replacement.SetPath( path );
        replacement.SetType( LIBRARY_TABLE_TYPE::SYMBOL );
        replacement.SetOk();
        std::set<wxString> names;
        for( uint32_t i = 0; i < count; ++i )
        {
            const auto& input = rows[i];
            if( !input.name || !input.kind || !input.uri || !input.options || !input.description
                || !*input.name || !*input.uri || !*input.kind || input.enabled > 1 || input.visible > 1 )
            {
                session->m_Error = "Every library requires a name, type, and URI";
                return KSCH_ERR_INVALID_ARG;
            }
            const wxString name = wxString::FromUTF8( input.name );
            if( LIB_ID::FindIllegalLibraryNameChar( UTF8( name ) ) || !names.insert( name ).second )
            {
                session->m_Error = "Library names must be unique and cannot contain a colon or newline";
                return KSCH_ERR_INVALID_ARG;
            }
            auto& row = replacement.InsertRow();
            row.SetNickname( name ); row.SetType( wxString::FromUTF8( input.kind ) );
            row.SetURI( wxString::FromUTF8( input.uri ) );
            row.SetOptions( wxString::FromUTF8( input.options ) );
            row.SetDescription( wxString::FromUTF8( input.description ) );
            row.SetDisabled( !input.enabled ); row.SetHidden( !input.visible ); row.SetScope( targetScope );
        }
        auto result = replacement.Save();
        if( !result )
        {
            session->m_Error = result.error().message.utf8_string();
            return KSCH_ERR_INVALID_ARG;
        }
        session->m_SymbolLibraries.reset();
        return KSCH_OK;
    } );
}

#include "sch_host_library_symbols.inc"
#include "sch_host_library_config.inc"
#include "sch_host_remote_config.inc"
#include "sch_host_library_pin_maps.inc"
#include "sch_host_simulation_models.inc"
#include "sch_host_simulation_results.inc"
#include "sch_host_simulation.inc"
