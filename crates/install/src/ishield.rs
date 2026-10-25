//! InstallShield (v6+, the FFXIV 1.x disc is engine 15) cabinet reader.
//!
//! Follows libunshield (MIT, twogood/unshield): `data1.hdr` holds the file
//! table, directories and file groups; file data lives in `data<N>.cab`
//! volumes, compressed as a run of `[u16 len][raw deflate]` blocks, and a file
//! may continue from one volume into the next. Each file carries an MD5.

use std::io::{Read, Seek, SeekFrom, Write};

use anyhow::{Context, Result, bail, ensure};
use flate2::{Decompress, FlushDecompress, Status};
use md5::{Digest, Md5};

const CAB_SIGNATURE: u32 = 0x2863_5349;
const COMMON_HEADER_SIZE: usize = 20;
const VOLUME_HEADER_SIZE: usize = 64;

pub const FILE_SPLIT: u16 = 1;
pub const FILE_OBFUSCATED: u16 = 2;
pub const FILE_COMPRESSED: u16 = 4;
pub const FILE_INVALID: u16 = 8;

const LINK_PREV: u8 = 1;

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes(b[o..o + 4].try_into().unwrap())
}
fn u64le(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes(b[o..o + 8].try_into().unwrap())
}
fn cstr(b: &[u8], o: usize) -> String {
    let end = b[o..].iter().position(|&c| c == 0).map_or(b.len(), |n| o + n);
    // Names are ANSI; the FFXIV disc's are ASCII apart from one group name.
    b[o..end].iter().map(|&c| c as char).collect()
}

#[derive(Debug, Clone)]
pub struct FileEntry {
    pub index: u32,
    /// `directory\name`, backslash-separated as InstallShield stores it.
    pub path: String,
    pub flags: u16,
    pub expanded_size: u64,
    pub compressed_size: u64,
    pub data_offset: u64,
    pub md5: [u8; 16],
    pub volume: u16,
    pub link_previous: u32,
    pub link_flags: u8,
}

#[derive(Debug, Clone)]
pub struct FileGroup {
    pub name: String,
    pub first_file: u32,
    pub last_file: u32,
}

pub struct Cabinet {
    pub files: Vec<FileEntry>,
    pub groups: Vec<FileGroup>,
}

impl Cabinet {
    /// Parses `data1.hdr`.
    pub fn parse(hdr: &[u8]) -> Result<Self> {
        ensure!(hdr.len() >= COMMON_HEADER_SIZE && u32le(hdr, 0) == CAB_SIGNATURE, "not an InstallShield header");
        let version = u32le(hdr, 4);
        let major_version = match version >> 24 {
            1 => (version >> 12) & 0xf,
            2 | 4 => (version & 0xffff) / 100,
            _ => 0,
        }
        .max(5);
        ensure!(major_version >= 6, "InstallShield {major_version} cabinets are not supported");
        let cd = u32le(hdr, 12) as usize; // cab descriptor offset
        let file_table_offset = u32le(hdr, cd + 0x0c) as usize;
        let directory_count = u32le(hdr, cd + 0x1c) as usize;
        let file_count = u32le(hdr, cd + 0x28) as usize;
        let file_table_offset2 = u32le(hdr, cd + 0x2c) as usize;
        let ft = cd + file_table_offset;
        let table: Vec<u32> = (0..directory_count).map(|i| u32le(hdr, ft + i * 4)).collect();
        let dirs: Vec<String> = table.iter().map(|&o| cstr(hdr, ft + o as usize)).collect();

        let mut files = Vec::with_capacity(file_count);
        for i in 0..file_count {
            let p = ft + file_table_offset2 + i * 0x57;
            let name_offset = u32le(hdr, p + 0x3a) as usize;
            let dir = u16le(hdr, p + 0x3e) as usize;
            let name = cstr(hdr, ft + name_offset);
            let dir_name = dirs.get(dir).with_context(|| format!("file {i}: bad directory {dir}"))?;
            let path = if dir_name.is_empty() { name } else { format!("{dir_name}\\{name}") };
            files.push(FileEntry {
                index: i as u32,
                path,
                flags: u16le(hdr, p),
                expanded_size: u64le(hdr, p + 2),
                compressed_size: u64le(hdr, p + 10),
                data_offset: u64le(hdr, p + 18),
                md5: hdr[p + 26..p + 42].try_into().unwrap(),
                link_previous: u32le(hdr, p + 0x4c),
                link_flags: hdr[p + 0x54],
                volume: u16le(hdr, p + 0x55),
            });
        }

        let mut groups = Vec::new();
        for i in 0..71 {
            let mut next = u32le(hdr, cd + 0x3e + i * 4) as usize;
            while next != 0 {
                let list = cd + next;
                let descriptor = cd + u32le(hdr, list + 4) as usize;
                next = u32le(hdr, list + 8) as usize;
                let name = cstr(hdr, cd + u32le(hdr, descriptor) as usize);
                let first_file = u32le(hdr, descriptor + 4 + 0x12);
                let last_file = u32le(hdr, descriptor + 4 + 0x12 + 4);
                groups.push(FileGroup { name, first_file, last_file });
            }
        }
        Ok(Cabinet { files, groups })
    }
}

/// Opens cabinet volume `n` (`data<n>.cab`).
pub trait Volumes {
    type Reader: Read + Seek;
    fn open(&mut self, n: u16) -> Result<Self::Reader>;
}

struct VolumeHeader {
    first_file_index: u32,
    last_file_index: u32,
    first_file_offset: u64,
    first_file_size_expanded: u64,
    first_file_size_compressed: u64,
    last_file_offset: u64,
    last_file_size_expanded: u64,
    last_file_size_compressed: u64,
}

fn read_volume_header<R: Read>(r: &mut R) -> Result<VolumeHeader> {
    let mut b = [0u8; COMMON_HEADER_SIZE + VOLUME_HEADER_SIZE];
    r.read_exact(&mut b)?;
    ensure!(u32le(&b, 0) == CAB_SIGNATURE, "not a cabinet volume");
    let v = &b[COMMON_HEADER_SIZE..];
    let wide = |o: usize| u64::from(u32le(v, o)) | (u64::from(u32le(v, o + 4)) << 32);
    Ok(VolumeHeader {
        first_file_index: u32le(v, 8),
        last_file_index: u32le(v, 12),
        first_file_offset: wide(16),
        first_file_size_expanded: wide(24),
        first_file_size_compressed: wide(32),
        last_file_offset: wide(40),
        last_file_size_expanded: wide(48),
        last_file_size_compressed: wide(56),
    })
}

/// Reads one file's stored bytes, following it across volumes.
struct SpanReader<'v, V: Volumes> {
    volumes: &'v mut V,
    file: FileEntry,
    reader: V::Reader,
    volume: u16,
    left_in_volume: u64,
    obfuscation_seed: u32,
}

impl<'v, V: Volumes> SpanReader<'v, V> {
    fn new(volumes: &'v mut V, file: &FileEntry) -> Result<Self> {
        let reader = volumes.open(file.volume)?;
        let mut s = SpanReader {
            volumes,
            file: file.clone(),
            reader,
            volume: file.volume,
            left_in_volume: 0,
            obfuscation_seed: 0,
        };
        s.position()?;
        Ok(s)
    }

    /// Seeks to this file's data in the current volume.
    fn position(&mut self) -> Result<()> {
        self.reader.seek(SeekFrom::Start(0))?;
        let vh = read_volume_header(&mut self.reader)?;
        let compressed = self.file.flags & FILE_COMPRESSED != 0;
        let (offset, expanded, packed) = if self.file.flags & FILE_SPLIT != 0 {
            if self.file.index == vh.last_file_index && vh.last_file_offset != 0x7FFF_FFFF {
                (vh.last_file_offset, vh.last_file_size_expanded, vh.last_file_size_compressed)
            } else if self.file.index == vh.first_file_index {
                (vh.first_file_offset, vh.first_file_size_expanded, vh.first_file_size_compressed)
            } else {
                bail!("file {} is not in volume {}", self.file.index, self.volume);
            }
        } else {
            (self.file.data_offset, self.file.expanded_size, self.file.compressed_size)
        };
        self.left_in_volume = if compressed { packed } else { expanded };
        self.reader.seek(SeekFrom::Start(offset))?;
        Ok(())
    }

    fn read_exact(&mut self, buf: &mut [u8]) -> Result<()> {
        let mut done = 0;
        while done < buf.len() {
            if self.left_in_volume == 0 {
                self.volume += 1;
                self.reader = self.volumes.open(self.volume)?;
                self.position()?;
                ensure!(self.left_in_volume > 0, "file {} has no data in volume {}", self.file.index, self.volume);
            }
            let n = (buf.len() - done).min(self.left_in_volume as usize);
            self.reader.read_exact(&mut buf[done..done + n])?;
            self.left_in_volume -= n as u64;
            done += n;
        }
        if self.file.flags & FILE_OBFUSCATED != 0 {
            for b in buf.iter_mut() {
                *b = (*b ^ 0xd5).rotate_right(2).wrapping_sub((self.obfuscation_seed % 0x47) as u8);
                self.obfuscation_seed = self.obfuscation_seed.wrapping_add(1);
            }
        }
        Ok(())
    }
}

/// Extracts file `index` into `out`, checking its size and MD5.
pub fn extract<V: Volumes, W: Write>(cab: &Cabinet, volumes: &mut V, index: u32, out: &mut W) -> Result<u64> {
    // Identical files are stored once; later copies link back, possibly through a chain.
    let mut file = &cab.files[index as usize];
    let mut hops = 0;
    while file.link_flags & LINK_PREV != 0 {
        file = cab.files.get(file.link_previous as usize).context("bad file link")?;
        hops += 1;
        // Chains of identical files can be thousands long; only a true loop exceeds the file count.
        ensure!(hops <= cab.files.len(), "file link loop at {}", file.path);
    }
    ensure!(file.flags & FILE_INVALID == 0 && file.data_offset != 0, "file {} has no data", file.path);
    let mut reader = SpanReader::new(volumes, file)?;
    let mut md5 = Md5::new();
    let mut written = 0u64;
    if file.flags & FILE_COMPRESSED != 0 {
        let mut left = file.compressed_size;
        let mut input = vec![0u8; 0x10000];
        let mut output = vec![0u8; 0x10000 * 4];
        while left > 0 {
            let mut len = [0u8; 2];
            reader.read_exact(&mut len)?;
            let n = u16::from_le_bytes(len) as usize;
            ensure!(n > 0, "{}: empty compressed block", file.path);
            reader.read_exact(&mut input[..n])?;
            left = left.checked_sub(2 + n as u64).context("compressed size overrun")?;
            // Each block is a complete raw deflate stream.
            let mut z = Decompress::new(false);
            loop {
                let before_in = z.total_in() as usize;
                let before_out = z.total_out();
                let status = z.decompress(&input[before_in..n], &mut output, FlushDecompress::Finish)?;
                let produced = (z.total_out() - before_out) as usize;
                md5.update(&output[..produced]);
                out.write_all(&output[..produced])?;
                written += produced as u64;
                match status {
                    Status::StreamEnd => break,
                    _ if produced == 0 && z.total_in() as usize == before_in => {
                        bail!("{}: deflate block stalled", file.path)
                    }
                    _ => {}
                }
            }
        }
    } else {
        let mut left = file.expanded_size;
        let mut buf = vec![0u8; 1 << 20];
        while left > 0 {
            let n = buf.len().min(left as usize);
            reader.read_exact(&mut buf[..n])?;
            md5.update(&buf[..n]);
            out.write_all(&buf[..n])?;
            written += n as u64;
            left -= n as u64;
        }
    }
    ensure!(written == file.expanded_size, "{}: expanded to {written} bytes, expected {}", file.path, file.expanded_size);
    let digest: [u8; 16] = md5.finalize().into();
    ensure!(digest == file.md5, "{}: MD5 mismatch", file.path);
    Ok(written)
}
