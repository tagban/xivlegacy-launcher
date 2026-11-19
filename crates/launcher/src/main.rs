//! XIVLegacy: one program for FINAL FANTASY XIV 1.23b on a private server.
//! The first run sets the game up (from files the player has, or archive.org);
//! after that it opens straight to the server list: pick a server and Join.
#![cfg_attr(windows, windows_subsystem = "windows")]

mod config;
mod dx;
mod game;
mod icon;
mod shortcut;
mod lan;
mod make;
mod pad;
mod scan;
mod settings;
mod sqex;
mod update;
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
use pad::Nav;
use settings::{Display, GameSettings, VSync};

const VERSION: &str = env!("CARGO_PKG_VERSION");
/// The launcher was started with --mute.
static MUTE: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
const DIRECTX_URL: &str = "https://www.microsoft.com/en-us/download/details.aspx?id=8109";

fn main() -> eframe::Result {
    // XIVLegacy --make-game <game folder>: make the game module from a terminal, then exit.
    let args: Vec<String> = std::env::args().collect();
    // --mute (or XIVLEGACY_NOAUDIO in the launcher's own environment): this run's games have no
    // sound, whatever Settings say (test sessions)
    let env_mute = std::env::var("XIVLEGACY_NOAUDIO").is_ok_and(|v| !v.is_empty() && v != "0");
    MUTE.store(env_mute || args.iter().any(|a| a == "--mute"), std::sync::atomic::Ordering::Relaxed);
    if args.get(1).map(String::as_str) == Some("--make-game") {
        let Some(dir) = args.get(2) else {
            eprintln!("usage: XIVLegacy --make-game <FINAL FANTASY XIV 1.23b folder>");
            std::process::exit(2);
        };
        let start = Instant::now();
        let last = std::sync::Mutex::new(String::new());
        let r = make::ensure(Path::new(dir), &|step, done, total| {
            let line = format!("{}: {}%", step.label(), if total > 0 { done * 100 / total } else { 0 });
            let mut l = last.lock().unwrap();
            if *l != line {
                eprintln!("{line}");
                *l = line;
            }
        });
        match r {
            Ok(p) => {
                eprintln!("made {} in {:.0?}", p.display(), start.elapsed());
                std::process::exit(0);
            }
            Err(e) => {
                eprintln!("couldn't make the game: {e:#}");
                std::process::exit(1);
            }
        }
    }
    let mut viewport = egui::ViewportBuilder::default()
        .with_title("XIVLegacy")
        .with_inner_size([560.0, 500.0])
        .with_min_inner_size([480.0, 420.0]);
    if let Some(icon) = Config::load().game_dir.and_then(|d| game_icon(&d)) {
        viewport = viewport.with_icon(icon);
    }
    let options = eframe::NativeOptions { viewport, ..Default::default() };
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
    /// Making the game for this computer: (step, done, total)
    Making(make::Step, usize, usize),
    Update(Option<update::Available>),
    Updated(Result<(), String>),
    /// Repair finished: (game files missing, files different), or why it couldn't run.
    Repaired(Result<(usize, usize), String>),
}

#[derive(PartialEq)]
enum Screen {
    Setup,
    Installing,
    Launcher,
    Settings,
}

/// The Settings screen's rows, in order; up and down move between them, left and right change one.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Row {
    Sound,
    Volume,
    Resolution,
    Display,
    VSync,
    Api,
    /// SWITCHES[i]
    Switch(usize),
    Extra,
    Reset,
    Back,
}

impl Row {
    fn all() -> Vec<Row> {
        let mut v = vec![Row::Sound, Row::Volume, Row::Resolution, Row::Display, Row::VSync];
        if settings::graphics_apis().len() > 1 {
            v.push(Row::Api);
        }
        v.extend((0..settings::SWITCHES.len()).map(Row::Switch));
        v.extend([Row::Extra, Row::Reset, Row::Back]);
        v
    }
}

struct App {
    ctx: egui::Context,
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
    // updates and repair
    update: Option<update::Available>,
    updating: bool,
    broken: bool,
    making: Option<(make::Step, usize, usize)>,
    // settings
    pads: pad::Pads,
    row: usize,
    /// The row changed by the pad or keys: scrolled into view once.
    row_moved: bool,
    /// The extra switches box had the keyboard last frame.
    typed: bool,
    /// The launch profile for the next Join (settings::PROFILES); back to Normal once a game starts.
    profile: usize,
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
            ctx: cc.egui_ctx.clone(),
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
            update: None,
            updating: false,
            broken: false,
            making: None,
            pads: pad::Pads::new(),
            row: 0,
            row_moved: false,
            typed: false,
            profile: 0,
            cfg,
        };
        if !ready {
            app.start_scan(&cc.egui_ctx);
        } else if let Some(dir) = app.cfg.game_dir.clone() {
            app.make_shortcuts_once(&dir);
            app.cfg.save();
        }
        update::clean_up();
        let (tx, ctx) = (app.tx.clone(), cc.egui_ctx.clone());
        thread::spawn(move || {
            let found = update::check(VERSION).unwrap_or(None);
            let _ = tx.send(Msg::Update(found));
            ctx.request_repaint();
        });
        app
    }

    fn set_icon(&self, game_dir: &Path) {
        if let Some(icon) = game_icon(game_dir) {
            self.ctx.send_viewport_cmd(egui::ViewportCommand::Icon(Some(std::sync::Arc::new(icon))));
        }
    }

    /// The Desktop and Start menu shortcuts with the game's icon (Windows), once: on the first
    /// install, or the first start of a version that makes them. The caller saves the config.
    fn make_shortcuts_once(&mut self, game_dir: &Path) {
        if self.cfg.shortcuts || !cfg!(windows) {
            return;
        }
        self.cfg.shortcuts = true;
        let dir = game_dir.to_path_buf();
        std::thread::spawn(move || shortcut::make(&dir));
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
        let game_settings = self.cfg.game.clone();
        let profile = self.profile;
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
                let say = |t: &str| {
                    let _ = tx.send(Msg::Status(t.to_string()));
                    ctx.request_repaint();
                };
                // The stock exe needs 2010's DirectX; our client doesn't use it.
                if !game::uses_module() {
                    dx::ensure(&dir, &say)?;
                }
                let module = if game::uses_module() {
                    let last = std::sync::Mutex::new((make::Step::Compiler, usize::MAX));
                    let prog = |step: make::Step, done: usize, total: usize| {
                        // at most one update per percent
                        let pct = if total > 0 { done * 100 / total } else { 0 };
                        let mut l = last.lock().unwrap();
                        if l.0 != step || l.1 != pct {
                            *l = (step, pct);
                            let _ = tx.send(Msg::Making(step, done, total));
                            ctx.request_repaint();
                        }
                    };
                    Some(make::ensure(&dir, &prog)?)
                } else {
                    None
                };
                say("Starting the game…");
                config::log(&format!("joining {host} via {target}"));
                let (w, h) = game_settings.size();
                if let Err(e) = settings::apply_config_sys(&settings::my_games(module.is_some()), &game_settings) {
                    config::log(&format!("couldn't write config.sys: {e}"));
                }
                config::log(&format!(
                    "settings: {w}x{h} {}, sound {}",
                    format!("{:?}", game_settings.display).to_lowercase(),
                    if !game_settings.sound || MUTE.load(std::sync::atomic::Ordering::Relaxed) {
                        "off".to_string()
                    } else {
                        format!("{}%", game_settings.volume)
                    }
                ));
                if profile != 0 {
                    config::log(&format!("launch profile: {}", settings::PROFILES[profile].name));
                }
                let env = game_settings.env(MUTE.load(std::sync::atomic::Ordering::Relaxed), profile);
                game::launch(&dir, &target, &sid, module.as_deref(), &env)
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
                self.set_icon(&dir);
                self.make_shortcuts_once(&dir);
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
                self.set_icon(&dir);
                self.make_shortcuts_once(&dir);
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
            Msg::Making(step, done, total) => {
                self.making = Some((step, done, total));
                self.status.clear();
            }
            Msg::Started(r) => {
                self.busy = false;
                self.making = None;
                match r {
                    Ok(()) => {
                        // a profile is for one Join
                        self.profile = 0;
                        self.say("The game is running. Have fun!", false)
                    }
                    Err(e) => {
                        config::log(&format!("join failed: {e}"));
                        self.say(e, true)
                    }
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
            Msg::Update(a) => self.update = a,
            Msg::Updated(Ok(())) => std::process::exit(0),
            Msg::Updated(Err(e)) => {
                self.updating = false;
                self.say(format!("Update failed: {e}"), true);
            }
            Msg::Repaired(r) => {
                self.busy = false;
                match r {
                    Ok((0, 0)) => {
                        self.broken = false;
                        self.say("Repair finished: DirectX is in place and every game file checks out.", false);
                    }
                    Ok((missing, wrong)) => {
                        self.broken = true;
                        self.say(
                            format!("{missing} game files are missing and {wrong} are damaged. Reinstall the game files to fix them."),
                            true,
                        );
                    }
                    Err(e) => self.say(format!("Repair stopped: {e}"), true),
                }
            }
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

/// The game's own icon (from the player's ffxivgame.exe), for the window and taskbar.
fn game_icon(game_dir: &Path) -> Option<egui::IconData> {
    let (rgba, width, height) = icon::from_exe(&game_dir.join("ffxivgame.exe"))?;
    Some(egui::IconData { rgba, width, height })
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
        let navs = self.navs(ctx);
        if self.pads.connected() || self.pads.holding() {
            // pads aren't window events: look again soon
            ctx.request_repaint_after(Duration::from_millis(if self.pads.holding() { 30 } else { 60 }));
        }
        if self.screen == Screen::Installing || self.busy {
            ctx.request_repaint_after(Duration::from_millis(250));
        }

        egui::TopBottomPanel::bottom("footer").show(ctx, |ui| {
            ui.horizontal(|ui| {
                ui.weak(format!("XIVLegacy {VERSION}"));
                if let (Screen::Launcher, Some(dir)) = (&self.screen, self.cfg.game_dir.clone()) {
                    ui.with_layout(egui::Layout::right_to_left(egui::Align::Center), |ui| {
                        if ui.small_button("Settings…").on_hover_text("Sound, resolution, window or full screen, test switches").clicked() {
                            self.open_settings();
                        }
                        if ui.add_enabled(!self.busy, egui::Button::new("Repair…").small())
                            .on_hover_text("Reinstall DirectX if needed and check every game file")
                            .clicked()
                        {
                            self.repair(ctx);
                        }
                        if ui
                            .small_button("Game folder…")
                            .on_hover_text(format!("{}\nUse another game folder, or install again", dir.display()))
                            .clicked()
                        {
                            self.screen = Screen::Setup;
                            self.say("", false);
                            if self.disc.is_none() && self.patches.is_none() {
                                self.start_scan(ctx);
                            }
                        }
                    });
                }
            });
        });

        egui::CentralPanel::default().show(ctx, |ui| {
            ui.add_space(6.0);
            ui.heading("XIVLegacy");
            ui.label("FINAL FANTASY XIV 1.23b on private servers");
            ui.separator();
            self.update_banner(ui, ctx);
            match self.screen {
                Screen::Setup => self.setup_ui(ui, ctx),
                Screen::Installing => self.installing_ui(ui),
                Screen::Launcher => self.launcher_ui(ui, ctx, &navs),
                Screen::Settings => self.settings_ui(ui, ctx, &navs),
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
    fn update_banner(&mut self, ui: &mut egui::Ui, ctx: &egui::Context) {
        let Some(a) = self.update.clone() else { return };
        ui.horizontal_wrapped(|ui| {
            ui.colored_label(egui::Color32::from_rgb(120, 180, 255), format!("XIVLegacy {} is available.", a.version));
            if update::can_install(&a) {
                if ui.add_enabled(!self.updating && !self.busy, egui::Button::new("Update now")).clicked() {
                    self.updating = true;
                    self.say("Downloading the update…", false);
                    let (tx, ctx, a) = (self.tx.clone(), ctx.clone(), a.clone());
                    thread::spawn(move || {
                        let r = update::install(&a).map_err(|e| format!("{e:#}"));
                        let _ = tx.send(Msg::Updated(r));
                        ctx.request_repaint();
                    });
                }
            } else {
                ui.hyperlink_to("Download", update::RELEASES_PAGE);
            }
            ui.hyperlink_to("What's new", &a.page);
            if self.updating {
                ui.spinner();
            }
        });
        ui.separator();
    }

    /// Repair: DirectX (the game's DLLs and XAudio2), then every game file hashed.
    fn repair(&mut self, ctx: &egui::Context) {
        let Some(dir) = self.cfg.game_dir.clone() else { return };
        self.busy = true;
        self.say("Repairing: checking DirectX…", false);
        let (tx, ctx) = (self.tx.clone(), ctx.clone());
        thread::spawn(move || {
            let r = (|| -> anyhow::Result<(usize, usize)> {
                let say = |t: &str| {
                    let _ = tx.send(Msg::Status(t.to_string()));
                    ctx.request_repaint();
                };
                dx::ensure(&dir, &say)?;
                say("Repairing: checking every game file (a minute or two)…");
                let c = verify::check(&dir, true, &Counter::default())?;
                config::log(&format!("repair: {} missing, {} different, game.ver {:?}", c.missing.len(), c.wrong.len(), c.game_version));
                Ok((c.missing.len(), c.wrong.len() + usize::from(!c.ok() && c.missing.is_empty() && c.wrong.is_empty())))
            })();
            let _ = tx.send(Msg::Repaired(r.map_err(|e| format!("{e:#}"))));
            ctx.request_repaint();
        });
    }

    /// Moves the damaged install aside and opens setup to build it again in the same place.
    fn reinstall(&mut self, ctx: &egui::Context) {
        let Some(dir) = self.cfg.game_dir.clone() else { return };
        let mut aside = dir.with_file_name(format!("{} (old)", file_name(&dir)));
        let mut n = 2;
        while aside.exists() {
            aside = dir.with_file_name(format!("{} (old {n})", file_name(&dir)));
            n += 1;
        }
        if let Err(e) = std::fs::rename(&dir, &aside) {
            return self.say(format!("Couldn't move the old game folder aside: {e}"), true);
        }
        config::log(&format!("reinstall: moved {} to {}", dir.display(), aside.display()));
        self.broken = false;
        self.install_dir = dir;
        self.screen = Screen::Setup;
        self.say(format!("The old files are in \"{}\"; delete that folder once the new install works.", file_name(&aside)), false);
        if self.disc.is_none() && self.patches.is_none() {
            self.start_scan(ctx);
        }
    }

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

    fn launcher_ui(&mut self, ui: &mut egui::Ui, ctx: &egui::Context, navs: &[Nav]) {
        for n in navs {
            match n {
                Nav::Menu => self.open_settings(),
                // left and right pick the launch profile
                Nav::Left | Nav::Right if !self.busy => {
                    let len = settings::PROFILES.len();
                    self.profile = (self.profile + if *n == Nav::Right { 1 } else { len - 1 }) % len;
                }
                // A joins once the name and password are in (a remembered sign-in)
                Nav::Accept if !self.busy && !self.password.is_empty() && !self.cfg.username.trim().is_empty() => {
                    self.join(ctx, false)
                }
                _ => {}
            }
        }
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
            let names: Vec<&str> = settings::PROFILES.iter().map(|p| p.name).collect();
            egui::ComboBox::from_id_salt("profile")
                .selected_text(names[self.profile])
                .show_ui(ui, |ui| {
                    for (i, n) in names.iter().enumerate() {
                        ui.selectable_value(&mut self.profile, i, *n);
                    }
                })
                .response
                .on_hover_text("Launch profile: switches for the next Join only (speed test runs)");
            if self.busy {
                ui.spinner();
            }
        });
        // what this Join will set, so a test run is never taken for a normal one
        let mute = MUTE.load(std::sync::atomic::Ordering::Relaxed);
        let on: Vec<String> =
            self.cfg.game.shown(mute, self.profile).into_iter().filter(|s| !s.starts_with("XIVLEGACY_BUG_TAG=")).collect();
        if self.profile != 0 || !on.is_empty() {
            let p = &settings::PROFILES[self.profile];
            let head = if self.profile != 0 { format!("{}, for this Join", p.name) } else { "This Join sets".to_string() };
            let sets = format!("{head}: {}", if on.is_empty() { "nothing".into() } else { on.join("  ") });
            ui.add(egui::Label::new(egui::RichText::new(sets).color(egui::Color32::from_rgb(240, 180, 80))).wrap());
            if !p.tag.is_empty() {
                let report = format!("Report this run in the game with Ctrl+F12 (or hold both sticks in): the chat line opens with \"!bug {} \".", p.tag);
                ui.add(egui::Label::new(egui::RichText::new(report).weak()).wrap());
            }
        }
        if let Some((step, done, total)) = self.making {
            ui.add_space(6.0);
            ui.label(egui::RichText::new(step.label()).strong());
            let frac = if total > 0 { done as f32 / total as f32 } else { 0.0 };
            ui.add(egui::ProgressBar::new(frac).show_percentage());
            ui.weak("The first time only: XIVLegacy makes the game from your own copy of FFXIV, so nothing of the game is downloaded. This takes a few minutes.");
        }
        if self.broken && !self.busy && ui.button("Reinstall the game files").clicked() {
            self.reinstall(ctx);
        }
        if self.pads.connected() {
            ui.add_space(4.0);
            ui.weak("Gamepad: A to Join, left and right for the launch profile, Start for Settings");
        }
    }
}

// --- Settings ---------------------------------------------------------------------------------

const ACCENT: egui::Color32 = egui::Color32::from_rgb(120, 180, 255);
const EXTRA_ID: &str = "settings-extra";

impl App {
    fn open_settings(&mut self) {
        if !self.busy {
            self.screen = Screen::Settings;
            self.row = 0;
            self.row_moved = true;
            self.say("", false);
        }
    }

    fn close_settings(&mut self) {
        self.cfg.save();
        self.screen = Screen::Launcher;
    }

    /// This frame's menu moves: from the pads always, and from the arrow keys, Enter and Escape on
    /// the Settings screen while no text box has the keyboard.
    fn navs(&mut self, ctx: &egui::Context) -> Vec<Nav> {
        let mut v = self.pads.poll();
        // not while typing, nor in the frame after (the Escape that ended it isn't a Back too)
        if self.screen == Screen::Settings && !ctx.wants_keyboard_input() && !self.typed {
            ctx.input_mut(|i| {
                for (key, n) in [
                    (egui::Key::ArrowUp, Nav::Up),
                    (egui::Key::ArrowDown, Nav::Down),
                    (egui::Key::ArrowLeft, Nav::Left),
                    (egui::Key::ArrowRight, Nav::Right),
                    (egui::Key::Enter, Nav::Accept),
                    (egui::Key::Space, Nav::Accept),
                    (egui::Key::Escape, Nav::Back),
                ] {
                    // taken, so the Enter that opens the text box doesn't also type into it
                    // every press counts, even two in one frame
                    for _ in 0..i.count_and_consume_key(egui::Modifiers::NONE, key) {
                        v.push(n);
                    }
                }
            });
        }
        v
    }

    /// The resolutions to step through: the usual ones, and the one set now if it's another.
    fn resolutions(&self) -> Vec<(u32, u32)> {
        let mut v = settings::RESOLUTIONS.to_vec();
        let now = (self.cfg.game.width, self.cfg.game.height);
        if !v.contains(&now) {
            v.insert(0, now);
        }
        v
    }

    /// Left (-1) or right (+1) on a row.
    fn step(&mut self, row: Row, d: i32) {
        let g = &mut self.cfg.game;
        let cycle = |i: usize, n: usize| ((i as i32 + d).rem_euclid(n as i32)) as usize;
        match row {
            Row::Sound => g.sound = !g.sound,
            Row::Volume if g.sound => g.volume = (g.volume as i32 + 5 * d).clamp(0, 100) as u8,
            Row::Resolution => {
                let list = self.resolutions();
                let g = &mut self.cfg.game;
                let i = list.iter().position(|&r| r == (g.width, g.height)).unwrap_or(0);
                (g.width, g.height) = list[cycle(i, list.len())];
            }
            Row::Display => {
                let all = [Display::Window, Display::Borderless, Display::FullScreen];
                let i = all.iter().position(|&v| v == g.display).unwrap_or(0);
                g.display = all[cycle(i, all.len())];
            }
            Row::VSync => {
                let all = [VSync::Game, VSync::On, VSync::Off];
                let i = all.iter().position(|&v| v == g.vsync).unwrap_or(0);
                g.vsync = all[cycle(i, all.len())];
            }
            Row::Switch(i) => {
                let sw = &settings::SWITCHES[i];
                let n = g.switch_index(sw);
                g.set_switch(sw, cycle(n, sw.choices.len()));
            }
            Row::Api => {
                let apis = settings::graphics_apis();
                let i = apis.iter().position(|a| a.0 == g.graphics_api).unwrap_or(0);
                g.graphics_api = apis[cycle(i, apis.len())].0.to_string();
            }
            _ => return,
        }
        self.cfg.save();
    }

    fn accept(&mut self, row: Row, ctx: &egui::Context) {
        match row {
            Row::Extra => ctx.memory_mut(|m| m.request_focus(egui::Id::new(EXTRA_ID))),
            Row::Reset => {
                self.cfg.game = GameSettings::default();
                self.cfg.save();
                self.say("Settings are back to the defaults.", false);
            }
            Row::Back => self.close_settings(),
            Row::Volume => {}
            r => self.step(r, 1),
        }
    }

    fn value_text(&self, row: Row) -> String {
        let g = &self.cfg.game;
        match row {
            Row::Sound if MUTE.load(std::sync::atomic::Ordering::Relaxed) => "Off for this run (--mute)".into(),
            Row::Sound => if g.sound { "On" } else { "Off" }.into(),
            Row::Volume if !g.sound => "—".into(),
            Row::Volume => format!("{}%", g.volume),
            Row::Resolution => {
                let (w, h) = g.size();
                if (w, h) == (1280, 720) { format!("{w} × {h} (the game's own)") } else { format!("{w} × {h}") }
            }
            Row::Display => match g.display {
                Display::Window => "Window",
                Display::Borderless => "Borderless window",
                Display::FullScreen => "Full screen",
            }
            .into(),
            Row::VSync => match g.vsync {
                VSync::Game => "As the game asks (on)",
                VSync::On => "On",
                VSync::Off => "Off",
            }
            .into(),
            Row::Switch(i) => {
                let sw = &settings::SWITCHES[i];
                sw.choices[g.switch_index(sw)].0.into()
            }
            Row::Api => settings::graphics_apis()
                .iter()
                .find(|a| a.0 == g.graphics_api)
                .map_or("Automatic", |a| a.1)
                .into(),
            _ => String::new(),
        }
    }

    fn settings_ui(&mut self, ui: &mut egui::Ui, ctx: &egui::Context, navs: &[Nav]) {
        let rows = Row::all();
        self.row = self.row.min(rows.len() - 1);
        let extra_id = egui::Id::new(EXTRA_ID);
        let typing = ctx.memory(|m| m.has_focus(extra_id));
        for &n in navs {
            let row = rows[self.row];
            match n {
                // B or Escape in the text box: leave the box (Escape already does, in egui)
                Nav::Back if typing => ctx.memory_mut(|m| m.surrender_focus(extra_id)),
                Nav::Back | Nav::Menu => self.close_settings(),
                _ if typing => {}
                Nav::Up => {
                    self.row = (self.row + rows.len() - 1) % rows.len();
                    self.row_moved = true;
                }
                Nav::Down => {
                    self.row = (self.row + 1) % rows.len();
                    self.row_moved = true;
                }
                Nav::Left => self.step(row, -1),
                Nav::Right => self.step(row, 1),
                Nav::Accept => self.accept(row, ctx),
            }
        }
        if self.screen != Screen::Settings {
            return;
        }

        ui.horizontal(|ui| {
            ui.label(egui::RichText::new("Settings").strong());
            ui.weak("Used each time you Join.");
        });
        let help = if self.pads.connected() {
            "D-pad: choose and change  ·  A: select  ·  B: back"
        } else {
            "Arrow keys: choose and change  ·  Enter: select  ·  Esc: back"
        };
        ui.weak(help);
        ui.add_space(4.0);

        egui::ScrollArea::vertical().auto_shrink([false, true]).show(ui, |ui| {
            for (i, &row) in rows.iter().enumerate() {
                if row == Row::Switch(0) {
                    ui.add_space(6.0);
                    ui.label(egui::RichText::new("Test switches").strong());
                    ui.weak(
                        "Set only for the game this launcher starts. Left at the first choice, a switch is unset, \
                         even if setx set it. A client that doesn't know a switch (a speed switch on a build \
                         without it) just ignores it.",
                    );
                }
                let selected = i == self.row;
                let frame = egui::Frame::new()
                    .inner_margin(egui::Margin::symmetric(8, 4))
                    .corner_radius(4.0)
                    .fill(if selected { ui.visuals().selection.bg_fill.gamma_multiply(0.35) } else { egui::Color32::TRANSPARENT })
                    .stroke(if selected { egui::Stroke::new(1.0, ACCENT) } else { egui::Stroke::NONE });
                let resp = frame
                    .show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        self.settings_row(ui, ctx, row, selected);
                    })
                    .response;
                if selected && self.row_moved {
                    resp.scroll_to_me(None);
                }
                if resp.interact(egui::Sense::click()).clicked() {
                    self.row = i;
                }
            }
        });
        self.row_moved = false;
        self.typed = ctx.memory(|m| m.has_focus(extra_id));
    }

    fn settings_row(&mut self, ui: &mut egui::Ui, ctx: &egui::Context, row: Row, selected: bool) {
        let label = |ui: &mut egui::Ui, t: &str| {
            let text = egui::RichText::new(t);
            let text = if selected { text.color(ACCENT) } else { text };
            ui.allocate_ui_with_layout(egui::vec2(150.0, 22.0), egui::Layout::left_to_right(egui::Align::Center), |ui| {
                ui.set_min_width(150.0);
                ui.label(text);
            });
        };
        let chooser = |app: &mut App, ui: &mut egui::Ui, row: Row, enabled: bool| {
            if ui.add_enabled(enabled, egui::Button::new("◀").small()).clicked() {
                app.step(row, -1);
            }
            ui.add_sized([210.0, 22.0], egui::Label::new(app.value_text(row)));
            if ui.add_enabled(enabled, egui::Button::new("▶").small()).clicked() {
                app.step(row, 1);
            }
        };
        let muted = MUTE.load(std::sync::atomic::Ordering::Relaxed);
        match row {
            Row::Sound => {
                ui.horizontal(|ui| {
                    label(ui, "Sound");
                    chooser(self, ui, row, !muted);
                });
            }
            Row::Volume => {
                ui.horizontal(|ui| {
                    label(ui, "Volume");
                    chooser(self, ui, row, self.cfg.game.sound && !muted);
                });
            }
            Row::Resolution => {
                ui.horizontal(|ui| {
                    label(ui, "Resolution");
                    chooser(self, ui, row, true);
                });
            }
            Row::Display => {
                ui.horizontal(|ui| {
                    label(ui, "Display");
                    chooser(self, ui, row, true);
                });
                match self.cfg.game.display {
                    Display::Window => {}
                    Display::Borderless => {
                        ui.weak("A window without a frame at the display's top left; at the display's own size it covers it.");
                    }
                    Display::FullScreen => {
                        ui.weak("Covers the whole display without changing its mode; the game draws at the resolution above, scaled.");
                    }
                }
            }
            Row::VSync => {
                ui.horizontal(|ui| {
                    label(ui, "VSync");
                    chooser(self, ui, row, true);
                });
            }
            Row::Api => {
                ui.horizontal(|ui| {
                    label(ui, "Graphics API");
                    chooser(self, ui, row, true);
                });
            }
            Row::Switch(i) => {
                let sw = &settings::SWITCHES[i];
                ui.horizontal(|ui| {
                    label(ui, sw.name.trim_start_matches("XIVLEGACY_"));
                    chooser(self, ui, row, true);
                });
                ui.weak(sw.help);
            }
            Row::Extra => {
                label(ui, "Extra switches (advanced)");
                ui.weak("For tests: one per line, NAME=value (NAME= clears one set with setx). setx and set lines can be pasted as they are.");
                let edit = egui::TextEdit::multiline(&mut self.cfg.game.extra_env)
                    .id(egui::Id::new(EXTRA_ID))
                    .font(egui::TextStyle::Monospace)
                    .hint_text("for example  XIVLEGACY_CPUSETS=0")
                    .desired_rows(3)
                    .desired_width(f32::INFINITY);
                let r = ui.add(edit);
                if r.changed() {
                    self.cfg.save();
                }
                if r.has_focus() {
                    ui.weak(if self.pads.connected() {
                        "B: done typing (Steam Deck: Steam + X shows the keyboard)"
                    } else {
                        "Esc: done typing"
                    });
                }
                let (_, bad) = settings::parse_extra(&self.cfg.game.extra_env);
                for b in bad {
                    ui.colored_label(egui::Color32::from_rgb(240, 110, 100), format!("Line {} isn't NAME=value, so it's skipped: {}", b.line, b.text));
                }
                let _ = ctx;
            }
            Row::Reset => {
                ui.horizontal(|ui| {
                    label(ui, "");
                    if ui.button("Reset to defaults").clicked() {
                        self.accept(row, ctx);
                    }
                });
            }
            Row::Back => {
                ui.horizontal(|ui| {
                    label(ui, "");
                    if ui.button(egui::RichText::new("Back").strong()).clicked() {
                        self.close_settings();
                    }
                });
            }
        }
    }
}
