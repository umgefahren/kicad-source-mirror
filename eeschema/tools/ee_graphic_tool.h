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

#pragma once

#include <vector>

#include <tool/shape_draw_behavior.h>
#include <tools/sch_tool_base.h>
#include <sch_base_frame.h>
#include <sch_shape.h>

class SCH_EDIT_FRAME;
class SYMBOL_EDIT_FRAME;


/**
 * Tool responsible for drawing graphical shapes (rectangles, circles, arcs,
 * beziers, text boxes, etc.) in both the schematic and symbol editors.
 */
class EE_GRAPHIC_TOOL : public SCH_TOOL_BASE<SCH_BASE_FRAME>
{
public:
    enum class MODE
    {
        NONE,
        ARC,
        BEZIER,
        ELLIPSE_ARC,
    };

    EE_GRAPHIC_TOOL();

    bool Init() override;

    /**
     * Runs on an editing context that is not a wxFrame.
     *
     * What it asks the editor for is SCHEMATIC_HOLDER's: the screen it commits new
     * shapes to, and the cursor shape that says a click here draws. Without a frame it
     * loses the windows: the message panel readout of the shape being drawn, the
     * right-click menu, and the unit preference the drawing assistant labels distances
     * with (they read in internal units instead). The two actions that *are* a dialog —
     * drawing a text box, which ends in the text properties dialog, and importing
     * graphics — decline to run rather than draw something the user never described.
     */
    bool runsWithoutAFrame() const override { return true; }

    int DrawShape( const TOOL_EVENT& aEvent );
    int DrawArc( const TOOL_EVENT& aEvent );
    int DrawBezier( const TOOL_EVENT& aEvent );
    int DrawEllipseArc( const TOOL_EVENT& aEvent );
    int ImportGraphics( const TOOL_EVENT& aEvent );

private:
    void setTransitions() override;

    ///< The layer to use for new shapes in the current editor.
    SCH_LAYER_ID getShapeLayer() const;

    /**
     * The symbol editor's frame, or null when this is not the symbol editor.
     *
     * The symbol editor is always a wxFrame, so a null answer means a schematic
     * context rather than a missing window.
     */
    SYMBOL_EDIT_FRAME* symbolEditFrame() const;

    /**
     * The units the drawing assistant labels distances in.
     *
     * The unit preference is a window's, and the assistant overlay is the only thing
     * here that reads it — the geometry is in internal units either way — so an editor
     * without a frame gets unscaled labels rather than no shape at all.
     */
    EDA_UNITS getUserUnits() const;

    /**
     * Show an item's properties in the message panel, where there is one.
     *
     * The message panel is part of the frame's window; an editor without one simply
     * goes without the readout, and the drawing is unaffected.
     */
    void setMsgPanel( EDA_ITEM* aItem ) const;

    ///< Commit a completed item.
    void commitItem( SCH_COMMIT& aCommit, std::unique_ptr<SCH_ITEM> aItem, const wxString& aDescription );

    ///< Return the default text size (in IU) for the active editor.
    int getDefaultTextSize() const;

    /**
     * When in the symbol editor, apply unit and body-style restrictions to @a aItem
     * according to the current draw-specific flags on the frame.
     */
    void applySymbolEditorFlags( SCH_ITEM& aItem ) const;

    /**
     * Run the interactive drawing event loop for any shape driven by a
     * @ref SHAPE_DRAW_BEHAVIOR (arcs, ellipse arcs, etc.).
     *
     * @return the outcome of the drawing loop, handle appropriately (commit,
     *         start another, or stop).
     */
    SHAPE_DRAW_RESULT drawManagedShape( const TOOL_EVENT& aTool, std::unique_ptr<SCH_SHAPE>& aShape,
                                        SHAPE_DRAW_BEHAVIOR& aBehavior, const std::vector<VECTOR2D>& aInitialPts );

    FILL_T        m_lastFillStyle;
    COLOR4D       m_lastFillColor;
    STROKE_PARAMS m_lastStroke;

    FILL_T        m_lastTextboxFillStyle;
    COLOR4D       m_lastTextboxFillColor;
    STROKE_PARAMS m_lastTextboxStroke;

    bool              m_lastTextBold;
    bool              m_lastTextItalic;
    EDA_ANGLE         m_lastTextboxAngle;
    GR_TEXT_H_ALIGN_T m_lastTextboxHJustify;
    GR_TEXT_V_ALIGN_T m_lastTextboxVJustify;

    MODE m_mode;

    // Re-entrancy guards
    bool m_inDrawingTool;
};
