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

#include <exception>
#include <fstream>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

#include <bitmaps/bitmap_info.h>
#include <bitmaps/bitmaps_list.h>
#include <gal/recording/draw_stream.h>
#include <hotkeys_basic.h>
#include <ki_exception.h>
#include <locale_io.h>
#include <pgm_base.h>
#include <sch_host/sch_host_abi.h>
#include <tool/action_manager.h>
#include <tool/host_tool_dispatcher.h>
#include <tool/tool_action.h>
#include <view/host_view_controls.h>
#include <wx/filename.h>
#include <wx/string.h>

#include "sch_host.h"
#include <tools/sch_find_replace_tool.h>
#include <tool/tool_manager.h>
#include <sch_item.h>


/**
 * The session as C sees it.
 *
 * A thin shell around SCH_HOST holding the two things the ABI needs but the
 * C++ class does not: the error string a C caller reads back, and storage that
 * keeps the strings in a ::ksch_sheet_info alive across the return.
 */
struct ksch_session
{
    SCH_HOST m_Host;

    /// Last error, as UTF-8. Owned here so the returned pointer outlives the call.
    std::string m_Error;

    /// Backing store for the strings of the most recent ksch_session_sheet_info().
    std::string m_SheetName;
    std::string m_SheetPath;
    std::string m_SheetPage;

    /// Backing store for the strings of the most recent ksch_session_editor_state().
    std::string m_ToolName;
    std::string m_StatusText;
};


namespace
{

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
