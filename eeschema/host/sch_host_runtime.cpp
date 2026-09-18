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
 * @file sch_host_runtime.cpp
 * @brief The process singletons the host shared library owns.
 *
 * This file is the difference between `libkicad_sch_host` and the host objects
 * inside `eeschema_kiface_objects`. The objects implement sessions; this
 * implements the *process* a session needs to exist in — a `PGM_BASE`, a
 * `KIFACE_BASE` and an initialised wxWidgets — and it is deliberately the only
 * place that defines them, because a program that has its own must keep them.
 *
 * It exists because there are exactly two ways to get those singletons in
 * KiCad today, and a non-wx UI has neither:
 *
 * - the GUI, where `single_top.cpp` builds a `PGM_BASE` and the loaded kiface
 *   module installs its settings during `OnKifaceStart`;
 * - a standalone tool, which writes the twenty lines below in its own `main()`
 *   — `qa/tools/sch_dump` and `qa/tools/drc_benchmark` both do.
 *
 * The Rust UI owns `main()` and cannot write C++ there, so the sequence moves
 * behind ::ksch_runtime_init. Keeping it here rather than in `sch_host_abi.cpp`
 * is what lets `qa_eeschema` and `kicad-sch-dump` go on using their own.
 */

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>

#include <eeschema_settings.h>
#include <kiface_base.h>
#include <kiway.h>
#include <pgm_base.h>
#include <settings/kicad_settings.h>
#include <settings/settings_manager.h>
#include <symbol_editor/symbol_editor_settings.h>

#include <wx/app.h>
#include <wx/init.h>
#include <wx/log.h>
#include <wx/utils.h>

#include <sch_host/sch_host_abi.h>


namespace
{

/**
 * Minimal concrete PGM_BASE.
 *
 * PGM_BASE has exactly one pure virtual, and everything the settings manager
 * needs is on the base class, so this is the whole of it. Same approach as
 * `qa/tools/sch_dump` and `qa/tools/drc_benchmark`.
 */
struct SCH_HOST_PGM : public PGM_BASE
{
    void MacOpenFile( const wxString& aFileName ) override {}
};


/**
 * Minimal concrete KIFACE_BASE.
 *
 * `eeschema_kiface_objects` references the global Kiface() but does not define
 * it — `eeschema.cpp`, which does, is linked only into the kiface module — so a
 * library that wraps those objects has to supply one. None of the three methods
 * is ever called here: the only thing that touches this object is the painter,
 * through KifaceSettings(), which the base class answers from what
 * InitSettings() was given.
 */
struct SCH_HOST_KIFACE : public KIFACE_BASE
{
    SCH_HOST_KIFACE() : KIFACE_BASE( "eeschema", KIWAY::FACE_SCH ) {}

    bool OnKifaceStart( PGM_BASE*, int, KIWAY* ) override { return true; }

    wxWindow* CreateKiWindow( wxWindow*, int, KIWAY*, int ) override { return nullptr; }

    void* IfaceOrAddress( int ) override { return nullptr; }
};


SCH_HOST_PGM    g_program;
SCH_HOST_KIFACE g_kiface;

/// Set once ksch_runtime_init() has done its work, so it is idempotent.
bool g_initialised = false;

/// True when the process stood its own singletons up and we must not tear
/// anything down: a library does not get to destroy what it did not create.
bool g_foreign = false;

/// Storage for a failure message, which has no session to be recorded on.
std::string g_error;

} // namespace


/**
 * The global that the eeschema objects link against.
 *
 * It must return the same object InitSettings() was called on. Returning a
 * fresh one instead links perfectly well, and then the painter's eeconfig() is
 * null the first time it draws text — a null dereference a long way from its
 * cause.
 */
KIFACE_BASE& Kiface()
{
    return g_kiface;
}


extern "C" ksch_status ksch_runtime_init( void )
{
    if( g_initialised )
        return KSCH_OK;

    try
    {
        // Someone else owns this process: the QA binaries and kicad-sch-dump
        // install their own PGM_BASE before touching the ABI. Adopt it rather
        // than replacing it, and remember not to tear it down.
        if( PgmOrNull() )
        {
            g_foreign = true;
            g_initialised = true;
            return KSCH_OK;
        }

        // A host that only reads a schematic has no business rewriting the
        // user's configuration, and this library is embedded in a UI that is
        // not the one those settings were written by. An embedder that does
        // want writeback sets the variable itself.
        if( !wxGetEnv( wxT( "KICAD_INHIBIT_SETTINGS_WRITES" ), nullptr ) )
            wxSetEnv( wxT( "KICAD_INHIBIT_SETTINGS_WRITES" ), wxT( "1" ) );

        // Order matters: SetPgm before InitPgm, and the settings have to be
        // registered before anything asks the settings manager for a theme.
        SetPgm( &g_program );

        if( !wxApp::GetInstance() )
            wxApp::SetInstance( new wxAppConsole );

        if( !wxInitialize() )
        {
            g_error = "wxInitialize() failed.";
            SetPgm( nullptr );
            return KSCH_ERR_INTERNAL;
        }

        // Informational wx logging on a UI's stderr is noise. Errors still come
        // through, because a silent failure is worse than a noisy one.
        wxLog::SetLogLevel( wxLOG_Error );

        // Headless, and `aIsUnitTest` — which returns early, right after the
        // settings manager exists and before anything that wants a GUI. That
        // early return is why the `false` it gives back is not an error. This is
        // the same call kicad-sch-dump makes, deliberately: the session loads
        // KiCad's default colour theme explicitly, so nothing below that point
        // affects what gets recorded.
        Pgm().InitPgm( /* aHeadless */ true, /* aIsUnitTest */ true );

        SETTINGS_MANAGER& settings = Pgm().GetSettingsManager();

        settings.RegisterSettings( new KICAD_SETTINGS, false );

        EESCHEMA_SETTINGS* eeschemaSettings = new EESCHEMA_SETTINGS;
        settings.RegisterSettings( eeschemaSettings, false );
        settings.RegisterSettings( new SYMBOL_EDITOR_SETTINGS, false );
        settings.Load();

        // The painter reads Kiface().KifaceSettings() with no null check, so
        // this is not optional. The settings manager owns the object; this only
        // hands the kiface the pointer it will be asked for.
        g_kiface.InitSettings( eeschemaSettings );

        g_initialised = true;
        return KSCH_OK;
    }
    catch( const std::exception& e )
    {
        g_error = e.what();
    }
    catch( ... )
    {
        g_error = "Unknown exception while initialising the schematic host runtime.";
    }

    return KSCH_ERR_INTERNAL;
}


extern "C" void ksch_runtime_shutdown( void )
{
    if( !g_initialised || g_foreign )
    {
        g_initialised = false;
        return;
    }

    try
    {
        Pgm().Destroy();
        wxUninitialize();
        SetPgm( nullptr );
    }
    catch( ... )
    {
        // Teardown of a process that is going away anyway. There is nobody left
        // to report to, and unwinding into C would terminate.
    }

    g_initialised = false;
}


extern "C" int ksch_runtime_is_ready( void )
{
    return PgmOrNull() ? 1 : 0;
}
