//! The retail disc → install layout, and getting the .iso out of a .7z.

use std::fs::{self, File};
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use sha1::{Digest, Sha1};

use crate::ishield::{self, Cabinet};
use crate::progress::Counter;
use crate::udf::{OwnedFile, Udf};

/// The retail US/JP disc (Redump "Final Fantasy XIV Online (USA, Japan) (En,Ja,Fr,De)").
pub const ISO_SIZE: u64 = 5_527_535_616;
pub const ISO_SHA1: &str = "3df40d83b227484a302d3f51615d1d90f419daf6";

/// Where each InstallShield file group goes in the install.
fn group_target(name: &str, files: &[ishield::FileEntry]) -> Option<&'static str> {
    match name {
        "NewComponent1" => Some("client"),
        "NewComponent2" => Some("data"),
        // The programs' group has a Japanese name ("ファイル"); find it by content.
        _ if files.iter().any(|f| f.path.eq_ignore_ascii_case("ffxivboot.exe")) => Some(""),
        _ => None, // InstallShield's own support files
    }
}

struct DiscVolumes<'a> {
    udf: &'a Udf,
    entries: Vec<crate::udf::Entry>,
}

impl ishield::Volumes for DiscVolumes<'_> {
    type Reader = OwnedFile;
    fn open(&mut self, n: u16) -> Result<OwnedFile> {
        let name = format!("data{n}.cab");
        let e = self.entries.iter().find(|e| e.name.eq_ignore_ascii_case(&name)).with_context(|| format!("{name} is not on the disc"))?;
        self.udf.open_owned(e)
    }
}

fn native(root: &Path, rel: &str) -> PathBuf {
    rel.split(['\\', '/']).filter(|p| !p.is_empty()).fold(root.to_path_buf(), |p, part| p.join(part))
}

/// A disc image opened for installing: the cabinet index and the file plan.
pub struct Disc {
    udf: Udf,
    entries: Vec<crate::udf::Entry>,
    cab: Cabinet,
    plan: Vec<(u32, String, &'static str)>,
}

impl Disc {
    pub fn open(iso: &Path) -> Result<Self> {
        let mut udf = Udf::open(iso)?;
        let dir = udf.find("data").context("this does not look like the FFXIV disc (no data folder)")?;
        let entries = udf.list(&dir)?;
        let hdr_entry = entries.iter().find(|e| e.name.eq_ignore_ascii_case("data1.hdr")).context("no data/data1.hdr on the disc")?;
        let hdr = udf.read_all(hdr_entry)?;
        let cab = Cabinet::parse(&hdr)?;
        let mut plan = Vec::new();
        for g in &cab.groups {
            let files = &cab.files[g.first_file as usize..=g.last_file as usize];
            let Some(target) = group_target(&g.name, files) else { continue };
            for f in files {
                if f.flags & ishield::FILE_INVALID == 0 && f.data_offset != 0 {
                    plan.push((f.index, f.path.clone(), target));
                }
            }
        }
        if !plan.iter().any(|(_, p, t)| t.is_empty() && p.eq_ignore_ascii_case("ffxivboot.exe")) {
            bail!("this disc has no FINAL FANTASY XIV program files");
        }
        Ok(Disc { udf, entries, cab, plan })
    }

    pub fn file_count(&self) -> usize {
        self.plan.len()
    }

    /// Bytes the install will take from the disc.
    pub fn total_size(&self) -> u64 {
        self.plan.iter().map(|(i, _, _)| self.cab.files[*i as usize].expanded_size).sum()
    }

    /// Unpacks the game's files into `out` (client/, data/, root), skipping the
    /// first `skip` files. `counter` advances by bytes written.
    pub fn install(&self, out: &Path, skip: usize, counter: &Counter) -> Result<u64> {
        let mut volumes = DiscVolumes { udf: &self.udf, entries: self.entries.clone() };
        let mut files = 0;
        for (index, rel, target) in self.plan.iter().skip(skip) {
            let path = native(&out.join(target), rel);
            if let Some(parent) = path.parent() {
                fs::create_dir_all(parent)?;
            }
            let f = &self.cab.files[*index as usize];
            let mut w = CountingWriter { inner: BufWriter::new(File::create(&path).with_context(|| format!("creating {}", path.display()))?), counter };
            ishield::extract(&self.cab, &mut volumes, *index, &mut w)
                .with_context(|| format!("file {index} {} (flags {:#x}, volume {}, offset {:#x})", f.path, f.flags, f.volume, f.data_offset))?;
            w.inner.flush()?;
            files += 1;
        }
        Ok(files)
    }
}

struct CountingWriter<'a, W> {
    inner: W,
    counter: &'a Counter,
}

impl<W: Write> Write for CountingWriter<'_, W> {
    fn write(&mut self, buf: &[u8]) -> std::io::Result<usize> {
        let n = self.inner.write(buf)?;
        self.counter.add(n as u64);
        Ok(n)
    }
    fn flush(&mut self) -> std::io::Result<()> {
        self.inner.flush()
    }
}

/// Unpacks the single .iso from a .7z into `dir`, checking its SHA-1 when it
/// is the known retail disc. Returns the .iso path.
pub fn unpack_7z(archive: &Path, dir: &Path, counter: &Counter) -> Result<PathBuf> {
    fs::create_dir_all(dir)?;
    let mut reader = sevenz_rust2::ArchiveReader::open(archive, sevenz_rust2::Password::empty())
        .with_context(|| format!("opening {}", archive.display()))?;
    let mut result: Option<PathBuf> = None;
    let mut failure: Option<anyhow::Error> = None;
    reader
        .for_each_entries(|entry, data| {
            if entry.is_directory() || !entry.name().to_ascii_lowercase().ends_with(".iso") || result.is_some() {
                std::io::copy(data, &mut std::io::sink())?;
                return Ok(true);
            }
            let name = Path::new(entry.name()).file_name().map(|n| n.to_owned()).unwrap_or_else(|| "disc.iso".into());
            let path = dir.join(&name);
            let part = dir.join(format!("{}.part", name.to_string_lossy()));
            let mut out = BufWriter::new(File::create(&part)?);
            let mut h = Sha1::new();
            let mut buf = vec![0u8; 1 << 20];
            let mut size = 0u64;
            loop {
                let n = data.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                out.write_all(&buf[..n])?;
                h.update(&buf[..n]);
                counter.add(n as u64);
                size += n as u64;
            }
            out.flush()?;
            drop(out);
            let got = hex::encode(h.finalize());
            if size == ISO_SIZE && got != ISO_SHA1 {
                let _ = fs::remove_file(&part);
                failure = Some(anyhow::anyhow!("the disc image in {} is damaged (SHA-1 {got})", archive.display()));
                return Ok(false);
            }
            fs::rename(&part, &path)?;
            result = Some(path);
            Ok(true)
        })
        .with_context(|| format!("unpacking {}", archive.display()))?;
    if let Some(e) = failure {
        return Err(e);
    }
    result.with_context(|| format!("{} contains no .iso", archive.display()))
}
