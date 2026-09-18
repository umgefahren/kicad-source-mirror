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
# Cargo integration for the incremental Rust migration.
#
# This is deliberately a small, self-contained module rather than a dependency
# on Corrosion or a similar third-party package: KiCad's build already has a
# long dependency list, and everything needed here is a handful of lines of
# `add_custom_command`. Cargo does its own dependency tracking, so the custom
# command is unconditional and cheap when nothing has changed.
#
# Usage:
#
#   kicad_add_rust_library( kicad_gal_ffi
#       CRATE     kicad-sch-ffi
#       MANIFEST  ${CMAKE_SOURCE_DIR}/rust/Cargo.toml )
#
#   kicad_add_rust_executable( eeschema_gpui
#       CRATE     kicad-eeschema-gpui
#       MANIFEST  ${CMAKE_SOURCE_DIR}/rust/Cargo.toml )
#

include_guard( GLOBAL )

find_program( CARGO_EXECUTABLE cargo
              HINTS ENV CARGO_HOME
              PATH_SUFFIXES bin
              DOC "The Rust cargo build tool" )

find_program( RUSTC_EXECUTABLE rustc
              HINTS ENV CARGO_HOME
              PATH_SUFFIXES bin
              DOC "The Rust compiler" )

if( CARGO_EXECUTABLE AND RUSTC_EXECUTABLE )
    execute_process( COMMAND ${RUSTC_EXECUTABLE} --version
                     OUTPUT_VARIABLE _rustc_version
                     OUTPUT_STRIP_TRAILING_WHITESPACE
                     ERROR_QUIET )
    set( KICAD_RUST_FOUND TRUE )
    message( STATUS "Found Rust toolchain: ${_rustc_version}" )
else()
    set( KICAD_RUST_FOUND FALSE )
endif()


#
# Everything cargo builds for this project goes in one target directory so that
# crates share compiled dependencies. gpui and wgpu together are a large
# dependency graph, and rebuilding them per-target would be painful.
#
set( KICAD_RUST_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-target"
     CACHE PATH "Directory cargo builds the Rust crates into" )


#
# Translate the CMake build type into a cargo profile, and report the
# subdirectory of the target directory that cargo will place artifacts in.
#
function( _kicad_rust_profile out_profile_args out_profile_dir )
    # Cargo's built-in profiles are `dev` (output in `debug/`) and `release`.
    # Everything that is not a plain Debug build gets the optimised profile;
    # an unoptimised renderer is not usable interactively.
    if( CMAKE_BUILD_TYPE STREQUAL "Debug" )
        set( ${out_profile_args} "" PARENT_SCOPE )
        set( ${out_profile_dir} "debug" PARENT_SCOPE )
    else()
        set( ${out_profile_args} "--release" PARENT_SCOPE )
        set( ${out_profile_dir} "release" PARENT_SCOPE )
    endif()
endfunction()


#
# Shared argument handling and custom-command construction.
#
function( _kicad_rust_build_command target artifact_name out_artifact )
    cmake_parse_arguments( ARG "NO_DEFAULT_FEATURES;ALL_FEATURES"
                               "CRATE;MANIFEST"
                               "FEATURES;DEPENDS;ENVIRONMENT" ${ARGN} )

    if( NOT ARG_CRATE )
        message( FATAL_ERROR "kicad_add_rust_*: CRATE is required" )
    endif()

    if( NOT ARG_MANIFEST )
        set( ARG_MANIFEST "${CMAKE_SOURCE_DIR}/rust/Cargo.toml" )
    endif()

    _kicad_rust_profile( _profile_args _profile_dir )

    set( _artifact "${KICAD_RUST_TARGET_DIR}/${_profile_dir}/${artifact_name}" )

    set( _features_args "" )
    if( ARG_FEATURES )
        list( JOIN ARG_FEATURES "," _feature_list )
        set( _features_args --features "${_feature_list}" )
    endif()
    if( ARG_NO_DEFAULT_FEATURES )
        list( APPEND _features_args --no-default-features )
    endif()
    if( ARG_ALL_FEATURES )
        list( APPEND _features_args --all-features )
    endif()

    # `--locked` keeps a build from silently resolving different dependency
    # versions than the checked-in Cargo.lock; a KiCad release build must be
    # reproducible.
    add_custom_command(
        OUTPUT ${_artifact}
        COMMAND ${CMAKE_COMMAND} -E env
                "CARGO_TARGET_DIR=${KICAD_RUST_TARGET_DIR}"
                ${ARG_ENVIRONMENT}
                ${CARGO_EXECUTABLE} build
                    --locked
                    --manifest-path "${ARG_MANIFEST}"
                    --package ${ARG_CRATE}
                    ${_profile_args}
                    ${_features_args}
        DEPENDS ${ARG_DEPENDS}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Building Rust crate ${ARG_CRATE}"
        USES_TERMINAL
        VERBATIM )

    set( ${out_artifact} "${_artifact}" PARENT_SCOPE )
endfunction()


#
# Build a Rust crate as a static library and expose it as an imported target.
#
# The crate must set `crate-type = ["staticlib"]`. Rust's staticlib output
# carries the Rust standard library with it but still needs the platform's
# threading, dynamic-loading and math libraries at link time.
#
function( kicad_add_rust_library target )
    cmake_parse_arguments( ARG "" "CRATE" "" ${ARGN} )

    if( NOT KICAD_RUST_FOUND )
        message( FATAL_ERROR
                 "kicad_add_rust_library(${target}): no Rust toolchain found. "
                 "Install one from https://rustup.rs or set KICAD_USE_RUST_SCH_UI=OFF." )
    endif()

    string( REPLACE "-" "_" _libstem "${ARG_CRATE}" )
    _kicad_rust_build_command( ${target} "lib${_libstem}.a" _artifact ${ARGN} )

    add_custom_target( ${target}-build DEPENDS ${_artifact} )

    add_library( ${target} STATIC IMPORTED GLOBAL )
    set_target_properties( ${target} PROPERTIES
                           IMPORTED_LOCATION ${_artifact} )
    add_dependencies( ${target} ${target}-build )

    if( UNIX AND NOT APPLE )
        set_property( TARGET ${target} APPEND PROPERTY
                      INTERFACE_LINK_LIBRARIES pthread dl m )
    elseif( APPLE )
        set_property( TARGET ${target} APPEND PROPERTY
                      INTERFACE_LINK_LIBRARIES
                      "-framework CoreFoundation" "-framework Security" )
    endif()
endfunction()


#
# Build a Rust crate as an executable and expose it as an imported target.
#
function( kicad_add_rust_executable target )
    cmake_parse_arguments( ARG "" "CRATE;OUTPUT_NAME" "" ${ARGN} )

    if( NOT KICAD_RUST_FOUND )
        message( FATAL_ERROR
                 "kicad_add_rust_executable(${target}): no Rust toolchain found. "
                 "Install one from https://rustup.rs or set KICAD_USE_RUST_SCH_UI=OFF." )
    endif()

    if( NOT ARG_OUTPUT_NAME )
        set( ARG_OUTPUT_NAME "${ARG_CRATE}" )
    endif()

    set( _exe_name "${ARG_OUTPUT_NAME}${CMAKE_EXECUTABLE_SUFFIX}" )
    _kicad_rust_build_command( ${target} "${_exe_name}" _artifact ${ARGN} )

    add_custom_target( ${target} ALL DEPENDS ${_artifact} )

    set_target_properties( ${target} PROPERTIES
                           KICAD_RUST_ARTIFACT "${_artifact}" )
endfunction()


#
# Register `cargo test` for a crate with CTest, so the Rust tests run as part of
# the normal QA suite rather than needing a separate invocation.
#
function( kicad_add_rust_test name )
    cmake_parse_arguments( ARG "" "CRATE;MANIFEST" "ENVIRONMENT" ${ARGN} )

    if( NOT KICAD_RUST_FOUND )
        return()
    endif()

    if( NOT ARG_MANIFEST )
        set( ARG_MANIFEST "${CMAKE_SOURCE_DIR}/rust/Cargo.toml" )
    endif()

    _kicad_rust_profile( _profile_args _profile_dir )

    add_test( NAME ${name}
              COMMAND ${CMAKE_COMMAND} -E env
                      "CARGO_TARGET_DIR=${KICAD_RUST_TARGET_DIR}"
                      ${ARG_ENVIRONMENT}
                      ${CARGO_EXECUTABLE} test
                          --locked
                          --manifest-path "${ARG_MANIFEST}"
                          --package ${ARG_CRATE}
                          ${_profile_args} )

    # The GPU-backed tests need a software Vulkan device in headless CI.
    set_tests_properties( ${name} PROPERTIES LABELS "rust" )
endfunction()
