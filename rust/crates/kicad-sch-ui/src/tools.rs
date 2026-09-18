// This program is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation, either version 3 of the License, or (at your option)
// any later version.
//
// SPDX-License-Identifier: GPL-3.0-or-later

//! The tool palette: which drawing tools exist and how they present.
//!
//! Every entry names a real KiCad `TOOL_ACTION` (the strings come from
//! `eeschema/tools/sch_actions.cpp` and `common/tool/actions.cpp`), so when
//! `ACTION_REGISTRY` starts enumerating actions headlessly this table becomes a
//! lookup into it rather than a second source of truth. The icons and the
//! ordering are the shell's own business and stay here either way.

use gpui_kit::CursorStyle;
use gpui_kit::assets::IconName;

use crate::input::ToolId;

/// A drawing tool the user can have active.
///
/// Ordered as the left palette presents them: pointer tools, then the things
/// that carry a net, then the things that carry a name, then sheets, then
/// graphics, then the utilities.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Tool {
    /// Pick and edit existing items. The tool a session starts in.
    #[default]
    Select,
    /// Click a net to highlight everything it reaches.
    HighlightNet,
    /// Place a symbol from a library.
    PlaceSymbol,
    /// Place a power port.
    PlacePower,
    /// Draw wires.
    DrawWire,
    /// Draw buses.
    DrawBus,
    /// Place a wire-to-bus entry.
    PlaceBusEntry,
    /// Place a junction dot.
    PlaceJunction,
    /// Place a no-connect flag.
    PlaceNoConnect,
    /// Place a local net label.
    PlaceLabel,
    /// Place a global label.
    PlaceGlobalLabel,
    /// Place a hierarchical label.
    PlaceHierLabel,
    /// Draw a hierarchical sheet.
    DrawSheet,
    /// Place free text.
    PlaceText,
    /// Draw a graphic rectangle.
    DrawRectangle,
    /// Draw a graphic circle.
    DrawCircle,
    /// Draw a graphic arc.
    DrawArc,
    /// Draw graphic lines.
    DrawLine,
    /// Place a bitmap image.
    PlaceImage,
    /// Click items to delete them.
    DeleteItems,
    /// Measure a distance.
    Measure,
}

/// How a tool presents in the palette and what it means to the host.
#[derive(Clone, Copy, Debug)]
pub struct ToolSpec {
    /// The tool itself.
    pub tool: Tool,
    /// The KiCad `TOOL_ACTION` name this tool activates.
    pub id: ToolId,
    /// Element id for the palette button, and the handle tests reach it by.
    pub button_id: &'static str,
    /// Human-readable name, shown in the tooltip and the command palette.
    pub label: &'static str,
    /// One line of help, shown under the label in the tooltip.
    pub description: &'static str,
    /// The palette icon.
    pub icon: IconName,
    /// Default hotkey, as a gpui keystroke string, or `None` for tools KiCad
    /// does not bind by default.
    pub shortcut: Option<&'static str>,
    /// The pointer shape while the tool is active over the canvas.
    pub cursor: CursorStyle,
    /// Whether the palette draws a separator *before* this tool, which is how
    /// the six groups above are made visible without labelling them.
    pub group_break: bool,
}

impl Tool {
    /// The presentation and identity of this tool.
    pub fn spec(self) -> &'static ToolSpec {
        // A linear scan over twenty-one entries is faster than a hash and
        // keeps the table the single, readable definition.
        TOOLS
            .iter()
            .find(|spec| spec.tool == self)
            .expect("every Tool variant has a row in TOOLS, which a test asserts")
    }

    /// The KiCad action name this tool activates.
    pub fn id(self) -> ToolId {
        self.spec().id
    }

    /// The tool's display name.
    pub fn label(self) -> &'static str {
        self.spec().label
    }
}

/// Every tool, in palette order.
pub static TOOLS: &[ToolSpec] = &[
    ToolSpec {
        tool: Tool::Select,
        id: ToolId("common.InteractiveSelection.selectionTool"),
        button_id: "tool-select",
        label: "Select",
        description: "Select and edit items (Esc)",
        // Escape is bound to `CancelTool`, which also returns here, so the
        // palette button carries no binding of its own.
        icon: IconName::MousePointer2,
        shortcut: None,
        cursor: CursorStyle::Arrow,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::HighlightNet,
        id: ToolId("eeschema.EditorControl.highlightNetTool"),
        button_id: "tool-highlight",
        label: "Highlight Net",
        description: "Highlight every wire on the net under the cursor",
        icon: IconName::Highlighter,
        shortcut: Some("`"),
        cursor: CursorStyle::PointingHand,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceSymbol,
        id: ToolId("eeschema.InteractiveDrawing.placeSymbol"),
        button_id: "tool-symbol",
        label: "Place Symbol",
        description: "Choose a symbol from a library and place it",
        icon: IconName::Component,
        shortcut: Some("a"),
        cursor: CursorStyle::Crosshair,
        group_break: true,
    },
    ToolSpec {
        tool: Tool::PlacePower,
        id: ToolId("eeschema.InteractiveDrawing.placePowerSymbol"),
        button_id: "tool-power",
        label: "Place Power Port",
        description: "Place a power or ground symbol",
        icon: IconName::PlugZap,
        shortcut: Some("p"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::DrawWire,
        id: ToolId("eeschema.InteractiveDrawingLineWireBus.drawWires"),
        button_id: "tool-wire",
        label: "Draw Wire",
        description: "Draw a wire between two points",
        icon: IconName::Spline,
        shortcut: Some("w"),
        cursor: CursorStyle::Crosshair,
        group_break: true,
    },
    ToolSpec {
        tool: Tool::DrawBus,
        id: ToolId("eeschema.InteractiveDrawingLineWireBus.drawBuses"),
        button_id: "tool-bus",
        label: "Draw Bus",
        description: "Draw a bus between two points",
        icon: IconName::Route,
        shortcut: Some("b"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceBusEntry,
        id: ToolId("eeschema.InteractiveDrawing.placeBusWireEntry"),
        button_id: "tool-bus-entry",
        label: "Place Bus Entry",
        description: "Place a wire-to-bus entry",
        icon: IconName::CornerDownRight,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceJunction,
        id: ToolId("eeschema.InteractiveDrawing.placeJunction"),
        button_id: "tool-junction",
        label: "Place Junction",
        description: "Place a junction dot where wires cross",
        icon: IconName::CircleDot,
        shortcut: Some("j"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceNoConnect,
        id: ToolId("eeschema.InteractiveDrawing.placeNoConnect"),
        button_id: "tool-no-connect",
        label: "Place No-Connect Flag",
        description: "Mark a pin as intentionally unconnected",
        icon: IconName::X,
        shortcut: Some("q"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceLabel,
        id: ToolId("eeschema.InteractiveDrawing.placeLabel"),
        button_id: "tool-label",
        label: "Place Net Label",
        description: "Name a net inside this sheet",
        icon: IconName::Tag,
        shortcut: Some("l"),
        cursor: CursorStyle::Crosshair,
        group_break: true,
    },
    ToolSpec {
        tool: Tool::PlaceGlobalLabel,
        id: ToolId("eeschema.InteractiveDrawing.placeGlobalLabel"),
        button_id: "tool-global-label",
        label: "Place Global Label",
        description: "Name a net across the whole design",
        icon: IconName::Globe,
        shortcut: Some("ctrl-l"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceHierLabel,
        id: ToolId("eeschema.InteractiveDrawing.placeHierarchicalLabel"),
        button_id: "tool-hier-label",
        label: "Place Hierarchical Label",
        description: "Name a net at this sheet's boundary",
        icon: IconName::Network,
        shortcut: Some("h"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::DrawSheet,
        id: ToolId("eeschema.InteractiveDrawing.drawSheet"),
        button_id: "tool-sheet",
        label: "Draw Sheet",
        description: "Add a hierarchical sub-sheet",
        icon: IconName::SquareDashed,
        shortcut: Some("s"),
        cursor: CursorStyle::Crosshair,
        group_break: true,
    },
    ToolSpec {
        tool: Tool::PlaceText,
        id: ToolId("eeschema.InteractiveDrawing.placeSchematicText"),
        button_id: "tool-text",
        label: "Place Text",
        description: "Add a free-standing text note",
        icon: IconName::Type,
        shortcut: Some("t"),
        cursor: CursorStyle::IBeam,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::DrawRectangle,
        id: ToolId("eeschema.InteractiveDrawing.drawRectangle"),
        button_id: "tool-rectangle",
        label: "Draw Rectangle",
        description: "Draw a graphic rectangle",
        icon: IconName::Square,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: true,
    },
    ToolSpec {
        tool: Tool::DrawCircle,
        id: ToolId("eeschema.InteractiveDrawing.drawCircle"),
        button_id: "tool-circle",
        label: "Draw Circle",
        description: "Draw a graphic circle",
        icon: IconName::Circle,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::DrawArc,
        id: ToolId("eeschema.InteractiveDrawing.drawArc"),
        button_id: "tool-arc",
        label: "Draw Arc",
        description: "Draw a graphic arc",
        icon: IconName::Spline,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::DrawLine,
        id: ToolId("eeschema.InteractiveDrawingLineWireBus.drawLines"),
        button_id: "tool-line",
        label: "Draw Lines",
        description: "Draw graphic lines that carry no net",
        icon: IconName::PenLine,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::PlaceImage,
        id: ToolId("eeschema.InteractiveDrawing.placeImage"),
        button_id: "tool-image",
        label: "Place Image",
        description: "Place a bitmap image on the sheet",
        icon: IconName::Image,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
    ToolSpec {
        tool: Tool::DeleteItems,
        id: ToolId("common.Interactive.deleteTool"),
        button_id: "tool-delete",
        label: "Delete Items",
        description: "Click items to delete them",
        icon: IconName::Eraser,
        shortcut: None,
        cursor: CursorStyle::Crosshair,
        group_break: true,
    },
    ToolSpec {
        tool: Tool::Measure,
        id: ToolId("common.Interactive.measureTool"),
        button_id: "tool-measure",
        label: "Measure",
        description: "Measure the distance between two points",
        icon: IconName::Ruler,
        shortcut: Some("ctrl-shift-m"),
        cursor: CursorStyle::Crosshair,
        group_break: false,
    },
];

/// Look a tool up by the element id of its palette button.
pub fn tool_for_button(button_id: &str) -> Option<Tool> {
    TOOLS
        .iter()
        .find(|spec| spec.button_id == button_id)
        .map(|spec| spec.tool)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    /// Every variant has to appear, or `Tool::spec` panics at runtime. This is
    /// the test that licenses the `expect` in it.
    #[test]
    fn every_tool_variant_has_a_row() {
        let variants = [
            Tool::Select,
            Tool::HighlightNet,
            Tool::PlaceSymbol,
            Tool::PlacePower,
            Tool::DrawWire,
            Tool::DrawBus,
            Tool::PlaceBusEntry,
            Tool::PlaceJunction,
            Tool::PlaceNoConnect,
            Tool::PlaceLabel,
            Tool::PlaceGlobalLabel,
            Tool::PlaceHierLabel,
            Tool::DrawSheet,
            Tool::PlaceText,
            Tool::DrawRectangle,
            Tool::DrawCircle,
            Tool::DrawArc,
            Tool::DrawLine,
            Tool::PlaceImage,
            Tool::DeleteItems,
            Tool::Measure,
        ];
        assert_eq!(variants.len(), TOOLS.len());
        for tool in variants {
            assert_eq!(tool.spec().tool, tool);
        }
    }

    #[test]
    fn button_ids_tool_ids_and_shortcuts_are_unique() {
        let mut buttons = HashSet::new();
        let mut ids = HashSet::new();
        let mut keys = HashSet::new();
        for spec in TOOLS {
            assert!(
                buttons.insert(spec.button_id),
                "duplicate {}",
                spec.button_id
            );
            assert!(ids.insert(spec.id), "duplicate {}", spec.id);
            if let Some(key) = spec.shortcut {
                assert!(keys.insert(key), "duplicate shortcut {key}");
            }
        }
    }

    #[test]
    fn tool_ids_are_namespaced_kicad_action_names() {
        for spec in TOOLS {
            let name = spec.id.as_str();
            assert!(
                name.starts_with("eeschema.") || name.starts_with("common."),
                "{name} is not a KiCad action name"
            );
            assert!(name.matches('.').count() >= 2, "{name} lacks a tool class");
        }
    }

    #[test]
    fn buttons_resolve_back_to_their_tool() {
        assert_eq!(tool_for_button("tool-wire"), Some(Tool::DrawWire));
        assert_eq!(tool_for_button("tool-nonexistent"), None);
    }
}
