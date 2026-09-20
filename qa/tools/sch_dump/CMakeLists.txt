# This program source code file is part of KiCad, a free EDA CAD application.
#
# Copyright The KiCad Developers, see AUTHORS.txt for contributors.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

# kicad-sch-dump: record a schematic's draw stream to a file.
#
# This is the end-to-end proof for the recording GAL and the source of the
# golden fixtures in qa/data/draw_streams/. It owns its own main(), its own
# PGM_BASE and its own KIFACE_BASE, so unlike the QA binaries it pulls in no
# Boost test or mock infrastructure.
add_executable( kicad_sch_dump
    sch_dump.cpp
)

set_target_properties( kicad_sch_dump PROPERTIES OUTPUT_NAME kicad-sch-dump )

# Build against the same definitions the eeschema kiface uses, so internal units
# resolve to eeschema's 100 nm rather than pcbnew's 1 nm.
target_compile_definitions( kicad_sch_dump PRIVATE EESCHEMA )

# Linking the kiface objects pulls in generated lexer sources; depend on the
# object library so those are finished before a parallel build links us.
add_dependencies( kicad_sch_dump eeschema_kiface_objects )

if( TARGET api_schema_build_copy )
    add_dependencies( kicad_sch_dump api_schema_build_copy )
endif()

include_directories( BEFORE ${INC_BEFORE} )
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/eeschema
    ${INC_AFTER}
)

target_link_libraries( kicad_sch_dump
    eeschema_kiface_objects
    common
    kicommon
    pads_common
    kimath
    markdown_lib
    ${wxWidgets_LIBRARIES}
    ${GDI_PLUS_LIBRARIES}
    Boost::headers
)

if( NOT KICAD_BUILD_QA_TESTS )
    set_target_properties( kicad_sch_dump PROPERTIES EXCLUDE_FROM_ALL TRUE )
endif()
