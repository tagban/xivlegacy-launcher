//! Static recompiler: a 32-bit x86 image (ffxivgame.exe) to C, as a library a launcher can call
//! in-process. `recomp::run` reads the image, its function metadata (meta/*.meta.json, from the
//! Ghidra discovery scripts) and optional instruction patches, and writes funcs.h, funcs_NNN.c and
//! table.c. Everything it writes is derived from Square Enix's code and is never committed.
//!
//! No global state: `on` receives progress points and messages, on the calling thread.
pub mod disasm;
pub mod pe;
pub mod program;
pub mod recomp;
pub mod x86c;

use std::fmt;
use std::path::{Path, PathBuf};

#[derive(Debug)]
pub enum Error {
    Io { path: PathBuf, err: std::io::Error },
    Pe(String),
    Meta(String),
    Translate(String),
}

impl Error {
    pub fn io(path: &Path, err: std::io::Error) -> Error {
        Error::Io { path: path.to_path_buf(), err }
    }
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Error::Io { path, err } => write!(f, "{}: {}", path.display(), err),
            Error::Pe(s) => write!(f, "PE: {}", s),
            Error::Meta(s) | Error::Translate(s) => f.write_str(s),
        }
    }
}

impl std::error::Error for Error {}

/// What the pipeline reports as it goes.
#[derive(Clone, Copy, Debug)]
pub enum Event<'a> {
    /// `@progress <phase> <what> <done> <total>` (the lines a launcher's progress bar reads)
    Progress { phase: &'a str, what: &'a str, done: usize, total: usize },
    /// a line for the log
    Message(&'a str),
}

impl Event<'_> {
    /// The line --progress (or XIVLEGACY_PROGRESS=1) prints for a progress point.
    pub fn progress_line(&self) -> Option<String> {
        match self {
            Event::Progress { phase, what, done, total } => Some(format!("@progress {} {} {} {}", phase, what, done, total)),
            Event::Message(_) => None,
        }
    }
}

pub(crate) fn read(path: &Path) -> Result<Vec<u8>, Error> {
    std::fs::read(path).map_err(|e| Error::io(path, e))
}

pub(crate) fn read_json(path: &Path) -> Result<serde_json::Value, Error> {
    let text = std::fs::read_to_string(path).map_err(|e| Error::io(path, e))?;
    serde_json::from_str(&text).map_err(|e| Error::Meta(format!("{}: {}", path.display(), e)))
}

/// Leave unchanged files alone so an incremental C build only recompiles what moved. (Compared
/// as Python's text mode reads it: universal newlines.)
pub fn write_if_changed(path: &Path, text: &str) -> Result<(), Error> {
    if let Ok(old) = std::fs::read(path) {
        if let Ok(old) = String::from_utf8(old) {
            let same = if old.contains('\r') { old.replace("\r\n", "\n").replace('\r', "\n") == text } else { old == text };
            if same {
                return Ok(());
            }
        }
    }
    std::fs::write(path, text).map_err(|e| Error::io(path, e))
}

/// A hexadecimal number as Python's int(v, 16) reads it ("0x" optional, "_" separators, a sign).
pub fn parse_hex(v: &str) -> Result<u64, Error> {
    let t = v.trim();
    let (neg, t) = match t.strip_prefix('-') { Some(r) => (true, r), None => (false, t.strip_prefix('+').unwrap_or(t)) };
    let t = t.strip_prefix("0x").or_else(|| t.strip_prefix("0X")).unwrap_or(t);
    let n = u64::from_str_radix(&t.replace('_', ""), 16)
        .map_err(|_| Error::Meta(format!("invalid literal for int() with base 16: '{}'", v)))?;
    if neg { Ok(n.wrapping_neg()) } else { Ok(n) }
}
