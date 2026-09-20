/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright (C) 2004 Jean-Pierre Charras, jean-pierre.charras@gipsa-lab.inpg.fr
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

#include <core/kicad_algo.h>
#include <general.h>
#include <sch_bus_entry.h>
#include <sch_edit_frame.h>
#include <sch_junction.h>
#include <sch_line.h>
#include <sch_no_connect.h>
#include <sch_commit.h>
#include <tool/tool_manager.h>
#include <tools/sch_line_wire_bus_tool.h>
#include <tools/sch_selection_tool.h>
#include <trigo.h>


void SCH_EDIT_FRAME::TestDanglingEnds()
{
    std::function<void( SCH_ITEM* )> changeHandler =
            [&]( SCH_ITEM* aChangedItem ) -> void
            {
                GetCanvas()->GetView()->Update( aChangedItem, KIGFX::REPAINT );
            };

    GetScreen()->TestDanglingEnds( nullptr, &changeHandler );
}




void SCH_EDIT_FRAME::DeleteJunction( SCH_COMMIT* aCommit, SCH_ITEM* aJunction )
{
    SCHEMATIC_HOLDER::DeleteJunction( aCommit, aJunction );
}


void SCH_EDIT_FRAME::UpdateHopOveredWires( SCH_ITEM* aItem )
{
    std::vector<KIGFX::VIEW::LAYER_ITEM_PAIR> items;

    GetCanvas()->GetView()->Query( aItem->GetBoundingBox(), items );

    for( const auto& it : items )
    {
        if( !it.first->IsSCH_ITEM() )
            continue;

        SCH_ITEM* item = static_cast<SCH_ITEM*>( it.first );

        if( item == aItem )
            continue;

        if( item->IsType( { SCH_ITEM_LOCATE_WIRE_T, SCH_ITEM_LOCATE_BUS_T } ) )
        {
            GetCanvas()->GetView()->Update( item );
        }
    }
}
