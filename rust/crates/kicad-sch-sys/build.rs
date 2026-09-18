// This program source code file is part of KiCad, a free EDA CAD application.
//
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Generates the bindings and finds the host library.
//!
//! Two things are deliberate here.
//!
//! **The bindings are generated, not checked in.** The C header is the single
//! source of truth for the ABI, and a checked-in copy is a second one that can
//! silently fall behind it. bindgen also emits a layout assertion per struct,
//! which is the two sides agreeing about every field offset at compile time —
//! worth more than the build-time cost of running it.
//!
//! **A missing host library is not an error.** The rest of this workspace is
//! developed and tested against recorded draw streams with no C++ linked at all,
//! and that has to keep working: `cargo test` in a fresh checkout must not
//! require a multi-hour eeschema build first. So when no library is found this
//! emits nothing, and `lib.rs` compiles an API whose every entry point reports
//! [`Error::NoHost`]. `cfg(ksch_linked)` is what tells the two apart.

use std::env;
use std::path::{Path, PathBuf};

/// The library CMake produces from `eeschema/host/`.
const LIB_STEM: &str = "kicad_sch_host";

fn main() {
    // Declared so that `cfg(ksch_linked)` is a known name rather than an
    // "unexpected cfg" warning in the no-host build.
    println!("cargo::rustc-check-cfg=cfg(ksch_linked)");
    println!("cargo::rerun-if-env-changed=KICAD_SCH_HOST_DIR");
    println!("cargo::rerun-if-env-changed=KICAD_BUILD_DIR");
    println!("cargo::rerun-if-env-changed=KICAD_SOURCE_DIR");

    let source_dir = source_dir();
    let header = source_dir.join("include/sch_host/sch_host_abi.h");

    if !header.is_file() {
        // Without the header there is nothing to generate, whatever else is
        // present. Say so once, clearly, rather than failing inside bindgen.
        println!(
            "cargo::warning=kicad-sch-sys: {} not found, so the C++ host is not \
             linked into this build. Set KICAD_SOURCE_DIR if the tree is elsewhere.",
            header.display()
        );
        return;
    }

    let Some(lib_dir) = find_library(&source_dir) else {
        // The common case in a Rust-only checkout, and not worth a warning on
        // every build: the API says it is unavailable and the binary explains
        // how to get one.
        return;
    };

    println!("cargo::rustc-link-search=native={}", lib_dir.display());
    println!("cargo::rustc-link-lib=dylib={LIB_STEM}");

    // The library is loaded by path at run time. On macOS CMake writes an
    // absolute install name into it (the tree sets CMAKE_MACOSX_RPATH FALSE), so
    // nothing more is needed; ELF needs to be told where to look.
    if cfg!(target_os = "linux") {
        println!("cargo::rustc-link-arg=-Wl,-rpath,{}", lib_dir.display());
    }

    generate_bindings(&source_dir, &header);

    println!("cargo::rustc-cfg=ksch_linked");
}

/// The KiCad source tree this crate lives in.
///
/// `rust/crates/kicad-sch-sys` is three levels down from the root, which is how
/// this is found without configuration in the normal case.
fn source_dir() -> PathBuf {
    if let Some(dir) = env::var_os("KICAD_SOURCE_DIR") {
        return PathBuf::from(dir);
    }

    let manifest = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").expect("cargo sets this"));

    manifest
        .ancestors()
        .nth(3)
        .map(Path::to_path_buf)
        .unwrap_or(manifest)
}

/// Where the host library is, if it has been built.
///
/// In order: an explicit `KICAD_SCH_HOST_DIR` (which is what the CMake build
/// passes), the `eeschema/` subdirectory of `KICAD_BUILD_DIR`, then the
/// conventional in-tree `build/`.
///
/// Setting `KICAD_SCH_HOST_DIR` to the empty string forces the no-host build,
/// which is how that path stays tested on a machine that does have a library.
fn find_library(source_dir: &Path) -> Option<PathBuf> {
    let mut candidates: Vec<PathBuf> = Vec::new();

    if let Some(dir) = env::var_os("KICAD_SCH_HOST_DIR") {
        if dir.is_empty() {
            return None;
        }

        candidates.push(PathBuf::from(dir));
    }

    if let Some(dir) = env::var_os("KICAD_BUILD_DIR") {
        candidates.push(PathBuf::from(&dir).join("eeschema"));
        candidates.push(PathBuf::from(dir));
    }

    candidates.push(source_dir.join("build/eeschema"));

    for dir in candidates {
        for name in library_names() {
            let path = dir.join(name);

            if path.is_file() {
                println!("cargo::rerun-if-changed={}", path.display());
                return Some(dir);
            }
        }
    }

    None
}

/// The file names the linker would accept for the host library here.
fn library_names() -> Vec<String> {
    if cfg!(target_env = "msvc") {
        // MSVC links through the import library, not the DLL.
        vec![format!("{LIB_STEM}.lib")]
    } else {
        vec![format!(
            "{}{LIB_STEM}{}",
            env::consts::DLL_PREFIX,
            env::consts::DLL_SUFFIX
        )]
    }
}

fn generate_bindings(source_dir: &Path, header: &Path) {
    let bindings = bindgen::Builder::default()
        .header(header.to_string_lossy())
        // draw_stream_abi.h, which the ABI header includes.
        .clang_arg(format!("-I{}", source_dir.join("include").display()))
        // Drops the export marker, which is meaningless to a parser and would
        // otherwise resolve to a dllimport attribute on Windows.
        .clang_arg("-DKISCH_HOST_STATIC")
        .allowlist_function("ksch_.*")
        .allowlist_type("ksch_.*")
        .allowlist_var("KSCH_.*")
        // The draw-stream types are kicad-gal's, hand written and layout-tested
        // there against the same header. Generating a second set would mean two
        // Rust definitions of one C struct and a transmute between them.
        .blocklist_type("kgds_.*")
        .raw_line("use kicad_gal::abi::kgds_stream_view;")
        // Layout assertions are the point; keep them.
        .layout_tests(true)
        .derive_debug(true)
        .derive_default(true)
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .generate()
        .expect("sch_host_abi.h is bindgen-clean; see docs/rust-migration/04-host-seam.md §3");

    let out = PathBuf::from(env::var_os("OUT_DIR").expect("cargo sets this"));

    bindings
        .write_to_file(out.join("sch_host_abi.rs"))
        .expect("writing the generated bindings");
}
