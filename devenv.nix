# The development environment for this tree.
#
#   devenv shell             -- interactive
#   devenv shell -- <cmd>    -- one command inside it
#
# The build is two halves and needs two toolchains:
#
#   * The C++ logic core, driven by cmake/ninja. The library list below is the
#     nixpkgs translation of the apt list in
#     docs/rust-migration/03-build-notes.md -- everything here is here because
#     some find_package() in the top-level CMakeLists.txt asks for it, and the
#     comments say which.
#
#   * The Rust schematic UI in rust/, which needs a *nightly* compiler: gpui
#     0.3.5 uses the still-unstable `cold_path` intrinsic.
#     rust/rust-toolchain.toml states the same requirement for rustup users;
#     inside this shell the toolchain comes from the rust-overlay input and
#     rustup is not involved, so that file is inert (and the nightly check in
#     cmake/KiCadRust.cmake still passes, because `rustc --version` says
#     nightly).
#
# Platform notes: this is exercised on aarch64-darwin, where wxWidgets is the
# Cocoa port -- see the macOS section of docs/rust-migration/03-build-notes.md.
# The Linux entries mirror the container reference environment in those notes
# (wxGTK3 + X11/GL) and are not tested from here.
{ pkgs, lib, config, ... }:

let
  inherit (pkgs.stdenv.hostPlatform) isLinux;
in
{
  # ------------------------------------------------------------------ Rust --
  languages.rust = {
    enable = true;
    channel = "nightly";
    components = [
      "rustc"
      "cargo"
      "rustfmt" # rust/rustfmt.toml
      "clippy" # the workspace is clean under `clippy -D warnings`
      "rust-src"
      "rust-analyzer"
    ];
  };

  # ------------------------------------------------------------------- C++ --
  # The compiler comes from the nix stdenv (clang on macOS, gcc on Linux); only
  # the build drivers and the libraries KiCad looks for are listed here.
  packages =
    with pkgs;
    [
      cmake
      ninja
      pkg-config
      ccache # tools/build/configure-dev.sh uses it when present
      git # cmake/CreateGitVersionHeader.cmake shells out to it
      python3 # find_package( PythonInterp 3.6 ), configure time only
      gettext # only needed with -DKICAD_BUILD_I18N=ON

      # Required by the top-level CMakeLists.txt. Configure fails without any
      # one of these, even for a build that only wants eeschema.
      wxwidgets_3_2 # >= 3.2; components gl aui adv html core net base propgrid xml stc richtext webview
      boost # locale, plus unit_test_framework for the QA suite
      cairo # the Cairo GAL backend
      pixman
      glm
      freetype
      harfbuzz
      fontconfig
      libpng # these two are for thirdparty/libwmf, the OrCAD/WMF importer
      libjpeg
      curl
      libgit2 # >= 1.5
      zlib
      zstd
      bzip2
      openssl
      libngspice # find_package( ngspice REQUIRED ) -- the simulator
      opencascade-occt # >= 7.6; the 3D viewer and the STEP/IGES exporters
      protobuf_29 # the IPC API: kiapi is an unconditional part of kicommon
      nng # ditto, kinng
      unixodbc # sql.h for thirdparty/nanodbc, compiled into kicommon

      # Not required to build, but this tree's C++ style is enforced with them
      # (_clang-format, .clang-tidy, .githooks/).
      clang-tools

      # kicad-sch-sys runs bindgen over include/sch_host/sch_host_abi.h in its
      # build script. This hook is what makes libclang find the platform
      # headers: it reads the compiler wrapper's flags and exports them as
      # BINDGEN_EXTRA_CLANG_ARGS, which bindgen does read. Without it bindgen
      # fails on `#include <stdint.h>`.
      rustPlatform.bindgenHook
    ]
    ++ lib.optionals isLinux [
      gtk3 # wx's port here; qa_utils also reaches into GTK directly
      libGL
      libGLU # find_package( OpenGL REQUIRED ) wants GLU as well
      libx11
      libsecret # libs/kiplatform/os/unix/secrets.cpp
      libspnav # find_package( SPNAV REQUIRED ) on unix-not-apple
      poppler # kiplatform printing: find_package( Poppler COMPONENTS Glib )
    ];

  env = {
    # A build tree under the repo root, which .gitignore already covers, instead
    # of configure-dev.sh's /tmp default.
    KICAD_BUILD_DIR = "${config.devenv.root}/build";

    # This fontconfig has no system-wide configuration to fall back on, so
    # anything that lays out text -- KiCad's outline font manager, and the QA
    # binaries on their way past it -- reports "Cannot load default config
    # file". The generated config covers the platform's own font directories,
    # which is how the outline font list gets populated at all.
    FONTCONFIG_FILE = pkgs.makeFontsConf { fontDirectories = [ ]; };
  };
}
