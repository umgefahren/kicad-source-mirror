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

#include <schematic_holder.h>

#include <sch_item.h>
#include <sch_label.h>
#include <sch_screen.h>
#include <sch_sheet_path.h>
#include <schematic.h>


const std::vector<std::unique_ptr<SCH_ITEM>>& SCHEMATIC_HOLDER::GetRepeatItems() const
{
    // An editor with no repeat-item list answers the empty one rather than making every
    // caller test for absence.
    static const std::vector<std::unique_ptr<SCH_ITEM>> none;

    return none;
}


void SCHEMATIC_HOLDER::AutoRotateItem( SCH_SCREEN* aScreen, SCH_ITEM* aItem )
{
    SCHEMATIC* schematic = GetSchematic();

    if( !schematic || !aScreen )
        return;

    const SCH_SHEET_PATH& sheet = schematic->CurrentSheet();

    if( aItem->Type() == SCH_GLOBAL_LABEL_T || aItem->Type() == SCH_HIER_LABEL_T )
    {
        SCH_LABEL_BASE* label = static_cast<SCH_LABEL_BASE*>( aItem );

        if( label->AutoRotateOnPlacement() )
        {
            SPIN_STYLE spin = aScreen->GetLabelOrientationForPoint( label->GetPosition(), label->GetSpinStyle(),
                                                                    &sheet );

            if( spin != label->GetSpinStyle() )
            {
                label->SetSpinStyle( spin );

                for( SCH_ITEM* item : aScreen->Items().OfType( SCH_GLOBAL_LABEL_T ) )
                {
                    SCH_LABEL_BASE* otherLabel = static_cast<SCH_LABEL_BASE*>( item );

                    if( otherLabel != label && otherLabel->GetText() == label->GetText() )
                        otherLabel->AutoplaceFields( aScreen, AUTOPLACE_AUTO );
                }
            }
        }
    }
}
