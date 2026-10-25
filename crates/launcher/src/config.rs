//! What the launcher remembers: the game folder, the servers and who signed in
//! last. Passwords are not here; they go to the system's credential store
//! (Windows Credential Manager, the macOS keychain) when "Remember me" is on.

use std::fs;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

pub const DEFAULT_SERVER: &str = "xiv.ffxi.cc";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Server {
    pub name: String,
    pub host: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub game_dir: Option<PathBuf>,
    pub servers: Vec<Server>,
    pub server: usize,
    pub username: String,
    pub remember: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            game_dir: None,
            servers: vec![Server { name: "XIVLegacy".into(), host: DEFAULT_SERVER.into() }],
            server: 0,
            username: String::new(),
            remember: true,
        }
    }
}

/// %APPDATA%\XIVLegacy on Windows, ~/Library/Application Support/XIVLegacy on macOS.
pub fn dir() -> PathBuf {
    dirs::config_dir().unwrap_or_else(|| PathBuf::from(".")).join("XIVLegacy")
}

/// Where a new install goes: %LOCALAPPDATA%\XIVLegacy\FINAL FANTASY XIV 1.23b.
pub fn default_game_dir() -> PathBuf {
    dirs::data_local_dir().unwrap_or_else(|| PathBuf::from(".")).join("XIVLegacy").join("FINAL FANTASY XIV 1.23b")
}

impl Config {
    pub fn load() -> Config {
        let mut c: Config =
            fs::read(dir().join("launcher.json")).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default();
        if c.servers.is_empty() {
            c.servers = Config::default().servers;
        }
        c.server = c.server.min(c.servers.len() - 1);
        c
    }

    pub fn save(&self) {
        let _ = fs::create_dir_all(dir());
        if let Ok(json) = serde_json::to_vec_pretty(self) {
            let _ = fs::write(dir().join("launcher.json"), json);
        }
    }

    pub fn host(&self) -> &str {
        &self.servers[self.server].host
    }
}

/// The saved password for (server, user), if any.
pub fn password(host: &str, user: &str) -> Option<String> {
    keyring::Entry::new(&format!("XIVLegacy {host}"), user).ok()?.get_password().ok()
}

pub fn set_password(host: &str, user: &str, password: Option<&str>) {
    let Ok(e) = keyring::Entry::new(&format!("XIVLegacy {host}"), user) else { return };
    let _ = match password {
        Some(p) => e.set_password(p),
        None => e.delete_credential(),
    };
}
