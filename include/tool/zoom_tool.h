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

#ifndef _ZOOM_TOOL_H
#define _ZOOM_TOOL_H

#include <tool/tool_interactive.h>

class CANVAS_HOLDER;
class EDA_DRAW_FRAME;


class ZOOM_TOOL : public TOOL_INTERACTIVE
{
public:
    ZOOM_TOOL();
    ~ZOOM_TOOL();

    /// @copydoc TOOL_INTERACTIVE::Init()
    bool Init() override;

    /// @copydoc TOOL_BASE::Reset()
    void Reset( RESET_REASON aReason ) override;

    /// Main loop
    int Main( const TOOL_EVENT& aEvent );

    /// @copydoc TOOL_BASE::setTransitions()
    void setTransitions() override;

private:
    bool selectRegion();

    /// Whatever owns the canvas: the view, its preferences and the cursor. Non-null
    /// whenever the tool initialised at all, including when #m_frame is null.
    CANVAS_HOLDER*  m_canvas;

    /// The frame, or **null** when the canvas holder is not one (a headless host).
    EDA_DRAW_FRAME* m_frame;
};

#endif // _ZOOM_TOOL_H
