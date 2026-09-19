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

#ifndef SCH_INSPECTION_TOOL_H
#define SCH_INSPECTION_TOOL_H

#include <tools/sch_tool_base.h>
#include <dialogs/dialog_book_reporter.h>
#include <sch_base_frame.h>


class SCH_SELECTION_TOOL;
class SCH_BASE_FRAME;
class DIALOG_ERC;
class SYMBOL_DIFF_WIDGET;


class SCH_INSPECTION_TOOL : public wxEvtHandler, public SCH_TOOL_BASE<SCH_BASE_FRAME>
{
public:
    SCH_INSPECTION_TOOL();
    ~SCH_INSPECTION_TOOL() {}

    /// @copydoc TOOL_INTERACTIVE::Init()
    bool Init() override;

    /**
     * Runs on an editing context that is not a wxFrame.
     *
     * The tool itself computes nothing: it asks the editor for the windows that do. ERC
     * is run by DIALOG_ERC, the symbol/library diff by DIALOG_BOOK_REPORTER and the
     * schematic comparison by DIALOG_KICAD_DIFF, so what registering it without a frame
     * buys is that it is present and declines rather than being absent — and that the
     * message-panel and cross-probe handlers bound to the selection events stop being a
     * reason for the selection tool's events to reach a dead end.
     *
     * Every action needs a window and declines without one: ::RunERC, ::PrevMarker,
     * ::NextMarker, ::ExcludeMarker and ::CrossProbe (the ERC dialog), ::CheckSymbol
     * (the symbol editor's current symbol, a UNITS_PROVIDER to format coordinates with,
     * and an HTML_MESSAGE_BOX to show them in), ::DiffSymbol,
     * ::CompareSchematicWithFile, ::CompareSchematicWithHistory, ::ShowBusSyntaxHelp,
     * ::ShowDatasheet and ::RunSimulation (all dialogs, or a Kiway player).
     * ::UpdateMessagePanel keeps running and updates nothing, because the message panel
     * and the hierarchy navigator are both the frame's.
     */
    bool runsWithoutAFrame() const override { return true; }

    void Reset( RESET_REASON aReason ) override;

    int RunERC( const TOOL_EVENT& aEvent );
    void ShowERCDialog();

    int PrevMarker( const TOOL_EVENT& aEvent );
    int NextMarker( const TOOL_EVENT& aEvent );

    /// Called when clicking on a item:
    int CrossProbe( const TOOL_EVENT& aEvent );

    void CrossProbe( const SCH_MARKER* aMarker );

    wxString InspectERCErrorMenuText( const std::shared_ptr<RC_ITEM>& aERCItem );
    void InspectERCError( const std::shared_ptr<RC_ITEM>& aERCItem );

    int ExcludeMarker( const TOOL_EVENT& aEvent );

    int ShowBusSyntaxHelp( const TOOL_EVENT& aEvent );

    int CheckSymbol( const TOOL_EVENT& aEvent );
    int DiffSymbol( const TOOL_EVENT& aEvent );
    void DiffSymbol( SCH_SYMBOL* aSymbol );

    /// Diff the current schematic against a user-selected .kicad_sch file.
    int CompareSchematicWithFile( const TOOL_EVENT& aEvent );

    /// Diff the current schematic against the most recent local-history commit.
    int CompareSchematicWithHistory( const TOOL_EVENT& aEvent );

    int RunSimulation( const TOOL_EVENT& aEvent );

    int ShowDatasheet( const TOOL_EVENT& aEvent );

    /// Display the selected item info (when clicking on a item)
    int UpdateMessagePanel( const TOOL_EVENT& aEvent );

private:
    /// Diff the schematic at aOtherPath against the live one and show the dialog.
    int showSchematicComparison( const wxString& aOtherPath, const wxString& aProjectPath,
                                 const wxString& aComparisonLabel );

    SYMBOL_DIFF_WIDGET* constructDiffPanel( wxPanel* aParentPanel );

    ///< @copydoc TOOL_INTERACTIVE::setTransitions();
    void setTransitions() override;

private:
    HTML_MESSAGE_BOX* m_busSyntaxHelp;
};

#endif /* SCH_INSPECTION_TOOL_H */
