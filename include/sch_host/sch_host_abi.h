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

#ifndef KICAD_SCH_HOST_ABI_H
#define KICAD_SCH_HOST_ABI_H

/**
 * @file sch_host_abi.h
 * @brief The C ABI a Rust UI uses to drive a headless schematic editor session.
 *
 * This is the companion to `gal/recording/draw_stream_abi.h`. That header
 * describes what a frame *is*; this one describes how to get one: open a
 * session, load a `.kicad_sch`, point a camera at it, render, and read the
 * resulting draw stream. It also exposes KiCad's action registry, so the UI
 * can build its menus, toolbars and hotkey editor from the real ~445 actions
 * rather than a hand-maintained copy that will drift.
 *
 * ## Rules this header keeps
 *
 * - Plain C. No C++ types cross the boundary, every struct is POD with an
 *   explicit layout, and the whole file is `bindgen`-clean.
 * - No exceptions escape. Every entry point catches everything and returns a
 *   ::ksch_status. A C caller never unwinds through KiCad.
 * - Every pointer documents who owns it. The rule, with no exceptions, is that
 *   the session owns everything it hands out and the caller owns nothing it did
 *   not allocate itself.
 * - Null handles and null out-parameters are errors, not crashes.
 *
 * ## Threading
 *
 * **Everything here belongs to one thread: the one that called
 * ::ksch_runtime_init.** Not one session per thread — one thread, full stop.
 * wxWidgets records that thread as its main thread during initialisation, and
 * eeschema's connectivity engine asserts on it: `SCH_CONNECTIVITY::ENGINE::Clear`
 * and `INPUT_STORE::Invalidate` both `wxASSERT( wxThread::IsMain() )`, and both
 * are reached by an ordinary document load. So a session created on a second
 * thread does not merely race — it trips assertions on the way to whatever the
 * engine does with state it believes is thread-confined.
 *
 * The action registry calls (::ksch_action_count and friends) read a table built
 * once on first use and are safe from any thread afterwards, but the first call
 * must not race with itself.
 *
 * ## String lifetimes, stated once
 *
 * Every `const char*` returned by this ABI is UTF-8 and NUL-terminated, and is
 * borrowed. There are exactly two lifetimes:
 *
 * - Strings in ::ksch_sheet_info and the session's error string belong to the
 *   session and are valid until the next call on that session or until it is
 *   destroyed, whichever comes first.
 * - Strings in ::ksch_action and the global error string live for the lifetime
 *   of the process.
 *
 * Copy anything you need to keep. The caller must never free any of them.
 *
 * ## Coordinates
 *
 * All coordinates and distances are `double`s in KiCad internal units, which
 * for eeschema are 100 nm. This matches the draw stream, so a bounding box and
 * the geometry in a frame are in the same space and need no conversion.
 */

#include <stddef.h>
#include <stdint.h>

#include <gal/recording/draw_stream_abi.h>

/*
 * Export marker. A consumer that links the host statically defines
 * KISCH_HOST_STATIC and gets plain declarations; the shared-library build
 * defines KISCH_HOST_BUILD.
 */
#if defined( KISCH_HOST_STATIC )
#define KISCH_API
#elif defined( _WIN32 )
#if defined( KISCH_HOST_BUILD )
#define KISCH_API __declspec( dllexport )
#else
#define KISCH_API __declspec( dllimport )
#endif
#elif defined( __GNUC__ ) && __GNUC__ >= 4
#define KISCH_API __attribute__( ( visibility( "default" ) ) )
#else
#define KISCH_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ABI version, reported by ::ksch_abi_version.
 *
 * Bump on any change to a struct layout, an enumerator value or a function
 * signature. A caller built against a different version must refuse to run
 * rather than reinterpret a struct.
 */
#define KSCH_ABI_VERSION 2u

/* --------------------------------------------------------------- status */

/**
 * Result of every fallible call.
 *
 * ::KSCH_OK is zero and everything else is non-zero, so `if( status )` reads as
 * "something went wrong". When a call returns an error, out-parameters are left
 * untouched; the caller's buffer keeps whatever it held.
 */
typedef enum ksch_status
{
    KSCH_OK = 0,

    /** A handle was null, an out-parameter was null, or an argument was out of
     *  its documented domain (a non-positive viewport dimension, say). */
    KSCH_ERR_INVALID_ARG = 1,

    /** The call needs a loaded document and the session has none. */
    KSCH_ERR_NO_DOCUMENT = 2,

    /** The path does not name an existing file. */
    KSCH_ERR_FILE_NOT_FOUND = 3,

    /** The file exists but could not be read as a schematic. The session's
     *  error string carries the reader's own message. */
    KSCH_ERR_LOAD_FAILED = 4,

    /** An index was past the end of the collection it addresses. */
    KSCH_ERR_OUT_OF_RANGE = 5,

    /** A write failed: the path is not writable, or the disk filled. */
    KSCH_ERR_IO = 6,

    /** Allocation failed. */
    KSCH_ERR_OUT_OF_MEMORY = 7,

    /** An exception escaped a KiCad call. The session's error string carries
     *  what could be recovered from it. This is a bug somewhere; report it. */
    KSCH_ERR_INTERNAL = 8
} ksch_status;

/**
 * A short, stable, ASCII name for a status code, for logging.
 *
 * Never returns null: an unrecognised code yields "KSCH_ERR_UNKNOWN". The
 * returned string is static.
 */
KISCH_API const char* ksch_status_name( ksch_status aStatus );

/** The ::KSCH_ABI_VERSION this library was built against. */
KISCH_API uint32_t ksch_abi_version( void );

/* -------------------------------------------------------------- runtime */

/**
 * Stand up the process-wide state KiCad needs, once, before the first session.
 *
 * KiCad's document model reaches for two process singletons that a GUI build
 * gets from `main()` and a kiface module: a `PGM_BASE`, which owns the settings
 * manager, and a `KIFACE_BASE`, whose `KifaceSettings()` the schematic painter
 * dereferences without a null check. There is no session-scoped way to supply
 * them, so they are the embedder's job — and an embedder that has to discover
 * that from a crash inside the font code has been handed a bad ABI. This call
 * is that job, done once:
 *
 * - installs a minimal `PGM_BASE` and a minimal `KIFACE_BASE`;
 * - initialises wxWidgets in console mode — no `wxApp`, no toolkit, no window;
 * - creates the settings manager, registers eeschema's settings objects and
 *   loads them;
 * - leaves wx logging at the error level, so a UI's stderr stays readable.
 *
 * Settings **writeback is inhibited** unless `KICAD_INHIBIT_SETTINGS_WRITES` is
 * already set in the environment: a host that only reads a schematic has no
 * business rewriting the user's configuration. The rendered colours do not
 * depend on it either way — the session loads KiCad's default theme explicitly,
 * so a recorded stream is reproducible across machines.
 *
 * Calling this twice is harmless. If the process already installed its own
 * `PGM_BASE` — the QA binaries and `kicad-sch-dump` do — nothing is touched and
 * ::KSCH_OK is returned, because clobbering a live singleton would be worse
 * than doing nothing.
 *
 * On failure ::ksch_last_global_error describes it and no session can be
 * created.
 *
 * **Threading.** This call decides which thread everything else belongs to: wx
 * takes the caller to be its main thread, and the document model asserts on that
 * afterwards. Call it from the thread that will own the sessions, and do not race
 * it with itself.
 *
 * @note This entry point lives in the host *shared library*, not in the host
 *       objects: it defines the process singletons, and a program that has its
 *       own must keep them.
 */
KISCH_API ksch_status ksch_runtime_init( void );

/**
 * Release what ::ksch_runtime_init created.
 *
 * Destroy every session first; this tears down the settings manager they read
 * through. Safe to call when the runtime was never initialised, and safe to
 * call twice. Does nothing if the process owned its singletons already.
 */
KISCH_API void ksch_runtime_shutdown( void );

/** Non-zero once a session can be created, whoever stood the process up. */
KISCH_API int ksch_runtime_is_ready( void );

/* -------------------------------------------------------------- session */

/**
 * An opaque schematic editor session.
 *
 * Wraps a SCH_HOST: a SCHEMATIC, a KIGFX::VIEW, a RECORDING_GAL, the render
 * settings and the current sheet path. No window, no GL context, no wxFrame.
 */
typedef struct ksch_session ksch_session;

/**
 * Create an empty session.
 *
 * The process singletons must be standing: either ::ksch_runtime_init has been
 * called, or the program installed its own `PGM_BASE`. Without them this fails
 * cleanly here rather than dereferencing null several frames into the document
 * model.
 *
 * @return a handle the caller owns and must release with ::ksch_session_destroy,
 *         or null if the session could not be created — in which case
 *         ::ksch_last_global_error says why.
 */
KISCH_API ksch_session* ksch_session_create( void );

/**
 * Destroy a session and everything it owns, including the document and every
 * buffer behind a ::kgds_stream_view previously handed out.
 *
 * Any stream view, sheet-info string or error string borrowed from this session
 * dangles after this returns. Null is accepted and ignored.
 */
KISCH_API void ksch_session_destroy( ksch_session* aSession );

/**
 * The last error recorded on this session.
 *
 * Never returns null; an empty string means nothing has failed. Valid until the
 * next call on the session. A null handle yields "" rather than null, so the
 * result is always safe to print.
 */
KISCH_API const char* ksch_session_last_error( const ksch_session* aSession );

/**
 * The last error from a call that has no session to record it on, currently
 * only a failed ::ksch_session_create.
 *
 * Never returns null. The storage is per-process and lives as long as it.
 */
KISCH_API const char* ksch_last_global_error( void );

/* ------------------------------------------------------------- document */

/**
 * Load a `.kicad_sch` file, replacing whatever the session held.
 *
 * Goes through the existing SCH_IO_KICAD_SEXPR reader by way of
 * EESCHEMA_HELPERS::LoadSchematic, so symbol links, instance-data migration,
 * page numbering and connectivity are resolved exactly as they are for
 * `kicad-cli`. On success the root sheet becomes current, the view is populated
 * and the camera is framed on the page.
 *
 * On failure the session is left empty — not in its previous state — and the
 * error string describes the failure.
 *
 * @param aSession  the session; must not be null.
 * @param aPathUtf8 NUL-terminated UTF-8 path, borrowed for the duration of the
 *                  call only.
 */
KISCH_API ksch_status ksch_session_load_file( ksch_session* aSession, const char* aPathUtf8 );

/** Drop the loaded document, leaving the session reusable. Idempotent. */
KISCH_API ksch_status ksch_session_unload( ksch_session* aSession );

/** Non-zero if a document is loaded. A null handle reports 0. */
KISCH_API int ksch_session_is_loaded( const ksch_session* aSession );

/**
 * Summary of the loaded document.
 */
typedef struct ksch_document_info
{
    uint32_t sheet_count;   /**< Sheets in the hierarchy; at least 1 when loaded. */
    uint32_t current_sheet; /**< Index of the current sheet, into the sheet list. */
    uint64_t item_count;    /**< Items on the current sheet's screen. */
    uint32_t modified;      /**< Non-zero if any screen has unsaved changes. */
    uint32_t reserved;      /**< Must be ignored; present to keep the struct 8-byte aligned. */
} ksch_document_info;

/**
 * Fill in a ::ksch_document_info.
 *
 * @param aOut receives the summary; must not be null.
 */
KISCH_API ksch_status ksch_session_document_info( const ksch_session*  aSession,
                                                  ksch_document_info* aOut );

/**
 * An axis-aligned box in internal units.
 *
 * Width and height are non-negative. An empty box reports all four fields zero.
 */
typedef struct ksch_bbox
{
    double x;
    double y;
    double width;
    double height;
} ksch_bbox;

/**
 * The current sheet's bounding box.
 *
 * @param aIncludeAllVisible non-zero returns the whole page rectangle, which is
 *        what a zoom-to-fit should frame and what SCH_EDIT_FRAME reports. Zero
 *        returns the union of the items' own bounding boxes, excluding the
 *        drawing sheet, which is what you want to know how much of the page is
 *        actually used.
 */
KISCH_API ksch_status ksch_session_bbox( const ksch_session* aSession, int aIncludeAllVisible,
                                         ksch_bbox* aOut );

/* --------------------------------------------------------------- sheets */

/**
 * One sheet of the hierarchy.
 *
 * The three strings are borrowed from the session and are valid only until the
 * next call on it.
 */
typedef struct ksch_sheet_info
{
    const char* name;        /**< The sheet's own name. */
    const char* path;        /**< Human-readable hierarchical path, e.g. "/power/regulators". */
    const char* page_number; /**< As shown in the sheet list; not necessarily numeric. */
    uint64_t    item_count;  /**< Items on this sheet's screen. */
} ksch_sheet_info;

/** Number of sheets in the hierarchy, ordered by page number. */
KISCH_API ksch_status ksch_session_sheet_count( const ksch_session* aSession, uint32_t* aOut );

/**
 * Describe one sheet.
 *
 * @param aIndex an index below the count reported by ::ksch_session_sheet_count.
 */
KISCH_API ksch_status ksch_session_sheet_info( const ksch_session* aSession, uint32_t aIndex,
                                               ksch_sheet_info* aOut );

/**
 * Make a sheet current, repopulating the view from its screen.
 *
 * This invalidates every retained group in the stream, because the previous
 * sheet's items are gone. The next ::ksch_session_render therefore re-records
 * the geometry, and any group id the renderer cached must be discarded.
 */
KISCH_API ksch_status ksch_session_set_sheet( ksch_session* aSession, uint32_t aIndex );

/* ------------------------------------------------------------- viewport */

/**
 * The camera.
 *
 * @p scale is the world-to-screen factor: a world distance multiplied by
 * @p scale gives pixels — so for eeschema, pixels per 100 nm. @p center_x /
 * @p center_y are the world point drawn at the middle of the viewport.
 *
 * Note that this is deliberately *not* what `KIGFX::VIEW` calls a scale, which
 * is the GAL zoom factor and differs from this by the screen DPI times
 * eeschema's world unit length. A consumer of this ABI holds a camera over the
 * recorded coordinates, and those are in internal units, so pixels per internal
 * unit is the quantity that needs no conversion on its side.
 */
typedef struct ksch_viewport
{
    uint32_t width_px;
    uint32_t height_px;
    double   center_x;
    double   center_y;
    double   scale;
} ksch_viewport;

/**
 * Point the camera.
 *
 * The scale is clamped to eeschema's own zoom limits — the same ones the wx
 * editor is bound by — so ::ksch_session_get_viewport afterwards does not
 * necessarily report what was asked for. A caller driving its own camera should
 * read it back and adopt it, because the session culls the frame it records to
 * the scale it is holding: a caller showing a wider view than the session
 * believes in would find the geometry outside that view missing from the frame.
 *
 * @param aViewport must have positive dimensions and a positive scale;
 *        anything else is ::KSCH_ERR_INVALID_ARG and nothing changes.
 */
KISCH_API ksch_status ksch_session_set_viewport( ksch_session*        aSession,
                                                 const ksch_viewport* aViewport );

/** Read the camera back. */
KISCH_API ksch_status ksch_session_get_viewport( const ksch_session* aSession,
                                                 ksch_viewport*     aOut );

/**
 * Frame the whole page in the current viewport, keeping the viewport size.
 *
 * Needs a loaded document.
 */
KISCH_API ksch_status ksch_session_zoom_to_fit( ksch_session* aSession );

/* --------------------------------------------------------------- render */

/**
 * Record a frame and borrow the resulting stream.
 *
 * Runs KIGFX::VIEW::Redraw through the recording backend, so SCH_PAINTER draws
 * exactly what it would draw on any other canvas.
 *
 * **Ownership.** @p aOut is filled with pointers into buffers the session owns.
 * They stay valid until the next call that mutates the stream — another render,
 * a sheet change, an unload, or destruction. Nothing is copied and the caller
 * must not free anything. Read what you need, or copy it, before the next call.
 */
KISCH_API ksch_status ksch_session_render( ksch_session* aSession, kgds_stream_view* aOut );

/**
 * Borrow the stream recorded by the last ::ksch_session_render without
 * recording a new one. Same ownership rules.
 */
KISCH_API ksch_status ksch_session_publish( const ksch_session* aSession, kgds_stream_view* aOut );

/**
 * Serialise the last recorded frame to a file in the ::kgds_file_header format.
 *
 * This is how golden fixtures are produced: a checked-in stream lets the Rust
 * renderer be developed and tested with no C++ linked at all.
 *
 * @param aPathUtf8 NUL-terminated UTF-8 path, borrowed for the call.
 */
KISCH_API ksch_status ksch_session_write_stream( ksch_session* aSession, const char* aPathUtf8 );

/* ------------------------------------------------------- action registry */

/** Bits in ksch_action::flags. */
enum ksch_action_flag
{
    /** The action activates a tool (KiCad's AF_ACTIVATE). */
    KSCH_ACTION_FLAG_ACTIVATION = 1u << 0,
    /** The action is a notification rather than a command (AF_NOTIFY). */
    KSCH_ACTION_FLAG_NOTIFICATION = 1u << 1
};

/** Matches KiCad's TOOL_ACTION_SCOPE, value for value. */
typedef enum ksch_action_scope
{
    /** Belongs to one tool, i.e. a context-menu entry (AS_CONTEXT). */
    KSCH_ACTION_SCOPE_CONTEXT = 1,
    /** Available to all active tools (AS_ACTIVE). */
    KSCH_ACTION_SCOPE_ACTIVE = 2,
    /** Global: toolbar, main menu, global shortcut (AS_GLOBAL). */
    KSCH_ACTION_SCOPE_GLOBAL = 3
} ksch_action_scope;

/**
 * One entry of KiCad's action registry.
 *
 * Every string is UTF-8, never null (absent fields are ""), and lives for the
 * lifetime of the process.
 */
typedef struct ksch_action
{
    /**
     * The stable dotted id, e.g. "eeschema.SymbolDrawing.placeSymbolPin".
     *
     * This is the key to use everywhere. It is ASCII, it is what the hotkey
     * config file is keyed by, and it does not change between builds — unlike
     * ::id, which is assigned in static-initialisation order.
     */
    const char* name;

    /** Translated label for a menu item or a toolbar button. */
    const char* friendly_name;
    /** Translated menu label, which may differ from the friendly name. */
    const char* menu_label;
    /** Translated tooltip, including the hotkey. */
    const char* tooltip;
    /** Translated long description. */
    const char* description;

    /**
     * The icon's base name, with no theme, size or extension — e.g. "pin".
     *
     * Resolve it as `resources/bitmaps_png/sources/{light,dark}/<name>.svg` and
     * render the SVG at the device pixel ratio. The enumerator name *is* the
     * file name, which is why a name is exported rather than an enum: a vector
     * icon at the exact size beats KiCad's own pre-rasterised PNG archive, and
     * it keeps the light/dark split.
     *
     * Empty when the action has no icon.
     */
    const char* icon_name;

    /** The owning tool, i.e. ::name with its last component removed. */
    const char* tool_name;

    /** Runtime id. Useful for round-tripping within one process; never persist it. */
    int32_t id;
    /** Id in KiCad's own UI-event space. */
    int32_t ui_id;

    /**
     * Hotkeys, as WXK_* key codes OR'd with the MD_* modifier bits.
     *
     * Zero means unbound. A UI that generates key events must map its own keys
     * onto these same numbers, or every default binding and every saved binding
     * breaks. The values are the ones in `wx/defs.h`.
     */
    int32_t default_hotkey;
    int32_t default_hotkey_alt;
    int32_t hotkey;     /**< After user configuration. */
    int32_t hotkey_alt;

    int32_t  scope; /**< A ::ksch_action_scope. */
    uint32_t flags; /**< A bitwise-or of ::ksch_action_flag. */
} ksch_action;

/**
 * Number of registered actions.
 *
 * Needs no session and no document: TOOL_ACTION's constructor registers each
 * action into a process-wide list during static initialisation, so the registry
 * is complete before `main()` runs.
 */
KISCH_API uint32_t ksch_action_count( void );

/**
 * Describe the action at @p aIndex, which must be below ::ksch_action_count.
 *
 * The order is stable for the lifetime of the process but is not otherwise
 * meaningful; sort by ::ksch_action::name if you need a deterministic order.
 */
KISCH_API ksch_status ksch_action_at( uint32_t aIndex, ksch_action* aOut );

/**
 * Look an action up by its dotted name.
 *
 * @return ::KSCH_OK, or ::KSCH_ERR_OUT_OF_RANGE if no action has that name.
 */
KISCH_API ksch_status ksch_action_find( const char* aNameUtf8, ksch_action* aOut );

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* KICAD_SCH_HOST_ABI_H */
