//! What the launcher remembers: the game folder, the servers and who signed in
//! last. Passwords are not here; they go to the system's credential store
//! (Windows Credential Manager, the macOS keychain, the Secret Service on
//! Linux) when "Remember me" is on. A Linux desktop without a Secret Service
//! keeps them in passwords.json in the settings folder, readable only by the
//! player.

use std::fs;
use std::path::PathBuf;

use serde::{Deserialize, Serialize};

pub const DEFAULT_SERVER: &str = "xiv.ffxi.cc";

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub struct Server {
    pub name: String,
    pub host: String,
    /// The server's address on this network, found when `host` didn't answer
    /// from here (a home server behind a router without hairpin NAT).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub lan: Option<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(default)]
pub struct Config {
    pub game_dir: Option<PathBuf>,
    pub servers: Vec<Server>,
    pub server: usize,
    pub username: String,
    pub remember: bool,
    /// The Desktop and Start menu shortcuts were made (Windows; see shortcut.rs). Once, so a
    /// player who deletes them isn't given them back.
    pub shortcuts: bool,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            game_dir: None,
            servers: vec![Server { name: "XIVLegacy".into(), host: DEFAULT_SERVER.into(), lan: None }],
            server: 0,
            username: String::new(),
            remember: true,
            shortcuts: false,
        }
    }
}

/// %APPDATA%\XIVLegacy on Windows, ~/Library/Application Support/XIVLegacy on macOS,
/// $XDG_DATA_HOME/XIVLegacy (~/.local/share/XIVLegacy) on Linux: besides the settings it holds
/// the game module, zig and the logs, which are data rather than configuration.
pub fn dir() -> PathBuf {
    let base = if cfg!(target_os = "linux") { dirs::data_dir() } else { dirs::config_dir() };
    base.unwrap_or_else(|| PathBuf::from(".")).join("XIVLegacy")
}

/// Where a new install goes: %LOCALAPPDATA%\XIVLegacy\FINAL FANTASY XIV 1.23b
/// (~/.local/share/XIVLegacy/FINAL FANTASY XIV 1.23b on Linux).
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

/// Appends a line to launcher.log in the settings folder (what happened at
/// Join, for troubleshooting). The log is trimmed when it passes 256 KB.
pub fn log(line: &str) {
    use std::io::Write;
    let path = dir().join("launcher.log");
    let _ = fs::create_dir_all(dir());
    if path.metadata().is_ok_and(|m| m.len() > 256 * 1024) {
        let _ = fs::rename(&path, dir().join("launcher.old.log"));
    }
    let secs = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(path) {
        let _ = writeln!(f, "[{secs}] {line}");
    }
}

/// The saved password for (server, user), if any. On macOS, the earlier Mac
/// launcher's keychain entry (service "XIVLegacy", one password per user) is
/// taken over the first time.
#[allow(unreachable_code)]
pub fn password(host: &str, user: &str) -> Option<String> {
    if let Some(p) = keyring::Entry::new(&format!("XIVLegacy {host}"), user).ok().and_then(|e| e.get_password().ok()) {
        return Some(p);
    }
    #[cfg(target_os = "linux")]
    return file_store::get(host, user);
    #[cfg(target_os = "macos")]
    {
        let old = keyring::Entry::new("XIVLegacy", user).ok()?.get_password().ok()?;
        set_password(host, user, Some(&old));
        return Some(old);
    }
    None
}

pub fn set_password(host: &str, user: &str, password: Option<&str>) {
    let stored = keyring::Entry::new(&format!("XIVLegacy {host}"), user).and_then(|e| match password {
        Some(p) => e.set_password(p),
        None => e.delete_credential().or_else(|e| if matches!(e, keyring::Error::NoEntry) { Ok(()) } else { Err(e) }),
    });
    #[cfg(not(target_os = "linux"))]
    let _ = stored;
    #[cfg(target_os = "linux")]
    {
        // no Secret Service (a desktop without GNOME Keyring or KWallet, or none running): the file
        if let Err(e) = &stored {
            if password.is_some() {
                log(&format!("no Secret Service ({e}); keeping the password in passwords.json"));
            }
        }
        file_store::set(host, user, if stored.is_ok() { None } else { password });
    }
}

#[cfg(target_os = "linux")]
/// Linux without a Secret Service: passwords.json in the settings folder, mode 0600, so only the
/// player's own account can read it (as with an SSH key; it is not encrypted).
mod file_store {
    use std::collections::BTreeMap;

    fn path() -> std::path::PathBuf {
        super::dir().join("passwords.json")
    }

    fn load() -> BTreeMap<String, String> {
        std::fs::read(path()).ok().and_then(|b| serde_json::from_slice(&b).ok()).unwrap_or_default()
    }

    fn key(host: &str, user: &str) -> String {
        format!("{user}@{host}")
    }

    pub fn get(host: &str, user: &str) -> Option<String> {
        load().remove(&key(host, user))
    }

    pub fn set(host: &str, user: &str, password: Option<&str>) {
        let mut all = load();
        let changed = match password {
            Some(p) => all.insert(key(host, user), p.to_string()).as_deref() != Some(p),
            None => all.remove(&key(host, user)).is_some(),
        };
        if !changed {
            return;
        }
        if all.is_empty() {
            let _ = std::fs::remove_file(path());
            return;
        }
        let _ = std::fs::create_dir_all(super::dir());
        let Ok(json) = serde_json::to_vec_pretty(&all) else { return };
        let tmp = path().with_extension("json.part");
        let mut opts = std::fs::OpenOptions::new();
        opts.write(true).create(true).truncate(true);
        #[cfg(unix)]
        std::os::unix::fs::OpenOptionsExt::mode(&mut opts, 0o600);
        if let Ok(mut f) = opts.open(&tmp) {
            use std::io::Write;
            if f.write_all(&json).is_ok() {
                let _ = std::fs::rename(&tmp, path());
            }
        }
    }
}
