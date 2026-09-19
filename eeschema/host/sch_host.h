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
#include <memory>
#include <vector>

#include <gal/gal_display_options.h>
#include <gal/recording/draw_stream_abi.h>
#include <gal/recording/recording_gal.h>
#include <math/box2.h>
#include <math/vector2d.h>
#include <sch_sheet_path.h>
#include <wx/string.h>

class SCHEMATIC;
class SCH_SCREEN;
class SCH_RENDER_SETTINGS;

namespace KIGFX
{
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
 * ## What is deliberately absent
 *
 * No TOOL_MANAGER, no undo/redo, no selection, no dialogs.
 *
 * `TOOLS_HOLDER::GetToolCanvas() -> wxWindow*` being pure virtual is not the
 * obstacle it looks like: returning nullptr from it is already a production
 * state, which SIMULATOR_FRAME and MERGETOOL_FRAME both do. Installing a
 * TOOLS_HOLDER that is not a frame used to be undefined behaviour instead, and
 * no longer is — see `docs/rust-migration/06-what-is-missing.md` Stage 3. But
 * every eeschema tool now *declines* such a holder, deliberately, because
 * `m_frame` is its route to the screen, the selection, the undo stack and every
 * dialog. Registering the tool set here therefore needs a decision about what
 * `m_frame` is for a host that is not a frame; that is the next milestone and is
 * written up in `docs/rust-migration/04-host-seam.md` §6.
 *
 * ## Threading
 *
 * Not thread safe, and neither is anything it owns. One session per thread.
 */
class SCH_HOST
{
public:
    SCH_HOST();
    ~SCH_HOST();

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

    bool IsLoaded() const { return m_schematic != nullptr; }

    /// Why the last call that could fail did. Empty when nothing has failed.
    const wxString& GetLastError() const { return m_lastError; }

    // ------------------------------------------------------------- document

    SCHEMATIC*  GetSchematic() const { return m_schematic; }
    SCH_SCREEN* GetScreen() const;

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

    // ---------------------------------------------------------- collaborators

    KIGFX::RECORDING_GAL& Gal() { return *m_gal; }
    KIGFX::SCH_VIEW&      View() { return *m_view; }
    SCH_RENDER_SETTINGS&  RenderSettings() const;

private:
    /**
     * Make sure Kiface().KifaceSettings() is live before anything draws.
     *
     * SCH_PAINTER dereferences it without a null check, and only the eeschema
     * kiface module normally installs it.
     */
    static void ensureKifaceSettings();

    /// Build the GAL/view/painter quartet. Called once, from the constructor.
    void buildCanvas();

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

    VECTOR2I m_viewportSize;

    wxString m_lastError;
};

#endif // KICAD_EESCHEMA_HOST_SCH_HOST_H
