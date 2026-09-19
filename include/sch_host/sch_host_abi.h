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
#define KSCH_ABI_VERSION 4u

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

/* ------------------------------------------------------------------ input */

/**
 * What kind of input a ::ksch_input_event carries.
 *
 * Deliberately smaller than the wx vocabulary this replaces. KiCad's own
 * `TOOL_DISPATCHER` consumes fifteen mouse-click event types plus motion, wheel,
 * magnify and a synthetic refresh, and spends most of its 827 lines reconciling
 * them with what the operating system actually did. A UI that delivers ordered,
 * reliable events needs none of that reconciliation, so what crosses here is the
 * vocabulary the tools care about.
 */
typedef enum ksch_input_type
{
    /** The pointer moved. Uses @p x / @p y. */
    KSCH_INPUT_POINTER_MOTION = 0,

    /** A button went down. Uses @p x / @p y and @p button. */
    KSCH_INPUT_POINTER_DOWN = 1,

    /** A button came up. Uses @p x / @p y and @p button. */
    KSCH_INPUT_POINTER_UP = 2,

    /** A double click, delivered *instead of* the second down of the pair. */
    KSCH_INPUT_POINTER_DBLCLICK = 3,

    /**
     * The pointer left the canvas.
     *
     * Send this. It is the only notice the session gets that a button may have
     * been released somewhere it will never hear about, and without it a tool goes
     * on believing a drag is in progress.
     */
    KSCH_INPUT_POINTER_LEAVE = 4,

    /** The wheel turned, or a two-finger scroll. Uses @p scroll_x / @p scroll_y. */
    KSCH_INPUT_SCROLL = 5,

    /** A key went down. Uses @p key. */
    KSCH_INPUT_KEY_DOWN = 6,

    /**
     * A key came up. Uses @p key.
     *
     * Produces no tool event — KiCad's tools are driven by presses — and is
     * accepted so that a UI can forward its whole key stream without filtering.
     */
    KSCH_INPUT_KEY_UP = 7,

    /** Cancel whatever is running, as Escape does. Uses nothing. */
    KSCH_INPUT_CANCEL = 8
} ksch_input_type;


/**
 * Which pointer button an event is about.
 *
 * These are ordinals, not the bit values KiCad uses internally, so that a UI does
 * not have to know about `BUT_LEFT` and friends.
 */
typedef enum ksch_pointer_button
{
    KSCH_BUTTON_NONE = 0,
    KSCH_BUTTON_LEFT = 1,
    KSCH_BUTTON_RIGHT = 2,
    KSCH_BUTTON_MIDDLE = 3,
    /** The mouse-side "back" button, KiCad's BUT_AUX1. */
    KSCH_BUTTON_BACK = 4,
    /** The mouse-side "forward" button, KiCad's BUT_AUX2. */
    KSCH_BUTTON_FORWARD = 5
} ksch_pointer_button;


/**
 * Bits in ksch_input_event::modifiers.
 *
 * Report the *physical* keys and let the session decide what they mean. On macOS
 * that is not the identity mapping: KiCad's hotkeys are written in terms of
 * "Control", and on macOS they are reached with **Command**, because wxWidgets
 * defines `wxMOD_CMD == wxMOD_CONTROL` there. So ::KSCH_MOD_META becomes KiCad's
 * `MD_CTRL` on macOS and physical Control produces no modifier at all, exactly as
 * it does in the wx editor. Pre-translating on the caller's side would get this
 * backwards and every keyboard shortcut in the tree would silently do nothing.
 */
enum ksch_modifier
{
    KSCH_MOD_SHIFT = 1u << 0,
    /** The physical Control key. */
    KSCH_MOD_CTRL = 1u << 1,
    KSCH_MOD_ALT = 1u << 2,
    /** Command on macOS, Super elsewhere. */
    KSCH_MOD_META = 1u << 3
};


/** Bits in ksch_input_event::flags. */
enum ksch_input_flag
{
    /** The key press is an auto-repeat rather than a fresh one. */
    KSCH_INPUT_FLAG_AUTOREPEAT = 1u << 0
};


/**
 * One input event from the UI.
 *
 * **Positions are in screen pixels**, relative to the top-left of the canvas —
 * not in internal units like everything else in this header. That is deliberate:
 * the session derives the world position itself, through the same
 * `KIGFX::VIEW_CONTROLS` a tool reads it back from, so that a cursor a tool has
 * forced or placed is the one the following events carry. A caller handing world
 * coordinates in would bypass that and the tools would disagree with the view
 * about where the cursor is.
 */
typedef struct ksch_input_event
{
    int32_t  type;      /**< A ::ksch_input_type. */
    int32_t  button;    /**< A ::ksch_pointer_button; ::KSCH_BUTTON_NONE if none. */
    uint32_t modifiers; /**< A bitwise-or of ::ksch_modifier. */
    uint32_t flags;     /**< A bitwise-or of ::ksch_input_flag. */

    /**
     * The key's name, for a key event; null or "" otherwise.
     *
     * Named in the UI's own vocabulary, lower case and unpunctuated — "escape",
     * "pagedown", "f11", "w" — and resolved to KiCad's `WXK_*` key code on this
     * side of the boundary. That is the point: the hotkey registry is keyed by
     * `WXK_*` integers (see ::ksch_action), and having the mapping here means the
     * numbers come out of `wx/defs.h` through a compiler rather than out of a
     * table transcribed by hand into another language, where one wrong entry is a
     * shortcut that silently does nothing.
     *
     * A name the session does not know is dropped rather than sent as key zero.
     * Borrowed for the duration of the call only.
     */
    const char* key;

    double x; /**< Pointer position, screen pixels from the canvas' top-left. */
    double y;

    double scroll_x; /**< Scroll delta in wheel detents; positive y is away from the user. */
    double scroll_y;
} ksch_input_event;


/** Bits returned by ::ksch_session_dispatch_input and ::ksch_session_run_action. */
enum ksch_input_result
{
    /** A tool or a hotkey claimed the event. */
    KSCH_INPUT_HANDLED = 1u << 0,

    /**
     * Something asked for the canvas to be repainted, so the frame the caller is
     * holding is stale and ::ksch_session_render should be called again.
     *
     * This is the only notice a UI gets that the document or the view changed
     * behind its back — a tool moving the view, an edit, a selection change. A UI
     * that ignores it will show a frame that no longer matches the document.
     */
    KSCH_INPUT_REDRAW = 1u << 1
};


/**
 * Give one input event to the tool framework.
 *
 * @param aEvent    the event; must not be null.
 * @param aOutFlags receives a bitwise-or of ::ksch_input_result, or may be null.
 *                  Passing null leaves a pending ::KSCH_INPUT_REDRAW *pending*, so
 *                  the next call that does ask reports it: a repaint the UI never
 *                  hears about is a stale window, whereas one attributed to the
 *                  following event costs a redundant re-record and nothing else.
 *
 * @note A session with no document accepts input and does nothing useful with it,
 *       rather than failing: a UI is allowed to have a window open before a file
 *       is loaded, and its pointer still moves.
 */
KISCH_API ksch_status ksch_session_dispatch_input( ksch_session*           aSession,
                                                   const ksch_input_event* aEvent,
                                                   uint32_t*               aOutFlags );

/**
 * Forget which buttons are down, because the UI lost focus.
 *
 * Without this, a button released while the window was not focused leaves a tool
 * believing its drag is still running. The last known cursor position is kept.
 */
KISCH_API ksch_status ksch_session_reset_input( ksch_session* aSession );

/**
 * Run a registered action by its dotted name, as a menu item or a toolbar button
 * does.
 *
 * @param aNameUtf8 a ::ksch_action::name; borrowed for the call.
 * @param aOutFlags receives a bitwise-or of ::ksch_input_result. May be null.
 *
 * An action that no registered tool handles reports ::KSCH_OK with
 * ::KSCH_INPUT_HANDLED clear rather than an error, because a UI built from the
 * whole registry will legitimately offer plenty of them.
 */
KISCH_API ksch_status ksch_session_run_action( ksch_session* aSession, const char* aNameUtf8,
                                               uint32_t* aOutFlags );


/** Bits in ksch_editor_state::flags. */
enum ksch_editor_flag
{
    /** The pointer is over the canvas, so a crosshair should be drawn. */
    KSCH_EDITOR_POINTER_OVER_CANVAS = 1u << 0,

    /** Some screen in the hierarchy has unsaved changes. */
    KSCH_EDITOR_MODIFIED = 1u << 1
};


/**
 * What the editor is doing, for a UI's status bar and overlays.
 *
 * The cursor is the interesting field: it is what the *tools* see, which is the
 * pointer snapped to the grid, or wherever a tool has forced it to be — not the
 * raw pointer position the UI already knows.
 */
typedef struct ksch_editor_state
{
    double cursor_x; /**< The cursor the tools read, in internal units. */
    double cursor_y;

    uint32_t selection_count; /**< Items currently selected. */
    uint32_t flags;           /**< A bitwise-or of ::ksch_editor_flag. */

    /**
     * Commands on the undo and redo stacks, for a UI that greys out its menu items.
     *
     * Zero on a session that has not edited anything, which is also the answer for one
     * whose tools cannot edit — so a UI cannot tell "nothing to undo" from "undo is not
     * implemented", and does not need to.
     */
    uint32_t undo_count;
    uint32_t redo_count;

    /** The user-level tool on top of the tool stack, or "" if none. Session-scoped. */
    const char* tool_name;

    /** The last status text a tool asked to show, or "". Session-scoped. */
    const char* status_text;
} ksch_editor_state;

/**
 * Read the editor state.
 *
 * @param aOut receives the state; must not be null.
 */
KISCH_API ksch_status ksch_session_editor_state( ksch_session*      aSession,
                                                 ksch_editor_state* aOut );

/* ----------------------------------------------------------------- undo, redo, save */

/**
 * Undo the newest command.
 *
 * This is not ::ksch_session_run_action with `"common.Interactive.undo"`: that action is
 * handled by `SCH_EDITOR_CONTROL`, which still declines an editing context that is not a
 * `wxFrame`. Undo itself does not need one, so it is an entry point of its own until that
 * tool is converted.
 *
 * @param aOutUndone receives 1 if anything was undone and 0 if not, or may be null. Zero
 *                   means the stack was empty, which is not an error. An `int` rather than
 *                   a `bool` because this header is plain C89-compatible, as
 *                   ::ksch_session_bbox's flag already is.
 */
KISCH_API ksch_status ksch_session_undo( ksch_session* aSession, int* aOutUndone );

/**
 * Redo the newest undone command. See ::ksch_session_undo.
 *
 * @param aOutRedone receives whether anything was redone, or may be null.
 */
KISCH_API ksch_status ksch_session_redo( ksch_session* aSession, int* aOutRedone );

/**
 * Write every sheet of the hierarchy back to the files it was loaded from.
 *
 * Deliberately narrower than the editor's "Save": it writes the `.kicad_sch` files through
 * the same writer and stops there. It does **not** write the project file, the symbol
 * library table, a backup archive or the embedded-file cache, each of which is a decision
 * about the *project* rather than about the document, and each of which a UI that wants it
 * should ask for separately. A file written here reopens in KiCad.
 *
 * On failure the document is left loaded and unmodified-flags are left alone, so a caller
 * can report the error and try again.
 */
KISCH_API ksch_status ksch_session_save( ksch_session* aSession );

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
 * Textual primary and alternate hotkeys for the action at @p aIndex.
 *
 * Uses KiCad's platform-specific key names (e.g. "Cmd+Z", "Ctrl+Z", "Esc").
 * Both outputs are required. Strings live for the process lifetime and are
 * empty for an unbound key. This keeps WXK numeric constants out of callers.
 */
KISCH_API ksch_status ksch_action_hotkey_names( uint32_t aIndex, const char** aPrimary,
                                              const char** aAlternate );

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
