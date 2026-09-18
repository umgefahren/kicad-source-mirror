// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! Starts the gpui-kit schematic editor shell.
//!
//! Everything of substance lives in `kicad-sch-ui`. This binary exists so the
//! shell is runnable — and photographable — while the renderer and the C++
//! host are still being built.

use std::time::Duration;

use gpui_kit::component::Root;
use gpui_kit::component::theme::ThemeMode;
use gpui_kit::{App, Bounds, WindowBounds, WindowOptions, point, px, size};
use kicad_sch_ui::shell::{self, SchematicShell};

/// Command-line options. Hand-parsed: three flags do not justify a dependency
/// in a crate that will eventually be started by C++ rather than by a shell.
struct Options {
    /// Start in the light theme.
    light: bool,
    /// Start with the frame-time readout showing.
    frame_stats: bool,
    /// Quit after this long. Used by the headless screenshot harness, which
    /// needs the process to exit on its own.
    run_for: Option<Duration>,
    /// Window size in logical pixels.
    width: f32,
    height: f32,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            light: false,
            frame_stats: false,
            run_for: None,
            width: 1600.,
            height: 1000.,
        }
    }
}

fn usage() -> &'static str {
    "kicad-eeschema-gpui [--light] [--frame-stats] [--run-for SECONDS] \
     [--size WIDTHxHEIGHT]"
}

fn parse_args(args: impl Iterator<Item = String>) -> Result<Options, String> {
    let mut options = Options::default();
    let mut args = args.peekable();
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--light" => options.light = true,
            "--frame-stats" => options.frame_stats = true,
            "--run-for" => {
                let value = args.next().ok_or_else(|| "--run-for needs a number".to_string())?;
                let seconds: f64 = value
                    .parse()
                    .map_err(|_| format!("--run-for: {value} is not a number"))?;
                if !seconds.is_finite() || seconds <= 0. {
                    return Err("--run-for must be a positive number".into());
                }
                options.run_for = Some(Duration::from_secs_f64(seconds));
            }
            "--size" => {
                let value = args.next().ok_or_else(|| "--size needs WIDTHxHEIGHT".to_string())?;
                let (width, height) = value
                    .split_once(['x', 'X'])
                    .ok_or_else(|| format!("--size: {value} is not WIDTHxHEIGHT"))?;
                options.width = width
                    .parse()
                    .map_err(|_| format!("--size: {width} is not a number"))?;
                options.height = height
                    .parse()
                    .map_err(|_| format!("--size: {height} is not a number"))?;
            }
            "--help" | "-h" => return Err(usage().to_string()),
            other => return Err(format!("unknown argument {other}\n{}", usage())),
        }
    }
    Ok(options)
}

fn main() {
    let options = match parse_args(std::env::args().skip(1)) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("{message}");
            std::process::exit(2);
        }
    };

    gpui_kit::application()
        // The full Lucide catalogue rather than the default hundred: the tool
        // palette alone needs a dozen icons outside the default set.
        .with_assets(gpui_kit::assets::AllAssets)
        .run(move |cx: &mut App| {
            shell::init(cx);
            if options.light {
                kicad_sch_ui::theme::apply(ThemeMode::Light, None, cx);
            }
            if let Some(run_for) = options.run_for {
                // The screenshot harness has no way to close the window, so
                // the app closes itself.
                cx.spawn(async move |cx| {
                    cx.background_executor().timer(run_for).await;
                    let _ = cx.update(|cx| cx.quit());
                })
                .detach();
            }

            let bounds = Bounds {
                origin: point(px(0.), px(0.)),
                size: size(px(options.width), px(options.height)),
            };
            let frame_stats = options.frame_stats;
            cx.spawn(async move |cx| {
                let opened = cx.open_window(
                    WindowOptions {
                        window_bounds: Some(WindowBounds::Windowed(bounds)),
                        app_id: Some("org.kicad.eeschema-gpui".into()),
                        titlebar: Some(gpui_kit::TitlebarOptions {
                            title: Some("KiCad Schematic Editor".into()),
                            ..Default::default()
                        }),
                        // Never throttle while unfocused: the frame-time
                        // readout would measure the throttle, not the shell.
                        inactive_frame_interval: None,
                        ..Default::default()
                    },
                    move |window, cx| {
                        let view = cx.new(|cx| {
                            let mut shell = SchematicShell::new(window, cx);
                            if frame_stats {
                                shell.set_frame_stats_enabled(true);
                            }
                            shell
                        });
                        cx.new(|cx| Root::new(view, window, cx))
                    },
                );
                if let Err(error) = opened {
                    eprintln!("could not open the editor window: {error}");
                    let _ = cx.update(|cx| cx.quit());
                }
            })
            .detach();
        });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(args: &[&str]) -> Result<Options, String> {
        parse_args(args.iter().map(|arg| arg.to_string()))
    }

    #[test]
    fn defaults_are_a_dark_window_that_stays_open() {
        let options = parse(&[]).expect("no arguments is valid");
        assert!(!options.light);
        assert!(options.run_for.is_none());
        assert!(options.width > 0. && options.height > 0.);
    }

    #[test]
    fn the_screenshot_flags_parse() {
        let options = parse(&["--light", "--frame-stats", "--run-for", "2.5", "--size", "800x600"])
            .expect("valid arguments");
        assert!(options.light);
        assert!(options.frame_stats);
        assert_eq!(options.run_for, Some(Duration::from_millis(2500)));
        assert_eq!(options.width, 800.);
        assert_eq!(options.height, 600.);
    }

    #[test]
    fn bad_arguments_are_rejected_rather_than_ignored() {
        assert!(parse(&["--nope"]).is_err());
        assert!(parse(&["--run-for"]).is_err());
        assert!(parse(&["--run-for", "-1"]).is_err());
        assert!(parse(&["--size", "wide"]).is_err());
    }
}
