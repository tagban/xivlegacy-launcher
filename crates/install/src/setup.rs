//! The whole setup: get the disc and patches (local files or archive.org),
//! unpack, install, patch — reporting progress the entire time.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result, bail};

use crate::disc::{self, Disc};
use crate::download::{self, Source};
use crate::patches::PatchSet;
use crate::progress::{Counter, Phase, Report};

/// The disc image on archive.org (Redump dump, packed as .7z), and a copy on Google Drive.
pub const ISO_7Z: Source = Source {
    name: "Final Fantasy XIV Online (USA, Japan) (En,Ja,Fr,De).7z",
    url: "https://archive.org/download/Nova_FFXIV_JPN/Final%20Fantasy%20XIV%20Online%20%28USA%2C%20Japan%29%20%28En%2CJa%2CFr%2CDe%29.7z",
    mirrors: &["https://drive.usercontent.google.com/download?id=1jJgBGn98CRbpOo8qRw8GP7T1VoSR58LM&export=download&confirm=t"],
    size: 4_894_433_109,
    sha1: "6cc15039ba362a095ab422239d066f9ba77e28b1",
};

/// All 52 patches on archive.org (and a copy on Google Drive, used if archive.org keeps failing).
pub const PATCHES_ZIP: Source = Source {
    name: "ffxiv_patches.zip",
    url: "https://archive.org/download/ffxiv_patches/ffxiv_patches.zip",
    mirrors: &["https://drive.usercontent.google.com/download?id=1Hv6AEjpjM3WplGF251-AnO2gvZljvUMn&export=download&confirm=t"],
    size: 6_174_308_511,
    sha1: "65d763b150f46d9a68c4c58c763ee14966a74acd",
};

#[derive(Debug, Clone)]
pub enum DiscSource {
    /// A .iso or a .7z holding one.
    File(PathBuf),
    Download,
}

#[derive(Debug, Clone)]
pub enum PatchSource {
    /// Any mix of ffxiv_patches.zip, folders and .patch files.
    Files(Vec<PathBuf>),
    Download,
}

#[derive(Debug, Clone)]
pub struct Plan {
    pub disc: DiscSource,
    pub patches: PatchSource,
    /// Where downloads and the unpacked .iso are kept.
    pub cache: PathBuf,
    /// The new install folder (must not exist yet).
    pub out: PathBuf,
}

/// Calls `report` every quarter second with `counter` until stopped.
struct Ticker {
    stop: Arc<AtomicBool>,
    handle: Option<thread::JoinHandle<()>>,
}

impl Ticker {
    fn start(report: Arc<dyn Report>, phase: Phase, counter: Counter, total: u64, detail: Arc<Mutex<String>>) -> Self {
        let stop = Arc::new(AtomicBool::new(false));
        let s = stop.clone();
        let handle = thread::spawn(move || {
            while !s.load(Ordering::Relaxed) {
                report.report(phase, counter.get().min(total), total, &detail.lock().unwrap());
                thread::sleep(Duration::from_millis(250));
            }
        });
        Ticker { stop, handle: Some(handle) }
    }
}

impl Drop for Ticker {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(h) = self.handle.take() {
            let _ = h.join();
        }
    }
}

pub fn run(plan: &Plan, report: Arc<dyn Report>) -> Result<()> {
    if plan.out.exists() {
        bail!("{} already exists; choose a new folder", plan.out.display());
    }
    // Check local inputs up front, before any long download.
    if let DiscSource::File(p) = &plan.disc {
        if !p.is_file() {
            bail!("{} not found", p.display());
        }
    }
    let local_patches = match &plan.patches {
        PatchSource::Files(inputs) => Some(PatchSet::find(inputs)?),
        PatchSource::Download => None,
    };

    // 1. Downloads, in parallel.
    let downloads: Vec<&Source> = [
        matches!(plan.disc, DiscSource::Download).then_some(&ISO_7Z),
        matches!(plan.patches, PatchSource::Download).then_some(&PATCHES_ZIP),
    ]
    .into_iter()
    .flatten()
    .collect();
    let mut fetched: Vec<PathBuf> = Vec::new();
    if !downloads.is_empty() {
        let total: u64 = downloads.iter().map(|s| s.size).sum();
        let counter = Counter::default();
        let detail = Arc::new(Mutex::new(downloads.iter().map(|s| s.name).collect::<Vec<_>>().join(" + ")));
        let _t = Ticker::start(report.clone(), Phase::Download, counter.clone(), total, detail);
        let results: Vec<Result<PathBuf>> = thread::scope(|s| {
            let hs: Vec<_> = downloads.iter().map(|src| {
                let c = counter.clone();
                let cache = plan.cache.clone();
                s.spawn(move || download::fetch(src, &cache, &c))
            }).collect();
            hs.into_iter().map(|h| h.join().unwrap_or_else(|_| Err(anyhow::anyhow!("download thread panicked")))).collect()
        });
        for r in results {
            fetched.push(r?);
        }
        report.report(Phase::Download, total, total, "");
    }
    let mut fetched = fetched.into_iter();
    let disc_file = match &plan.disc {
        DiscSource::File(p) => p.clone(),
        DiscSource::Download => fetched.next().context("disc download missing")?,
    };
    let patch_set = match local_patches {
        Some(set) => set,
        None => PatchSet::find(&[fetched.next().context("patch download missing")?])?,
    };

    // 2. The .iso: unpack a .7z, or check a .iso.
    let iso = if disc_file.to_string_lossy().to_ascii_lowercase().ends_with(".7z") {
        let counter = Counter::default();
        let detail = Arc::new(Mutex::new(disc_file.file_name().unwrap_or_default().to_string_lossy().into_owned()));
        let _t = Ticker::start(report.clone(), Phase::UnpackIso, counter.clone(), disc::ISO_SIZE, detail);
        disc::unpack_7z(&disc_file, &plan.cache, &counter)?
    } else {
        disc_file
    };

    // 3. Install from the disc.
    let disc = Disc::open(&iso)?;
    let counter = Counter::default();
    let detail = Arc::new(Mutex::new(format!("{} files", disc.file_count())));
    {
        let _t = Ticker::start(report.clone(), Phase::Disc, counter.clone(), disc.total_size(), detail);
        disc.install(&plan.out, 0, &counter)?;
    }
    // The DirectX 9 libraries the game needs, from the disc's redistributable.
    crate::directx::install(&iso, &plan.out)?;

    // 4. Patch.
    let counter = Counter::default();
    let detail = Arc::new(Mutex::new(String::new()));
    let total = patch_set.total_size();
    {
        let _t = Ticker::start(report.clone(), Phase::Patch, counter.clone(), total, detail.clone());
        patch_set.apply(&plan.out, &counter, |name| *detail.lock().unwrap() = name.to_string())?;
    }
    report.report(Phase::Patch, total, total, "done");
    Ok(())
}

/// Default cache folder for downloads.
pub fn default_cache(out: &Path) -> PathBuf {
    out.with_file_name(format!("{} downloads", out.file_name().map(|n| n.to_string_lossy()).unwrap_or_default()))
}
