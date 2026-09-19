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

#include <algorithm>

#include <base_units.h>
#include <drawing_sheet/ds_data_model.h>
#include <drawing_sheet/ds_proxy_view_item.h>
#include <eeschema_helpers.h>
#include <eeschema_settings.h>
#include <kiface_base.h>
#include <pgm_base.h>
#include <project.h>
#include <settings/app_settings.h>
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <advanced_config.h>
#include <sch_item.h>
#include <sch_painter.h>
#include <sch_render_settings.h>
#include <schematic_undo_redo.h>
#include <sch_io/kicad_sexpr/sch_io_kicad_sexpr.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_view.h>
#include <schematic.h>
#include <tool/action_manager.h>
#include <tool/actions.h>
#include <tool/common_control.h>
#include <tool/common_tools.h>
#include <tool/embed_tool.h>
#include <tool/host_tool_dispatcher.h>
#include <tool/picker_tool.h>
#include <tool/properties_tool.h>
#include <tool/tool_manager.h>
#include <tool/zoom_tool.h>
#include <tools/ee_graphic_tool.h>
#include <tools/sch_actions.h>
#include <tools/sch_align_tool.h>
#include <tools/sch_design_block_control.h>
#include <tools/sch_drawing_tools.h>
#include <tools/sch_edit_table_tool.h>
#include <tools/sch_edit_tool.h>
#include <tools/sch_editor_control.h>
#include <tools/sch_find_replace_tool.h>
#include <tools/sch_group_tool.h>
#include <tools/sch_inspection_tool.h>
#include <tools/sch_line_wire_bus_tool.h>
#include <tools/sch_move_tool.h>
#include <tools/sch_navigate_tool.h>
#include <tools/sch_point_editor.h>
#include <tools/sch_selection_tool.h>
#include <view/host_view_controls.h>
#include <view/view.h>
#include <wildcards_and_files_ext.h>
#include <wx/filename.h>
#include <zoom_defines.h>

#include "sch_host.h"
#include "sch_host_control.h"


/// Fraction of the viewport left as margin by ZoomToFit(), matching the feel of
/// the editor's own zoom-to-fit rather than butting the drawing against the edge.
static constexpr double ZOOM_FIT_MARGIN = 1.05;


SCH_HOST::SCH_HOST() :
        m_schematic( nullptr ),
        m_currentSheetIndex( 0 ),
        m_viewportSize( 1920, 1080 ),
        m_redrawRequested( false ),
        m_cursor( KICURSOR::ARROW )
{
    buildCanvas();
    setupTools();
}


SCH_HOST::~SCH_HOST()
{
    // The tools go first, and explicitly rather than by member order, because
    // Unload() below deletes the SCHEMATIC and a tool's destructor may reach for the
    // model or the view. Leaving it to the member destructors would tear the document
    // down in the constructor's body and the tools afterwards, which is the wrong way
    // round — SCH_SELECTION_TOOL's destructor, for one, unlinks itself from the view.
    if( m_ownedToolManager )
    {
        m_ownedToolManager->ShutdownAllTools();

        m_dispatcher.reset();
        m_ownedToolManager.reset(); // deletes every registered tool
        m_ownedActions.reset();

        // TOOLS_HOLDER's pointers are aliases of what was just freed.
        m_toolManager = nullptr;
        m_actions = nullptr;
    }

    Unload();
}


void SCH_HOST::ensureKifaceSettings()
{
    // SCH_PAINTER reads eeconfig() — which is Kiface().KifaceSettings() — and
    // dereferences it with no null check, at sch_painter.cpp:594 and six other
    // sites. In the GUI the eeschema kiface module installs those settings during
    // OnKifaceStart, so the pointer is always live by the time anything draws. A
    // process that never loaded that module — a test binary, a CLI tool, a Rust
    // host — has nothing to install them, and the first piece of text drawn is a
    // null dereference several frames deep inside KIFONT.
    //
    // Text is most of a schematic's geometry, so that is not a corner case; it is
    // the first thing that happens. The host stands the settings up rather than
    // making it the embedder's problem, because a C ABI that segfaults when the
    // caller forgets an undocumented global is not an ABI.
    if( Kiface().KifaceSettings() )
        return;

    // Deliberately leaked. It has to outlive every SCH_HOST and the global
    // Kiface() itself, and a function-local static would tie its destruction to
    // an order we do not control.
    static EESCHEMA_SETTINGS* fallbackSettings = new EESCHEMA_SETTINGS;

    Kiface().InitSettings( fallbackSettings );
}


void SCH_HOST::buildCanvas()
{
    ensureKifaceSettings();

    m_gal = std::make_unique<KIGFX::RECORDING_GAL>( m_displayOptions );

    // Eeschema's internal unit is 100 nm, not the 1 nm the GAL base class assumes.
    // Everything that converts between world and screen space reads this, so it has
    // to be set before the view is given the GAL.
    m_gal->SetWorldUnitLength( SCH_WORLD_UNIT );
    m_gal->ResizeScreen( m_viewportSize.x, m_viewportSize.y );

    m_view = std::make_unique<KIGFX::SCH_VIEW>( nullptr );
    m_view->SetGAL( m_gal.get() );

    m_painter = std::make_unique<KIGFX::SCH_PAINTER>( m_gal.get() );
    m_painter->GetSettings()->LoadColors( ::GetColorSettings( DEFAULT_THEME ) );
    m_view->SetPainter( m_painter.get() );

    m_view->SetScaleLimits( ZOOM_MAX_LIMIT_EESCHEMA, ZOOM_MIN_LIMIT_EESCHEMA );
    m_view->SetMirror( false, false );

    m_gal->SetClearColor( m_painter->GetSettings()->GetBackgroundColor() );

    // Layer order and targets, copied from SCH_DRAW_PANEL. The order matters because
    // VIEW walks m_orderedLayers when it draws, and the targets decide which layers
    // VIEW caches into retained groups.
    for( std::size_t ii = 0; ii < sizeof( SCH_LAYER_ORDER ) / sizeof( int ); ++ii )
        m_view->SetLayerOrder( SCH_LAYER_ORDER[ii], static_cast<int>( ii ) );

    // Unlike SCH_DRAW_PANEL, which only caches under OpenGL, everything cacheable is
    // cached here. Retained groups are the whole point of the recording backend: they
    // are what lets a pan re-upload nothing, and leaving them off would mean the
    // stream never exercised BeginGroup/DrawGroup at all.
    for( int ii = 0; ii < KIGFX::VIEW::VIEW_MAX_LAYERS; ++ii )
        m_view->SetLayerTarget( ii, KIGFX::TARGET_CACHED );

    m_view->SetLayerTarget( LAYER_SCHEMATIC_ANCHOR, KIGFX::TARGET_NONCACHED );
    m_view->SetLayerDisplayOnly( LAYER_SCHEMATIC_ANCHOR );

    m_view->SetLayerTarget( LAYER_DRAW_BITMAPS, KIGFX::TARGET_NONCACHED );

    m_view->SetLayerTarget( LAYER_GP_OVERLAY, KIGFX::TARGET_OVERLAY );
    m_view->SetLayerDisplayOnly( LAYER_GP_OVERLAY );

    m_view->SetLayerTarget( LAYER_SELECT_OVERLAY, KIGFX::TARGET_OVERLAY );
    m_view->SetLayerDisplayOnly( LAYER_SELECT_OVERLAY );

    m_view->SetLayerTarget( LAYER_DRAWINGSHEET, KIGFX::TARGET_NONCACHED );
    m_view->SetLayerDisplayOnly( LAYER_DRAWINGSHEET );

    m_view->SetLayerTarget( LAYER_OP_VOLTAGES, KIGFX::TARGET_OVERLAY );
    m_view->SetLayerDisplayOnly( LAYER_OP_VOLTAGES );
    m_view->SetLayerTarget( LAYER_OP_CURRENTS, KIGFX::TARGET_OVERLAY );
    m_view->SetLayerDisplayOnly( LAYER_OP_CURRENTS );

    m_view->SetLayerTarget( LAYER_SELECTION_SHADOWS, KIGFX::TARGET_OVERLAY );
    m_view->SetLayerDisplayOnly( LAYER_SELECTION_SHADOWS );

    m_view->SetLayerDisplayOnly( LAYER_NET_COLOR_HIGHLIGHT );
    m_view->SetLayerDisplayOnly( LAYER_DANGLING );

    // The on-canvas UI layers, which SCH_DRAW_PANEL does not name and so leaves on its
    // default target. They carry the tools' transient previews — CONSTRUCTION_GEOM puts
    // its snap guides on LAYER_UI_START deliberately, to be drawn over the axis cross —
    // and the loop above would otherwise have made them cached, which is wrong twice: it
    // costs a retained group per preview item whether or not anything is previewing, and
    // the geometry it would retain changes on every pointer move.
    for( int ii = LAYER_UI_START; ii < LAYER_UI_END; ++ii )
    {
        m_view->SetLayerTarget( ii, KIGFX::TARGET_OVERLAY );
        m_view->SetLayerDisplayOnly( ii );
    }

    m_view->UpdateAllLayersOrder();

    initGrid();
}


void SCH_HOST::initGrid()
{
    // KIGFX::GAL's constructor sets every graphics default it has and leaves m_gridSize at
    // VECTOR2D()'s zero, because in a GUI COMMON_TOOLS::Reset() always fills it in from the
    // window settings. That tool declines a non-frame holder, so nothing here would — and a
    // zero grid is not merely "no grid": GRID_HELPER divides the cursor position by it, so
    // the first tool that snaps gets an infinity and KiROUND asserts on it. Found by
    // running a selection, which is the first thing that snaps.
    //
    // The values are the user's own, read exactly as COMMON_TOOLS::Reset() reads them, so
    // there is no second idea of what eeschema's grid is.
    EESCHEMA_SETTINGS* cfg = eeconfig();

    if( !cfg )
        return;

    GRID_SETTINGS& gridSettings = cfg->m_Window.grid;

    if( gridSettings.grids.empty() )
        gridSettings.grids = cfg->DefaultGridSizeList();

    std::vector<VECTOR2D> grids;

    for( const GRID& gridDef : gridSettings.grids )
    {
        double x = EDA_UNIT_UTILS::UI::DoubleValueFromString( schIUScale, EDA_UNITS::MM, gridDef.x );
        double y = EDA_UNIT_UTILS::UI::DoubleValueFromString( schIUScale, EDA_UNITS::MM, gridDef.y );

        grids.emplace_back( x, y );
    }

    if( grids.empty() )
        return;

    const int index = std::clamp( gridSettings.last_size_idx, 0,
                                  static_cast<int>( grids.size() ) - 1 );

    // SetGridSize clamps to at least one internal unit, so this cannot reinstate the zero.
    m_gal->SetGridSize( grids[index] );
    m_gal->SetGridVisibility( gridSettings.show );

    // Eeschema has no movable grid origin: SCH_BASE_FRAME::GetGridOrigin() returns a
    // constant zero for every schematic frame.
    m_gal->SetGridOrigin( VECTOR2D( 0, 0 ) );
}


void SCH_HOST::setupTools()
{
    m_viewControls = std::make_unique<KIGFX::HOST_VIEW_CONTROLS>( m_view.get() );

    m_ownedToolManager = std::make_unique<TOOL_MANAGER>();
    m_toolManager = m_ownedToolManager.get();

    m_ownedActions = std::make_unique<SCH_ACTIONS>();
    m_actions = m_ownedActions.get();

    m_dispatcher = std::make_unique<HOST_TOOL_DISPATCHER>( m_toolManager, m_viewControls.get() );

    // TOOLS_HOLDER::m_toolDispatcher is a TOOL_DISPATCHER*, which is a wxEvtHandler,
    // and HOST_TOOL_DISPATCHER deliberately is not one. It stays null, which is a
    // state the tree already tolerates — GetToolDispatcher() has no unguarded caller
    // outside the frames that install one.

    // No model yet: the document arrives with LoadFile(), which calls this again with
    // one. The settings are the kiface's, which is where eeconfig() reads from, and
    // ensureKifaceSettings() has already stood them up by the time buildCanvas()
    // returned.
    m_toolManager->SetEnvironment( nullptr, m_view.get(), m_viewControls.get(),
                                   Kiface().KifaceSettings(), this );

    registerTools();

    m_toolManager->InitTools();

    // TOOLS_HOLDER's constructor leaves its input preferences at defaults that every frame
    // then overwrites from the user's common settings, in CommonSettingsChanged(). Without
    // this the host would have `m_dragAction == MOUSE_DRAG_ACTION::SELECT`, so a drag over
    // a selected item would draw a rubber band instead of moving it — the setting says
    // otherwise and nothing was reading it. Same shape as the grid in ::initGrid: a default
    // that only looks harmless because the GUI never uses it.
    CommonSettingsChanged();
}


void SCH_HOST::registerTools()
{
    // The roster SCH_EDIT_FRAME::setupTools() registers, in its order.
    //
    // SCH_SELECTION_TOOL asks the holder for a SCHEMATIC_HOLDER rather than for a frame,
    // so it initialises here and runs. Every other one still learns its `m_frame` from the
    // holder and returns false when the holder is not its frame type, so InitTools()
    // unregisters and deletes it and GetTool<T>() is null.
    //
    // They are all registered anyway: converting a tool is then a change to that tool and
    // nothing here. `qa/tests/eeschema/test_sch_host.cpp` pins which ones survive, so the
    // day another is converted the test says so by failing.
    m_toolManager->RegisterTool( new COMMON_CONTROL );
    m_toolManager->RegisterTool( new COMMON_TOOLS );
    m_toolManager->RegisterTool( new ZOOM_TOOL );
    m_toolManager->RegisterTool( new SCH_SELECTION_TOOL );
    m_toolManager->RegisterTool( new PICKER_TOOL );
    m_toolManager->RegisterTool( new SCH_DRAWING_TOOLS );
    m_toolManager->RegisterTool( new EE_GRAPHIC_TOOL );
    m_toolManager->RegisterTool( new SCH_LINE_WIRE_BUS_TOOL );
    m_toolManager->RegisterTool( new SCH_MOVE_TOOL );
    m_toolManager->RegisterTool( new SCH_ALIGN_TOOL );
    m_toolManager->RegisterTool( new SCH_EDIT_TOOL );
    m_toolManager->RegisterTool( new SCH_EDIT_TABLE_TOOL );
    m_toolManager->RegisterTool( new SCH_GROUP_TOOL );
    m_toolManager->RegisterTool( new SCH_INSPECTION_TOOL );
    m_toolManager->RegisterTool( new SCH_DESIGN_BLOCK_CONTROL );
    m_toolManager->RegisterTool( new SCH_EDITOR_CONTROL );
    m_toolManager->RegisterTool( new SCH_FIND_REPLACE_TOOL );
    m_toolManager->RegisterTool( new SCH_POINT_EDITOR );
    m_toolManager->RegisterTool( new SCH_NAVIGATE_TOOL );
    m_toolManager->RegisterTool( new PROPERTIES_TOOL );
    m_toolManager->RegisterTool( new EMBED_TOOL );

    // Not part of SCH_EDIT_FRAME's roster: undo, redo and save for a holder with no frame.
    // See SCH_HOST_CONTROL for why these are a tool rather than three ABI calls.
    m_toolManager->RegisterTool( new SCH_HOST_CONTROL );
}


bool SCH_HOST::DispatchInput( const HOST_INPUT_EVENT& aEvent )
{
    // Nothing reaches a tool before there is a document. A tool asks the editing context
    // for the screen and uses the answer, as it may in a frame — SCH_EDIT_FRAME always has
    // a SCHEMATIC and an empty SCH_SCREEN, from its constructor on — and this host has
    // neither until something is loaded. Giving the host an empty document at construction
    // is the better long-term answer and is a change to what the ABI reports for an empty
    // session; see `docs/rust-migration/06-what-is-missing.md` Stage 4b.
    if( !m_schematic )
        return false;

    return m_dispatcher->Dispatch( aEvent );
}


bool SCH_HOST::RunActionByName( const std::string& aActionName )
{
    // Deliberately not TOOL_MANAGER::RunAction( const std::string& ): that overload
    // reports whether the *name resolved*, not whether anything ran. It discards
    // doRunAction()'s result and returns true for any registered action — and
    // ACTION_MANAGER's constructor registers the whole process-wide list, so every
    // one of KiCad's ~440 actions resolves here even though InitTools() deleted the
    // entire tool roster. A UI would be told that every menu item it offered had
    // been handled.
    //
    // Looking the action up and using the TOOL_ACTION& overload, whose result *is*
    // processEvent()'s, is what makes the answer mean something.
    //
    // And nothing runs before there is a document, for the reason ::DispatchInput gives.
    if( !m_schematic )
        return false;

    TOOL_ACTION* action = m_toolManager->GetActionManager()->FindAction( aActionName );

    if( !action )
        return false;

    return m_toolManager->RunAction( *action );
}


void SCH_HOST::ResetInputState()
{
    m_dispatcher->ResetState();
}


VECTOR2D SCH_HOST::GetCursorPosition() const
{
    // The one-argument overload reads the snapping setting, which is what every tool
    // gets when it asks, so it is what a consumer drawing a crosshair should show.
    return m_viewControls->VIEW_CONTROLS::GetCursorPosition();
}


SELECTION& SCH_HOST::GetCurrentSelection()
{
    // Same answer SCH_EDIT_FRAME gives: the selection belongs to the selection tool. The
    // empty fallback is still reachable — the tool only exists once InitTools() has run —
    // so it stays rather than being replaced by a dereference.
    if( SCH_SELECTION_TOOL* tool = m_toolManager->GetTool<SCH_SELECTION_TOOL>() )
        return tool->GetSelection();

    return m_dummySelection;
}


std::size_t SCH_HOST::GetSelectionCount()
{
    return GetCurrentSelection().GetSize();
}


bool SCH_HOST::TakeRedrawRequest()
{
    bool requested = m_redrawRequested;

    m_redrawRequested = false;

    return requested;
}


SCH_RENDER_SETTINGS& SCH_HOST::RenderSettings() const
{
    return *m_painter->GetSettings();
}


void SCH_HOST::AddToScreen( EDA_ITEM* aItem, SCH_SCREEN* aScreen )
{
    // Same shape as SCH_BASE_FRAME::AddToScreen, including the guard: a null item reaches
    // boost::ptr_vector and raises boost::bad_pointer, which nothing here handles.
    wxCHECK( aItem, /* void */ );

    SCH_SCREEN* screen = aScreen ? aScreen : GetScreen();

    wxCHECK( screen, /* void */ );

    if( aItem->Type() != SCH_TABLECELL_T )
        screen->Append( static_cast<SCH_ITEM*>( aItem ) );

    if( screen == GetScreen() )
    {
        m_view->Add( aItem );
        UpdateItem( aItem, true );
    }
}


void SCH_HOST::RemoveFromScreen( EDA_ITEM* aItem, SCH_SCREEN* aScreen )
{
    wxCHECK( aItem, /* void */ );

    SCH_SCREEN* screen = aScreen ? aScreen : GetScreen();

    wxCHECK( screen, /* void */ );

    if( screen == GetScreen() )
        m_view->Remove( aItem );

    if( aItem->Type() != SCH_TABLECELL_T )
        screen->Remove( static_cast<SCH_ITEM*>( aItem ) );

    if( screen == GetScreen() )
        UpdateItem( aItem, true );
}


void SCH_HOST::UpdateItem( EDA_ITEM* aItem, bool aIsAddOrDelete, bool aUpdateRtree )
{
    wxCHECK( aItem, /* void */ );

    // The rules about what redraws with what are SCH_VIEW's, shared with the frame, so
    // that the two editing contexts cannot drift.
    m_view->UpdateSchItem( aItem, GetScreen(), aIsAddOrDelete, aUpdateRtree );
}


EDA_ITEM* SCH_HOST::ResolveItem( const KIID& aId, bool aAllowNullptrReturn ) const
{
    if( !m_schematic )
        return nullptr;

    return m_schematic->ResolveItem( aId, nullptr, aAllowNullptrReturn );
}


SCH_SELECTION_TOOL* SCH_HOST::GetSelectionTool()
{
    return m_toolManager ? m_toolManager->GetTool<SCH_SELECTION_TOOL>() : nullptr;
}


EESCHEMA_SETTINGS* SCH_HOST::eeconfig() const
{
    // SCH_BASE_FRAME reads this from EDA_BASE_FRAME::config(), which resolves to the
    // kiface's settings; ::ensureKifaceSettings has already guaranteed there are some.
    return dynamic_cast<EESCHEMA_SETTINGS*>( Kiface().KifaceSettings() );
}


SCH_RENDER_SETTINGS* SCH_HOST::GetRenderSettings()
{
    return m_painter->GetSettings();
}


bool SCH_HOST::GetShowAllPins() const
{
    return RenderSettings().m_ShowHiddenPins;
}


void SCH_HOST::OnModify()
{
    // A frame additionally requests an autosave and retitles its window. Neither exists
    // here; the screen's own modified flag is what IsModified() and the ABI report, and
    // SCH_COMMIT has already set it.
    RefreshCanvas();
}


void SCH_HOST::SaveCopyInUndoList( const PICKED_ITEMS_LIST& aItemsList, UNDO_REDO aTypeCommand,
                                   bool aAppend )
{
    SCH_UNDO_REDO::SaveCopyInUndoList( *this, aItemsList, aTypeCommand, aAppend );
}


bool SCH_HOST::Undo()
{
    return SCH_UNDO_REDO::Undo( *this );
}


bool SCH_HOST::Redo()
{
    return SCH_UNDO_REDO::Redo( *this );
}


bool SCH_HOST::Save()
{
    if( !m_schematic )
    {
        m_lastError = wxT( "No document to save." );
        return false;
    }

    // One entry per *screen* rather than per sheet path, because a sheet used twice in the
    // hierarchy shares one screen and one file. SCH_EDIT_FRAME::SaveProject iterates the
    // same way, for the same reason.
    SCH_SCREENS screens( m_schematic->Root() );

    // The writer throws IO_ERROR on a path it cannot write. The ABI's guard() would catch
    // it, but catching it here lets the message land on the session's error string and the
    // remaining screens keep their modified flags, so a caller can fix the path and retry.
    try
    {
        // Sheet page numbers are serialised from the hierarchy, which SaveProject also
        // makes sure is current before writing.
        m_schematic->SetSheetNumberAndCount();

        // Which sheet path each screen is reachable by; the writer serialises a page
        // number per screen and needs to know whether the screen is used once or many
        // times. SaveProject does this too, for the same reason.
        screens.BuildClientSheetPathList();

        SCH_IO_KICAD_SEXPR io;
        int                written = 0;

        for( std::size_t ii = 0; ii < screens.GetCount(); ++ii )
        {
            SCH_SCREEN* screen = screens.GetScreen( ii );
            SCH_SHEET*  sheet = screens.GetSheet( ii );

            if( !screen || !sheet )
                continue;

            // A screen with no file name has nowhere to go. That is not a failure: the
            // hierarchy `SCHEMATIC::Reset()` starts from contains a placeholder top-level
            // sheet, and SaveProject skips the same case rather than inventing a path.
            if( screen->GetFileName().IsEmpty() )
                continue;

            screen->SetVirtualPageNumber( screen->GetClientSheetPaths().size() == 1 ? 1 : 0 );

            io.SaveSchematicFile( screen->GetFileName(), sheet, m_schematic );
            ++written;
        }

        if( written == 0 )
        {
            m_lastError = wxT( "No sheet of this document has a file name to save to." );
            return false;
        }
    }
    catch( const IO_ERROR& e )
    {
        m_lastError = e.What();
        return false;
    }
    catch( const std::exception& e )
    {
        m_lastError = wxString::FromUTF8( e.what() );
        return false;
    }

    for( std::size_t ii = 0; ii < screens.GetCount(); ++ii )
    {
        if( SCH_SCREEN* screen = screens.GetScreen( ii ) )
        {
            if( !screen->GetFileName().IsEmpty() )
                screen->SetContentModified( false );
        }
    }

    return true;
}


void SCH_HOST::ClearUndoORRedoList( UNDO_REDO_LIST aList, int aItemCount )
{
    // Identical to SCH_EDIT_FRAME's, which is not shared because the two classes reach
    // their stacks through different inheritance paths and the body is six lines.
    if( aItemCount == 0 )
        return;

    UNDO_REDO_CONTAINER& list = ( aList == UNDO_LIST ) ? m_undoList : m_redoList;

    if( aItemCount < 0 )
    {
        list.ClearCommandList();
        return;
    }

    for( int ii = 0; ii < aItemCount; ++ii )
    {
        if( list.m_CommandsList.empty() )
            break;

        PICKED_ITEMS_LIST* command = list.m_CommandsList.front();

        list.m_CommandsList.erase( list.m_CommandsList.begin() );

        command->ClearListAndDeleteItems( []( EDA_ITEM* aItem )
                                          {
                                              delete aItem;
                                          } );
        delete command;
    }
}


bool SCH_HOST::RecalculateConnections( SCH_COMMIT* aCommit, SCH_CLEANUP_FLAGS aCleanupFlags,
                                       PROGRESS_REPORTER* aProgressReporter, bool aCleanupDone )
{
    if( !m_schematic )
        return true;

    // The change handler is why this is not simply SCHEMATIC::RecalculateConnections with
    // nulls: an item whose drawing changed has to be dropped from the view, or its retained
    // group is replayed with the geometry it had before the rebuild.
    std::function<void( SCH_ITEM* )> changeHandler =
            [this]( SCH_ITEM* aChangedItem ) -> void
            {
                m_view->Update( aChangedItem, KIGFX::REPAINT );
            };

    PICKED_ITEMS_LIST* lastUndo = m_undoList.m_CommandsList.empty()
                                          ? nullptr
                                          : m_undoList.m_CommandsList.back();

    try
    {
        m_schematic->RecalculateConnections( aCommit, aCleanupFlags, m_toolManager,
                                            aProgressReporter, m_view.get(), &changeHandler,
                                            lastUndo, aCleanupDone );
    }
    catch( const std::exception& error )
    {
        if( !ADVANCED_CFG::GetCfg().m_ConnectivityEngine )
            throw;

        // The engine clears its publication before rethrowing; an edit must not unwind the
        // tool. A frame shows this in its info bar; here it is the session's last error,
        // which the ABI reports.
        m_lastError = wxString::Format( wxT( "Unable to rebuild schematic connectivity: %s" ),
                                        wxString::FromUTF8( error.what() ) );
        return false;
    }

    return true;
}


void SCH_HOST::UpdateHopOveredWires( SCH_ITEM* aItem )
{
    // SCH_EDIT_FRAME recomputes the arcs a wire draws where it crosses another. That is
    // view-only presentation of an unchanged document, and it is not done here yet: a
    // crossing simply draws as two lines. Recorded in
    // `docs/rust-migration/06-what-is-missing.md` Stage 4b rather than left silent.
}


void SCH_HOST::SaveCopyForRepeatItem( const SCH_ITEM* aItem )
{
    if( !aItem )
        return;

    m_itemsToRepeat.clear();
    AddCopyForRepeatItem( aItem );
}


void SCH_HOST::AddCopyForRepeatItem( const SCH_ITEM* aItem )
{
    if( !aItem )
        return;

    // A pointer into the document would dangle the moment the item is deleted — which a
    // line concatenation does routinely — so this owns a copy. Same reasoning, and the same
    // flag and parent clearing, as SCH_EDIT_FRAME::AddCopyForRepeatItem.
    std::unique_ptr<SCH_ITEM> copy(
            static_cast<SCH_ITEM*>( aItem->Duplicate( IGNORE_PARENT_GROUP ) ) );

    copy->ClearFlags();
    copy->SetParent( nullptr );

    m_itemsToRepeat.emplace_back( std::move( copy ) );
}


bool SCH_HOST::LoadFile( const wxString& aFileName )
{
    Unload();
    m_lastError.Clear();

    wxFileName fn( aFileName );

    if( !fn.FileExists() )
    {
        m_lastError = wxString::Format( wxT( "Schematic file '%s' does not exist." ), aFileName );
        return false;
    }

    // A schematic with no sibling .kicad_pro falls back to SETTINGS_MANAGER::Prj(), and with
    // no project loaded at all that returns a static PROJECT whose PROJECT_FILE is null.
    // SCHEMATIC::Settings() dereferences that file unconditionally, so loading such a file
    // into a bare process is a null dereference rather than an error. Standing up the empty
    // project first gives the fallback something real behind it. The GUI never hits this
    // because a frame always has a project open; a headless host has to arrange it.
    SETTINGS_MANAGER& settingsManager = Pgm().GetSettingsManager();

    if( !settingsManager.IsProjectOpen() )
        settingsManager.LoadProject( wxEmptyString );

    // The reader throws IO_ERROR (and, on a corrupt file, can surface a parse error as any
    // number of exception types), so nothing below may escape into a C caller.
    try
    {
        m_schematic = EESCHEMA_HELPERS::LoadSchematic( fn.GetFullPath(), /* aSetActive */ false,
                                                       /* aForceDefaultProject */ false );
    }
    catch( const IO_ERROR& e )
    {
        m_schematic = nullptr;
        m_lastError = e.What();
        return false;
    }
    catch( const std::exception& e )
    {
        m_schematic = nullptr;
        m_lastError = wxString::FromUTF8( e.what() );
        return false;
    }
    catch( ... )
    {
        m_schematic = nullptr;
        m_lastError = wxT( "Unknown error while loading the schematic." );
        return false;
    }

    if( !m_schematic )
    {
        m_lastError = wxString::Format( wxT( "Failed to load schematic '%s'." ), aFileName );
        return false;
    }

    initRenderSettings();
    rebuildSheetList();

    if( m_sheets.empty() )
    {
        // A document with no sheets at all is malformed rather than merely empty:
        // BuildSheetListSortedByPageNumbers() always yields at least the root.
        Unload();
        m_lastError = wxString::Format( wxT( "Schematic '%s' contains no sheets." ), aFileName );
        return false;
    }

    // What SCH_EDIT_FRAME's constructor does with the same call: the schematic reaches
    // back through this for the handful of things only the editing context knows — the
    // selection tool, adding and removing items from the screen, intersheet references.
    m_schematic->SetSchematicHolder( this );

    m_currentSheetIndex = 0;
    displayCurrentSheet();
    ZoomToFit();

    // The tools' model. SCH_EDIT_FRAME can hand it over once, in setupTools(),
    // because its SCHEMATIC exists for the frame's whole life; here the document
    // arrives now, so the environment is re-stated and the tools are told to reload.
    m_toolManager->SetEnvironment( m_schematic, m_view.get(), m_viewControls.get(),
                                   Kiface().KifaceSettings(), this );
    m_toolManager->ResetTools( TOOL_BASE::MODEL_RELOAD );

    // "Run the selection tool, it is supposed to be always active", as
    // SCH_EDIT_FRAME::setupTools() puts it. It posts the action because it is still
    // building a frame; here the document has just arrived and the tools have been reset,
    // so the action can simply run. Without this the tool is registered and initialised
    // but its Main() loop is not waiting on anything, and no click reaches it.
    if( m_toolManager->GetTool<SCH_SELECTION_TOOL>() )
        m_toolManager->RunAction( ACTIONS::selectionActivate );

    return true;
}


void SCH_HOST::Unload()
{
    // The tools must stop referring to the document before it goes. Restating the
    // environment with a null model is what SCH_EDIT_FRAME's equivalent does not need
    // to do, because its schematic outlives its tools.
    if( m_ownedToolManager )
    {
        m_ownedToolManager->SetEnvironment( nullptr, m_view.get(), m_viewControls.get(),
                                           Kiface().KifaceSettings(), this );
        m_ownedToolManager->ResetTools( TOOL_BASE::MODEL_RELOAD );
    }

    // SCH_VIEW keeps a drawing-sheet proxy and an invalidation listener registered on the
    // schematic's text-variable tracker. Both have to go before the schematic does.
    if( m_view )
    {
        m_view->Cleanup();
        m_view->DetachTextVarTracker();
    }

    if( m_gal )
        m_gal->ClearCache();

    m_currentSheet.clear();
    m_currentSheetIndex = 0;
    m_sheets.clear();

    if( m_schematic )
    {
        // The project outlives the schematic and holds a back-reference; detaching first
        // is what EESCHEMA_JOBS_HANDLER::ClearCachedSchematic() does for the same reason.
        m_schematic->SetProject( nullptr );
        delete m_schematic;
        m_schematic = nullptr;
    }
}


void SCH_HOST::initRenderSettings()
{
    SCH_RENDER_SETTINGS& settings = RenderSettings();

    settings.LoadColors( ::GetColorSettings( DEFAULT_THEME ) );

    // Match what the CLI exporters show, so a recorded stream and a `kicad-cli` SVG of the
    // same sheet are comparable. See EESCHEMA_JOBS_HANDLER::InitRenderSettings().
    settings.m_ShowHiddenPins = false;
    settings.m_ShowHiddenFields = false;
    settings.m_ShowPinAltIcons = false;

    settings.SetDefaultPenWidth( m_schematic->Settings().m_DefaultLineWidth );
    settings.m_LabelSizeRatio = m_schematic->Settings().m_LabelSizeRatio;
    settings.m_TextOffsetRatio = m_schematic->Settings().m_TextOffsetRatio;
    settings.m_PinSymbolSize = m_schematic->Settings().m_PinSymbolSize;
    settings.m_ShowDNPMarkers = m_schematic->Settings().m_ShowDNPMarkers;

    settings.SetDashLengthRatio( m_schematic->Settings().m_DashedLineDashRatio );
    settings.SetGapLengthRatio( m_schematic->Settings().m_DashedLineGapRatio );

    m_gal->SetClearColor( settings.GetBackgroundColor() );

    // The drawing sheet (page border, title block) is a document-level resource loaded by
    // name. Failing to find it is not fatal — DS_DATA_MODEL falls back to the built-in
    // default sheet, which is what an empty name asks for anyway.
    wxString msg;
    DS_DATA_MODEL::GetTheInstance().LoadFromName( m_schematic->Settings().m_SchDrawingSheetFileName,
                                                  m_schematic->Project().GetProjectPath(),
                                                  &m_schematic->Project(),
                                                  { m_schematic->GetEmbeddedFiles() }, &msg );
}


void SCH_HOST::rebuildSheetList()
{
    m_sheets.clear();

    if( !m_schematic )
        return;

    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();

    m_sheets.reserve( sheets.size() );

    for( const SCH_SHEET_PATH& path : sheets )
    {
        SCH_HOST_SHEET_INFO info;

        SCH_SHEET* last = path.Last();
        info.m_Name = last ? last->GetName() : wxString();
        info.m_Path = path.PathHumanReadable();
        info.m_PageNumber = path.GetPageNumber();

        SCH_SCREEN* screen = path.LastScreen();
        info.m_ItemCount = screen ? static_cast<std::size_t>( screen->Items().size() ) : 0;

        m_sheets.push_back( info );
    }
}


bool SCH_HOST::SetCurrentSheetIndex( std::size_t aIndex )
{
    if( !m_schematic || aIndex >= m_sheets.size() )
        return false;

    if( aIndex == m_currentSheetIndex && !m_currentSheet.empty() )
        return true;

    m_currentSheetIndex = aIndex;
    displayCurrentSheet();

    return true;
}


bool SCH_HOST::DisplaySheet( const SCH_SHEET_PATH& aPath )
{
    if( !m_schematic )
        return false;

    // The same list ::displayCurrentSheet indexes into, so the index this finds is the
    // one that names aPath for the rest of the session.
    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();

    for( std::size_t i = 0; i < sheets.size(); ++i )
    {
        if( sheets[i] == aPath )
            return SetCurrentSheetIndex( i );
    }

    return false;
}


void SCH_HOST::displayCurrentSheet()
{
    if( !m_schematic )
        return;

    SCH_SHEET_LIST sheets = m_schematic->BuildSheetListSortedByPageNumbers();

    if( m_currentSheetIndex >= sheets.size() )
        return;

    m_currentSheet = sheets[m_currentSheetIndex];

    // SCH_PAINTER and the drawing-sheet proxy both resolve text variables and intersheet
    // references against the schematic's idea of the current sheet, not ours.
    m_schematic->SetCurrentSheet( m_currentSheet );
    m_currentSheet.UpdateAllScreenReferences();

    SCH_SCREEN* screen = m_currentSheet.LastScreen();

    // Changing sheets replaces every item in the view, so the retained groups recorded for
    // the previous sheet are dead. Dropping them here is what keeps the group arena from
    // growing once per sheet visited.
    m_view->Cleanup();
    m_gal->ClearCache();

    if( screen )
        m_view->DisplaySheet( screen );
}


SCH_SCREEN* SCH_HOST::GetScreen() const
{
    if( !m_schematic || m_currentSheet.empty() )
        return nullptr;

    return m_currentSheet.LastScreen();
}


std::size_t SCH_HOST::GetItemCount() const
{
    SCH_SCREEN* screen = GetScreen();

    return screen ? static_cast<std::size_t>( screen->Items().size() ) : 0;
}


BOX2I SCH_HOST::GetDocumentBBox( bool aIncludeAllVisible ) const
{
    BOX2I bbox;
    SCH_SCREEN* screen = GetScreen();

    if( !screen )
        return bbox;

    if( aIncludeAllVisible )
    {
        // The whole page, exactly as SCH_EDIT_FRAME::GetDocumentExtents() reports it.
        int sizeX = screen->GetPageSettings().GetWidthIU( schIUScale.IU_PER_MILS );
        int sizeY = screen->GetPageSettings().GetHeightIU( schIUScale.IU_PER_MILS );

        bbox = BOX2I( VECTOR2I( 0, 0 ), VECTOR2I( sizeX, sizeY ) );
    }
    else
    {
        EDA_ITEM* drawingSheet = static_cast<EDA_ITEM*>( m_view->GetDrawingSheet() );

        for( EDA_ITEM* item : screen->Items() )
        {
            if( item != drawingSheet )
                bbox.Merge( item->GetBoundingBox() );
        }
    }

    return bbox;
}


bool SCH_HOST::IsModified() const
{
    if( !m_schematic || !m_schematic->HasHierarchy() )
        return false;

    return m_schematic->Hierarchy().IsModified();
}


double SCH_HOST::PixelsPerIUAtUnitZoom() const
{
    // KIGFX::VIEW's "scale" is the GAL zoom factor, not pixels per internal unit:
    // GAL::computeWorldScale() derives the latter as
    //
    //     worldScale = screenDPI * worldUnitLength * zoomFactor * zoomCorrection
    //
    // and eeschema's worldUnitLength is 1e-7/0.0254 inch per IU (SCH_WORLD_UNIT).
    // The ABI speaks pixels per IU, because that is what the recorded coordinates
    // and a consumer's camera are in, so the two have to be converted between.
    //
    // The factor is recovered from the GAL rather than recomputed from the formula
    // above, so that the zoom-correction factor in the common settings — and
    // anything else that ends up in there later — is included without this code
    // knowing about it.
    const double zoom = m_view->GetScale();

    if( zoom > 0.0 )
        return m_gal->GetWorldScale() / zoom;

    // Unreachable: VIEW starts at a zoom of 1 and SetScale() clamps to
    // m_minScale, which is positive. The fallback is the formula without the
    // correction factor, which is the closest thing to right available without a
    // zoom to divide by, and it beats returning zero into a division.
    return m_gal->GetScreenDPI() * m_gal->GetWorldUnitLength();
}


void SCH_HOST::SetViewport( int aWidthPx, int aHeightPx, const VECTOR2D& aCenter,
                            double aPixelsPerIU )
{
    SetViewportSize( aWidthPx, aHeightPx );

    if( aPixelsPerIU > 0.0 )
    {
        // VIEW::SetScale clamps to eeschema's own zoom limits, so a caller asking
        // for more than the editor allows gets what the editor allows — which
        // GetViewScale() then reports back, rather than the request.
        m_view->SetScale( aPixelsPerIU / PixelsPerIUAtUnitZoom() );
    }

    // After the scale: VIEW::SetScale keeps its anchor fixed and therefore moves
    // the centre, so setting the centre first would undo it.
    m_view->SetCenter( aCenter );
}


void SCH_HOST::SetViewportSize( int aWidthPx, int aHeightPx )
{
    // A zero-area viewport makes VIEW::Redraw() compute an empty world rectangle and the
    // frame comes out silently blank, so clamp rather than propagate the mistake.
    m_viewportSize.x = std::max( 1, aWidthPx );
    m_viewportSize.y = std::max( 1, aHeightPx );

    m_gal->ResizeScreen( m_viewportSize.x, m_viewportSize.y );
    m_view->MarkDirty();
}


void SCH_HOST::ZoomToFit()
{
    BOX2I bbox = GetDocumentBBox( true );

    if( !m_schematic || bbox.GetWidth() <= 0 || bbox.GetHeight() <= 0 )
        return;

    // Pixels per internal unit, which is not what VIEW::SetScale wants; see
    // PixelsPerIUAtUnitZoom().
    double scaleX = static_cast<double>( m_viewportSize.x ) / bbox.GetWidth();
    double scaleY = static_cast<double>( m_viewportSize.y ) / bbox.GetHeight();

    m_view->SetScale( std::min( scaleX, scaleY ) / ZOOM_FIT_MARGIN / PixelsPerIUAtUnitZoom() );
    m_view->SetCenter( VECTOR2D( bbox.Centre() ) );
}


VECTOR2D SCH_HOST::GetViewCenter() const
{
    return m_view->GetCenter();
}


double SCH_HOST::GetViewScale() const
{
    // Pixels per internal unit, matching what SetViewport() takes. That is exactly
    // the GAL's world scale, which is where the zoom factor ends up.
    return m_gal->GetWorldScale();
}


kgds_stream_view SCH_HOST::Render()
{
    if( !m_schematic )
        return m_gal->Publish();

    SCH_RENDER_SETTINGS& settings = RenderSettings();

    // Push the geometry cache up to date first. This is where VIEW opens and closes the
    // retained groups the frame then refers to, so it has to happen outside the frame
    // bracket — exactly as EDA_DRAW_PANEL_GAL::DoRePaint sequences it.
    m_view->UpdateItems();

    // DoRePaint early-outs on a clean view. We do not: the caller asked for a frame, and
    // damage tracking belongs to the renderer on the other side of the boundary.
    m_view->MarkDirty();

    m_gal->BeginDrawing();

    m_gal->SetClearColor( settings.GetBackgroundColor() );
    m_gal->SetGridColor( settings.GetGridColor() );
    m_gal->SetCursorColor( settings.GetCursorColor() );

    m_gal->ClearScreen();
    m_view->ClearTargets();
    m_view->Redraw();

    m_gal->EndDrawing();

    return m_gal->Publish();
}


kgds_stream_view SCH_HOST::PublishLastFrame() const
{
    return m_gal->Publish();
}
