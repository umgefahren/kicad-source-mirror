/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2019-2023 CERN
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

#ifndef SCH_DRAWING_TOOLS_H
#define SCH_DRAWING_TOOLS_H

#include "sch_sheet_path.h"
#include <tools/sch_tool_base.h>
#include <sch_base_frame.h>
#include <sch_label.h>
#include <status_popup.h>
#include <sync_sheet_pin/dialog_sync_sheet_pins.h>

class SCH_SYMBOL;
class SCH_BUS_WIRE_ENTRY;
class SCH_EDIT_FRAME;
class SCH_SELECTION_TOOL;
class DIALOG_SYNC_SHEET_PINS;


/**
 * Tool responsible for drawing/placing items (symbols, wires, buses, labels, etc.).
 */

class SCH_DRAWING_TOOLS : public SCH_TOOL_BASE<SCH_EDIT_FRAME>
{
public:
    ///< The possible drawing modes of @ref SCH_DRAWING_TOOLS
    enum class MODE
    {
        NONE,
        RULE_AREA,
    };

    SCH_DRAWING_TOOLS();
    ~SCH_DRAWING_TOOLS() override { }

    /// @copydoc TOOL_INTERACTIVE::Init()
    bool Init() override;

    /**
     * Runs on an editing context that is not a wxFrame.
     *
     * Most of what this tool places needs nothing but the screen, the schematic settings
     * and the repeat list, all of which are SCHEMATIC_HOLDER's: junctions, no-connects,
     * wire-to-bus entries, sheet pins, tables, rule areas, auto-placed sheet pins, a label
     * whose name the wire it lands on already supplies, and an image handed in as a
     * parameter.
     *
     * What needs a window, and declines without one saying so at the site:
     *
     * * ::PlaceSymbol and ::PlaceNextSymbolUnit — the library chooser is the action's
     *   first step, and PickSymbolFromLibrary(), GetLibSymbol() and Prj() are the frame's.
     * * ::ImportSheet (place design block, import sheet) and ::DrawSheet — a chooser pane
     *   or file dialog picks the source, EditSheetProperties() names the sheet, and
     *   AnnotateSymbols() renumbers what comes in.
     * * ::SyncSheetsPins and ::SyncAllSheetsPins — the action *is* DIALOG_SYNC_SHEET_PINS.
     * * a text item, and a label the attached wire does not name: both take their content
     *   from a properties dialog, so ::createNewText and ::createNewLabel give up.
     * * choosing an image file in ::PlaceImage, which ends the tool rather than looping on
     *   a click it cannot answer.
     *
     * Everything else that is a window only decorates: the info bar, the message panel,
     * the "click over a sheet" popups and the right-click menu are each skipped, and the
     * edit still happens. ::DrawTable is the one place where skipping the dialog changes
     * the outcome rather than losing it — the table as dragged out is committed, instead
     * of the user being offered a chance to amend it first.
     */
    bool runsWithoutAFrame() const override { return true; }

    int PlaceSymbol( const TOOL_EVENT& aEvent );
    int PlaceNextSymbolUnit( const TOOL_EVENT& aEvent );
    int SingleClickPlace( const TOOL_EVENT& aEvent );
    int TwoClickPlace( const TOOL_EVENT& aEvent );
    int ImportSheet( const TOOL_EVENT& aEvent );
    int DrawRuleArea( const TOOL_EVENT& aEvent );
    int DrawTable( const TOOL_EVENT& aEvent );
    int DrawSheet( const TOOL_EVENT& aEvent );
    int DrawSheetHost( const TOOL_EVENT& aEvent, const wxString& aSource );
    int PlaceImage( const TOOL_EVENT& aEvent );
    int SyncSheetsPins( const TOOL_EVENT& aEvent );
    int SyncAllSheetsPins( const TOOL_EVENT& aEvent );
    int AutoPlaceAllSheetPins( const TOOL_EVENT& aEvent );

private:
    SCH_LINE* findWire( const VECTOR2I& aPosition );

    ///< Gets the (global) label name driving this wire, if it is driven by a label
    wxString findWireLabelDriverName( SCH_LINE* aWire );

    SCH_TEXT* createNewText( const VECTOR2I& aPosition );

    bool createNewLabel( const VECTOR2I& aPosition, int aType,
                        std::list<std::unique_ptr<SCH_LABEL_BASE>>& aLabelList );

    SCH_SHEET_PIN* createNewSheetPin( SCH_SHEET* aSheet, const VECTOR2I& aPosition );

    SCH_SHEET_PIN* createNewSheetPinFromLabel( SCH_SHEET* aSheet, const VECTOR2I& aPosition,
                                               SCH_HIERLABEL* aLabel );

    void sizeSheet( SCH_SHEET* aSheet, const VECTOR2I& aPos );

    ///< Set up handlers for various events.
    void setTransitions() override;

    int doSyncSheetsPins( std::list<SCH_SHEET_PATH> aSheets, SCH_SHEET* aInitialSheet = nullptr );

    ///< Try finding any hierlabel that does not have a sheet pin associated with it
    SCH_HIERLABEL* importHierLabel( SCH_SHEET* aSheet );

    std::vector<SCH_HIERLABEL*> importHierLabels( SCH_SHEET* aSheet );

    std::vector<PICKED_SYMBOL> m_symbolHistoryList;
    std::vector<PICKED_SYMBOL> m_powerHistoryList;
    std::vector<LIB_ID>        m_designBlockHistoryList;

    LABEL_FLAG_SHAPE           m_lastSheetPinType;
    LABEL_FLAG_SHAPE           m_lastGlobalLabelShape;
    LABEL_FLAG_SHAPE           m_lastNetClassFlagShape;
    SPIN_STYLE                 m_lastTextOrientation;
    bool                       m_lastTextBold;
    bool                       m_lastTextItalic;
    EDA_ANGLE                  m_lastTextAngle;
    GR_TEXT_H_ALIGN_T          m_lastTextHJustify;
    GR_TEXT_V_ALIGN_T          m_lastTextVJustify;
    wxString                   m_mruPath;
    bool                       m_lastAutoLabelRotateOnPlacement;
    MODE                       m_mode;

    bool                                    m_inDrawingTool; // Re-entrancy guard
    std::unique_ptr<STATUS_TEXT_POPUP>      m_statusPopup;
    std::unique_ptr<DIALOG_SYNC_SHEET_PINS> m_dialogSyncSheetPin;
};

#endif /* SCH_DRAWING_TOOLS_H */
