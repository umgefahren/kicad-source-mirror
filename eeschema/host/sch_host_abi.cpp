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
#include <ki_exception.h>
#include <sch_host/sch_host_abi.h>
#include <tool/action_manager.h>
#include <tool/tool_action.h>
#include <wx/filename.h>
#include <wx/string.h>

#include "sch_host.h"


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
