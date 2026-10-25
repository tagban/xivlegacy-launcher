//! The two DirectX 9 libraries ffxivgame.exe imports that Windows no longer
//! ships: d3dx9_41.dll and xinput1_3.dll. The disc carries Microsoft's March
//! 2009 redistributable; instead of running its installer (administrator
//! rights, writes to the system folders), the two DLLs are taken from its
//! cabinets and put next to the game, where Windows looks first.

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
