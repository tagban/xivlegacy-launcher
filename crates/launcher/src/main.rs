//! XIVLegacy: one program for FINAL FANTASY XIV 1.23b on a private server.
//! The first run sets the game up (from files the player has, or archive.org);
//! after that it opens straight to the server list: pick a server and Join.
#![cfg_attr(windows, windows_subsystem = "windows")]

mod config;
mod game;
mod lan;
mod scan;
mod sqex;
mod web;

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, Sender, channel};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use eframe::egui;
use xivlegacy_install::progress::{Counter, Phase, Report};
use xivlegacy_install::setup::{self, DiscSource, ISO_7Z, PATCHES_ZIP, PatchSource, Plan};
use xivlegacy_install::{directx, verify};

use config::{Config, Server};

const VERSION: &str = env!("CARGO_PKG_VERSION");
const DIRECTX_URL: &str = "https://www.microsoft.com/en-us/download/details.aspx?id=8109";

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_title("XIVLegacy")
            .with_inner_size([560.0, 500.0])
            .with_min_inner_size([480.0, 420.0]),
        ..Default::default()
    };
    eframe::run_native("XIVLegacy", options, Box::new(|cc| Ok(Box::new(App::new(cc)))))
}

/// Progress of the running setup, written by the installer's threads.
#[derive(Default)]
struct Progress {
    phase: Option<Phase>,
    done: u64,
    total: u64,
    detail: String,
    phase_start: Option<(Phase, Instant, u64)>,
    rate: f64,
}

struct Shared(Arc<Mutex<Progress>>);

impl Report for Shared {
    fn report(&self, phase: Phase, done: u64, total: u64, detail: &str) {
        let mut p = self.0.lock().unwrap();
        let (start, base) = match p.phase_start {
            Some((ph, t, b)) if ph == phase => (t, b),
            _ => {
                p.phase_start = Some((phase, Instant::now(), done));
                (Instant::now(), done)
            }
        };
        let secs = start.elapsed().as_secs_f64();
        p.rate = if secs > 2.0 { done.saturating_sub(base) as f64 / secs } else { 0.0 };
        p.phase = Some(phase);
        p.done = done;
        p.total = total;
        p.detail = detail.to_string();
    }
}

enum Msg {
    Scanned(scan::Found),
    Installed(Result<PathBuf, String>),
    Checked(Result<(PathBuf, Option<String>), String>),
    Started(Result<(), String>),
    /// (host, whether it answers, its local address if it answers only there)
    Alive(String, bool, Option<String>),
    Status(String),
}

#[derive(PartialEq)]
enum Screen {
    Setup,
    Installing,
    Launcher,
}

struct App {
    cfg: Config,
    screen: Screen,
    tx: Sender<Msg>,
    rx: Receiver<Msg>,
    // setup
    scanning: bool,
    disc: Option<PathBuf>,
    patches: Option<PathBuf>,
    install_dir: PathBuf,
    progress: Arc<Mutex<Progress>>,
    // launcher
    password: String,
    busy: bool,
    status: String,
    error: bool,
    alive: HashMap<String, bool>,
    polled: Option<Instant>,
    editing: bool,
    new_name: String,
    new_host: String,
}

impl App {
    fn new(cc: &eframe::CreationContext) -> Self {
        cc.egui_ctx.set_visuals(egui::Visuals::dark());
        cc.egui_ctx.style_mut(|s| {
            s.spacing.item_spacing = egui::vec2(8.0, 8.0);
            s.spacing.button_padding = egui::vec2(12.0, 6.0);
        });
        let cfg = Config::load();
        let (tx, rx) = channel();
        let ready = cfg.game_dir.as_deref().is_some_and(game::installed);
        let password = if cfg.remember && !cfg.username.is_empty() {
            config::password(cfg.host(), &cfg.username).unwrap_or_default()
        } else {
            String::new()
        };
        let mut app = App {
            screen: if ready { Screen::Launcher } else { Screen::Setup },
            tx,
            rx,
            scanning: false,
            disc: None,
            patches: None,
            install_dir: config::default_game_dir(),
            progress: Arc::default(),
            password,
            busy: false,
            status: String::new(),
            error: false,
            alive: HashMap::new(),
            polled: None,
            editing: false,
            new_name: String::new(),
            new_host: String::new(),
            cfg,
        };
        if !ready {
            app.start_scan(&cc.egui_ctx);
        }
        app
    }

    fn say(&mut self, text: impl Into<String>, error: bool) {
        self.status = text.into();
        self.error = error;
    }

    fn start_scan(&mut self, ctx: &egui::Context) {
        self.scanning = true;
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        thread::spawn(move || {
            let _ = tx.send(Msg::Scanned(scan::scan()));
            ctx.request_repaint();
        });
    }

    fn start_install(&mut self, ctx: &egui::Context) {
        let out = self.install_dir.clone();
        let plan = Plan {
            disc: self.disc.clone().map_or(DiscSource::Download, DiscSource::File),
            patches: self.patches.clone().map_or(PatchSource::Download, |p| PatchSource::Files(vec![p])),
            cache: setup::default_cache(&out),
            out: out.clone(),
        };
        *self.progress.lock().unwrap() = Progress::default();
        let report = Arc::new(Shared(self.progress.clone()));
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        self.screen = Screen::Installing;
        self.say("", false);
        thread::spawn(move || {
            let r = (|| -> anyhow::Result<PathBuf> {
                if let Some(parent) = plan.out.parent() {
                    std::fs::create_dir_all(parent)?;
                }
                setup::run(&plan, report)?;
                // Downloads and any unpacked disc image are no longer needed.
                let _ = std::fs::remove_dir_all(&plan.cache);
                Ok(plan.out.clone())
            })();
            let _ = tx.send(Msg::Installed(r.map_err(|e| format!("{e:#}"))));
            ctx.request_repaint();
        });
    }

    fn use_existing(&mut self, ctx: &egui::Context) {
        let Some(dir) = rfd::FileDialog::new().set_title("Choose your FINAL FANTASY XIV 1.23b folder").pick_folder()
        else {
            return;
        };
        let iso = self.disc.clone().filter(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("iso")));
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        self.busy = true;
        self.say("Checking the folder…", false);
        thread::spawn(move || {
            let r = (|| -> anyhow::Result<(PathBuf, Option<String>)> {
                let c = verify::check(&dir, false, &Counter::default())?;
                if !c.ok() {
                    anyhow::bail!(
                        "That folder isn't a complete 1.23b install ({} files missing, {} different, game.ver {}).",
                        c.missing.len(),
                        c.wrong.len(),
                        c.game_version.as_deref().unwrap_or("missing")
                    );
                }
                let mut note = None;
                if cfg!(windows) && !directx::present(&dir) {
                    match iso {
                        Some(iso) => directx::install(&iso, &dir)?,
                        None => note = Some("DirectX 9 files are missing; the game may not start until Microsoft's DirectX End-User Runtime is installed.".into()),
                    }
                }
                Ok((dir, note))
            })();
            let _ = tx.send(Msg::Checked(r.map_err(|e| format!("{e:#}"))));
            ctx.request_repaint();
        });
    }

    fn join(&mut self, ctx: &egui::Context, create: bool) {
        let user = self.cfg.username.trim().to_string();
        let pass = self.password.clone();
        if user.is_empty() || pass.is_empty() {
            return self.say("Enter your username and password.", true);
        }
        let Some(dir) = self.cfg.game_dir.clone() else { return };
        let host = self.cfg.host().trim().to_string();
        if let Err(e) = game::host_slot(&host) {
            return self.say(format!("{e}"), true);
        }
        self.cfg.username = user.clone();
        self.cfg.save();
        let remember = self.cfg.remember;
        let known_lan = self.cfg.servers[self.cfg.server].lan.clone();
        self.busy = true;
        self.say(if create { "Creating your account…" } else { "Signing in…" }, false);
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        thread::spawn(move || {
            let r = (|| -> anyhow::Result<()> {
                let target = match reach(&host, known_lan.as_deref(), &tx) {
                    Some(t) => t,
                    None => anyhow::bail!("The server at {host} didn't answer."),
                };
                if create {
                    web::signup(&target, &user, &pass)?;
                }
                let sid = web::login(&target, &user, &pass)?;
                config::set_password(&host, &user, remember.then_some(pass.as_str()));
                game::launch(&dir, &target, &sid)
            })();
            let _ = tx.send(Msg::Started(r.map_err(|e| format!("{e:#}"))));
            ctx.request_repaint();
        });
    }

    fn poll_servers(&mut self, ctx: &egui::Context) {
        if self.polled.is_some_and(|t| t.elapsed() < Duration::from_secs(20)) {
            return;
        }
        self.polled = Some(Instant::now());
        for s in &self.cfg.servers {
            let (tx, ctx, host, lan) = (self.tx.clone(), ctx.clone(), s.host.clone(), s.lan.clone());
            thread::spawn(move || {
                let msg = if web::alive(&host) {
                    Msg::Alive(host, true, None)
                } else {
                    let local = lan::find(&host, lan.as_deref());
                    Msg::Alive(host, local.is_some(), local)
                };
                let _ = tx.send(msg);
                ctx.request_repaint();
            });
        }
    }

    fn handle(&mut self, msg: Msg) {
        match msg {
            Msg::Scanned(f) => {
                self.scanning = false;
                self.disc = self.disc.take().or(f.disc);
                self.patches = self.patches.take().or(f.patches);
            }
            Msg::Installed(Ok(dir)) => {
                self.cfg.game_dir = Some(dir);
                self.cfg.save();
                self.screen = Screen::Launcher;
                self.say("FINAL FANTASY XIV 1.23b is installed. Pick a server and Join.", false);
            }
            Msg::Installed(Err(e)) => {
                self.screen = Screen::Setup;
                self.say(format!("Setup stopped: {e}"), true);
            }
            Msg::Checked(Ok((dir, note))) => {
                self.busy = false;
                self.cfg.game_dir = Some(dir);
                self.cfg.save();
                self.screen = Screen::Launcher;
                match note {
                    Some(n) => self.say(n, true),
                    None => self.say("Game folder found. Pick a server and Join.", false),
                }
            }
            Msg::Checked(Err(e)) => {
                self.busy = false;
                self.say(e, true);
            }
            Msg::Started(r) => {
                self.busy = false;
                match r {
                    Ok(()) => self.say("The game is starting. Have fun!", false),
                    Err(e) => self.say(e, true),
                }
            }
            Msg::Alive(host, up, local) => {
                if let Some(local) = local {
                    for s in self.cfg.servers.iter_mut().filter(|s| s.host == host) {
                        if s.lan.as_deref() != Some(local.as_str()) {
                            s.lan = Some(local.clone());
                        }
                    }
                    self.cfg.save();
                }
                self.alive.insert(host, up);
            }
            Msg::Status(text) => self.say(text, false),
        }
    }
}

/// XIVLEGACY_SCREENSHOT=<file>: after a few seconds, write the window as raw RGBA
/// (width and height as two little-endian u32s first) and quit. For checking the
/// layout without a screen.
fn dev_screenshot(ctx: &egui::Context) {
    let Some(path) = std::env::var_os("XIVLEGACY_SCREENSHOT") else { return };
    let shot = ctx.input(|i| {
        i.events.iter().find_map(|e| match e {
            egui::Event::Screenshot { image, .. } => Some(image.clone()),
            _ => None,
        })
    });
    if let Some(img) = shot {
        let mut out = Vec::with_capacity(8 + img.pixels.len() * 4);
        out.extend((img.size[0] as u32).to_le_bytes());
        out.extend((img.size[1] as u32).to_le_bytes());
        for p in &img.pixels {
            out.extend(p.to_array());
        }
        let _ = std::fs::write(path, out);
        ctx.send_viewport_cmd(egui::ViewportCommand::Close);
    } else if ctx.input(|i| i.time) > 6.0 {
        ctx.send_viewport_cmd(egui::ViewportCommand::Screenshot(egui::UserData::default()));
    }
    ctx.request_repaint_after(Duration::from_millis(200));
}

/// The address to use for `host` from here: `host` itself when it answers,
/// else its address on this network (the one found before, or a fresh search).
fn reach(host: &str, known_lan: Option<&str>, tx: &Sender<Msg>) -> Option<String> {
    if web::alive(host) {
        return Some(host.to_string());
    }
    let _ = tx.send(Msg::Status(format!("{host} didn't answer; looking for it on this network…")));
    let found = lan::find(host, known_lan)?;
    let _ = tx.send(Msg::Alive(host.to_string(), true, Some(found.clone())));
    Some(found)
}

fn gb(b: u64) -> String {
    format!("{:.1} GB", b as f64 / 1e9)
}

/// A one-line label that shortens with "…" instead of pushing the buttons beside it away.
fn path_label(ui: &mut egui::Ui, text: String) -> egui::Response {
    ui.allocate_ui_with_layout(egui::vec2(240.0, 20.0), egui::Layout::left_to_right(egui::Align::Center), |ui| {
        ui.set_width(240.0);
        ui.add(egui::Label::new(text).truncate())
    })
    .inner
}

fn file_name(p: &Path) -> String {
    p.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| p.display().to_string())
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        while let Ok(m) = self.rx.try_recv() {
            self.handle(m);
        }
        dev_screenshot(ctx);
        if self.screen == Screen::Launcher {
            self.poll_servers(ctx);
        }
        if self.screen == Screen::Installing || self.busy {
            ctx.request_repaint_after(Duration::from_millis(250));
        }

        egui::TopBottomPanel::bottom("footer").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.weak(format!("XIVLegacy {VERSION}"));
                if let (Screen::Launcher, Some(dir)) = (&self.screen, self.cfg.game_dir.clone()) {
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if ui.small_button("Change…").on_hover_text("Use another game folder, or install again").clicked() {
                            self.screen = Screen::Setup;
                            self.say("", false);
                            if self.disc.is_none() && self.patches.is_none() {
                                self.start_scan(ctx);
                            }
                        }
                        ui.weak(dir.display().to_string());
                    });
                }
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(6.0);
            ui.heading("XIVLegacy");
            ui.label("FINAL FANTASY XIV 1.23b on private servers");
            ui.separator();
            match self.screen {
                Screen::Setup => self.setup_ui(ui, ctx),
                Screen::Installing => self.installing_ui(ui),
                Screen::Launcher => self.launcher_ui(ui, ctx),
            }
            if !self.status.is_empty() {
                ui.add_space(8.0);
                let color = if self.error { egui::Color32::from_rgb(240, 110, 100) } else { ui.visuals().text_color() };
                ui.colored_label(color, &self.status);
                if self.error && self.status.contains("DirectX") {
                    ui.hyperlink_to("Get the DirectX End-User Runtime", DIRECTX_URL);
                }
            }
        });
    }
}

impl App {
    fn setup_ui(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        ui.label(egui::RichText::new("First, the game files").strong());
        ui.label(
            "XIVLegacy builds a clean 1.23b install from the original disc and the 52 official patches. \
             It uses copies you already have, and downloads anything missing from archive.org.",
        );
        ui.add_space(4.0);
        egui::Grid::new("inputs").num_columns(3).spacing([10.0, 8.0]).show(ui, |ui| {
            ui.label("Disc image");
            path_label(ui, match (&self.disc, self.scanning) {
                (Some(p), _) => file_name(p),
                (None, true) => "Looking…".into(),
                (None, false) => format!("Download ({})", gb(ISO_7Z.size)),
            })
            .on_hover_text(self.disc.as_ref().map(|p| p.display().to_string()).unwrap_or_default());
            ui.horizontal(|ui| {
                if ui.button("Browse…").clicked() {
                    if let Some(p) = rfd::FileDialog::new().add_filter("Disc image", &["iso", "7z"]).pick_file() {
                        self.disc = Some(p);
                    }
                }
                if self.disc.is_some() && ui.small_button("Download").on_hover_text("Download this from archive.org instead").clicked() {
                    self.disc = None;
                }
            });
            ui.end_row();

            ui.label("Patches");
            path_label(ui, match (&self.patches, self.scanning) {
                (Some(p), _) => file_name(p),
                (None, true) => "Looking…".into(),
                (None, false) => format!("Download ({})", gb(PATCHES_ZIP.size)),
            })
            .on_hover_text(self.patches.as_ref().map(|p| p.display().to_string()).unwrap_or_default());
            ui.horizontal(|ui| {
                if ui.button("Browse…").clicked() {
                    if let Some(p) = rfd::FileDialog::new().add_filter("Patches", &["zip"]).pick_file() {
                        self.patches = Some(p);
                    }
                }
                if self.patches.is_some() && ui.small_button("Download").on_hover_text("Download this from archive.org instead").clicked() {
                    self.patches = None;
                }
            });
            ui.end_row();

            ui.label("Install to");
            path_label(ui, self.install_dir.display().to_string()).on_hover_text(self.install_dir.display().to_string());
            if ui.button("Change…").clicked() {
                if let Some(p) = rfd::FileDialog::new().set_title("Install FFXIV 1.23b inside…").pick_folder() {
                    self.install_dir = p.join("FINAL FANTASY XIV 1.23b");
                }
            }
            ui.end_row();
        });
        let download = self.disc.is_none().then_some(ISO_7Z.size).unwrap_or(0)
            + self.patches.is_none().then_some(PATCHES_ZIP.size).unwrap_or(0);
        ui.weak(format!(
            "About 12 GB installed{}. Downloads are deleted when setup finishes.",
            if download > 0 { format!(", plus {} to download", gb(download)) } else { String::new() }
        ));
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            let exists = self.install_dir.exists();
            let go = ui.add_enabled(!self.scanning && !self.busy && !exists, egui::Button::new(egui::RichText::new("Install").strong()));
            if exists {
                go.on_disabled_hover_text("That folder already exists; choose another or use it as an existing install.");
            } else if go.clicked() {
                self.start_install(ctx);
            }
            if ui.add_enabled(!self.busy, egui::Button::new("I already have the game…")).clicked() {
                self.use_existing(ctx);
            }
            if self.cfg.game_dir.as_deref().is_some_and(game::installed) && ui.button("Back").clicked() {
                self.screen = Screen::Launcher;
                self.say("", false);
            }
        });
    }

    fn installing_ui(&mut self, ui: &mut egui::Ui) {
        let p = self.progress.lock().unwrap();
        let Some(phase) = p.phase else {
            ui.label("Starting…");
            return;
        };
        let (n, of) = phase.step();
        ui.label(egui::RichText::new(format!("Step {n} of {of}: {}", phase.label())).strong());
        let frac = if p.total > 0 { p.done as f32 / p.total as f32 } else { 0.0 };
        ui.add(egui::ProgressBar::new(frac).show_percentage());
        let mut line = format!("{} of {}", gb(p.done), gb(p.total));
        if p.rate > 0.0 {
            line += &format!("  ·  {:.1} MB/s", p.rate / 1e6);
            if p.total > p.done {
                let s = ((p.total - p.done) as f64 / p.rate) as u64;
                line += &format!("  ·  about {}m {:02}s left", s / 60, s % 60);
            }
        }
        ui.label(line);
        if !p.detail.is_empty() {
            ui.weak(&p.detail);
        }
        ui.add_space(6.0);
        ui.weak("Downloads pick up where they left off if setup is interrupted.");
    }

    fn launcher_ui(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        ui.label(egui::RichText::new("Server").strong());
        ui.horizontal(|ui| {
            let names: Vec<String> = self.cfg.servers.iter().map(|s| format!("{}  ({})", s.name, s.host)).collect();
            let before = self.cfg.server;
            egui::ComboBox::from_id_salt("server").width(300.0).selected_text(&names[self.cfg.server]).show_ui(ui, |ui| {
                for (i, n) in names.iter().enumerate() {
                    ui.selectable_value(&mut self.cfg.server, i, n);
                }
            });
            if self.cfg.server != before {
                self.cfg.save();
                if self.cfg.remember {
                    self.password = config::password(self.cfg.host(), &self.cfg.username).unwrap_or_default();
                }
            }
            let (text, color) = match self.alive.get(self.cfg.host()) {
                Some(true) if self.cfg.servers[self.cfg.server].lan.is_some() => ("Online (this network)", egui::Color32::from_rgb(90, 200, 120)),
                Some(true) => ("Online", egui::Color32::from_rgb(90, 200, 120)),
                Some(false) => ("Offline", egui::Color32::from_rgb(240, 110, 100)),
                None => ("Checking", egui::Color32::GRAY),
            };
            let (dot, _) = ui.allocate_exact_size(egui::vec2(10.0, 10.0), egui::Sense::hover());
            ui.painter().circle_filled(dot.center(), 5.0, color);
            ui.colored_label(color, text);
            if ui.small_button(if self.editing { "Done" } else { "Edit…" }).clicked() {
                self.editing = !self.editing;
            }
        });
        if self.editing {
            ui.group(|ui| {
                let mut remove = None;
                for (i, s) in self.cfg.servers.iter().enumerate() {
                    ui.horizontal(|ui| {
                        ui.label(format!("{}  ({})", s.name, s.host));
                        if self.cfg.servers.len() > 1 && ui.small_button("Remove").clicked() {
                            remove = Some(i);
                        }
                    });
                }
                if let Some(i) = remove {
                    self.cfg.servers.remove(i);
                    self.cfg.server = self.cfg.server.min(self.cfg.servers.len() - 1);
                    self.cfg.save();
                }
                ui.horizontal(|ui| {
                    ui.add(egui::TextEdit::singleline(&mut self.new_name).hint_text("Name").desired_width(120.0));
                    ui.add(egui::TextEdit::singleline(&mut self.new_host).hint_text("Address, e.g. xiv.example.com").desired_width(200.0));
                    let host = self.new_host.trim().to_string();
                    if ui.add_enabled(game::host_slot(&host).is_ok(), egui::Button::new("Add")).clicked() {
                        let name = if self.new_name.trim().is_empty() { host.clone() } else { self.new_name.trim().to_string() };
                        self.cfg.servers.push(Server { name, host, lan: None });
                        self.cfg.server = self.cfg.servers.len() - 1;
                        self.cfg.save();
                        self.new_name.clear();
                        self.new_host.clear();
                        self.polled = None;
                    }
                });
            });
        }

        ui.add_space(6.0);
        egui::Grid::new("account").num_columns(2).spacing([10.0, 8.0]).show(ui, |ui| {
            ui.label("Username");
            ui.add(egui::TextEdit::singleline(&mut self.cfg.username).desired_width(240.0));
            ui.end_row();
            ui.label("Password");
            let pw = ui.add(egui::TextEdit::singleline(&mut self.password).password(true).desired_width(240.0));
            if pw.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) && !self.busy {
                self.join(ctx, false);
            }
            ui.end_row();
        });
        if ui.checkbox(&mut self.cfg.remember, "Remember me").changed() {
            self.cfg.save();
            if !self.cfg.remember {
                config::set_password(self.cfg.host(), self.cfg.username.trim(), None);
            }
        }
        ui.add_space(6.0);
        ui.horizontal(|ui| {
            let join = ui.add_enabled(!self.busy, egui::Button::new(egui::RichText::new("Join").strong()).min_size(egui::vec2(110.0, 32.0)));
            if join.clicked() {
                self.join(ctx, false);
            }
            if ui.add_enabled(!self.busy, egui::Button::new("Create Account")).on_hover_text("Make an account on this server with the name and password above").clicked() {
                self.join(ctx, true);
            }
            if self.busy {
                ui.spinner();
            }
        });
    }
}
