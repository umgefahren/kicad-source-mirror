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

#ifndef KICAD_EESCHEMA_HOST_SCH_HOST_H
#define KICAD_EESCHEMA_HOST_SCH_HOST_H

#include <cstddef>
#include <eda_search_data.h>
#include <memory>
#include <utility>
#include <vector>

#include <gal/cursors.h>
#include <gal/gal_display_options.h>
#include <gal/recording/draw_stream_abi.h>
#include <gal/recording/recording_gal.h>
#include <math/box2.h>
#include <math/vector2d.h>
#include <sch_sheet_path.h>
#include <schematic_holder.h>
#include <tool/canvas_holder.h>
#include <tool/tools_holder.h>
#include <undo_redo_holder.h>
#include <units_provider.h>
#include <wx/string.h>

class ACTIONS;
class HOST_TOOL_DISPATCHER;
class GRID_HELPER;
class SCHEMATIC;
class SCH_ITEM;
class SCH_SCREEN;
class SCH_RENDER_SETTINGS;
class TOOL_MANAGER;

struct HOST_INPUT_EVENT;

namespace KIGFX
{
class HOST_VIEW_CONTROLS;
class SCH_PAINTER;
class SCH_VIEW;
} // namespace KIGFX


/**
 * One sheet of the loaded hierarchy, flattened for a UI that has no access to
 * SCH_SHEET_PATH.
 */
struct SCH_HOST_SHEET_INFO
{
    wxString    m_Name;       ///< The sheet's own name, or "Root" for the root sheet.
    wxString    m_Path;       ///< Human-readable hierarchical path, e.g. "/power/regulators".
    wxString    m_PageNumber; ///< As shown in the sheet list; not necessarily numeric.
    std::size_t m_ItemCount;  ///< Items on this sheet's screen.
};


/**
 * A schematic editor session with no wxFrame.
 *
 * SCH_EDIT_FRAME owns the document, the view, the canvas, the tool framework,
 * the undo stack and roughly two hundred pieces of windowing. This class owns
 * only the first two of those, plus the render settings and the current sheet
 * path, and it renders through RECORDING_GAL rather than to a screen. What
 * comes out is a draw stream a renderer on the other side of the FFI boundary
 * turns into pixels.
 *
 * It is deliberately **not** a refactor of SCH_EDIT_FRAME. The survey
 * (`docs/rust-migration/00-architecture-survey.md` §7) measured that roughly
 * 470 of the ~600 `m_frame->` call sites in `eeschema/tools/` are plain
 * model or settings access with no wx involvement, and rerouting those is a
 * large mechanical change that belongs in its own commit. SCH_HOST is a new,
 * parallel, minimal owner that proves the load -> VIEW -> SCH_PAINTER ->
 * RECORDING_GAL -> stream path works end to end before any of that is
 * attempted.
 *
 * ## The tool framework
 *
 * SCH_HOST *is* a TOOLS_HOLDER and owns a TOOL_MANAGER, a HOST_VIEW_CONTROLS and
 * a HOST_TOOL_DISPATCHER, so host input becomes `TOOL_EVENT`s and reaches the
 * framework. `GetToolCanvas()` returning nullptr is not the obstacle it looks
 * like: it is already a production state, which SIMULATOR_FRAME and
 * MERGETOOL_FRAME both rely on.
 *
 * It is *also* a SCHEMATIC_HOLDER, which is what makes a tool able to run here at
 * all: that interface is the document, the settings and the canvas notifications a
 * schematic tool needs, and `SCH_BASE_FRAME` is only one of the two things that can
 * supply them. `SCH_SELECTION_TOOL` asks for a SCHEMATIC_HOLDER rather than a frame
 * and therefore initialises on this holder and runs.
 *
 * The rest of the roster still declines, because each one still learns its `m_frame`
 * from the holder and returns false when the holder is not its frame type — see
 * `docs/rust-migration/06-what-is-missing.md` Stage 4b for which tool is next and
 * what it needs. ::registerTools registers all of them regardless, so a tool
 * converted upstream of here starts working with no wiring changes.
 *
 * It is also a CANVAS_HOLDER, which is the same idea one level up: the view, the grid
 * and the units are what `COMMON_TOOLS` and `ZOOM_TOOL` need in order to pan, zoom, pick
 * a grid and switch units, and those two tools are registered by every KiCad program and
 * so could not be given an eeschema-shaped seam. Everything on that interface that is
 * inherently a window — the status bar, the grid picker, the canvas widget — is left at
 * the base class's "do nothing"/null, which is the honest answer here.
 *
 * It is a UNITS_PROVIDER for the same reason: ::GetUnitsProvider has to hand one out, and
 * the session is the thing that knows which units the user is working in.
 *
 * It is finally an UNDO_REDO_HOLDER, so an edit made here is recorded and can be
 * undone. The stacks used to be members of `EDA_BASE_FRAME`; the schematic-specific
 * half of undo is `SCH_UNDO_REDO`, which a frame and this host both run.
 *
 * ## What is still deliberately absent
 *
 * No dialogs, so nothing that is only reachable through one — page settings being
 * the case undo itself has to skip.
 *
 * ## Threading
 *
 * Not thread safe, and neither is anything it owns. One session per thread.
 */
class LIBRARY_MANAGER;

class SCH_HOST : public TOOLS_HOLDER, public SCHEMATIC_HOLDER, public CANVAS_HOLDER,
                 public UNITS_PROVIDER, public UNDO_REDO_HOLDER
{
public:
    SCH_HOST();
    ~SCH_HOST() override;

    SCH_HOST( const SCH_HOST& ) = delete;
    SCH_HOST& operator=( const SCH_HOST& ) = delete;

    /**
     * Load a schematic through the existing SCH_IO_KICAD_SEXPR reader.
     *
     * This goes through EESCHEMA_HELPERS::LoadSchematic, the same entry point
     * `kicad-cli` uses, so instance-data migration, symbol-link resolution,
     * page numbering and connectivity all happen exactly as they do for any
     * other headless consumer. No file format code is duplicated here.
     *
     * On success the root sheet becomes the current sheet and its items are
     * added to the view. On failure the session is left empty and
     * GetLastError() describes why.
     *
     * @param aFileName absolute or relative path to a `.kicad_sch` file.
     * @return true if the document loaded.
     */
    bool LoadFile( const wxString& aFileName );

    /// Drop the loaded document, leaving the session reusable.
    void Unload();

    EDA_SEARCH_DATA* GetHostSearchData() override
    { return m_searchActive ? m_searchData.get() : nullptr; }
    void SetSearchData( const SCH_SEARCH_DATA& aData, bool aActive );

    /// Run the native ERC engine and rebuild marker drawing safely.
    void RunERC( LIBRARY_MANAGER* aLibraries = nullptr );

    bool IsLoaded() const { return m_schematic != nullptr; }

    /// Why the last call that could fail did. Empty when nothing has failed.
    const wxString& GetLastError() const { return m_lastError; }

    // ------------------------------------------------------------- document

    SCHEMATIC*  GetSchematic() const override { return m_schematic; }
    SCH_SCREEN* GetScreen() const override;

    const SCH_SHEET_PATH& GetCurrentSheet() const { return m_currentSheet; }

    /// Every sheet in the hierarchy, ordered by page number.
    const std::vector<SCH_HOST_SHEET_INFO>& GetSheetHierarchy() const { return m_sheets; }

    /**
     * Switch the current sheet, repopulating the view from the new screen.
     *
     * @param aIndex an index into GetSheetHierarchy().
     * @return false if the index is out of range, in which case nothing changes.
     */
    bool SetCurrentSheetIndex( std::size_t aIndex );

    /**
     * @copydoc SCHEMATIC_HOLDER::DisplaySheet
     *
     * The path is resolved to a position in ::BuildSheetListSortedByPageNumbers, which is
     * the order this host indexes sheets in and the order a UI pages through them, and
     * then ::SetCurrentSheetIndex does the work. A path this schematic does not contain
     * is declined rather than clamped.
     */
    bool DisplaySheet( const SCH_SHEET_PATH& aPath ) override;

    std::size_t GetCurrentSheetIndex() const { return m_currentSheetIndex; }

    /// Number of items on the current sheet's screen.
    std::size_t GetItemCount() const;

    /**
     * Bounding box of the current sheet, in internal units.
     *
     * @param aIncludeAllVisible true returns the whole page rectangle, as
     *        SCH_EDIT_FRAME::GetDocumentExtents() does, so that a zoom-to-fit
     *        frames the drawing sheet. False returns the union of the items'
     *        bounding boxes, excluding the drawing sheet itself.
     */
    BOX2I GetDocumentBBox( bool aIncludeAllVisible = true ) const;

    /// True if any screen in the hierarchy has unsaved changes.
    bool IsModified() const;

    // ------------------------------------------------------------- viewport

    /**
     * Set the camera.
     *
     * The scale is in pixels per internal unit, which is what the recorded
     * coordinates and a consumer's own camera are in — and is *not* what
     * KIGFX::VIEW means by a scale. See PixelsPerIUAtUnitZoom().
     *
     * @param aWidthPx     viewport width in pixels; must be positive.
     * @param aHeightPx    viewport height in pixels; must be positive.
     * @param aCenter      the world point at the centre of the viewport.
     * @param aPixelsPerIU pixels per internal unit; must be positive. Clamped to
     *                     eeschema's zoom limits, so what GetViewScale() reports
     *                     afterwards is not necessarily what was asked for.
     */
    void SetViewport( int aWidthPx, int aHeightPx, const VECTOR2D& aCenter,
                      double aPixelsPerIU );

    /// Resize the viewport, keeping the current centre and scale.
    void SetViewportSize( int aWidthPx, int aHeightPx );

    /**
     * Frame the whole document in the current viewport, with a small margin.
     *
     * Does nothing if no document is loaded or the viewport has no area.
     */
    void ZoomToFit();

    VECTOR2D GetViewCenter() const;

    /// Pixels per internal unit, matching what SetViewport() takes.
    double GetViewScale() const;

    /**
     * Pixels per internal unit at a GAL zoom factor of one.
     *
     * The conversion between the ABI's scale and KIGFX::VIEW's, which are not the
     * same quantity: VIEW's is the GAL zoom factor, and pixels per internal unit
     * additionally involves the screen DPI, eeschema's world unit length and the
     * user's zoom-correction factor. Read out of the GAL rather than recomputed,
     * so that it cannot drift from the GAL's own definition.
     */
    double PixelsPerIUAtUnitZoom() const;

    VECTOR2I GetViewportSize() const { return m_viewportSize; }

    // --------------------------------------------------------------- render

    /**
     * Record one frame and return a borrowed view of the stream.
     *
     * Mirrors EDA_DRAW_PANEL_GAL::DoRePaint: update the item geometry cache,
     * bracket the frame, clear, then walk the layers. Unlike DoRePaint this
     * always redraws rather than early-outing on a clean view, because the
     * caller asked for a frame and a Rust renderer does its own damage
     * tracking.
     *
     * The returned pointers belong to this session and stay valid until the
     * next call that mutates the stream — another Render(), a sheet change, or
     * destruction. Nothing is copied.
     */
    kgds_stream_view Render();

    /// The stream recorded by the last Render(), without recording a new one.
    kgds_stream_view PublishLastFrame() const;

    // ----------------------------------------------------------------- input

    /**
     * Translate one host input event and give the result to the tool framework.
     *
     * @return true if a tool or a hotkey claimed it. With no tool able to run on a
     *         non-frame holder (see the class comment) that means a hotkey, or
     *         nothing.
     */
    bool DispatchInput( const HOST_INPUT_EVENT& aEvent );

    /**
     * Run a registered action by its dotted name, as a menu or a toolbar does.
     *
     * @param aFromChrome suppress immediate cursor placement for toolbar/menu activation.
     * @return false if no action has that name, or if the action was not handled.
     */
    bool RunActionByName( const std::string& aActionName, bool aFromChrome = false );

    /// Forget which buttons are down, e.g. because the host lost focus.
    void ResetInputState();

    /**
     * The cursor, in internal units, as the tools see it: grid-snapped if snapping
     * is on, or wherever a tool has forced it to be.
     */
    VECTOR2D GetCursorPosition() const;

    /// Number of items in the current selection.
    std::size_t GetSelectionCount();

    /// The most recent status text a tool asked to display. Empty if none has.
    const wxString& GetToolMessage() const { return m_toolMessage; }

    /**
     * Whether anything asked for a repaint since this was last called; reading it
     * clears it.
     *
     * `TOOLS_HOLDER::RefreshCanvas()` is how a tool says "the view changed", and it
     * is the only notice a consumer on the far side of the ABI gets that the frame
     * it is holding is stale.
     */
    bool TakeRedrawRequest();

    // ------------------------------------------------------ TOOLS_HOLDER

    /**
     * No canvas, and that is a supported answer rather than a gap: three
     * implementations in the tree already return nullptr, and `TOOL_DISPATCHER`
     * null-checks the result before using it.
     */
    wxWindow* GetToolCanvas() const override { return nullptr; }

    SELECTION& GetCurrentSelection() override;

    void RefreshCanvas() override { m_redrawRequested = true; }

    void DisplayToolMsg( const wxString& aMsg ) override { m_toolMessage = aMsg; }

    wxString ConfigBaseName() override { return wxT( "SchHost" ); }

    // ---------------------------------------------------- SCHEMATIC_HOLDER

    void AddToScreen( EDA_ITEM* aItem, SCH_SCREEN* aScreen = nullptr ) override;
    void RemoveFromScreen( EDA_ITEM* aItem, SCH_SCREEN* aScreen ) override;

    void UpdateItem( EDA_ITEM* aItem, bool aIsAddOrDelete = false,
                     bool aUpdateRtree = false ) override;

    EDA_ITEM* ResolveItem( const KIID& aId, bool aAllowNullptrReturn = false ) const override;

    SCH_SELECTION_TOOL* GetSelectionTool() override;

    /// The schematic editing context, as opposed to a symbol one.
    bool IsSchematicEditor() const override { return true; }

    void OnModify() override;

    void SaveCopyInUndoList( const PICKED_ITEMS_LIST& aItemsList, UNDO_REDO aTypeCommand,
                             bool aAppend ) override;

    /**
     * Rebuild the connection graph, giving it the view so that a changed item's retained
     * geometry is dropped.
     *
     * `SCH_EDIT_FRAME`'s version additionally tracks which net is highlighted, for a pane
     * this host does not have. What it does *not* do differently is the rebuild itself,
     * which is `SCHEMATIC`'s.
     */
    bool RecalculateConnections( SCH_COMMIT* aCommit, SCH_CLEANUP_FLAGS aCleanupFlags,
                                 PROGRESS_REPORTER* aProgressReporter = nullptr,
                                 bool aCleanupDone = false ) override;

    void UpdateHopOveredWires( SCH_ITEM* aItem ) override;

    const std::vector<std::unique_ptr<SCH_ITEM>>& GetRepeatItems() const override
    {
        return m_itemsToRepeat;
    }

    void SaveCopyForRepeatItem( const SCH_ITEM* aItem ) override;
    void AddCopyForRepeatItem( const SCH_ITEM* aItem ) override;
    void ClearRepeatItemsList() override { m_itemsToRepeat.clear(); }

    /**
     * Drop \a aItemCount of the oldest commands, deleting the items that are no longer on
     * a screen. Same rules as `SCH_EDIT_FRAME`'s, which is why it is spelled the same way.
     */
    void ClearUndoORRedoList( UNDO_REDO_LIST aList, int aItemCount = -1 ) override;

    /// Undo the newest command. @return false if there was nothing to undo.
    bool Undo();

    /// Redo the newest undone command. @return false if there was nothing to redo.
    bool Redo();

    /**
     * Write every sheet of the hierarchy back to the file it was loaded from.
     *
     * Deliberately narrower than `SCH_EDIT_FRAME::SaveProject`, which also writes the
     * project file, the symbol library table, a backup archive and the embedded-file cache.
     * Those are decisions about the *project*; this writes the document, through the same
     * `SCH_IO_KICAD_SEXPR` the editor writes it with, so what comes out is a file KiCad
     * reopens.
     *
     * @return false if nothing is loaded, or if the writer failed — in which case
     *         GetLastError() says why and the screens keep their modified flags.
     */
    bool Save();

    EESCHEMA_SETTINGS* eeconfig() const override;

    SCH_RENDER_SETTINGS* GetRenderSettings() override;

    /**
     * Whether an invisible pin can be selected, which has to agree with whether one is
     * *drawn*.
     *
     * SCH_EDIT_FRAME answers from the application settings; this answers from the render
     * settings, because ::initRenderSettings deliberately overrides the setting to match
     * what the CLI exporters show. Reading the config here would let the user click a pin
     * that is not on the screen.
     */
    bool GetShowAllPins() const override;

    void RequestItemProperties( SCH_ITEM* ) override { m_pendingItemProperties = true; }
    bool TakePendingItemProperties() { return std::exchange( m_pendingItemProperties, false ); }

    /**
     * Records the request instead of repainting.
     *
     * A frame repaints synchronously; this host does not own the frame clock — the
     * consumer on the far side of the ABI does — so the honest answer is the same one
     * `RefreshCanvas()` gives, and ::TakeRedrawRequest is how the consumer collects it.
     *
     * One declaration, two base virtuals: SCHEMATIC_HOLDER and CANVAS_HOLDER both ask
     * for this and agree on the signature, so this overrides both and neither name
     * lookup nor the vtables are ambiguous.
     */
    void ForceRefreshCanvas() override { RefreshCanvas(); }

    /**
     * SCHEMATIC_HOLDER's spelling of ::SetCanvasCursor.
     *
     * CANVAS_HOLDER renames the cursor setter — `EDA_DRAW_FRAME::SetCurrentCursor` is
     * not virtual, so a second base declaring that name would make every existing
     * `m_frame->SetCurrentCursor()` ambiguous — and the two interfaces then both want
     * the same thing of this host. Forwarding rather than assigning twice keeps one
     * idea of what the cursor is.
     */
    void SetCurrentCursor( KICURSOR aCursor ) override { SetCanvasCursor( aCursor ); }

    /// The shape the tools last asked the pointer to take.
    KICURSOR GetCurrentCursor() const { return m_cursor; }

    // ------------------------------------------------------- CANVAS_HOLDER

    /**
     * @copydoc CANVAS_HOLDER::GetDocumentExtents
     *
     * ::GetDocumentBBox is the host's own spelling of exactly this, written against
     * `SCH_EDIT_FRAME::GetDocumentExtents()`, so there is one traversal and not two.
     */
    const BOX2I GetDocumentExtents( bool aIncludeAllVisible = true ) const override
    {
        return GetDocumentBBox( aIncludeAllVisible );
    }

    /**
     * The page rectangle, which is what a frame's canvas would answer, and an empty
     * box when nothing is loaded — there is then no page and no honest default.
     */
    BOX2I GetDefaultViewBBox() const override { return GetDocumentBBox( true ); }

    /**
     * The kiface's settings, which ::ensureKifaceSettings has already stood up.
     *
     * The same object ::eeconfig reads from — that one is this downcast to eeschema's
     * type, exactly as `SCH_BASE_FRAME::eeconfig()` is of `EDA_BASE_FRAME::config()`.
     */
    APP_SETTINGS_BASE* config() const override;

    /**
     * Eeschema keeps its zoom and grid preferences in the application settings' own
     * window block, as `EDA_BASE_FRAME` does; ::initGrid reads that same block.
     */
    WINDOW_SETTINGS* GetWindowSettings( APP_SETTINGS_BASE* aCfg ) override;

    KIGFX::GAL_DISPLAY_OPTIONS& GetGalDisplayOptions() override { return m_displayOptions; }

    /**
     * Always the origin: eeschema has no movable grid origin, which is what every
     * schematic frame answers too (`SCH_BASE_FRAME::GetGridOrigin()`).
     */
    const VECTOR2I& GetGridOrigin() const override;
    void            SetGridOrigin( const VECTOR2I& aPosition ) override {}

    bool GridVisible() const override;
    void SetGridVisibility( bool aVisible ) override;

    bool GridOverridden() const override;
    void SetGridOverrides( bool aOverride ) override;

    /// Eeschema's snapping rules, the ones `SCH_EDIT_FRAME::MakeGridHelper()` builds.
    std::unique_ptr<GRID_HELPER> MakeGridHelper() override;

    /// This session is its own units provider; see the class comment.
    UNITS_PROVIDER* GetUnitsProvider() override { return this; }

    /**
     * A frame additionally refreshes what it has drawn in the old units and tells its
     * children the units changed. Nothing here displays a formatted value, so the
     * units are simply what they now are.
     */
    void SwitchUserUnits( EDA_UNITS aUnits ) override { SetUserUnits( aUnits ); }

    /**
     * Records the cursor a tool asked for, for a consumer that owns the real pointer.
     *
     * `HOST_VIEW_CONTROLS` already covers *where* the cursor is; this is what shape it
     * should be, which is the tools' way of saying what a click would do here.
     */
    void SetCanvasCursor( KICURSOR aCursor ) override { m_cursor = aCursor; }

    /**
     * Records the redraw request, for the same reason ::ForceRefreshCanvas does.
     *
     * A frame rebuilds its GAL and repaints; there is no GAL to rebuild here — the
     * recording one is not a device and cannot be lost — so what is left of "redraw
     * everything" is telling the consumer its last frame is stale.
     */
    void HardRedraw() override { RefreshCanvas(); }

    // ---------------------------------------------------------- collaborators

    KIGFX::RECORDING_GAL&      Gal() { return *m_gal; }
    KIGFX::SCH_VIEW&           View() { return *m_view; }
    SCH_RENDER_SETTINGS&       RenderSettings() const;
    KIGFX::HOST_VIEW_CONTROLS& ViewControls() { return *m_viewControls; }

private:
    bool m_pendingItemProperties = false;
    std::unique_ptr<SCH_SEARCH_DATA> m_searchData = std::make_unique<SCH_SEARCH_DATA>();
    bool m_searchActive = false;

    /// Build the tool framework: view controls, manager, actions, dispatcher.
    /// Called once, from the constructor, after buildCanvas().
    void setupTools();

    /// Register the tool roster SCH_EDIT_FRAME registers. See the class comment for
    /// why none of it survives InitTools() yet.
    void registerTools();
    /**
     * Make sure Kiface().KifaceSettings() is live before anything draws.
     *
     * SCH_PAINTER dereferences it without a null check, and only the eeschema
     * kiface module normally installs it.
     */
    static void ensureKifaceSettings();

    /// Build the GAL/view/painter quartet. Called once, from the constructor.
    void buildCanvas();

    /**
     * Give the GAL a grid, which it does not have by default and which the tools divide by.
     *
     * Read from the user's eeschema settings exactly as COMMON_TOOLS::Reset() reads them —
     * that tool would do this in a frame, and declines here.
     */
    void initGrid();

    /**
     * ::GetWindowSettings of ::config, for the accessors that are const.
     *
     * CANVAS_HOLDER's grid queries are const and `GetWindowSettings` is not — it cannot
     * be, because `EDA_BASE_FRAME`'s is not — so the const ones come through here.
     *
     * @return null where there are no settings at all.
     */
    const WINDOW_SETTINGS* windowSettings() const;

    /// Re-read the sheet list into m_sheets after a load or a sheet change.
    void rebuildSheetList();

    /// Point the view at the current sheet's screen.
    void displayCurrentSheet();

    /// Copy the loaded document's settings into the render settings.
    void initRenderSettings();

    /**
     * The document. Owned, but held as a raw pointer because
     * EESCHEMA_HELPERS::LoadSchematic hands back a raw one and teardown has to
     * detach the project before the delete.
     */
    SCHEMATIC* m_schematic;

    SCH_SHEET_PATH m_currentSheet;
    std::size_t    m_currentSheetIndex;

    std::vector<SCH_HOST_SHEET_INFO> m_sheets;

    /**
     * Declared after m_schematic so that destruction tears the view down
     * first: SCH_VIEW holds a listener on the schematic's text-variable
     * tracker and must detach before the schematic is freed.
     */
    KIGFX::GAL_DISPLAY_OPTIONS            m_displayOptions;
    std::unique_ptr<KIGFX::RECORDING_GAL> m_gal;
    std::unique_ptr<KIGFX::SCH_VIEW>      m_view;
    std::unique_ptr<KIGFX::SCH_PAINTER>   m_painter;

    /**
     * The tool framework.
     *
     * TOOLS_HOLDER holds `m_toolManager` and `m_actions` as raw pointers it does not
     * own — every frame in KiCad deletes its own — so the lifetime lives here, and
     * the base's pointers are aliases of these. Declared after the view because the
     * tools unlink themselves from it as they are destroyed.
     */
    std::unique_ptr<KIGFX::HOST_VIEW_CONTROLS> m_viewControls;
    std::unique_ptr<TOOL_MANAGER>              m_ownedToolManager;
    std::unique_ptr<ACTIONS>                   m_ownedActions;
    std::unique_ptr<HOST_TOOL_DISPATCHER>      m_dispatcher;

    VECTOR2I m_viewportSize;

    wxString m_lastError;

    /// The last thing a tool asked to be shown in a status bar.
    wxString m_toolMessage;

    /// Set by RefreshCanvas(), cleared by TakeRedrawRequest().
    bool m_redrawRequested;

    /// The shape a tool last asked the pointer to take. See SetCurrentCursor().
    KICURSOR m_cursor;

    /// Clones of the items the insert key would repeat. See GetRepeatItems().
    std::vector<std::unique_ptr<SCH_ITEM>> m_itemsToRepeat;
};

#endif // KICAD_EESCHEMA_HOST_SCH_HOST_H
