#
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
# along with this program; if not, you may find one here:
# http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
# or you may search the http://www.gnu.org website for the version 2 license,
# or you may write to the Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
#

#
# The Rust half of the schematic editor UI.
#
# This is opt-in and off by default. The existing wxWidgets schematic editor is
# unaffected either way: nothing here replaces a file the old editor uses, and
# with KICAD_BUILD_RUST_SCH_UI=OFF cargo is never invoked at all.
#

include( ${KICAD_CMAKE_MODULE_PATH}/KiCadRust.cmake )

if( NOT KICAD_RUST_FOUND )
    message( FATAL_ERROR
             "KICAD_BUILD_RUST_SCH_UI is ON but no Rust toolchain was found.\n"
             "Install one from https://rustup.rs, or configure with "
             "-DKICAD_BUILD_RUST_SCH_UI=OFF." )
endif()

# gpui uses a standard-library feature that has not been stabilised yet, so the
# UI crates need a nightly compiler. rust/rust-toolchain.toml pins it, and
# rustup honours that automatically for anything run inside this directory.
# Surface the requirement at configure time rather than as a wall of E0658 in
# the middle of a build.
execute_process( COMMAND ${RUSTC_EXECUTABLE} --version
                 WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                 OUTPUT_VARIABLE _sch_ui_rustc
                 OUTPUT_STRIP_TRAILING_WHITESPACE
                 ERROR_QUIET )

if( NOT _sch_ui_rustc MATCHES "nightly" )
    message( WARNING
             "The Rust schematic UI needs a nightly toolchain (gpui uses the "
             "unstable `cold_path` intrinsic), but rustc in ${CMAKE_CURRENT_SOURCE_DIR} "
             "reports '${_sch_ui_rustc}'. If the build fails with E0658, install "
             "nightly with `rustup toolchain install nightly`." )
endif()

kicad_add_rust_executable( eeschema_gpui
    CRATE       kicad-eeschema-gpui
    OUTPUT_NAME eeschema-gpui
    MANIFEST    ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml )

if( KICAD_BUILD_QA_TESTS )
    # The Rust tests run under CTest with the rest of the QA suite. The GPU
    # backed ones detect a missing adapter and skip rather than fail, so this
    # stays green on a machine with no GPU and no software rasteriser.
    kicad_add_rust_test( qa_rust_kicad_gal
        CRATE    kicad-gal
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml )

    kicad_add_rust_test( qa_rust_sch_render
        CRATE    kicad-sch-render
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml )

    kicad_add_rust_test( qa_rust_sch_ui
        CRATE    kicad-sch-ui
        MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml )
endif()
