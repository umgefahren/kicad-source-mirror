// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The generated bindings, exactly as bindgen emits them.
//!
//! Public because a `-sys` crate that hides its raw layer is no use to anyone
//! who needs something the wrapper does not expose yet — the action registry,
//! for one. Prefer [`crate::Session`].
//!
//! The struct layout assertions bindgen generates are compiled as tests here,
//! which is the Rust and C sides agreeing about every field offset. They are the
//! reason these bindings are generated per build rather than checked in.

#![allow(
    non_camel_case_types,
    non_snake_case,
    non_upper_case_globals,
    dead_code,
    missing_docs
)]

include!(concat!(env!("OUT_DIR"), "/sch_host_abi.rs"));
