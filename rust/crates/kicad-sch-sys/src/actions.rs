// This program source code file is part of KiCad, a free EDA CAD application.
// Copyright The KiCad Developers, see AUTHORS.txt for contributors.
// SPDX-License-Identifier: GPL-3.0-or-later

use crate::Error;

/// An owned snapshot of one entry in KiCad's process-wide action registry.
///
/// Names, labels, icons and hotkeys originate in `TOOL_ACTION`. Registry presence
/// does not imply that a headless session implements the action's handler.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct ActionInfo {
    /// Stable dotted action name, used for dispatch and persistence.
    pub name: String,
    /// Translated toolbar label.
    pub friendly_name: String,
    /// Translated menu label, possibly containing wx mnemonic markers.
    pub menu_label: String,
    /// Translated tooltip, including the shortcut.
    pub tooltip: String,
    /// Translated long description.
    pub description: String,
    /// Icon base name; empty if none.
    pub icon_name: String,
    /// Owning tool's dotted name.
    pub tool_name: String,
    /// Process-local action identifier; do not persist.
    pub id: i32,
    /// KiCad UI event identifier.
    pub ui_id: i32,
    /// Default primary shortcut, in KiCad's numeric key representation.
    pub default_hotkey: i32,
    /// Default alternate shortcut.
    pub default_hotkey_alt: i32,
    /// Configured primary shortcut.
    pub hotkey: i32,
    /// Configured alternate shortcut.
    pub hotkey_alt: i32,
    /// Platform-specific textual primary shortcut; empty when unbound.
    pub hotkey_name: String,
    /// Platform-specific textual alternate shortcut; empty when unbound.
    pub hotkey_alt_name: String,
    /// KiCad scope: context (1), active tools (2), or global (3).
    pub scope: i32,
    /// KiCad flags: activation (bit 0), notification (bit 1).
    pub flags: u32,
}

/// Read the action registry, sorted by stable action name.
///
/// Needs neither a session nor a loaded document. All strings are copied out of
/// the C++ process-lifetime snapshot. The registry captures labels and configured
/// hotkeys on first access; it is not a live action enablement or hotkey API.
#[cfg(not(ksch_linked))]
pub fn actions() -> Result<Vec<ActionInfo>, Error> {
    Err(Error::NoHost)
}

/// Read the action registry, sorted by stable action name.
///
/// Needs neither a session nor a loaded document. All strings are copied out of
/// the C++ process-lifetime snapshot. The registry captures labels and configured
/// hotkeys on first access; it is not a live action enablement or hotkey API.
#[cfg(ksch_linked)]
pub fn actions() -> Result<Vec<ActionInfo>, Error> {
    use crate::{ffi, Status};
    use std::ffi::CStr;

    // SAFETY: this function reads a compile-time constant.
    let library = unsafe { ffi::ksch_abi_version() };
    if library != ffi::KSCH_ABI_VERSION {
        return Err(Error::AbiMismatch {
            library,
            expected: ffi::KSCH_ABI_VERSION,
        });
    }

    fn check(status: u32) -> Result<(), Error> {
        if status == ffi::ksch_status_KSCH_OK {
            Ok(())
        } else {
            Err(Error::Failed {
                status: Status::from_code(status),
                message: "reading the KiCad action registry".into(),
            })
        }
    }

    // SAFETY: each pointer comes from a successful registry ABI call, which
    // guarantees immutable, non-null, process-lifetime NUL-terminated strings.
    unsafe fn string(raw: *const std::os::raw::c_char) -> String {
        unsafe { CStr::from_ptr(raw) }
            .to_string_lossy()
            .into_owned()
    }

    // SAFETY: registry enumeration explicitly needs no runtime or session.
    let count = unsafe { ffi::ksch_action_count() };
    let mut result = Vec::with_capacity(count as usize);
    for index in 0..count {
        let mut raw = ffi::ksch_action::default();
        let mut primary = std::ptr::null();
        let mut alternate = std::ptr::null();
        // SAFETY: valid index and writable out-parameters owned by this call.
        unsafe {
            check(ffi::ksch_action_at(index, &mut raw))?;
            check(ffi::ksch_action_hotkey_names(
                index,
                &mut primary,
                &mut alternate,
            ))?;
            result.push(ActionInfo {
                name: string(raw.name),
                friendly_name: string(raw.friendly_name),
                menu_label: string(raw.menu_label),
                tooltip: string(raw.tooltip),
                description: string(raw.description),
                icon_name: string(raw.icon_name),
                tool_name: string(raw.tool_name),
                id: raw.id,
                ui_id: raw.ui_id,
                default_hotkey: raw.default_hotkey,
                default_hotkey_alt: raw.default_hotkey_alt,
                hotkey: raw.hotkey,
                hotkey_alt: raw.hotkey_alt,
                hotkey_name: string(primary),
                hotkey_alt_name: string(alternate),
                scope: raw.scope,
                flags: raw.flags,
            });
        }
    }
    result.sort_by(|left, right| left.name.cmp(&right.name));
    Ok(result)
}

#[cfg(all(test, not(ksch_linked)))]
mod tests {
    #[test]
    fn no_host_registry_fails_explicitly() {
        assert!(matches!(super::actions(), Err(crate::Error::NoHost)));
    }
}
