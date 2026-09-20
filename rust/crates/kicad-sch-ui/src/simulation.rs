// SPDX-License-Identifier: GPL-3.0-or-later
//! GPUI simulation model, analysis, and result controls.
use gpui_kit::component::button::{Button, ButtonVariants};
use gpui_kit::component::checkbox::Checkbox;
use gpui_kit::component::input::{Input, InputState};
use gpui_kit::component::select::{Select, SelectState};
use gpui_kit::component::{ActiveTheme, Sizable, StyledExt};
use gpui_kit::prelude::*;
use gpui_kit::{Context, Entity, EventEmitter, TestSupportExt, Window, div, px};

pub(crate) enum SimulationRequest {
    Load(u32),
    Apply(u32, Vec<String>),
    Close,
}
type SimulationChoice = Entity<SelectState<Vec<String>>>;

pub(crate) struct SimulationPanel {
    kind: u32,
    entries: Vec<(String, Entity<InputState>)>,
    status: String,
    booleans: Vec<Option<bool>>,
    choices: Vec<Option<SimulationChoice>>,
    flags: u32,
    plot: Option<Entity<crate::simulation_plot::SimulationPlot>>,
}
impl EventEmitter<SimulationRequest> for SimulationPanel {}
impl SimulationPanel {
    pub(crate) fn new(
        kind: u32,
        rows: Vec<(String, String)>,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) -> Self {
        let mut native_choices = std::collections::BTreeMap::<usize, Vec<String>>::new();
        for (label, value) in &rows {
            if let Some(index) = label
                .strip_prefix("Choice ")
                .and_then(|s| s.parse::<usize>().ok())
            {
                native_choices.entry(index).or_default().push(value.clone());
            }
        }
        let devices: Vec<String> = rows
            .iter()
            .filter(|(k, _)| k == "Device choice")
            .map(|(_, v)| v.clone())
            .collect();
        let model_types: Vec<String> = rows
            .iter()
            .filter(|(k, _)| k == "Type choice")
            .map(|(_, v)| v.clone())
            .collect();
        let wheel = rows
            .iter()
            .find(|(k, _)| k == "Wheel actions")
            .map(|(_, v)| {
                v.split(',')
                    .filter_map(|s| s.parse::<u32>().ok())
                    .collect::<Vec<_>>()
            })
            .unwrap_or_default();
        let plot = if kind == 17 {
            Some(cx.new(|_| {
                crate::simulation_plot::SimulationPlot::new(
                    rows.iter()
                        .filter(|(k, _)| k == "Sample")
                        .filter_map(|(_, v)| {
                            let n: Vec<f64> = v.split(',').filter_map(|s| s.parse().ok()).collect();
                            (n.len() == 3).then(|| [n[0], n[1], n[2]])
                        })
                        .collect(),
                    wheel,
                )
            }))
        } else {
            None
        };
        let vectors: Vec<String> = rows
            .iter()
            .filter(|(k, _)| k == "Available vector")
            .map(|(_, v)| v.clone())
            .collect();
        let rows: Vec<_> = rows
            .into_iter()
            .filter(|(k, _)| {
                !k.starts_with("Choice ")
                    && ![
                        "Sample",
                        "Available vector",
                        "Wheel actions",
                        "Device choice",
                        "Type choice",
                    ]
                    .contains(&k.as_str())
            })
            .collect();
        let booleans = rows
            .iter()
            .map(|(_, value)| match value.as_str() {
                "true" => Some(true),
                "false" => Some(false),
                _ => None,
            })
            .collect();
        let flags = rows.get(1).and_then(|(_, v)| v.parse().ok()).unwrap_or(0);
        let choices = rows
            .iter()
            .enumerate()
            .map(|(index, (label, value))| {
                let options: Vec<String> = if let Some(choices) = native_choices.get(&index) {
                    choices.clone()
                } else if kind == 0 && index == 1 {
                    devices.clone()
                } else if kind == 0 && index == 2 {
                    model_types.clone()
                } else if kind == 15 {
                    [
                        "None",
                        "Pan left/right",
                        "Pan right/left",
                        "Pan up/down",
                        "Zoom",
                        "Zoom horizontally",
                        "Zoom vertically",
                    ]
                    .into_iter()
                    .map(str::to_owned)
                    .collect()
                } else if kind == 17 && index == 0 {
                    vectors.clone()
                } else if kind == 1 && index == 2 {
                    [
                        "User configuration",
                        "Ngspice",
                        "PSpice",
                        "LTspice",
                        "LTspice and PSpice",
                        "HSpice",
                    ]
                    .into_iter()
                    .map(str::to_owned)
                    .collect()
                } else if label.ends_with("Electrical type") {
                    [
                        "Input",
                        "Output",
                        "Bidirectional",
                        "Tri-state",
                        "Passive",
                        "Free",
                        "Unspecified",
                        "Power input",
                        "Power output",
                        "Open collector",
                        "Open emitter",
                        "Unconnected",
                    ]
                    .into_iter()
                    .map(str::to_owned)
                    .collect()
                } else if label.ends_with("Orientation") {
                    ["Right", "Left", "Up", "Down"]
                        .into_iter()
                        .map(str::to_owned)
                        .collect()
                } else {
                    Vec::new()
                };
                if options.is_empty() {
                    None
                } else {
                    let selected = if (kind == 1 && index == 2) || kind == 15 {
                        options
                            .get(value.parse::<usize>().unwrap_or(1))
                            .cloned()
                            .unwrap_or_default()
                    } else {
                        value.clone()
                    };
                    Some(cx.new(|cx| {
                        let mut state = SelectState::new(options, None, window, cx);
                        state.set_selected_value(&selected, window, cx);
                        state
                    }))
                }
            })
            .collect();
        Self {
            kind,
            plot,
            booleans,
            choices,
            flags,
            entries: rows
                .into_iter()
                .map(|(label, value)| {
                    let masked = label == "Password"
                        || label == "Access token"
                        || label == "Connection string";
                    (
                        label,
                        cx.new(|cx| {
                            InputState::new(window, cx)
                                .default_value(value)
                                .masked(masked)
                        }),
                    )
                })
                .collect(),
            status: String::new(),
        }
    }
    fn rows(&self, cx: &Context<Self>) -> Vec<(String, String)> {
        self.entries
            .iter()
            .zip(self.values(cx))
            .map(|((label, _), value)| (label.clone(), value))
            .collect()
    }
    fn pin_map_count(&self, cx: &Context<Self>) -> usize {
        self.entries
            .get(2)
            .and_then(|(_, v)| v.read(cx).value().parse().ok())
            .unwrap_or(0)
    }
    fn add_pin_map(&mut self, copy: Option<usize>, window: &mut Window, cx: &mut Context<Self>) {
        let count = self.pin_map_count(cx);
        let mut rows = self.rows(cx);
        let mut number = 1;
        while rows
            .iter()
            .any(|(label, value)| label == "Map name" && value == &format!("Map {number}"))
        {
            number += 1;
        }
        let mut map = if let Some(index) = copy {
            rows[index..index + 2 + count].to_vec()
        } else {
            let mut map = vec![
                ("Map name".into(), String::new()),
                ("Associated footprints (; separated)".into(), String::new()),
            ];
            for pin in 0..count {
                map.push((
                    format!("Pin {} → pad", rows[3 + pin].1),
                    rows[3 + pin].1.clone(),
                ));
            }
            map
        };
        map[0].1 = format!("Map {number}");
        map[1].1.clear();
        rows.extend(map);
        *self = Self::new(self.kind, rows, window, cx);
        cx.notify();
    }
    fn remove_rows(
        &mut self,
        start: usize,
        count: usize,
        window: &mut Window,
        cx: &mut Context<Self>,
    ) {
        let mut rows = self.rows(cx);
        rows.drain(start..start + count);
        *self = Self::new(self.kind, rows, window, cx);
        cx.notify();
    }
    fn values(&self, cx: &Context<Self>) -> Vec<String> {
        self.entries
            .iter()
            .enumerate()
            .map(|(index, (_, input))| {
                if self.kind == 1 && index == 1 {
                    return self.flags.to_string();
                }
                if let Some(value) = self.booleans[index] {
                    return value.to_string();
                }
                if let Some(choice) = &self.choices[index] {
                    if let Some(value) = choice.read(cx).selected_value() {
                        if self.kind == 15 {
                            return [
                                "None",
                                "Pan left/right",
                                "Pan right/left",
                                "Pan up/down",
                                "Zoom",
                                "Zoom horizontally",
                                "Zoom vertically",
                            ]
                            .iter()
                            .position(|v| *v == value)
                            .unwrap_or(0)
                            .to_string();
                        }
                        if self.kind == 1 && index == 2 {
                            return [
                                "User configuration",
                                "Ngspice",
                                "PSpice",
                                "LTspice",
                                "LTspice and PSpice",
                                "HSpice",
                            ]
                            .iter()
                            .position(|v| *v == value)
                            .unwrap_or(1)
                            .to_string();
                        }
                        return value.clone();
                    }
                }
                input.read(cx).value().to_string()
            })
            .collect()
    }

    pub(crate) fn feedback(&mut self, status: String, cx: &mut Context<Self>) {
        self.status = status;
        cx.notify();
    }
}
impl Render for SimulationPanel {
    fn render(&mut self, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        div().id("simulation-panel").v_flex().gap_2().p_3().size_full().min_h(px(0.)).overflow_hidden()
            .bg(cx.theme().background).border_b_1().border_color(cx.theme().border)
            .on_key_down(cx.listener(|_, event: &gpui_kit::KeyDownEvent, _, cx| {
                if event.keystroke.key == "escape" { cx.emit(SimulationRequest::Close); cx.stop_propagation(); }
            }))
            .child(div().h_flex().flex_wrap().flex_shrink_0().gap_2().items_center()
                .child(div().flex_1().child(match self.kind { 0 => "Simulation Model", 1 => "Simulation Analysis", 2 => "Simulation Results", 3 => "Library Symbol Properties and Fields", 4 => "New Library Symbol", 5 => "Library Pin Table", 6 => "Import Library Symbol", 8 => "Library Symbol Fields Table", 9 => "Related Library Symbol Fields", 10 => "Remote Symbol Providers", 13 => "Simulation Value Format", 14 => "User-defined Signal", 15 => "Simulator Preferences", 16 => "Update Fields from Parent", 17 => "Simulation Plot",18 => "Library Pin Maps",19 => "Model Parameters and Pin Assignments",20 => "Model Library and IBIS Selection",21 => "IBIS / SPICE Parser Report",22 => "Schematic Symbol Pin Maps", _ => "Library Connection Settings" }))
                .when(self.kind < 3, |el| el.child(Button::new("simulation-model").label("Model").small().on_click(cx.listener(|_, _, _, cx| cx.emit(SimulationRequest::Load(0)))))
                .child(Button::new("simulation-analysis").label("Analysis").small().on_click(cx.listener(|_, _, _, cx| cx.emit(SimulationRequest::Load(1)))))
                .child(Button::new("simulation-results").label("Refresh Results").small().on_click(cx.listener(|_, _, _, cx| cx.emit(SimulationRequest::Load(2))))))
                .when([0,19,20,21].contains(&self.kind),|el|el
                    .child(Button::new("simulation-model-library").label("Library / IBIS").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(20)))))
                    .child(Button::new("simulation-model-details").label("Parameters and Pins").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(19)))))
                    .child(Button::new("simulation-parser-report").label("Parser Report").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(21))))))
                 .when([3,5,16,18,22].contains(&self.kind),|el|el.child(Button::new("schematic-pin-maps").label("Schematic Pin Maps").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(22))))))
                .when([3,5,16,18,22].contains(&self.kind),|el|el.child(Button::new("library-pin-maps").label("Pin Maps").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(18))))))
                .when(self.kind == 3 || self.kind == 16, |el| el.child(Button::new("library-update-parent").label("Update from Parent").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(16))))))
                .when([2,13,14,15,17].contains(&self.kind), |el| el
                    .child(Button::new("simulation-plot").label("Plot").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(17)))))
                    .child(Button::new("simulation-signal").label("User Signal").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(14)))))
                    .child(Button::new("simulation-preferences").label("Preferences").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(15)))))
                    .child(Button::new("simulation-format").label("Value Format").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(13)))))
                    .child(Button::new("simulation-return-results").label("Results").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(2))))))
                .child(Button::new("simulation-close").label("Close").small().on_click(cx.listener(|_, _, _, cx| cx.emit(SimulationRequest::Close)))))
            .child(div().id("simulation-fields").v_flex().min_h(px(0.)).gap_2().overflow_y_scroll()
                .when(self.plot.is_none(), |body| body.flex_1())
                .when(self.plot.is_some(), |body| body.flex_shrink_0().max_h(px(200.)))
            .when(self.kind==20,|el|el.child(div().text_xs().child("Load a library and choose its model or IBIS component. Apply Model in Parameters and Pins saves the selection to the symbol.")))
            .when(self.kind==22,|el|el.child(div().text_xs().child("Apply updates this symbol definition throughout the schematic. The change can be undone.")))
            .when(self.kind==18,|el|el.child(div().text_xs().child("Save writes the library's named pin maps and footprint associations. Blank pad cells use the original pin number.")))
            .when(self.kind == 1, |el| el.child(div().text_xs().child("Analyses: .op · .tran 1u 10m · .ac dec 100 1 1Meg · .dc V1 0 5 0.1. Results refresh after the engine finishes.")))
            .when(self.kind == 0, |el| el.child(div().text_xs().child("Set device/type and model parameters, or a library path and model name. Pin assignments use KiCad's Sim.Pins syntax. Apply validates with the simulation model loader.")))
            .when((3..=10).contains(&self.kind) || self.kind==16, |el| el.child(div().text_xs().child(if self.kind == 10 { "Configure provider metadata URLs and the destination for downloaded symbols. Save new providers before refreshing metadata." } else if self.kind == 7 { "Save updates this library connection configuration. Credentials remain masked." } else { "Save writes the library file. Reopen or update schematic symbols to use the edited definition." })))
            .children(self.entries.iter().enumerate().filter(|(index,(label,_))| !(self.kind==20 && *index>=3 && self.choices.get(3).and_then(Option::as_ref).is_none()) && !["Field schema","Pin map schema","Pin count","Pin definition","Model schema"].contains(&label.as_str()) && !(self.kind == 5 && label.ends_with(" / Identity")) && !([8,9].contains(&self.kind) && *index == 1)).map(|(index, (label, input))| {
                let editor = if self.kind==21 || ([18,19,20,22].contains(&self.kind)&&index==0) || (self.kind==17 && index>0) || label == "Field name (required)" || label.starts_with("Inherited: ") || self.kind == 2 || ([0,3,5].contains(&self.kind) && index == 0) || ([7,8,9].contains(&self.kind) && index < 2) || (self.kind == 10 && index >= 3 && (index - 3) % 4 >= 2) {
                    div().child(input.read(cx).value().to_string()).into_any_element()
                } else if self.kind == 1 && index == 1 {
                    div().v_flex().gap_1().children([
                        (0x10,"Resolve include paths"),(0x20,"Normalize passive values"),(0x40,"Save all voltages"),
                        (0x80,"Save all currents"),(0x100,"Save power dissipation"),(0x200,"Current sheet only"),(0x800,"Save events")
                    ].into_iter().map(|(mask,label)| Checkbox::new(("simulation-flag", mask as usize)).label(label).checked(self.flags & mask != 0)
                        .on_click(cx.listener(move |this, checked: &bool, _, cx| { if *checked { this.flags |= mask; } else { this.flags &= !mask; } cx.notify(); })))).into_any_element()
                } else if let Some(value)=self.booleans[index] {
                    Checkbox::new(("simulation-boolean",index)).checked(value).on_click(cx.listener(move |this, checked: &bool, _, cx| { this.booleans[index]=Some(*checked); cx.notify(); })).into_any_element()
                } else if let Some(choice)=&self.choices[index] { Select::new(choice).into_any_element() }
                else { Input::new(input).into_any_element() };
                div().id(("simulation-property",index)).test_support().h_flex().flex_shrink_0().gap_2().items_center().child(div().w(px(210.)).text_xs().child(if self.kind==1 && index==2 { "Compatibility".to_string() } else { label.clone() }))
                    .child(div().flex_1().child(editor))
                    .when([18,22].contains(&self.kind) && label=="Map name",|el|el
                        .child(Button::new(("map-remove",index)).label("Remove Map").small().on_click(cx.listener(move |this,_,window,cx|{let size=2+this.pin_map_count(cx);this.remove_rows(index,size,window,cx);})))
                        .child(Button::new(("map-copy",index)).label("Copy Map").small().on_click(cx.listener(move |this,_,window,cx|this.add_pin_map(Some(index),window,cx))))
                        .child(Button::new(("map-identity",index)).label("Reset to Identity").small().on_click(cx.listener(move |this,_,window,cx|{let mut rows=this.rows(cx);for pin in 0..this.pin_map_count(cx){rows[index+2+pin].1=rows[3+pin].1.clone();}*this=Self::new(this.kind,rows,window,cx);cx.notify();}))))
                    .when(self.kind==14 && index%2==0,|el|el.child(Button::new(("remove-user-signal",index)).label("Remove").small().on_click(cx.listener(move |this,_,window,cx|this.remove_rows(index,2,window,cx)))))
                    .when(self.kind == 3 && index >= 10 && (index-10)%2 == 0 && label == "Field name",|el|el.child(
                        Button::new(("library-field-remove",index)).label("Remove Field").small().on_click(cx.listener(move |this,_,window,cx|this.remove_rows(index,2,window,cx)))))
                    .when(self.kind == 5 && index >= 1 && (index-1)%14 == 0,|el|el.child(
                        Button::new(("library-pin-remove",index)).label("Remove Pin").small().on_click(cx.listener(move |this,_,window,cx|this.remove_rows(index,14,window,cx)))))
                    .when(self.kind == 10 && index >= 3 && (index-3)%4 == 0, |el| el.child(div().h_flex().gap_1()
                        .child(Button::new(("provider-refresh",index)).label("Refresh Metadata").small().on_click(cx.listener(move |this,_,_,cx| {
                            let url=this.entries[index].1.read(cx).value().to_string();cx.emit(SimulationRequest::Apply(11,vec![url]));
                        })))
                        .child(Button::new(("provider-signout",index)).label("Sign Out").small().on_click(cx.listener(move |this,_,_,cx| {
                            let url=this.entries[index].1.read(cx).value().to_string();cx.emit(SimulationRequest::Apply(12,vec![url]));
                        })))
                        .child(Button::new(("provider-remove",index)).label("Remove").small().on_click(cx.listener(move |this,_,_,cx| {
                            this.entries.drain(index..index+4);this.booleans.drain(index..index+4);this.choices.drain(index..index+4);cx.notify();
                        })))))
            }))
            )
            .child(div().h_flex().flex_wrap().flex_shrink_0().gap_2()
                .when(self.kind == 3,|el|el.child(Button::new("library-field-add").label("Add Field").small().on_click(cx.listener(|this,_,window,cx| {
                    let mut rows=this.rows(cx);rows.extend([("Field name".into(),"NewField".into()),("Field value".into(),String::new())]);
                    *this=Self::new(this.kind,rows,window,cx);cx.notify();
                }))))
                .when(self.kind == 5,|el|el.child(Button::new("library-pin-add").label("Add Pin").small().on_click(cx.listener(|this,_,window,cx| {
                    let mut rows=this.rows(cx);let number=(rows.len()-1)/14+1;
                    for (name,value) in [("Number",""),("Name",""),("Electrical type","Passive"),("Shape","Line"),("Orientation","Right"),("Length (mm)","2.54"),("X (mm)","0"),("Y (mm)","0"),("Name size (mm)","1.27"),("Number size (mm)","1.27"),("Unit","1"),("Body style","1"),("Visible","true"),("Identity","")] {
                        rows.push((format!("Pin {number} / {name}"),value.into()));
                    }
                    *this=Self::new(this.kind,rows,window,cx);cx.notify();
                }))))
                .when(self.kind == 10, |el| el
                    .child(Button::new("provider-add").label("Add Provider").small().on_click(cx.listener(|this,_,window,cx| {
                        let number=(this.entries.len()-3)/4+1;
                        for label in ["Metadata URL","Display name","Account","Authentication"] {
                            this.entries.push((format!("Provider {number} / {label}"),cx.new(|cx|InputState::new(window,cx))));
                            this.booleans.push(None);this.choices.push(None);
                        }
                        cx.notify();
                    })))
                    .child(Button::new("provider-reload").label("Reload Settings").small().on_click(cx.listener(|_,_,_,cx|cx.emit(SimulationRequest::Load(10))))))
                .when([18,22].contains(&self.kind),|el|el.child(Button::new("pin-map-add").label("Add Pin Map").small().on_click(cx.listener(|this,_,window,cx|this.add_pin_map(None,window,cx)))))
                .when(self.kind != 2 && self.kind !=21, |el| el.child(Button::new("simulation-apply").label(if self.kind==22 { "Apply to Schematic" } else if self.kind==20 { "Load Selection" } else if self.kind==19 { "Apply Model" } else if self.kind == 17 { "Plot Vector" } else if self.kind == 14 { "Evaluate Signal" } else if self.kind == 15 { "Save Preferences" } else if self.kind == 0 { "Apply Model" } else if self.kind == 1 { "Run Analysis" } else if self.kind == 10 || self.kind == 7 || self.kind == 13 { "Save Settings" } else { "Save to Library" }).primary().small()
                    .on_click(cx.listener(|this, _, _, cx| { let values = this.values(cx); cx.emit(SimulationRequest::Apply(this.kind, values)); }))))
                .when(self.kind == 1 || self.kind == 2, |el| el.child(Button::new("simulation-stop").label("Stop").small().on_click(cx.listener(|_, _, _, cx| cx.emit(SimulationRequest::Apply(2, Vec::new())))))))
            .when_some(self.plot.clone(),|el,plot|el.child(div().flex_1().min_h(px(160.)).child(plot)))
            .child(div().text_xs().child(self.status.clone()))
    }
}
