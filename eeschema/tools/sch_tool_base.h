/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2019 CERN
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

#pragma once

#include <tool/tool_interactive.h>
#include <string>

class EDA_ITEM;
class SCHEMATIC_HOLDER;
class SCH_COMMIT;
class SCH_BASE_FRAME;
class SCH_EDIT_FRAME;
class SYMBOL_EDIT_FRAME;
class SCH_SELECTION_TOOL;
enum class UNDO_REDO;

namespace KIGFX
{
class SCH_VIEW;
}

class SCH_SELECTION;

/**
 * A foundation class for a tool operating on a schematic or symbol.
 */


template <class T>
class SCH_TOOL_BASE : public TOOL_INTERACTIVE
{
public:
    /**
     * Create a tool with given name. The name must be unique.
     */
    SCH_TOOL_BASE( const std::string& aName );

    ~SCH_TOOL_BASE() override;

    /// @copydoc TOOL_INTERACTIVE::Init()
    bool Init() override;

    /// @copydoc TOOL_INTERACTIVE::Reset()
    void Reset( RESET_REASON aReason ) override;

    /**
     * Returns true if the tool is running in the symbol editor
     */
    bool IsSymbolEditor() const;

    int Increment( const TOOL_EVENT& aEvent );

    int InteractiveDelete( const TOOL_EVENT& aEvent );

protected:
    /// Await placement without nesting a wx event loop in a non-wx frontend.
    bool MoveWithCommit( SCH_COMMIT* aCommit );

    /**
     * Whether this tool can run when the thing editing the schematic is not a wxFrame.
     *
     * False by default, because `m_frame` is most tools' route to the screen, the
     * selection, the undo stack and every dialog, and a tool holding a null one would
     * crash on its first use rather than decline. A tool that answers true has been
     * converted to ask #m_editor for what it needs, and must tolerate a null `m_frame`
     * *and* a null `m_menu` — `TOOL_INTERACTIVE` only builds a menu when `Pgm().IsGUI()`.
     *
     * See `docs/rust-migration/06-what-is-missing.md` Stage 4b for which tools are
     * converted and what the next one costs.
     */
    virtual bool runsWithoutAFrame() const { return false; }

    template <class F = SCH_BASE_FRAME>
    F* frame() const
    {
        return getEditFrame<F>();
    }

    /**
     * Return the parent container for new draw-items in the active editor.
     *
     * In the schematic editor this is the @ref SCHEMATIC, in the symbol editor it is
     * the current @ref LIB_SYMBOL.
     */
    EDA_ITEM* getDrawParent() const;

    /**
     * Similar to getView()->Update(), but also updates the SCH_SCREEN's RTree.
     */
    void updateItem( EDA_ITEM* aItem, bool aUpdateRTree ) const;

    ///< Similar to m_frame->SaveCopyInUndoList(), but also handles connectivity.
    void saveCopyInUndoList( EDA_ITEM* aItem, UNDO_REDO aType, bool aAppend = false, bool aDirtyConnectivity = true );

protected:
    /// The frame, or **null** when the editing context is not one. See ::runsWithoutAFrame.
    T*                  m_frame;

    /**
     * Whatever is editing the schematic: the document, the settings and the canvas
     * notifications. Non-null whenever the tool initialised at all, including when
     * #m_frame is null.
     */
    SCHEMATIC_HOLDER*   m_editor;

    KIGFX::SCH_VIEW*    m_view;
    SCH_SELECTION_TOOL* m_selectionTool;
    bool                m_isSymbolEditor;
    EDA_ITEM*           m_pickerItem;
};

extern template class SCH_TOOL_BASE<SCH_BASE_FRAME>;
extern template class SCH_TOOL_BASE<SCH_EDIT_FRAME>;
extern template class SCH_TOOL_BASE<SYMBOL_EDIT_FRAME>;
