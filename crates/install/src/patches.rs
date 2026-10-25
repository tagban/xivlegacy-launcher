//! Finding and applying the ZIPATCH chain from whatever the player has:
//! `ffxiv_patches.zip`, a folder, or individual `.patch` files (any mix).

use std::collections::BTreeMap;
use std::fs::File;
use std::io::{BufReader, Read};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sha1::{Digest, Sha1};

use crate::manifest::{self, PATCHES};
use crate::progress::{Counter, Counting};
use crate::zipatch::{self, Stats};

/// Where one patch's bytes come from.
#[derive(Debug, Clone)]
enum Origin {
    File(PathBuf),
    Zip { archive: PathBuf, entry: String },
}

fn leaf(path: &str) -> &str {
    path.rsplit(['/', '\\']).next().unwrap_or(path)
}

/// Finds every known patch among `inputs`. Each input may be a .zip, a
/// folder (searched recursively, zips inside included) or a .patch file.
fn locate(inputs: &[PathBuf]) -> Result<BTreeMap<&'static str, Origin>> {
    let mut found = BTreeMap::new();
    let mut add = |name: &str, origin: Origin| {
        if let Some(p) = manifest::find(leaf(name)) {
            found.entry(p.name).or_insert(origin);
        }
    };
    let mut stack: Vec<PathBuf> = inputs.to_vec();
    while let Some(p) = stack.pop() {
        if p.is_dir() {
            for e in std::fs::read_dir(&p).with_context(|| format!("reading {}", p.display()))? {
                stack.push(e?.path());
            }
            continue;
        }
        let lower = p.to_string_lossy().to_ascii_lowercase();
        if lower.ends_with(".zip") {
            let zip = zip::ZipArchive::new(BufReader::new(File::open(&p)?)).with_context(|| format!("opening {}", p.display()))?;
            for name in zip.file_names() {
                add(name, Origin::Zip { archive: p.clone(), entry: name.to_string() });
            }
        } else if lower.ends_with(".patch") {
            add(&p.to_string_lossy(), Origin::File(p.clone()));
        }
    }
    Ok(found)
}

/// Checks that every patch is present; returns them in apply order.
pub struct PatchSet {
    found: BTreeMap<&'static str, Origin>,
}

impl PatchSet {
    pub fn find(inputs: &[PathBuf]) -> Result<Self> {
        let found = locate(inputs)?;
        let missing: Vec<&str> = PATCHES.iter().map(|p| p.name).filter(|n| !found.contains_key(n)).collect();
        if !missing.is_empty() {
            bail!(
                "{} of {} patches are missing: {}{}",
                missing.len(),
                PATCHES.len(),
                missing.iter().take(6).copied().collect::<Vec<_>>().join(", "),
                if missing.len() > 6 { ", …" } else { "" }
            );
        }
        Ok(PatchSet { found })
    }

    pub fn total_size(&self) -> u64 {
        manifest::total_size()
    }

    /// Applies all patches in order, then writes `boot.ver` / `game.ver`.
    /// `counter` advances by each patch's raw bytes as they are read.
    pub fn apply(&self, root: &Path, counter: &Counter, mut on_patch: impl FnMut(&str)) -> Result<Stats> {
        let mut stats = Stats::default();
        let mut zips: BTreeMap<PathBuf, zip::ZipArchive<BufReader<File>>> = BTreeMap::new();
        for p in PATCHES {
            on_patch(p.name);
            let origin = &self.found[p.name];
            let mut hasher = Sha1::new();
            let mut size = 0u64;
            let check = |hasher: Sha1, size: u64| -> Result<()> {
                let got = hex::encode(hasher.finalize());
                if size != p.size || got != p.sha1 {
                    bail!("{} is damaged or a different file (size {size}, SHA-1 {got})", p.name);
                }
                Ok(())
            };
            match origin {
                Origin::File(path) => {
                    let r = Hashing { inner: Counting::new(BufReader::with_capacity(1 << 20, File::open(path)?), counter.clone()), hasher: &mut hasher, size: &mut size };
                    zipatch::apply(r, root, &mut stats).with_context(|| p.name)?;
                }
                Origin::Zip { archive, entry } => {
                    if !zips.contains_key(archive.as_path()) {
                        zips.insert(archive.clone(), zip::ZipArchive::new(BufReader::new(File::open(archive)?))?);
                    }
                    let zip = zips.get_mut(archive.as_path()).unwrap();
                    let e = zip.by_name(entry)?;
                    let r = Hashing { inner: Counting::new(BufReader::with_capacity(1 << 20, e), counter.clone()), hasher: &mut hasher, size: &mut size };
                    zipatch::apply(r, root, &mut stats).with_context(|| p.name)?;
                }
            }
            check(hasher, size)?;
        }
        write_versions(root)?;
        Ok(stats)
    }
}

/// Hashes everything read through it.
struct Hashing<'a, R> {
    inner: R,
    hasher: &'a mut Sha1,
    size: &'a mut u64,
}

impl<R: Read> Read for Hashing<'_, R> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.hasher.update(&buf[..n]);
        *self.size += n as u64;
        Ok(n)
    }
}

/// `D2012.09.19.0001.patch` -> `2012.09.19.0001`
fn version_of(name: &str) -> &str {
    name.trim_start_matches(['D', 'd']).trim_end_matches(".patch")
}

/// Records the versions applied, as the retail updater did. The client reports `game.ver`.
fn write_versions(root: &Path) -> Result<()> {
    // The boot patch is the first; everything after it is a game patch.
    std::fs::write(root.join("boot.ver"), version_of(PATCHES[0].name))?;
    std::fs::write(root.join("game.ver"), version_of(PATCHES[PATCHES.len() - 1].name))?;
    Ok(())
}
