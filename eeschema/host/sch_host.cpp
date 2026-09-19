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
#include <settings/color_settings.h>
#include <settings/settings_manager.h>
#include <sch_painter.h>
#include <sch_render_settings.h>
#include <sch_screen.h>
#include <sch_sheet.h>
#include <sch_view.h>
#include <schematic.h>
#include <tool/action_manager.h>
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


/// Fraction of the viewport left as margin by ZoomToFit(), matching the feel of
/// the editor's own zoom-to-fit rather than butting the drawing against the edge.
static constexpr double ZOOM_FIT_MARGIN = 1.05;


SCH_HOST::SCH_HOST() :
        m_schematic( nullptr ),
        m_currentSheetIndex( 0 ),
        m_viewportSize( 1920, 1080 ),
        m_redrawRequested( false )
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

    m_view->UpdateAllLayersOrder();
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
}


void SCH_HOST::registerTools()
{
    // The roster SCH_EDIT_FRAME::setupTools() registers, in its order.
    //
    // Every one of these declines a holder that is not a SCH_BASE_FRAME (or, for the
    // ones in common/, an EDA_DRAW_FRAME), so InitTools() unregisters and deletes all
    // of them and GetTool<T>() is null for each. That is deliberate and it is tested:
    // `qa/tests/eeschema/test_sch_host.cpp` asserts the roster is registered and that
    // none of it survives, so the day a tool is taught to run without a frame it
    // starts working here with no further wiring, and the test says so by failing.
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
}


bool SCH_HOST::DispatchInput( const HOST_INPUT_EVENT& aEvent )
{
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
    // Same answer SCH_EDIT_FRAME gives: the selection belongs to the selection tool.
    // Null until a selection tool can run on a non-frame holder, and then this starts
    // reporting real items with no change here.
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

    m_currentSheetIndex = 0;
    displayCurrentSheet();
    ZoomToFit();

    // The tools' model. SCH_EDIT_FRAME can hand it over once, in setupTools(),
    // because its SCHEMATIC exists for the frame's whole life; here the document
    // arrives now, so the environment is re-stated and the tools are told to reload.
    m_toolManager->SetEnvironment( m_schematic, m_view.get(), m_viewControls.get(),
                                   Kiface().KifaceSettings(), this );
    m_toolManager->ResetTools( TOOL_BASE::MODEL_RELOAD );

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
