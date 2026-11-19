//! The game's settings, chosen in the launcher and applied at Join: sound, resolution, window or
//! full screen, VSync, the graphics API and extra environment switches for tests.
//!
//! Two things carry them to the game; nothing new is invented on its side:
//! - the game's own display file, `Documents\My Games\FINAL FANTASY XIV\config.sys`, which
//!   ffxivconfig.exe writes and the game reads at start-up (resolution and full screen);
//! - environment switches the client already reads (runtime/portable: XIVLEGACY_NOAUDIO,
//!   XIVLEGACY_VOLUME, XIVLEGACY_VSYNC, XIVLEGACY_WGPU_BACKEND and any others typed in).
//!
//! config.sys is a fixed block of little-endian u32s and two UTF-16 strings (version 0x20120419,
//! read by ffxivgame.exe at 0x405C90). The fields used here:
//! - +0x10: 0 a window with a frame, otherwise full screen (a frameless WS_POPUP at 0,0);
//! - +0x14, +0x18: the resolution (the game raises it to at least 1024x720).
//!
//! The older versions it still reads (0x20100706, 0x20101020) keep those three in the same place.
//! A file that is there is changed only at those offsets, so whatever else ffxivconfig.exe or the
//! player set stays.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

pub const CONFIG_SYS_VERSION: u32 = 0x2012_0419;
const KNOWN_VERSIONS: [u32; 3] = [0x2010_0706, 0x2010_1020, CONFIG_SYS_VERSION];
const CONFIG_SYS_LEN: usize = 0xE0;
const OFF_FULL_SCREEN: usize = 0x10;
const OFF_WIDTH: usize = 0x14;
const OFF_HEIGHT: usize = 0x18;
/// The game raises a smaller resolution to this.
pub const MIN_SIZE: (u32, u32) = (1024, 720);

/// Resolutions offered (the game's own default first). One the player set some other way is
/// shown too.
pub const RESOLUTIONS: [(u32, u32); 9] = [
    (1280, 720),
    (1366, 768),
    (1600, 900),
    (1920, 1080),
    (2560, 1440),
    (3840, 2160),
    (1024, 768),
    (1280, 1024),
    (1920, 1200),
];

/// How the game's window is shown.
#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, Default)]
pub enum Display {
    /// A window with a frame (config.sys +0x10 = 0).
    #[default]
    Window,
    /// The game's own full-screen mode on our client: a frameless window at the resolution, at the
    /// display's top left; one as big as the display covers it.
    Borderless,
    /// That window made to cover the display at any resolution, the picture scaled
    /// (XIVLEGACY_FULLSCREEN=1); the display's mode isn't changed.
    FullScreen,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq, Default)]
pub enum VSync {
    /// What the game asks for (on).
    #[default]
    Game,
    On,
    Off,
}

/// What the launcher keeps for the game (in launcher.json, under "game").
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(default)]
pub struct GameSettings {
    pub sound: bool,
    /// 0-100, the whole game's loudness.
    pub volume: u8,
    pub width: u32,
    pub height: u32,
    pub display: Display,
    pub vsync: VSync,
    /// wgpu's API (XIVLEGACY_WGPU_BACKEND): "" automatic, or "dx12", "vulkan", "gl". Not on macOS,
    /// where the client draws with Metal.
    pub graphics_api: String,
    /// One switch a line: `NAME=value`, or `NAME=` to clear one set elsewhere (setx). `set`,
    /// `setx` and `export` forms are taken too, and `#` starts a comment.
    pub extra_env: String,
    /// The test switches chosen (SWITCHES): name -> value. A switch not here is left unset.
    pub switches: std::collections::BTreeMap<String, String>,
}

/// A test switch the client reads, offered as a row: its choices are (label, value), the first
/// one leaving the variable unset (the client's own default).
pub struct Switch {
    pub name: &'static str,
    pub help: &'static str,
    pub choices: &'static [(&'static str, Option<&'static str>)],
}

const OFF_ON: &[(&str, Option<&str>)] = &[("Off", None), ("On", Some("1"))];

/// The test switches on the Settings screen. Speed switches come from launcher #31 (perf/windows);
/// a client without them ignores them, as it does any variable it doesn't read.
pub const SWITCHES: &[Switch] = &[
    Switch { name: "XIVLEGACY_FRAMESTATS", help: "fps, draws and clears every 60 frames, and each slow frame, in game.log", choices: OFF_ON },
    Switch { name: "XIVLEGACY_WGPU_STATS", help: "per-frame wgpu costs (passes, uploads) in game.log; wgpu builds (Windows, Linux)", choices: OFF_ON },
    Switch {
        name: "XIVLEGACY_WGPU_PRESENT",
        help: "wgpu's present mode, over VSync: fifo waits for the display, mailbox and immediate don't",
        choices: &[("As the game asks", None), ("fifo", Some("fifo")), ("mailbox", Some("mailbox")), ("immediate", Some("immediate"))],
    },
    Switch {
        name: "XIVLEGACY_MONSTERCAM",
        help: "Monster Mode camera distances; Log writes each body and its scale to game.log",
        choices: &[("On", None), ("Off", Some("0")), ("Log", Some("2"))],
    },
    Switch {
        name: "XIVLEGACY_ACTORLOOKUP",
        help: "the camera's actor-by-id lookup check; Off leaves the lookups unchecked, to rule it out",
        choices: &[("On", None), ("Off", Some("0"))],
    },
    Switch { name: "XIVLEGACY_PARALLEL", help: "speed test: the game's main loop runs beside the guest lock", choices: OFF_ON },
    Switch {
        name: "XIVLEGACY_CPUSETS",
        help: "speed test: keep the game on the V-Cache cores (Ryzen X3D); Off lets Windows place it",
        choices: &[("On", None), ("Off", Some("0"))],
    },
    Switch { name: "XIVLEGACY_NOPBUF", help: "speed test: wgpu's static-buffer switch (NOPBUF=1)", choices: OFF_ON },
    Switch {
        name: "XIVLEGACY_SPINNAP",
        help: "speed test: a short nap while a thread spins for the lock; Off spins without it",
        choices: &[("On", None), ("Off", Some("0"))],
    },
    Switch {
        name: "XIVLEGACY_LOCKWAKE",
        help: "speed test: which waiters a freed lock wakes; All wakes every one, not only the next",
        choices: &[("Next only", None), ("All", Some("all"))],
    },
];

/// A launch profile: switches for one Join, over the Settings, and the tag its bug reports get.
pub struct Profile {
    pub name: &'static str,
    /// Typed after "!bug " by the in-game report key (XIVLEGACY_BUG_TAG).
    pub tag: &'static str,
    pub env: &'static [(&'static str, &'static str)],
}

const MEASURE: (&str, &str) = ("XIVLEGACY_FRAMESTATS", "1");

/// Normal, then the Windows speed test runs (launcher #31's recipe), each measuring with FRAMESTATS.
pub const PROFILES: &[Profile] = &[
    Profile { name: "Normal", tag: "", env: &[] },
    Profile { name: "Speed test A (defaults)", tag: "speed A", env: &[MEASURE] },
    Profile { name: "Speed test B (PARALLEL=1)", tag: "speed B", env: &[MEASURE, ("XIVLEGACY_PARALLEL", "1")] },
    Profile { name: "Speed test C (CPUSETS=0)", tag: "speed C", env: &[MEASURE, ("XIVLEGACY_CPUSETS", "0")] },
    Profile { name: "Speed test D (NOPBUF=1)", tag: "speed D", env: &[MEASURE, ("XIVLEGACY_NOPBUF", "1")] },
    Profile { name: "Speed test E (SPINNAP=0)", tag: "speed E", env: &[MEASURE, ("XIVLEGACY_SPINNAP", "0")] },
    Profile { name: "Speed test F (LOCKWAKE=all)", tag: "speed F", env: &[MEASURE, ("XIVLEGACY_LOCKWAKE", "all")] },
    Profile { name: "Speed test G (WGPU_BACKEND=vulkan)", tag: "speed G", env: &[MEASURE, ("XIVLEGACY_WGPU_BACKEND", "vulkan")] },
    /* frame pacing off (the drawing thread doesn't wait for the main loop's tick), and the loading
     * naps off (no 30 ms nap a frame while loading or answering a tap) */
    Profile { name: "Speed test H (FRAMEPACE=0)", tag: "speed H", env: &[MEASURE, ("XIVLEGACY_FRAMEPACE", "0")] },
    Profile { name: "Speed test I (SLOW_LOADING=1)", tag: "speed I", env: &[MEASURE, ("XIVLEGACY_SLOW_LOADING", "1")] },
    /* Windows: the sampling profiler (host_win.c), 2 minutes from a minute after the start */
    Profile { name: "Speed test P (profile)", tag: "speed P", env: &[MEASURE, ("XIVLEGACY_PROFILE", "120:60")] },
];

/// What the environment list leaves set, in order (later wins): name -> value, None unset.
pub fn resolve(env: &[(String, Option<String>)]) -> std::collections::BTreeMap<String, Option<String>> {
    env.iter().cloned().collect()
}

impl Default for GameSettings {
    fn default() -> Self {
        GameSettings {
            sound: true,
            volume: 100,
            width: 1280,
            height: 720,
            display: Display::Window,
            vsync: VSync::Game,
            graphics_api: String::new(),
            extra_env: String::new(),
            switches: Default::default(),
        }
    }
}

/// Graphics APIs offered on this system: (value, label).
pub fn graphics_apis() -> &'static [(&'static str, &'static str)] {
    if cfg!(target_os = "macos") {
        &[("", "Metal")]
    } else if cfg!(windows) {
        &[("", "Automatic"), ("dx12", "Direct3D 12"), ("vulkan", "Vulkan")]
    } else {
        &[("", "Automatic"), ("vulkan", "Vulkan"), ("gl", "OpenGL")]
    }
}

/// One line of the extra switches box that couldn't be read.
#[derive(Debug, PartialEq)]
pub struct BadLine {
    pub line: usize,
    pub text: String,
}

fn valid_name(n: &str) -> bool {
    let mut c = n.chars();
    c.next().is_some_and(|f| f == '_' || f.is_ascii_alphabetic()) && c.all(|x| x == '_' || x.is_ascii_alphanumeric())
}

/// The extra switches: (name, Some(value)) to set, (name, None) to clear.
pub fn parse_extra(text: &str) -> (Vec<(String, Option<String>)>, Vec<BadLine>) {
    let (mut out, mut bad) = (Vec::new(), Vec::new());
    for (i, raw) in text.lines().enumerate() {
        let line = raw.split_once('#').map_or(raw, |(a, _)| a).trim();
        if line.is_empty() {
            continue;
        }
        let lower = line.to_ascii_lowercase();
        let parsed = if let Some(rest) = lower.strip_prefix("setx ").map(|_| line[5..].trim()) {
            // setx NAME value (the value may be quoted)
            rest.split_once(char::is_whitespace).map(|(n, v)| (n.trim(), v.trim().trim_matches('"')))
        } else {
            let rest = if lower.starts_with("set ") || lower.starts_with("export ") {
                line.split_once(' ').map_or(line, |(_, r)| r).trim()
            } else {
                line
            };
            rest.split_once('=').map(|(n, v)| (n.trim(), v.trim().trim_matches('"')))
        };
        match parsed {
            Some((n, v)) if valid_name(n) => out.push((n.to_string(), (!v.is_empty()).then(|| v.to_string()))),
            _ => bad.push(BadLine { line: i + 1, text: raw.trim().to_string() }),
        }
    }
    (out, bad)
}

impl GameSettings {
    /// The environment switches for the client, in the order they apply (later wins): the
    /// settings, the test switches, the launch profile (`PROFILES[profile]`), then the extra box.
    /// Every switch the screen manages is unset when left at its default, so a value set with setx
    /// can't leak into a test run. `mute` is the launcher's own --mute.
    pub fn env(&self, mute: bool, profile: usize) -> Vec<(String, Option<String>)> {
        let mut v: Vec<(String, Option<String>)> = Vec::new();
        let mut put = |k: &str, val: Option<String>| v.push((k.to_string(), val));
        // XIVLEGACY_NOAUDIO is checked for being there (before this client): unset, not "0"
        put("XIVLEGACY_NOAUDIO", (!self.sound || mute).then(|| "1".into()));
        put("XIVLEGACY_VOLUME", (self.sound && !mute && self.volume < 100).then(|| self.volume.to_string()));
        put("XIVLEGACY_FULLSCREEN", (self.display == Display::FullScreen).then(|| "1".into()));
        put(
            "XIVLEGACY_VSYNC",
            match self.vsync {
                VSync::Game => None,
                VSync::On => Some("1".into()),
                VSync::Off => Some("0".into()),
            },
        );
        if !cfg!(target_os = "macos") {
            put("XIVLEGACY_WGPU_BACKEND", (!self.graphics_api.is_empty()).then(|| self.graphics_api.clone()));
        }
        for sw in SWITCHES {
            let chosen = self.switches.get(sw.name).filter(|val| sw.choices.iter().any(|c| c.1 == Some(val.as_str())));
            put(sw.name, chosen.cloned());
        }
        let p = PROFILES.get(profile).unwrap_or(&PROFILES[0]);
        for (k, val) in p.env {
            put(k, Some(val.to_string()));
        }
        put("XIVLEGACY_BUG_TAG", (!p.tag.is_empty()).then(|| p.tag.to_string()));
        v.extend(parse_extra(&self.extra_env).0);
        v
    }

    /// The switches a Join sets (name=value), for the screen and the log: what makes a run a test run.
    pub fn shown(&self, mute: bool, profile: usize) -> Vec<String> {
        resolve(&self.env(mute, profile)).into_iter().filter_map(|(k, val)| val.map(|val| format!("{k}={val}"))).collect()
    }

    /// The choice index of a test switch (0: unset).
    pub fn switch_index(&self, sw: &Switch) -> usize {
        let cur = self.switches.get(sw.name).map(String::as_str);
        sw.choices.iter().position(|c| c.1 == cur).unwrap_or(0)
    }

    pub fn set_switch(&mut self, sw: &Switch, i: usize) {
        match sw.choices[i].1 {
            Some(val) => self.switches.insert(sw.name.to_string(), val.to_string()),
            None => self.switches.remove(sw.name),
        };
    }

    /// The resolution as the game will use it.
    pub fn size(&self) -> (u32, u32) {
        (self.width.max(MIN_SIZE.0), self.height.max(MIN_SIZE.1))
    }
}

/// config.sys with these settings: `old` changed at the display fields when it's one the game
/// reads, otherwise a new file holding the game's own defaults.
pub fn config_sys(old: Option<&[u8]>, s: &GameSettings) -> Vec<u8> {
    let known = old.filter(|b| {
        b.len() >= OFF_HEIGHT + 4 && KNOWN_VERSIONS.contains(&u32::from_le_bytes(b[0..4].try_into().unwrap()))
    });
    let mut b = match known {
        Some(b) => b.to_vec(),
        None => default_config_sys(),
    };
    let (w, h) = s.size();
    let put = |b: &mut Vec<u8>, at: usize, v: u32| b[at..at + 4].copy_from_slice(&v.to_le_bytes());
    put(&mut b, OFF_FULL_SCREEN, u32::from(s.display != Display::Window));
    put(&mut b, OFF_WIDTH, w);
    put(&mut b, OFF_HEIGHT, h);
    b
}

/// What ffxivgame.exe uses when there is no config.sys (its reader's defaults).
fn default_config_sys() -> Vec<u8> {
    let mut b = vec![0u8; CONFIG_SYS_LEN];
    let words: [(usize, u32); 14] = [
        (0x00, CONFIG_SYS_VERSION),
        (0x08, 0xD800),
        (0x14, 1280),
        (0x18, 720),
        (0x1C, 7),
        (0x20, 4),
        (0x28, 1),
        (0x2C, 2),
        (0x50, 1),
        (0x58, 2),
        (0x5C, 1),
        (0x04, 0),
        (0x10, 0),
        (0x24, 0),
    ];
    for (at, v) in words {
        b[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
    // +0x60: the font, "MS UI Gothic"; +0xA0: an empty string
    for (i, u) in "MS UI Gothic".encode_utf16().enumerate() {
        b[0x60 + 2 * i..0x62 + 2 * i].copy_from_slice(&u.to_le_bytes());
    }
    b
}

/// The game's My Games folder: under our user folder for our client, the real Documents for
/// the stock ffxivgame.exe.
pub fn my_games(native: bool) -> PathBuf {
    let docs = if native {
        crate::config::dir().join("user").join("Documents")
    } else {
        dirs::document_dir().unwrap_or_else(|| PathBuf::from("."))
    };
    docs.join("My Games").join("FINAL FANTASY XIV")
}

/// Writes config.sys into `my_games` (made if missing), when it would change.
pub fn apply_config_sys(my_games: &Path, s: &GameSettings) -> std::io::Result<()> {
    let path = my_games.join("config.sys");
    let old = std::fs::read(&path).ok();
    let new = config_sys(old.as_deref(), s);
    if old.as_deref() != Some(new.as_slice()) {
        std::fs::create_dir_all(my_games)?;
        std::fs::write(&path, &new)?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The resolution and full screen a config.sys holds, if it's one the game reads.
    fn read_config_sys(b: &[u8]) -> Option<(u32, u32, bool)> {
        if b.len() < OFF_HEIGHT + 4 || !KNOWN_VERSIONS.contains(&u32::from_le_bytes(b[0..4].try_into().unwrap())) {
            return None;
        }
        let at = |o: usize| u32::from_le_bytes(b[o..o + 4].try_into().unwrap());
        Some((at(OFF_WIDTH), at(OFF_HEIGHT), at(OFF_FULL_SCREEN) != 0))
    }

    fn word(b: &[u8], at: usize) -> u32 {
        u32::from_le_bytes(b[at..at + 4].try_into().unwrap())
    }

    #[test]
    fn new_config_sys_has_the_games_defaults() {
        let b = config_sys(None, &GameSettings::default());
        assert_eq!(b.len(), 0xE0);
        assert_eq!(word(&b, 0), CONFIG_SYS_VERSION);
        assert_eq!((word(&b, 0x14), word(&b, 0x18), word(&b, 0x10)), (1280, 720, 0));
        assert_eq!((word(&b, 0x08), word(&b, 0x1C), word(&b, 0x2C), word(&b, 0x5C)), (0xD800, 7, 2, 1));
        assert_eq!(&b[0x60..0x64], &[b'M', 0, b'S', 0]);
        assert_eq!(&b[0xA0..0xA2], &[0, 0]);
    }

    #[test]
    fn existing_config_sys_changes_only_the_display_fields() {
        let mut old = default_config_sys();
        old[0x1C] = 3; // something ffxivconfig.exe set
        old.extend_from_slice(&[9, 9]); // and a longer file stays as long
        let s = GameSettings { width: 1920, height: 1080, display: Display::Borderless, ..Default::default() };
        let b = config_sys(Some(&old), &s);
        assert_eq!(b.len(), old.len());
        assert_eq!(read_config_sys(&b), Some((1920, 1080, true)));
        assert_eq!(b[0x1C], 3);
        let differ: Vec<usize> = (0..b.len()).filter(|&i| b[i] != old[i]).collect();
        assert!(differ.iter().all(|&i| (0x10..0x1C).contains(&i)), "{differ:x?}");
    }

    #[test]
    fn unknown_config_sys_is_replaced() {
        let b = config_sys(Some(b"junk"), &GameSettings::default());
        assert_eq!(word(&b, 0), CONFIG_SYS_VERSION);
        let older = {
            let mut o = vec![0u8; 0x60];
            o[0..4].copy_from_slice(&0x2010_1020u32.to_le_bytes());
            o
        };
        let b = config_sys(Some(&older), &GameSettings { width: 1600, height: 900, ..Default::default() });
        assert_eq!(word(&b, 0), 0x2010_1020);
        assert_eq!(read_config_sys(&b), Some((1600, 900, false)));
    }

    #[test]
    fn small_resolutions_are_raised_like_the_game_does() {
        let s = GameSettings { width: 800, height: 600, ..Default::default() };
        assert_eq!(read_config_sys(&config_sys(None, &s)), Some((1024, 720, false)));
    }

    fn set(env: &[(String, Option<String>)]) -> Vec<(String, String)> {
        resolve(env).into_iter().filter_map(|(k, v)| v.map(|v| (k, v))).collect()
    }

    fn pairs(p: &[(&str, &str)]) -> Vec<(String, String)> {
        p.iter().map(|(a, b)| (a.to_string(), b.to_string())).collect()
    }

    #[test]
    fn env_from_settings() {
        let d = GameSettings::default();
        assert!(set(&d.env(false, 0)).is_empty());
        // left at the default, a managed switch is unset, so a setx value doesn't leak in
        let r = resolve(&d.env(false, 0));
        for k in ["XIVLEGACY_NOAUDIO", "XIVLEGACY_VSYNC", "XIVLEGACY_PARALLEL", "XIVLEGACY_BUG_TAG"] {
            assert_eq!(r.get(k), Some(&None), "{k}");
        }
        assert_eq!(set(&d.env(true, 0)), pairs(&[("XIVLEGACY_NOAUDIO", "1")]));
        let quiet = GameSettings { volume: 40, vsync: VSync::Off, ..Default::default() };
        assert_eq!(set(&quiet.env(false, 0)), pairs(&[("XIVLEGACY_VOLUME", "40"), ("XIVLEGACY_VSYNC", "0")]));
        let off = GameSettings { sound: false, volume: 40, ..Default::default() };
        assert_eq!(set(&off.env(false, 0)), pairs(&[("XIVLEGACY_NOAUDIO", "1")]));
        let full = GameSettings { display: Display::FullScreen, ..Default::default() };
        assert_eq!(set(&full.env(false, 0)), pairs(&[("XIVLEGACY_FULLSCREEN", "1")]));
        assert_eq!(read_config_sys(&config_sys(None, &full)), Some((1280, 720, true)));
        let api = GameSettings { graphics_api: "vulkan".into(), ..Default::default() };
        assert_eq!(set(&api.env(false, 0)).len(), usize::from(!cfg!(target_os = "macos")));
    }

    #[test]
    fn test_switches_and_profiles() {
        let mut g = GameSettings::default();
        let cam = SWITCHES.iter().find(|s| s.name == "XIVLEGACY_MONSTERCAM").unwrap();
        g.set_switch(cam, 2);
        assert_eq!(g.switch_index(cam), 2);
        assert_eq!(set(&g.env(false, 0)), pairs(&[("XIVLEGACY_MONSTERCAM", "2")]));
        g.set_switch(cam, 0);
        assert!(g.switches.is_empty());
        // a stale value in launcher.json that no choice has is left unset
        g.switches.insert("XIVLEGACY_CPUSETS".into(), "7".into());
        assert!(set(&g.env(false, 0)).is_empty());
        g.switches.clear();
        let b = PROFILES.iter().position(|p| p.tag == "speed B").unwrap();
        assert_eq!(
            set(&g.env(false, b)),
            pairs(&[("XIVLEGACY_BUG_TAG", "speed B"), ("XIVLEGACY_FRAMESTATS", "1"), ("XIVLEGACY_PARALLEL", "1")])
        );
        assert_eq!(g.shown(false, b), vec!["XIVLEGACY_BUG_TAG=speed B", "XIVLEGACY_FRAMESTATS=1", "XIVLEGACY_PARALLEL=1"]);
        // a profile wins over Settings: FRAMESTATS Off (unset) still comes out on
        let fs = SWITCHES.iter().find(|s| s.name == "XIVLEGACY_FRAMESTATS").unwrap();
        g.set_switch(fs, 0);
        assert_eq!(resolve(&g.env(false, b)).get("XIVLEGACY_FRAMESTATS"), Some(&Some("1".to_string())));
        // the extra box still wins
        g.extra_env = "XIVLEGACY_PARALLEL=".into();
        assert_eq!(resolve(&g.env(false, b)).get("XIVLEGACY_PARALLEL"), Some(&None));
        // every profile's tag is "speed <letter>": A to I in order, then P (the profiler)
        for (i, p) in PROFILES.iter().enumerate().skip(1) {
            let letter = if i <= 9 { (b'A' + i as u8 - 1) as char } else { 'P' };
            assert_eq!(p.tag, format!("speed {}", letter));
        }
    }

    #[test]
    fn extra_switches() {
        let text = "XIVLEGACY_CPUSETS=0\n  # a comment\nsetx XIVLEGACY_MONSTERCAM 2\nset XIVLEGACY_PARALLEL=1\n\
                    export XIVLEGACY_SPINNAP=\"0\"\nXIVLEGACY_NOPBUF=   # clear it\nnot a switch\n9BAD=1\n";
        let (ok, bad) = parse_extra(text);
        let s = |a: &str, b: Option<&str>| (a.to_string(), b.map(str::to_string));
        assert_eq!(
            ok,
            vec![
                s("XIVLEGACY_CPUSETS", Some("0")),
                s("XIVLEGACY_MONSTERCAM", Some("2")),
                s("XIVLEGACY_PARALLEL", Some("1")),
                s("XIVLEGACY_SPINNAP", Some("0")),
                s("XIVLEGACY_NOPBUF", None),
            ]
        );
        assert_eq!(bad.iter().map(|b| b.line).collect::<Vec<_>>(), vec![7, 8]);
        // the extra box wins over the settings above it
        let g = GameSettings { sound: false, extra_env: "XIVLEGACY_NOAUDIO=".into(), ..Default::default() };
        assert_eq!(resolve(&g.env(false, 0)).get("XIVLEGACY_NOAUDIO"), Some(&None));
    }

    #[test]
    fn old_launcher_json_gets_default_settings() {
        let c: crate::config::Config = serde_json::from_str(r#"{"username":"a","remember":true}"#).unwrap();
        assert_eq!(c.game, GameSettings::default());
        let c: crate::config::Config = serde_json::from_str(r#"{"game":{"sound":false}}"#).unwrap();
        assert!(!c.game.sound);
        assert_eq!(c.game.volume, 100);
    }
}
