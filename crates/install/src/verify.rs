//! Checking an existing FFXIV folder against a known-good 1.23b install, so a
//! player who already has the full game can point the launcher at it.
//!
//! `data/install-1.23b.manifest.gz` lists every file of a correct install
//! (`sha1 size path` per line), taken from an install built and verified by
//! this tool. Names and hashes only; no game data.

use std::collections::HashMap;
use std::fs::File;
use std::io::{BufRead, BufReader, Read};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result};
use flate2::read::GzDecoder;
use sha1::{Digest, Sha1};

use crate::progress::Counter;

static MANIFEST_GZ: &[u8] = include_bytes!("../data/install-1.23b.manifest.gz");

/// The version a complete install reports.
pub const GAME_VERSION: &str = "2012.09.19.0001";

/// Programs always hashed, even in a quick check.
const PROGRAMS: [&str; 5] = ["ffxivgame.exe", "ffxivboot.exe", "ffxivlogin.exe", "ffxivconfig.exe", "ffxivupdater.exe"];

struct Expected {
    sha1: String,
    size: u64,
    path: String,
}

fn manifest() -> Result<Vec<Expected>> {
    let mut out = Vec::with_capacity(190_000);
    for line in BufReader::new(GzDecoder::new(MANIFEST_GZ)).lines() {
        let line = line?;
        let mut it = line.splitn(3, ' ');
        let (Some(sha1), Some(size), Some(path)) = (it.next(), it.next(), it.next()) else { continue };
        out.push(Expected { sha1: sha1.to_string(), size: size.parse()?, path: path.to_string() });
    }
    Ok(out)
}

/// Every file under `root`, keyed by lowercased `/`-separated relative path.
fn index(root: &Path) -> Result<HashMap<String, PathBuf>> {
    let mut map = HashMap::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(dir) = stack.pop() {
        for e in std::fs::read_dir(&dir).with_context(|| format!("reading {}", dir.display()))? {
            let e = e?;
            let p = e.path();
            if e.file_type()?.is_dir() {
                stack.push(p);
            } else if let Ok(rel) = p.strip_prefix(root) {
                let key = rel.to_string_lossy().replace('\\', "/").to_ascii_lowercase();
                map.insert(key, p);
            }
        }
    }
    Ok(map)
}

fn sha1_of(path: &Path, counter: &Counter) -> Result<String> {
    let mut f = File::open(path)?;
    let mut h = Sha1::new();
    let mut buf = vec![0u8; 1 << 20];
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            return Ok(hex::encode(h.finalize()));
        }
        h.update(&buf[..n]);
        counter.add(n as u64);
    }
}

#[derive(Debug, Default)]
pub struct Check {
    pub expected_files: usize,
    pub missing: Vec<String>,
    /// Present but with the wrong size or contents.
    pub wrong: Vec<String>,
    /// What `game.ver` says, if it exists.
    pub game_version: Option<String>,
    /// Whether every file's contents were hashed (a full check).
    pub full: bool,
}

impl Check {
    pub fn ok(&self) -> bool {
        self.missing.is_empty() && self.wrong.is_empty() && self.game_version.as_deref() == Some(GAME_VERSION)
    }
}

/// Bytes a full check reads (for progress).
pub fn total_size() -> Result<u64> {
    Ok(manifest()?.iter().map(|e| e.size).sum())
}

/// Checks `root`. A quick check compares every file's presence and size and
/// hashes the programs; a full check hashes everything. Extra files are fine.
pub fn check(root: &Path, full: bool, counter: &Counter) -> Result<Check> {
    let expected = manifest()?;
    let have = index(root)?;
    let mut c = Check { expected_files: expected.len(), full, ..Default::default() };
    c.game_version = std::fs::read_to_string(root.join("game.ver")).ok().map(|s| s.trim().to_string());
    for e in &expected {
        let Some(path) = have.get(&e.path.to_ascii_lowercase()) else {
            c.missing.push(e.path.clone());
            counter.add(e.size);
            continue;
        };
        if e.path.ends_with(".ver") {
            // Version files may carry a trailing newline; game.ver is checked above.
            counter.add(e.size);
            continue;
        }
        let size = path.metadata()?.len();
        if size != e.size {
            c.wrong.push(e.path.clone());
            counter.add(e.size);
            continue;
        }
        if full || PROGRAMS.contains(&e.path.as_str()) {
            if sha1_of(path, counter)? != e.sha1 {
                c.wrong.push(e.path.clone());
            }
        } else {
            counter.add(e.size);
        }
    }
    Ok(c)
}
