/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file sch_dump.cpp
 * @brief `kicad-sch-dump` — record a schematic's draw stream to a file.
 *
 * This is the end-to-end proof for the recording backend. It loads a real
 * `.kicad_sch` with the real reader, populates a real KIGFX::VIEW, lets the
 * real SCH_PAINTER draw into a RECORDING_GAL, and serialises what comes out.
 * Nothing is stubbed and no window is opened.
 *
 * It exists for two reasons:
 *
 * 1. It is the cheapest possible answer to "does the seam work on real files?".
 *    A schematic that renders here renders in the Rust UI, because the same
 *    code produced the stream.
 * 2. It produces the golden fixtures in `qa/data/draw_streams/`, which let the
 *    Rust renderer and decoder be developed and regression-tested with no C++
 *    linked at all.
 *
 * Statistics are printed to stdout in a fixed, greppable format so that a
 * corpus run can be diffed across commits.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <wx/app.h>
#include <wx/filename.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wx/utils.h>
#include <wx/string.h>

#include <eeschema_settings.h>
#include <gal/recording/draw_stream.h>
#include <kiface_base.h>
#include <kiway.h>
#include <locale_io.h>
#include <pgm_base.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>
#include <symbol_editor/symbol_editor_settings.h>

#include <sch_host/sch_host_abi.h>

// The host session is driven through its own C ABI on purpose: if this tool
// works, the ABI works, and there is no second code path to keep in step.


namespace
{

/**
 * Minimal concrete PGM_BASE.
 *
 * PGM_BASE has exactly one pure virtual. Everything else the settings manager
 * needs is already implemented on the base class, so a standalone tool needs no
 * more than this to make Pgm() safe to dereference. Same approach as
 * `qa/tools/drc_benchmark`.
 */
struct SCH_DUMP_PGM : public PGM_BASE
{
    void MacOpenFile( const wxString& aFileName ) override {}
};


/**
 * Minimal concrete KIFACE_BASE.
 *
 * `eeschema_kiface_objects` references the global Kiface() but does not define
 * it — `eeschema.cpp`, which does, is only linked into the kiface module. A
 * standalone tool has to supply one. Nothing here is ever called: the loader
 * touches Kiface() only for KifaceSettings(), which the base class answers from
 * the settings installed by InitSettings().
 */
struct SCH_DUMP_KIFACE : public KIFACE_BASE
{
    SCH_DUMP_KIFACE() : KIFACE_BASE( "eeschema", KIWAY::FACE_SCH ) {}

    bool OnKifaceStart( PGM_BASE*, int, KIWAY* ) override { return true; }

    wxWindow* CreateKiWindow( wxWindow*, int, KIWAY*, int ) override { return nullptr; }

    void* IfaceOrAddress( int ) override { return nullptr; }
};


SCH_DUMP_PGM    g_program;
SCH_DUMP_KIFACE g_kiface;


/// Command-line options, after parsing.
struct OPTIONS
{
    wxString    m_Input;
    wxString    m_Output;
    int         m_Width = 1920;
    int         m_Height = 1080;
    long        m_Sheet = 0;     ///< -1 means every sheet.
    int         m_Repeat = 1;
    bool        m_Write = true;
    bool        m_Histogram = false;
    bool        m_Actions = false;
    bool        m_Quiet = false;
};


void printUsage()
{
    std::printf(
            "kicad-sch-dump — record a schematic's draw stream\n"
            "\n"
            "Usage: kicad-sch-dump [options] <file.kicad_sch>\n"
            "\n"
            "  -o, --output <path>  Stream file to write. Default: <input>.kgds, or\n"
            "                       <input>.sheet<N>.kgds when --sheet all is used.\n"
            "  -W, --width <px>     Viewport width  (default 1920)\n"
            "  -H, --height <px>    Viewport height (default 1080)\n"
            "      --sheet <n|all>  Sheet to record, by index into the page-ordered\n"
            "                       hierarchy. Default 0, the root sheet.\n"
            "      --repeat <n>     Record the frame n times. Retained group data must\n"
            "                       not grow across repeats on an unchanged document;\n"
            "                       the summary reports whether it did.\n"
            "      --no-write       Report statistics only; write nothing.\n"
            "      --histogram      Also print a per-opcode command histogram.\n"
            "      --actions        Print the action registry and exit. Needs no file.\n"
            "  -q, --quiet          Print only the one-line summary.\n"
            "  -h, --help           This text.\n" );
}


bool parseArgs( int argc, char** argv, OPTIONS& aOptions )
{
    for( int ii = 1; ii < argc; ++ii )
    {
        const std::string arg = argv[ii];

        const auto next = [&]( const char* aName ) -> const char*
        {
            if( ii + 1 >= argc )
            {
                std::fprintf( stderr, "error: %s needs a value\n", aName );
                return nullptr;
            }

            return argv[++ii];
        };

        if( arg == "-h" || arg == "--help" )
        {
            printUsage();
            std::exit( 0 );
        }
        else if( arg == "--actions" )
        {
            aOptions.m_Actions = true;
        }
        else if( arg == "-q" || arg == "--quiet" )
        {
            aOptions.m_Quiet = true;
        }
        else if( arg == "--no-write" )
        {
            aOptions.m_Write = false;
        }
        else if( arg == "--histogram" )
        {
            aOptions.m_Histogram = true;
        }
        else if( arg == "-o" || arg == "--output" )
        {
            const char* value = next( "--output" );

            if( !value )
                return false;

            aOptions.m_Output = wxString::FromUTF8( value );
        }
        else if( arg == "-W" || arg == "--width" )
        {
            const char* value = next( "--width" );

            if( !value )
                return false;

            aOptions.m_Width = std::atoi( value );
        }
        else if( arg == "-H" || arg == "--height" )
        {
            const char* value = next( "--height" );

            if( !value )
                return false;

            aOptions.m_Height = std::atoi( value );
        }
        else if( arg == "--repeat" )
        {
            const char* value = next( "--repeat" );

            if( !value )
                return false;

            aOptions.m_Repeat = std::max( 1, std::atoi( value ) );
        }
        else if( arg == "--sheet" )
        {
            const char* value = next( "--sheet" );

            if( !value )
                return false;

            aOptions.m_Sheet = ( std::strcmp( value, "all" ) == 0 ) ? -1 : std::atol( value );
        }
        else if( !arg.empty() && arg[0] == '-' )
        {
            std::fprintf( stderr, "error: unknown option '%s'\n", arg.c_str() );
            return false;
        }
        else
        {
            aOptions.m_Input = wxString::FromUTF8( arg.c_str() );
        }
    }

    if( aOptions.m_Width <= 0 || aOptions.m_Height <= 0 )
    {
        std::fprintf( stderr, "error: viewport dimensions must be positive\n" );
        return false;
    }

    return true;
}


/// Short name for an opcode, for the histogram. Unknown codes print as hex.
const char* opcodeName( std::uint16_t aOp )
{
    switch( static_cast<kgds_op>( aOp ) )
    {
    case KGDS_OP_NOP:                   return "NOP";
    case KGDS_OP_BEGIN_FRAME:           return "BEGIN_FRAME";
    case KGDS_OP_END_FRAME:             return "END_FRAME";
    case KGDS_OP_CLEAR_SCREEN:          return "CLEAR_SCREEN";
    case KGDS_OP_DRAW_GROUP:            return "DRAW_GROUP";
    case KGDS_OP_SET_TARGET:            return "SET_TARGET";
    case KGDS_OP_CLEAR_TARGET:          return "CLEAR_TARGET";
    case KGDS_OP_START_DIFF_LAYER:      return "START_DIFF_LAYER";
    case KGDS_OP_END_DIFF_LAYER:        return "END_DIFF_LAYER";
    case KGDS_OP_START_NEGATIVES_LAYER: return "START_NEGATIVES_LAYER";
    case KGDS_OP_END_NEGATIVES_LAYER:   return "END_NEGATIVES_LAYER";
    case KGDS_OP_SET_IS_FILL:           return "SET_IS_FILL";
    case KGDS_OP_SET_IS_STROKE:         return "SET_IS_STROKE";
    case KGDS_OP_SET_FILL_COLOR:        return "SET_FILL_COLOR";
    case KGDS_OP_SET_STROKE_COLOR:      return "SET_STROKE_COLOR";
    case KGDS_OP_SET_HOVER_COLOR:       return "SET_HOVER_COLOR";
    case KGDS_OP_SET_LINE_WIDTH:        return "SET_LINE_WIDTH";
    case KGDS_OP_SET_MIN_LINE_WIDTH:    return "SET_MIN_LINE_WIDTH";
    case KGDS_OP_SET_LAYER_DEPTH:       return "SET_LAYER_DEPTH";
    case KGDS_OP_SET_NEGATIVE_DRAW_MODE:return "SET_NEGATIVE_DRAW_MODE";
    case KGDS_OP_ENABLE_DEPTH_TEST:     return "ENABLE_DEPTH_TEST";
    case KGDS_OP_TRANSFORM:             return "TRANSFORM";
    case KGDS_OP_ROTATE:                return "ROTATE";
    case KGDS_OP_TRANSLATE:             return "TRANSLATE";
    case KGDS_OP_SCALE:                 return "SCALE";
    case KGDS_OP_SAVE:                  return "SAVE";
    case KGDS_OP_RESTORE:               return "RESTORE";
    case KGDS_OP_LINE:                  return "LINE";
    case KGDS_OP_SEGMENT:               return "SEGMENT";
    case KGDS_OP_SEGMENT_CHAIN:         return "SEGMENT_CHAIN";
    case KGDS_OP_POLYLINE:              return "POLYLINE";
    case KGDS_OP_POLYGON:               return "POLYGON";
    case KGDS_OP_CIRCLE:                return "CIRCLE";
    case KGDS_OP_ARC:                   return "ARC";
    case KGDS_OP_ARC_SEGMENT:           return "ARC_SEGMENT";
    case KGDS_OP_RECTANGLE:             return "RECTANGLE";
    case KGDS_OP_CURVE:                 return "CURVE";
    case KGDS_OP_ELLIPSE:               return "ELLIPSE";
    case KGDS_OP_ELLIPSE_ARC:           return "ELLIPSE_ARC";
    case KGDS_OP_HOLE_WALL:             return "HOLE_WALL";
    case KGDS_OP_BITMAP:                return "BITMAP";
    case KGDS_OP_GRID:                  return "GRID";
    case KGDS_OP_CURSOR:                return "CURSOR";
    case KGDS_OP_MAX:                   break;
    }

    return nullptr;
}


void printHistogram( const kgds_stream_view& aView )
{
    std::map<std::uint16_t, std::size_t> counts;
    std::size_t glyphCommands = 0;

    const auto tally = [&]( const kgds_cmd* aCmds, std::size_t aCount )
    {
        for( std::size_t ii = 0; ii < aCount; ++ii )
        {
            counts[aCmds[ii].op]++;

            if( aCmds[ii].flags & KGDS_FLAG_GLYPH )
                ++glyphCommands;
        }
    };

    tally( aView.group_cmds, aView.group_cmd_count );
    tally( aView.frame_cmds, aView.frame_cmd_count );

    std::printf( "  opcode histogram (group + frame):\n" );

    for( const auto& [op, count] : counts )
    {
        const char* name = opcodeName( op );

        if( name )
            std::printf( "    %-22s %8zu\n", name, count );
        else
            std::printf( "    0x%04x%-16s %8zu\n", op, "", count );
    }

    std::printf( "    %-22s %8zu\n", "(of which glyph)", glyphCommands );
}


int dumpActions()
{
    const std::uint32_t count = ksch_action_count();

    std::printf( "actions %u\n", count );

    for( std::uint32_t ii = 0; ii < count; ++ii )
    {
        ksch_action action;

        if( ksch_action_at( ii, &action ) != KSCH_OK )
            continue;

        std::printf( "%s\ticon=%s\thotkey=0x%x\tscope=%d\tflags=0x%x\tlabel=%s\n", action.name,
                     action.icon_name, static_cast<unsigned>( action.default_hotkey ),
                     action.scope, action.flags, action.friendly_name );
    }

    return 0;
}


/// Record one sheet and report on it. Returns false if anything failed.
bool dumpSheet( ksch_session* aSession, const OPTIONS& aOptions, std::uint32_t aSheetIndex,
                const wxString& aOutputPath )
{
    ksch_status status = ksch_session_set_sheet( aSession, aSheetIndex );

    if( status != KSCH_OK )
    {
        std::fprintf( stderr, "error: cannot select sheet %u: %s (%s)\n", aSheetIndex,
                      ksch_status_name( status ), ksch_session_last_error( aSession ) );
        return false;
    }

    // Frame the page, then set the requested viewport size around it.
    ksch_viewport viewport;
    viewport.width_px = static_cast<std::uint32_t>( aOptions.m_Width );
    viewport.height_px = static_cast<std::uint32_t>( aOptions.m_Height );
    viewport.center_x = 0.0;
    viewport.center_y = 0.0;
    viewport.scale = 1.0;

    if( ksch_session_set_viewport( aSession, &viewport ) != KSCH_OK
        || ksch_session_zoom_to_fit( aSession ) != KSCH_OK
        || ksch_session_get_viewport( aSession, &viewport ) != KSCH_OK )
    {
        std::fprintf( stderr, "error: cannot set up the viewport: %s\n",
                      ksch_session_last_error( aSession ) );
        return false;
    }

    ksch_sheet_info sheet;
    ksch_session_sheet_info( aSession, aSheetIndex, &sheet );

    ksch_bbox pageBox;
    ksch_bbox itemBox;
    ksch_session_bbox( aSession, 1, &pageBox );
    ksch_session_bbox( aSession, 0, &itemBox );

    ksch_document_info info;
    ksch_session_document_info( aSession, &info );

    // Repeat runs exist to prove the retained-group arena is stable: VIEW caches
    // an item's geometry into a group once, and a second frame over an unchanged
    // document must refer to those groups rather than re-record them.
    std::size_t firstGroupCmds = 0;
    double      firstRenderMs = 0.0;
    double      lastRenderMs = 0.0;
    kgds_stream_view view {};

    for( int pass = 0; pass < aOptions.m_Repeat; ++pass )
    {
        const auto start = std::chrono::steady_clock::now();

        status = ksch_session_render( aSession, &view );

        const auto end = std::chrono::steady_clock::now();

        if( status != KSCH_OK )
        {
            std::fprintf( stderr, "error: render failed: %s (%s)\n", ksch_status_name( status ),
                          ksch_session_last_error( aSession ) );
            return false;
        }

        lastRenderMs = std::chrono::duration<double, std::milli>( end - start ).count();

        if( pass == 0 )
        {
            firstGroupCmds = view.group_cmd_count;
            firstRenderMs = lastRenderMs;
        }
    }

    std::size_t bytes = 0;

    if( aOptions.m_Write )
    {
        status = ksch_session_write_stream( aSession, aOutputPath.utf8_str().data() );

        if( status != KSCH_OK )
        {
            std::fprintf( stderr, "error: cannot write '%s': %s (%s)\n",
                          aOutputPath.utf8_str().data(), ksch_status_name( status ),
                          ksch_session_last_error( aSession ) );
            return false;
        }

        bytes = static_cast<std::size_t>( wxFileName::GetSize( aOutputPath ).GetValue() );
    }

    if( !aOptions.m_Quiet )
    {
        std::printf( "  sheet %u  \"%s\"  page %s\n", aSheetIndex, sheet.name, sheet.page_number );
        std::printf( "    path             %s\n", sheet.path );
        std::printf( "    items            %llu\n",
                     static_cast<unsigned long long>( sheet.item_count ) );
        std::printf( "    page bbox        %.0f %.0f  %.0f x %.0f IU\n", pageBox.x, pageBox.y,
                     pageBox.width, pageBox.height );
        std::printf( "    item bbox        %.0f %.0f  %.0f x %.0f IU\n", itemBox.x, itemBox.y,
                     itemBox.width, itemBox.height );
        std::printf( "    viewport         %ux%u px  scale %.6g  centre %.0f %.0f\n",
                     viewport.width_px, viewport.height_px, viewport.scale, viewport.center_x,
                     viewport.center_y );
        std::printf( "    groups           %zu\n", view.group_count );
        std::printf( "    group commands   %zu  (first pass %zu)\n", view.group_cmd_count,
                     firstGroupCmds );
        std::printf( "    frame commands   %zu\n", view.frame_cmd_count );
        std::printf( "    coords           %zu group + %zu frame doubles\n",
                     view.group_coord_count, view.frame_coord_count );
        std::printf( "    images           %zu  (%zu bytes)\n", view.image_count,
                     view.image_bytes );
        std::printf( "    render           %.2f ms first, %.2f ms last (%d pass%s)\n",
                     firstRenderMs, lastRenderMs, aOptions.m_Repeat,
                     aOptions.m_Repeat == 1 ? "" : "es" );

        if( aOptions.m_Repeat > 1 )
        {
            std::printf( "    group growth     %s\n",
                         view.group_cmd_count == firstGroupCmds
                                 ? "none (retained geometry reused)"
                                 : "GREW — retained groups were re-recorded" );
        }

        if( aOptions.m_Write )
            std::printf( "    wrote            %s (%zu bytes)\n", aOutputPath.utf8_str().data(),
                         bytes );

        if( aOptions.m_Histogram )
            printHistogram( view );
    }

    std::printf( "SUMMARY %s sheet=%u items=%llu groups=%zu gcmds=%zu fcmds=%zu "
                 "coords=%zu bytes=%zu render_ms=%.2f\n",
                 aOptions.m_Input.utf8_str().data(), aSheetIndex,
                 static_cast<unsigned long long>( sheet.item_count ), view.group_count,
                 view.group_cmd_count, view.frame_cmd_count,
                 view.group_coord_count + view.frame_coord_count, bytes, firstRenderMs );

    return true;
}


/// Where sheet @p aIndex of @p aInput should be written, absent an explicit -o.
wxString defaultOutputPath( const OPTIONS& aOptions, std::uint32_t aIndex, bool aMultiSheet )
{
    if( !aOptions.m_Output.IsEmpty() && !aMultiSheet )
        return aOptions.m_Output;

    wxFileName fn( aOptions.m_Input );

    if( !aOptions.m_Output.IsEmpty() )
        fn = wxFileName( aOptions.m_Output );

    if( aMultiSheet )
        fn.SetName( wxString::Format( wxT( "%s.sheet%u" ), fn.GetName(), aIndex ) );

    fn.SetExt( wxT( "kgds" ) );

    return fn.GetFullPath();
}

} // namespace


int main( int argc, char** argv )
{
    OPTIONS options;

    if( !parseArgs( argc, argv, options ) )
        return 2;

    if( options.m_Input.IsEmpty() && !options.m_Actions )
    {
        printUsage();
        return 2;
    }

    // Recorded output must not depend on whoever is running the tool, and a run
    // over the repository's own fixtures must not write anything back into them.
    // Both are settled before the settings manager exists: point the config home
    // at a scratch directory and inhibit writeback, unless the caller has already
    // chosen otherwise.
    if( !wxGetEnv( wxT( "KICAD_CONFIG_HOME" ), nullptr ) )
    {
        wxFileName configDir( wxFileName::GetTempDir(), wxEmptyString );
        configDir.AppendDir( wxT( "kicad-sch-dump-config" ) );
        configDir.Mkdir( wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL );

        wxSetEnv( wxT( "KICAD_CONFIG_HOME" ), configDir.GetPath() );
    }

    if( !wxGetEnv( wxT( "KICAD_INHIBIT_SETTINGS_WRITES" ), nullptr ) )
        wxSetEnv( wxT( "KICAD_INHIBIT_SETTINGS_WRITES" ), wxT( "1" ) );

    // Standalone tools have to stand the program singletons up themselves. Order
    // matters: SetPgm before InitPgm, and the settings have to be registered
    // before anything asks the settings manager for a colour theme.
    SetPgm( &g_program );

    wxApp::SetInstance( new wxAppConsole );

    if( !wxInitialize( argc, argv ) )
    {
        std::fprintf( stderr, "error: wxInitialize() failed\n" );
        return 3;
    }

    // Keep informational wx logging off stderr so a corpus run's output stays readable.
    // Errors still come through, because a silent failure is worse than a noisy one.
    wxLog::SetLogLevel( wxLOG_Error );

    Pgm().InitPgm( /* aHeadless */ true, /* aIsUnitTest */ true );

    SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();
    settings.RegisterSettings( new KICAD_SETTINGS, false );

    EESCHEMA_SETTINGS* eeschemaSettings = new EESCHEMA_SETTINGS;
    settings.RegisterSettings( eeschemaSettings, false );
    settings.RegisterSettings( new SYMBOL_EDITOR_SETTINGS, false );
    settings.Load();

    // The loader reads Kiface().KifaceSettings(); without this it is null and
    // the schematic comes back without its per-application defaults.
    g_kiface.InitSettings( eeschemaSettings );

    int result = 0;

    if( options.m_Actions )
    {
        result = dumpActions();
        Pgm().Destroy();
        wxUninitialize();
        return result;
    }

    // Number formatting in the file readers is locale sensitive.
    LOCALE_IO localeGuard;

    ksch_session* session = ksch_session_create();

    if( !session )
    {
        std::fprintf( stderr, "error: cannot create a session: %s\n", ksch_last_global_error() );
        Pgm().Destroy();
        wxUninitialize();
        return 3;
    }

    const auto loadStart = std::chrono::steady_clock::now();

    ksch_status status = ksch_session_load_file( session, options.m_Input.utf8_str().data() );

    const auto loadEnd = std::chrono::steady_clock::now();

    if( status != KSCH_OK )
    {
        std::fprintf( stderr, "error: cannot load '%s': %s (%s)\n",
                      options.m_Input.utf8_str().data(), ksch_status_name( status ),
                      ksch_session_last_error( session ) );
        ksch_session_destroy( session );
        Pgm().Destroy();
        wxUninitialize();
        return 1;
    }

    const double loadMs =
            std::chrono::duration<double, std::milli>( loadEnd - loadStart ).count();

    std::uint32_t sheetCount = 0;
    ksch_session_sheet_count( session, &sheetCount );

    ksch_document_info info;
    ksch_session_document_info( session, &info );

    if( !options.m_Quiet )
    {
        std::printf( "%s\n", options.m_Input.utf8_str().data() );
        std::printf( "  load             %.2f ms\n", loadMs );
        std::printf( "  sheets           %u\n", sheetCount );
        std::printf( "  modified         %s\n", info.modified ? "yes" : "no" );
    }

    const bool allSheets = options.m_Sheet < 0;

    if( !allSheets && ( options.m_Sheet >= static_cast<long>( sheetCount ) ) )
    {
        std::fprintf( stderr, "error: sheet %ld is out of range (%u sheets)\n", options.m_Sheet,
                      sheetCount );
        ksch_session_destroy( session );
        Pgm().Destroy();
        wxUninitialize();
        return 1;
    }

    const std::uint32_t first = allSheets ? 0u : static_cast<std::uint32_t>( options.m_Sheet );
    const std::uint32_t last = allSheets ? sheetCount : first + 1u;

    for( std::uint32_t ii = first; ii < last; ++ii )
    {
        const wxString path = defaultOutputPath( options, ii, allSheets );

        if( !dumpSheet( session, options, ii, path ) )
        {
            result = 1;
            break;
        }
    }

    ksch_session_destroy( session );

    Pgm().Destroy();
    wxUninitialize();

    return result;
}
