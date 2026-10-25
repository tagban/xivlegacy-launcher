//! Looks for the disc image and the patches the player may already have, so
//! setup only downloads what is missing: the usual folders (Downloads, Desktop,
//! Documents, the launcher's own folder) and, on Windows, the top of every drive.

use std::fs;
use std::path::{Path, PathBuf};

use xivlegacy_install::disc::ISO_SIZE;
use xivlegacy_install::setup::{ISO_7Z, PATCHES_ZIP};

#[derive(Debug, Default, Clone)]
pub struct Found {
    /// A .iso of the disc, or the .7z it is preserved in.
    pub disc: Option<PathBuf>,
    /// ffxiv_patches.zip, or a folder of .patch files.
    pub patches: Option<PathBuf>,
}

fn roots() -> Vec<PathBuf> {
    let mut r: Vec<PathBuf> = [dirs::download_dir(), dirs::desktop_dir(), dirs::document_dir(), dirs::home_dir()]
        .into_iter()
        .flatten()
        .collect();
    if let Some(dir) = std::env::current_exe().ok().and_then(|e| e.parent().map(Path::to_path_buf)) {
        r.insert(0, dir);
    }
    if cfg!(windows) {
        for d in b'C'..=b'Z' {
            let p = PathBuf::from(format!("{}:\\", d as char));
            if p.exists() {
                r.push(p);
            }
        }
    }
    r
}

fn is_disc(p: &Path, size: u64) -> bool {
    let name = p.file_name().map(|n| n.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    (name.ends_with(".iso") && size == ISO_SIZE) || (name.ends_with(".7z") && size == ISO_7Z.size)
}

fn is_patch_zip(p: &Path, size: u64) -> bool {
    let name = p.file_name().map(|n| n.to_string_lossy().to_ascii_lowercase()).unwrap_or_default();
    name.ends_with(".zip") && (size == PATCHES_ZIP.size || name == "ffxiv_patches.zip")
}

/// A folder holding the patches as loose files (the 52 .patch files, possibly in subfolders).
fn is_patch_dir(p: &Path) -> bool {
    let count = |d: &Path| {
        fs::read_dir(d)
            .map(|it| it.flatten().filter(|e| e.path().extension().is_some_and(|x| x.eq_ignore_ascii_case("patch"))).count())
            .unwrap_or(0)
    };
    let mut n = count(p);
    if let Ok(it) = fs::read_dir(p) {
        for e in it.flatten().filter(|e| e.path().is_dir()) {
            n += count(&e.path());
        }
    }
    n >= 52
}

/// Searches two folder levels below each root.
pub fn scan() -> Found {
    let mut found = Found::default();
    for root in roots() {
        walk(&root, 0, &mut found);
        if found.disc.is_some() && found.patches.is_some() {
            break;
        }
    }
    found
}

fn walk(dir: &Path, depth: u32, found: &mut Found) {
    let Ok(it) = fs::read_dir(dir) else { return };
    let mut subdirs = Vec::new();
    for e in it.flatten() {
        let p = e.path();
        let Ok(meta) = e.metadata() else { continue };
        if meta.is_dir() {
            let name = e.file_name().to_string_lossy().to_ascii_lowercase();
            if !name.starts_with('.') && !matches!(name.as_str(), "windows" | "program files" | "program files (x86)" | "programdata" | "$recycle.bin" | "appdata" | "library") {
                subdirs.push(p);
            }
        } else if found.disc.is_none() && is_disc(&p, meta.len()) {
            found.disc = Some(p);
        } else if found.patches.is_none() && is_patch_zip(&p, meta.len()) {
            found.patches = Some(p);
        }
    }
    for d in subdirs {
        if found.patches.is_none() && is_patch_dir(&d) {
            found.patches = Some(d.clone());
        }
        if depth < 2 {
            walk(&d, depth + 1, found);
        }
    }
}
