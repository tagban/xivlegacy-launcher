//! DirectX for ffxivgame.exe, from the March 2009 redistributable on the disc.
//!
//! - d3dx9_41.dll and xinput1_3.dll, which the game imports and Windows no
//!   longer ships, are taken from its cabinets and put next to the game, where
//!   Windows looks first.
//! - The game also creates XAudio2 2.4 through COM, which only Microsoft's
//!   DirectX installer can register. On Windows the installer itself
//!   (DXSETUP.exe and its cabinets, about 100 MB) is kept beside the game
//!   folder, so the launcher can run it once, with the player's permission,
//!   after the disc image is gone.

use std::fs;
use std::io::{Cursor, Read};
use std::path::Path;

use anyhow::{Context, Result, bail};
use sha1::{Digest, Sha1};

use crate::udf::Udf;

/// (cabinet on the disc, file inside it, SHA-1 of that file)
const FILES: [(&str, &str, &str); 2] = [
    ("dx_march2009/Mar2009_d3dx9_41_x86.cab", "d3dx9_41.dll", "fafe52876151a08f39dbb6b4aa137dd85558ba5f"),
    ("dx_march2009/APR2007_xinput_x86.cab", "xinput1_3.dll", "d2a710b332de3ef7a576e0aed27b0ae66892b7e9"),
];

/// The files this step writes into the game folder.
pub fn file_names() -> impl Iterator<Item = &'static str> {
    FILES.iter().map(|f| f.1)
}

/// Whether `game` already has both libraries.
pub fn present(game: &Path) -> bool {
    file_names().all(|n| game.join(n).is_file())
}

/// Copies d3dx9_41.dll and xinput1_3.dll from the disc image into `out`.
pub fn install(iso: &Path, out: &Path) -> Result<()> {
    let mut udf = Udf::open(iso)?;
    for (cab_path, name, sha1) in FILES {
        let entry = udf.find(cab_path).with_context(|| format!("no {cab_path} on the disc"))?;
        let bytes = udf.read_all(&entry)?;
        let data = extract(&bytes, name).with_context(|| format!("{name} from {cab_path}"))?;
        let got = hex::encode(Sha1::digest(&data));
        if got != sha1 {
            bail!("{name} from {cab_path} has SHA-1 {got}, expected {sha1}");
        }
        fs::write(out.join(name), &data).with_context(|| format!("writing {}", out.join(name).display()))?;
    }
    Ok(())
}

/// Where the DirectX installer is kept for a game folder: "<game folder> DirectX".
pub fn installer_dir(game: &Path) -> std::path::PathBuf {
    game.with_file_name(format!(
        "{} DirectX",
        game.file_name().map(|n| n.to_string_lossy()).unwrap_or_default()
    ))
}

/// Copies the disc's DirectX installer (dx_march2009) into `dest`.
pub fn copy_installer(iso: &Path, dest: &Path) -> Result<()> {
    let mut udf = Udf::open(iso)?;
    let dir = udf.find("dx_march2009").context("no dx_march2009 folder on the disc")?;
    fs::create_dir_all(dest)?;
    for e in udf.list(&dir)?.into_iter().filter(|e| !e.is_dir) {
        let bytes = udf.read_all(&e)?;
        fs::write(dest.join(&e.name), bytes).with_context(|| format!("writing {}", dest.join(&e.name).display()))?;
    }
    if !dest.join("DXSETUP.exe").is_file() {
        bail!("the disc's DirectX folder has no DXSETUP.exe");
    }
    Ok(())
}

fn extract(cabinet: &[u8], name: &str) -> Result<Vec<u8>> {
    let mut cab = cab::Cabinet::new(Cursor::new(cabinet))?;
    let found = cab
        .folder_entries()
        .flat_map(|f| f.file_entries())
        .map(|f| f.name().to_string())
        .find(|n| n.eq_ignore_ascii_case(name))
        .context("not in the cabinet")?;
    let mut data = Vec::new();
    cab.read_file(&found)?.read_to_end(&mut data)?;
    Ok(data)
}
