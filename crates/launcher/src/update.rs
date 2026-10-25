//! Keeping XIVLegacy up to date: at start it asks GitHub for the newest
//! release (test builds included); when it is newer than this one, the window
//! offers it. On Windows, Update downloads the new XIVLegacy.exe, checks it
//! against the release's SHA256SUMS.txt, swaps it in for this program (the
//! running copy is renamed aside, which Windows allows) and restarts.

use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde::Deserialize;

const RELEASES: &str = "https://api.github.com/repos/tagban/xivlegacy-launcher/releases?per_page=10";
pub const RELEASES_PAGE: &str = "https://github.com/tagban/xivlegacy-launcher/releases";
const ASSET: &str = "XIVLegacy.exe";

#[derive(Deserialize)]
struct Release {
    tag_name: String,
    draft: bool,
    html_url: String,
    assets: Vec<Asset>,
}

#[derive(Deserialize)]
struct Asset {
    name: String,
    browser_download_url: String,
}

#[derive(Debug, Clone)]
pub struct Available {
    pub version: String,
    pub page: String,
    exe_url: Option<String>,
    sums_url: Option<String>,
}

fn version(tag: &str) -> Option<(u32, u32, u32)> {
    let mut it = tag.trim_start_matches('v').split('.').map(|p| p.parse::<u32>().ok());
    Some((it.next()??, it.next()??, it.next().flatten().unwrap_or(0)))
}

fn agent() -> ureq::Agent {
    ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(10))
        .timeout_read(Duration::from_secs(60))
        .user_agent(concat!("XIVLegacy/", env!("CARGO_PKG_VERSION")))
        .build()
}

/// The newest release, if it is newer than `current`.
pub fn check(current: &str) -> Result<Option<Available>> {
    let releases: Vec<Release> = agent().get(RELEASES).call()?.into_json()?;
    let mine = version(current).context("bad version")?;
    let newest = releases
        .into_iter()
        .filter(|r| !r.draft)
        .filter_map(|r| version(&r.tag_name).map(|v| (v, r)))
        .max_by_key(|(v, _)| *v);
    Ok(newest.filter(|(v, _)| *v > mine).map(|(_, r)| {
        let url = |name: &str| r.assets.iter().find(|a| a.name == name).map(|a| a.browser_download_url.clone());
        Available {
            version: r.tag_name.trim_start_matches('v').to_string(),
            page: r.html_url.clone(),
            exe_url: url(ASSET),
            sums_url: url("SHA256SUMS.txt"),
        }
    }))
}

/// Whether this platform can update itself (Windows); elsewhere the window
/// links to the releases page instead.
pub fn can_install(a: &Available) -> bool {
    cfg!(windows) && a.exe_url.is_some() && a.sums_url.is_some()
}

/// Leftover from the last update: the previous program, renamed aside.
pub fn old_path() -> Option<std::path::PathBuf> {
    std::env::current_exe().ok().map(|e| e.with_extension("old.exe"))
}

/// Removes the previous program after an update.
pub fn clean_up() {
    if let Some(p) = old_path() {
        let _ = std::fs::remove_file(p);
    }
}

/// Downloads, checks and swaps in the new program, then starts it. On success
/// the caller should exit.
pub fn install(a: &Available) -> Result<()> {
    use sha2::{Digest, Sha256};
    use std::io::Read;

    let (Some(exe_url), Some(sums_url)) = (&a.exe_url, &a.sums_url) else {
        bail!("that release has no Windows program");
    };
    let sums = agent().get(sums_url).call()?.into_string()?;
    let want = sums
        .lines()
        .find_map(|l| {
            let (hash, name) = l.split_once(char::is_whitespace)?;
            (name.trim().trim_start_matches('*') == ASSET).then(|| hash.trim().to_ascii_lowercase())
        })
        .context("SHA256SUMS.txt doesn't list XIVLegacy.exe")?;
    let mut bytes = Vec::new();
    agent().get(exe_url).call()?.into_reader().take(200 << 20).read_to_end(&mut bytes)?;
    let got = hex::encode(Sha256::digest(&bytes));
    if got != want {
        bail!("the downloaded update didn't match its checksum; try again later");
    }

    let me = std::env::current_exe()?;
    let old = old_path().context("no program path")?;
    let new = me.with_extension("new.exe");
    std::fs::write(&new, &bytes)?;
    let _ = std::fs::remove_file(&old);
    std::fs::rename(&me, &old).context("moving the running program aside")?;
    if let Err(e) = std::fs::rename(&new, &me) {
        let _ = std::fs::rename(&old, &me);
        return Err(e).context("putting the update in place");
    }
    std::process::Command::new(&me).spawn().context("starting the updated program")?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions() {
        assert!(version("v0.1.10").unwrap() > version("v0.1.9").unwrap());
        assert_eq!(version("0.2"), Some((0, 2, 0)));
        assert_eq!(version("nightly"), None);
    }
}

#[cfg(test)]
mod live {
    /// Asks GitHub: an old version is offered the newest release, a future one nothing.
    #[test]
    #[ignore]
    fn checks_github() {
        let a = super::check("0.1.0").unwrap().expect("a newer release");
        println!("offered {} ({})", a.version, a.page);
        assert!(a.exe_url.is_some() && a.sums_url.is_some());
        assert!(super::check("99.0.0").unwrap().is_none());
    }
}
