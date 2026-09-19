/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <memory>
#include <vector>

class EDA_ITEM;
class EESCHEMA_SETTINGS;
class SCHEMATIC;
class KIID;
class PICKED_ITEMS_LIST;
class PROGRESS_REPORTER;
class SCH_COMMIT;
class SCH_ITEM;
class SCH_RENDER_SETTINGS;
class SCH_SCREEN;
class SCH_SELECTION_TOOL;
class SCH_GLOBALLABEL;

struct SCH_SELECTION_FILTER_OPTIONS;

enum class KICURSOR;
enum class UNDO_REDO;
enum SCH_CLEANUP_FLAGS : int;

/**
 * What a schematic tool needs from whatever is editing the schematic.
 *
 * This started as a bridge class letting the schematic affect SCH_EDIT_FRAME without
 * passing callbacks through numerous files, with the stated long-term goal of making the
 * relationship between frame and schematic less intertwined. It is now also the seam a
 * tool reaches the document through, so that "the thing editing the schematic" and "a
 * wxFrame" can be two different objects: SCH_BASE_FRAME implements this, and so does
 * SCH_HOST, which has no window at all.
 *
 * The rule for what belongs here is what makes that possible: the document, the settings
 * that decide how selection and drawing behave, and notifications a canvas *owner* can
 * act on. Anything that is inherently a window — a dialog, an info bar, keyboard focus,
 * hypertext navigation — deliberately does not, and a tool reaching for one of those
 * downcasts to the frame it needs and does nothing when the answer is null.
 *
 * Two notes for whoever converts the next tool:
 *
 * * The tool framework already answers some of what looks like frame access.
 *   `TOOL_BASE::getView()` and `getViewControls()` come from `TOOL_MANAGER`, so
 *   `m_frame->GetCanvas()->GetView()` is not a reason to need a frame; and
 *   `TOOL_MANAGER::GetToolHolder()` answers `ToolStackIsEmpty()`, `GetDragAction()` and
 *   the rest of `TOOLS_HOLDER`.
 * * ::SetCurrentCursor and ::ForceRefreshCanvas are here rather than on `TOOLS_HOLDER`
 *   because that base class is inherited by every KiCad program and this is eeschema's
 *   change to make. pcbnew's tools reach for the same two through
 *   `m_frame->GetCanvas()`, so if a second editor needs them they belong one level up.
 */
class SCHEMATIC_HOLDER
{
public:
    virtual ~SCHEMATIC_HOLDER() = default;

    // ------------------------------------------------------------------ the document

    /**
     * Add an item to the screen (and view)
     * aScreen is the screen the item is located on, if not the current screen
     */
    virtual void AddToScreen( EDA_ITEM* aItem, SCH_SCREEN* aScreen = nullptr ) = 0;

    virtual void RemoveFromScreen( EDA_ITEM* aItem, SCH_SCREEN* aScreen ) = 0;

    /**
     * The screen being edited.
     *
     * Note for implementers that are also an EDA_DRAW_FRAME: that class declares a
     * `GetScreen()` of its own returning a `BASE_SCREEN*`, so a single declaration in the
     * derived class overrides both — the return type is covariant with each.
     */
    virtual SCH_SCREEN* GetScreen() const = 0;

    /**
     * Fetch an item by KIID, or null if this holder has no such item.
     *
     * `EDA_DRAW_FRAME` declares an identical virtual, so a frame has to declare one
     * itself to say which it means; see SCH_BASE_FRAME.
     */
    virtual EDA_ITEM* ResolveItem( const KIID& aId, bool aAllowNullptrReturn = false ) const = 0;

    /**
     * Mark an item, and whatever is drawn from it, for repaint.
     *
     * @param isAddOrDelete true when the item is arriving or leaving the view.
     * @param aUpdateRtree  re-index the item in the screen. This invalidates R-tree
     *                      iterators, so it cannot be done while iterating one.
     */
    virtual void UpdateItem( EDA_ITEM* aItem, bool isAddOrDelete = false,
                             bool aUpdateRtree = false ) = 0;

    /**
     * The document, or null where there is none.
     *
     * Null is a real answer rather than a defensive one: a symbol frame has no schematic,
     * and `SCH_HOST` has none until something is loaded.
     */
    virtual SCHEMATIC* GetSchematic() const { return nullptr; }

    virtual SCH_SELECTION_TOOL* GetSelectionTool() { return nullptr; }

    virtual void IntersheetRefUpdate( SCH_GLOBALLABEL* aItem ) {}

    /**
     * True for a holder editing a *schematic*, as opposed to a symbol.
     *
     * `SCH_COMMIT` needs the distinction: a symbol editor's edits go through a different
     * path (`pushLibEdit`, which saves whole symbols), and the symbol *viewer* edits
     * nothing at all. Both are SCHEMATIC_HOLDERs by inheritance and neither is this.
     */
    virtual bool IsSchematicEditor() const { return false; }

    // ------------------------------------------------------------------- editing

    /**
     * Must be called after a model change in order to set the "modify" flag and do other
     * editor-specific processing.
     *
     * `EDA_BASE_FRAME` declares an identical virtual, so a frame has to declare one itself
     * to say which it means; see SCH_BASE_FRAME.
     */
    virtual void OnModify() {}

    /**
     * Record a command on the undo stack, and clear the redo stack.
     *
     * The stacks themselves are #UNDO_REDO_HOLDER's, which both a frame and SCH_HOST also
     * are; this is the schematic-specific part — making the copies a CHANGED entry needs,
     * and carrying the repeat-item list along so that undo restores it too.
     *
     * @param aAppend true to add to the newest command rather than starting a new one.
     */
    virtual void SaveCopyInUndoList( const PICKED_ITEMS_LIST& aItemsList, UNDO_REDO aTypeCommand,
                                     bool aAppend )
    {}

    /**
     * Rebuild the connection graph after an edit.
     *
     * @param aCommit the commit the rebuild may add cleanup changes to, or null.
     * @param aCleanupDone true when the caller has already cleaned up the affected items.
     * @return false if the rebuild failed, which is reported to the user and recovered
     *         from rather than thrown.
     */
    virtual bool RecalculateConnections( SCH_COMMIT* aCommit, SCH_CLEANUP_FLAGS aCleanupFlags,
                                         PROGRESS_REPORTER* aProgressReporter = nullptr,
                                         bool aCleanupDone = false )
    {
        return true;
    }

    /// Recompute the hop-over arcs drawn where a wire crosses another.
    virtual void UpdateHopOveredWires( SCH_ITEM* aItem ) {}

    // ------------------------------------------------------- items to repeat

    /**
     * The items the insert key repeats.
     *
     * Model state rather than UI state — they are cloned `SCH_ITEM`s owned by the editor —
     * and undo carries them, so a restored command restores what the insert key would do.
     */
    virtual const std::vector<std::unique_ptr<SCH_ITEM>>& GetRepeatItems() const;

    /// Clone \a aItem and own that clone, replacing the current list.
    virtual void SaveCopyForRepeatItem( const SCH_ITEM* aItem ) {}

    /// Clone \a aItem and own that clone, adding to the current list.
    virtual void AddCopyForRepeatItem( const SCH_ITEM* aItem ) {}

    virtual void ClearRepeatItemsList() {}

    // ------------------------------------------------------------------- the settings

    /**
     * The eeschema application settings, or null where there are none — the symbol
     * editor answers null, and tools written against it test for that.
     */
    virtual EESCHEMA_SETTINGS* eeconfig() const = 0;

    virtual SCH_RENDER_SETTINGS* GetRenderSettings() = 0;

    /// Whether hidden pins are shown. Only some editors let this be false.
    virtual bool GetShowAllPins() const { return true; }

    /// Whether locked items can be selected and edited anyway.
    virtual bool GetOverrideLocks() const { return false; }

    // --------------------------------------------------- feedback that is not a window

    /**
     * Repaint now rather than posting a paint event.
     *
     * This is `EDA_DRAW_PANEL_GAL::ForceRefresh()`'s contract, and the synchronous half
     * of it is load bearing: a tool that changes what is on screen inside its own event
     * loop — a selection box, a drag preview — draws nothing until the loop next idles
     * otherwise. It is distinct from `TOOLS_HOLDER::RefreshCanvas()`, which posts.
     *
     * A holder that does not own the frame clock cannot repaint synchronously and
     * records the request instead, which is what SCH_HOST does.
     */
    virtual void ForceRefreshCanvas() {}

    /// Set the pointer's shape, to say what a click here would do.
    virtual void SetCurrentCursor( KICURSOR aCursor ) {}

    /**
     * Report which selection-filter categories rejected everything under the cursor, so
     * the user finds out why their click selected nothing.
     */
    virtual void HighlightSelectionFilter( const SCH_SELECTION_FILTER_OPTIONS& aOptions ) {}
};
