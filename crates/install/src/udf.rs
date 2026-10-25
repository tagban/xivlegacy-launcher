//! Read-only UDF (1.02, as on the FFXIV 1.x disc) reader: list and read files
//! straight from the .iso, no mounting.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

use anyhow::{Context, Result, bail, ensure};

const SECTOR: u64 = 2048;

mod tag {
    pub const AVDP: u16 = 2;
    pub const PARTITION: u16 = 5;
    pub const LOGICAL_VOLUME: u16 = 6;
    pub const TERMINATING: u16 = 8;
    pub const FILE_SET: u16 = 256;
    pub const FILE_ID: u16 = 257;
    pub const FILE_ENTRY: u16 = 261;
    pub const EXT_FILE_ENTRY: u16 = 266;
}

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}

/// A run of bytes on the image.
#[derive(Debug, Clone, Copy)]
struct Extent {
    offset: u64,
    len: u64,
}

#[derive(Debug, Clone)]
pub struct Entry {
    pub name: String,
    pub is_dir: bool,
    pub size: u64,
    extents: Vec<Extent>,
    /// Embedded data (allocation type 3).
    inline: Option<Vec<u8>>,
}

pub struct Udf {
    file: File,
    partition_start: u64,
    block: u64,
    root: Entry,
}

/// UDF dstring / d-characters: first byte 8 (Latin-1) or 16 (UCS-2 BE).
fn dchars(b: &[u8]) -> String {
    match b.first() {
        Some(8) => b[1..].iter().map(|&c| c as char).collect(),
        Some(16) => b[1..].chunks_exact(2).map(|c| char::from_u32(u16::from_be_bytes([c[0], c[1]]) as u32).unwrap_or('?')).collect(),
        _ => String::new(),
    }
}

impl Udf {
    pub fn open(path: &Path) -> Result<Self> {
        let file = File::open(path).with_context(|| format!("opening {}", path.display()))?;
        let mut udf = Udf { file, partition_start: 0, block: SECTOR, root: Entry::dir_placeholder() };

        let avdp = udf.read_at(256 * SECTOR, SECTOR as usize)?;
        ensure!(u16le(&avdp, 0) == tag::AVDP, "no UDF anchor at sector 256");
        let (vds_len, vds_loc) = (u32le(&avdp, 16) as u64, u32le(&avdp, 20) as u64);

        let mut fsd = None;
        for i in 0..vds_len.div_ceil(SECTOR) {
            let d = udf.read_at((vds_loc + i) * SECTOR, SECTOR as usize)?;
            match u16le(&d, 0) {
                tag::PARTITION => udf.partition_start = u32le(&d, 188) as u64 * SECTOR,
                tag::LOGICAL_VOLUME => {
                    udf.block = u32le(&d, 212) as u64;
                    fsd = Some(u32le(&d, 248 + 4) as u64); // long_ad lbn
                }
                tag::TERMINATING => break,
                _ => {}
            }
        }
        let fsd = fsd.context("no logical volume descriptor")?;
        let set = udf.read_at(udf.partition_start + fsd * udf.block, udf.block as usize)?;
        ensure!(u16le(&set, 0) == tag::FILE_SET, "no file set descriptor");
        let root_lbn = u32le(&set, 400 + 4) as u64;
        udf.root = udf.file_entry(root_lbn, String::new())?;
        Ok(udf)
    }

    fn read_at(&mut self, offset: u64, len: usize) -> Result<Vec<u8>> {
        let mut buf = vec![0u8; len];
        self.file.seek(SeekFrom::Start(offset))?;
        self.file.read_exact(&mut buf)?;
        Ok(buf)
    }

    fn file_entry(&mut self, lbn: u64, name: String) -> Result<Entry> {
        let fe = self.read_at(self.partition_start + lbn * self.block, self.block as usize)?;
        let (l_ea, l_ad, ad_start) = match u16le(&fe, 0) {
            tag::FILE_ENTRY => (u32le(&fe, 168) as usize, u32le(&fe, 172) as usize, 176),
            tag::EXT_FILE_ENTRY => (u32le(&fe, 208) as usize, u32le(&fe, 212) as usize, 216),
            t => bail!("expected a file entry at block {lbn}, found tag {t}"),
        };
        let file_type = fe[16 + 11];
        let is_dir = file_type == 4;
        let size = u64le(&fe, 56);
        let flags = u16le(&fe, 16 + 18);
        let ads = &fe[ad_start + l_ea..ad_start + l_ea + l_ad];
        let mut extents = Vec::new();
        let mut inline = None;
        match flags & 7 {
            0 => {
                for ad in ads.chunks_exact(8) {
                    let len = (u32le(ad, 0) & 0x3FFF_FFFF) as u64;
                    if len == 0 {
                        break;
                    }
                    extents.push(Extent { offset: self.partition_start + u32le(ad, 4) as u64 * self.block, len });
                }
            }
            1 => {
                for ad in ads.chunks_exact(16) {
                    let len = (u32le(ad, 0) & 0x3FFF_FFFF) as u64;
                    if len == 0 {
                        break;
                    }
                    extents.push(Extent { offset: self.partition_start + u32le(ad, 4) as u64 * self.block, len });
                }
            }
            3 => inline = Some(ads.to_vec()),
            t => bail!("unsupported allocation type {t}"),
        }
        Ok(Entry { name, is_dir, size, extents, inline })
    }

    /// Reads up to `len` bytes of `entry` from `pos`.
    pub fn read(&mut self, entry: &Entry, pos: u64, buf: &mut [u8]) -> Result<usize> {
        if let Some(data) = &entry.inline {
            let start = (pos as usize).min(data.len());
            let n = buf.len().min(data.len() - start);
            buf[..n].copy_from_slice(&data[start..start + n]);
            return Ok(n);
        }
        let want = buf.len().min(entry.size.saturating_sub(pos) as usize);
        let mut done = 0;
        let mut ext_base = 0u64;
        for ext in &entry.extents {
            let cur = pos + done as u64;
            if done == want {
                break;
            }
            if cur < ext_base + ext.len {
                let within = cur - ext_base;
                let n = (want - done).min((ext.len - within) as usize);
                self.file.seek(SeekFrom::Start(ext.offset + within))?;
                self.file.read_exact(&mut buf[done..done + n])?;
                done += n;
            }
            ext_base += ext.len;
        }
        Ok(done)
    }

    pub fn read_all(&mut self, entry: &Entry) -> Result<Vec<u8>> {
        let mut buf = vec![0u8; entry.size as usize];
        let n = self.read(entry, 0, &mut buf)?;
        ensure!(n == buf.len(), "short read of {}", entry.name);
        Ok(buf)
    }

    pub fn list(&mut self, dir: &Entry) -> Result<Vec<Entry>> {
        ensure!(dir.is_dir, "{} is not a directory", dir.name);
        let data = self.read_all(dir)?;
        let mut out = Vec::new();
        let mut o = 0;
        while o + 38 <= data.len() {
            ensure!(u16le(&data, o) == tag::FILE_ID, "bad directory record in {}", dir.name);
            let chars = data[o + 18];
            let l_fi = data[o + 19] as usize;
            let icb_lbn = u32le(&data, o + 20 + 4) as u64;
            let l_iu = u16le(&data, o + 36) as usize;
            let name_at = o + 38 + l_iu;
            if chars & 0x08 == 0 && chars & 0x04 == 0 {
                // not parent, not deleted
                let name = dchars(&data[name_at..name_at + l_fi]);
                out.push(self.file_entry(icb_lbn, name)?);
            }
            o += (38 + l_iu + l_fi + 3) & !3;
        }
        Ok(out)
    }

    /// Finds `path` (`/`-separated, case-insensitive).
    pub fn find(&mut self, path: &str) -> Result<Entry> {
        let mut cur = self.root.clone();
        for part in path.split('/').filter(|p| !p.is_empty()) {
            let children = self.list(&cur)?;
            cur = children
                .into_iter()
                .find(|e| e.name.eq_ignore_ascii_case(part))
                .with_context(|| format!("{path}: no {part}"))?;
        }
        Ok(cur)
    }

    /// A reader over one file, for streaming large files.
    pub fn open_file<'a>(&'a mut self, entry: &'a Entry) -> UdfFile<'a> {
        UdfFile { udf: self, entry, pos: 0 }
    }

    /// A reader over one file with its own handle on the image.
    pub fn open_owned(&self, entry: &Entry) -> Result<OwnedFile> {
        let udf = Udf { file: self.file.try_clone()?, partition_start: self.partition_start, block: self.block, root: Entry::dir_placeholder() };
        Ok(OwnedFile { udf, entry: entry.clone(), pos: 0 })
    }
}

pub struct OwnedFile {
    udf: Udf,
    entry: Entry,
    pos: u64,
}

impl Read for OwnedFile {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.udf.read(&self.entry, self.pos, buf).map_err(std::io::Error::other)?;
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for OwnedFile {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        let new = match to {
            SeekFrom::Start(p) => p as i64,
            SeekFrom::End(d) => self.entry.size as i64 + d,
            SeekFrom::Current(d) => self.pos as i64 + d,
        };
        if new < 0 {
            return Err(std::io::Error::other("seek before start"));
        }
        self.pos = new as u64;
        Ok(self.pos)
    }
}

impl Entry {
    fn dir_placeholder() -> Self {
        Entry { name: String::new(), is_dir: true, size: 0, extents: Vec::new(), inline: None }
    }
}

pub struct UdfFile<'a> {
    udf: &'a mut Udf,
    entry: &'a Entry,
    pos: u64,
}

impl Read for UdfFile<'_> {
    fn read(&mut self, buf: &mut [u8]) -> std::io::Result<usize> {
        let n = self.udf.read(self.entry, self.pos, buf).map_err(std::io::Error::other)?;
        self.pos += n as u64;
        Ok(n)
    }
}

impl Seek for UdfFile<'_> {
    fn seek(&mut self, to: SeekFrom) -> std::io::Result<u64> {
        let size = self.entry.size as i64;
        let new = match to {
            SeekFrom::Start(p) => p as i64,
            SeekFrom::End(d) => size + d,
            SeekFrom::Current(d) => self.pos as i64 + d,
        };
        if new < 0 {
            return Err(std::io::Error::other("seek before start"));
        }
        self.pos = new as u64;
        Ok(self.pos)
    }
}
