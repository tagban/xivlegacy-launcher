//! FFXIV 1.x ZIPATCH files.
//!
//! A 12-byte `\x91ZIPATCH\r\n\x1a\n` signature, then PNG-style chunks:
//! `size u32 BE, tag[4], body[size], crc32 u32 BE`.
//!
//! - `FHDR`, `APLY`, `APFS`: metadata, skipped.
//! - `ADIR` / `DELD`: `len u32 BE, path` — create / delete a directory.
//! - `ETRY`: `len u32 BE, path, count u32 BE`, then `count` items:
//!   `mode u32 LE, src_sha1[20], dst_sha1[20], comp u32 LE,
//!   csize u32 BE, old_size u32 BE, new_size u32 BE` and, on the last item
//!   only, `csize` bytes of data. Mode `A` (0x41) add, `M` (0x4D) modify,
//!   `D` (0x44) delete; comp `Z` (0x5A) zlib, `N` (0x4E) stored. The data is
//!   the whole new file; `dst_sha1` is its SHA-1 and, for modify, `src_sha1`
//!   that of the file it replaces.

use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail, ensure};
use flate2::read::ZlibDecoder;
use sha1::{Digest, Sha1};

const MAGIC: [u8; 12] = [0x91, b'Z', b'I', b'P', b'A', b'T', b'C', b'H', 0x0D, 0x0A, 0x1A, 0x0A];
const ZERO: [u8; 20] = [0; 20];

const MODE_ADD: u32 = 0x41;
const MODE_DELETE: u32 = 0x44;
const MODE_MODIFY: u32 = 0x4D;
const COMP_ZLIB: u32 = 0x5A;
const COMP_STORED: u32 = 0x4E;

#[derive(Debug, Default, Clone)]
pub struct Stats {
    pub written: u64,
    pub deleted: u64,
    pub dirs_removed: u64,
    /// Modify entries whose old file matched `src_sha1`.
    pub base_verified: u64,
    /// Modify entries whose old file was missing or different. Informational:
    /// the retail patches' `src_sha1` often names a version other than the one
    /// the previous patch wrote, and every entry carries the whole new file.
    pub base_mismatch: Vec<String>,
}

fn u32be<R: Read>(r: &mut R) -> io::Result<u32> {
    let mut b = [0u8; 4];
    r.read_exact(&mut b)?;
    Ok(u32::from_be_bytes(b))
}
fn u32le<R: Read>(r: &mut R) -> io::Result<u32> {
    let mut b = [0u8; 4];
    r.read_exact(&mut b)?;
    Ok(u32::from_le_bytes(b))
}

fn read_path<R: Read>(r: &mut R) -> Result<String> {
    let n = u32be(r)? as usize;
    ensure!(n < 4096, "path length {n}");
    let mut b = vec![0u8; n];
    r.read_exact(&mut b)?;
    Ok(String::from_utf8_lossy(&b).into_owned())
}

/// Joins a patch path onto `root`, refusing anything that would escape it.
fn native(root: &Path, rel: &str) -> Result<PathBuf> {
    let mut p = root.to_path_buf();
    for part in rel.split(['\\', '/']).filter(|s| !s.is_empty() && *s != ".") {
        ensure!(part != "..", "patch path escapes the install: {rel}");
        p.push(part);
    }
    Ok(p)
}

fn sha1_file(p: &Path) -> io::Result<[u8; 20]> {
    let mut f = File::open(p)?;
    let mut h = Sha1::new();
    io::copy(&mut f, &mut h)?;
    Ok(h.finalize().into())
}

/// Writes everything `r` yields to `path` (via a temp file), returning its SHA-1 and size.
fn write_file<R: Read>(path: &Path, mut r: R) -> Result<([u8; 20], u64)> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let tmp = path.with_extension("xivtmp");
    let mut h = Sha1::new();
    let mut size = 0u64;
    {
        let mut out = io::BufWriter::new(File::create(&tmp).with_context(|| format!("creating {}", tmp.display()))?);
        let mut buf = vec![0u8; 1 << 20];
        loop {
            let n = r.read(&mut buf)?;
            if n == 0 {
                break;
            }
            h.update(&buf[..n]);
            out.write_all(&buf[..n])?;
            size += n as u64;
        }
        out.flush()?;
    }
    fs::rename(&tmp, path)?;
    Ok((h.finalize().into(), size))
}

/// Applies one patch read from `r` (positioned at its start) to `root`.
pub fn apply<R: Read>(mut r: R, root: &Path, stats: &mut Stats) -> Result<()> {
    let mut sig = [0u8; 12];
    r.read_exact(&mut sig)?;
    ensure!(sig == MAGIC, "not a ZIPATCH file");
    loop {
        let mut head = [0u8; 8];
        match r.read_exact(&mut head) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e.into()),
        }
        let size = u32::from_be_bytes(head[0..4].try_into().unwrap()) as u64;
        let tag: [u8; 4] = head[4..8].try_into().unwrap();
        let mut body = (&mut r).take(size);
        match &tag {
            b"FHDR" | b"APLY" | b"APFS" => {}
            b"ADIR" => fs::create_dir_all(native(root, &read_path(&mut body)?)?)?,
            b"DELD" => {
                let p = native(root, &read_path(&mut body)?)?;
                if p.is_dir() {
                    fs::remove_dir_all(&p)?;
                    stats.dirs_removed += 1;
                }
            }
            b"ETRY" => entry(&mut body, root, stats)?,
            other => bail!("unknown ZIPATCH chunk {:?}", String::from_utf8_lossy(other)),
        }
        io::copy(&mut body, &mut io::sink())?;
        let mut crc = [0u8; 4];
        r.read_exact(&mut crc)?;
    }
    Ok(())
}

fn entry<R: Read>(r: &mut R, root: &Path, stats: &mut Stats) -> Result<()> {
    let rel = read_path(r)?;
    let path = native(root, &rel)?;
    let count = u32be(r)?;
    for i in 0..count {
        let mode = u32le(r)?;
        let mut src = [0u8; 20];
        let mut dst = [0u8; 20];
        r.read_exact(&mut src)?;
        r.read_exact(&mut dst)?;
        let comp = u32le(r)?;
        let csize = u32be(r)? as u64;
        let _old_size = u32be(r)?;
        let new_size = u32be(r)? as u64;
        ensure!(matches!(mode, MODE_ADD | MODE_DELETE | MODE_MODIFY), "{rel}: unknown mode {mode:#x}");
        ensure!(matches!(comp, COMP_ZLIB | COMP_STORED), "{rel}: unknown compression {comp:#x}");
        if i + 1 < count {
            ensure!(csize == 0, "{rel}: data on a non-final item");
            continue;
        }
        if mode == MODE_MODIFY && src != ZERO {
            match sha1_file(&path) {
                Ok(h) if h == src => stats.base_verified += 1,
                _ => stats.base_mismatch.push(rel.clone()),
            }
        }
        if mode == MODE_DELETE {
            match fs::remove_file(&path) {
                Ok(()) => {}
                Err(e) if e.kind() == io::ErrorKind::NotFound => {}
                Err(e) => return Err(e.into()),
            }
            stats.deleted += 1;
            continue;
        }
        let data = r.take(csize);
        let (hash, size) = if comp == COMP_ZLIB {
            write_file(&path, ZlibDecoder::new(data))?
        } else {
            write_file(&path, data)?
        };
        ensure!(size == new_size, "{rel}: wrote {size} bytes, expected {new_size}");
        ensure!(dst == ZERO || hash == dst, "{rel}: SHA-1 mismatch");
        stats.written += 1;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::Compression;
    use flate2::write::ZlibEncoder;

    fn chunk(tag: &[u8; 4], body: &[u8]) -> Vec<u8> {
        let mut out = (body.len() as u32).to_be_bytes().to_vec();
        out.extend_from_slice(tag);
        out.extend_from_slice(body);
        out.extend_from_slice(&[0; 4]); // crc, not checked
        out
    }

    fn path_body(p: &str) -> Vec<u8> {
        let mut b = (p.len() as u32).to_be_bytes().to_vec();
        b.extend_from_slice(p.as_bytes());
        b
    }

    fn etry(p: &str, mode: u32, src: [u8; 20], data: Option<&[u8]>) -> Vec<u8> {
        let mut b = path_body(p);
        b.extend_from_slice(&1u32.to_be_bytes());
        b.extend_from_slice(&mode.to_le_bytes());
        b.extend_from_slice(&src);
        let (dst, comp, payload, new_size) = match data {
            Some(d) => {
                let mut z = ZlibEncoder::new(Vec::new(), Compression::default());
                z.write_all(d).unwrap();
                (Sha1::digest(d).into(), COMP_ZLIB, z.finish().unwrap(), d.len())
            }
            None => (ZERO, COMP_STORED, Vec::new(), 0),
        };
        b.extend_from_slice(&dst);
        b.extend_from_slice(&comp.to_le_bytes());
        b.extend_from_slice(&(payload.len() as u32).to_be_bytes());
        b.extend_from_slice(&0u32.to_be_bytes());
        b.extend_from_slice(&(new_size as u32).to_be_bytes());
        b.extend_from_slice(&payload);
        chunk(b"ETRY", &b)
    }

    fn patch(chunks: &[Vec<u8>]) -> Vec<u8> {
        let mut out = MAGIC.to_vec();
        out.extend(chunk(b"FHDR", &[0; 8]));
        for c in chunks {
            out.extend_from_slice(c);
        }
        out
    }

    fn tmpdir(name: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!("xivlegacy-zipatch-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[test]
    fn add_modify_delete() {
        let root = tmpdir("amd");
        let v1 = b"first version".as_slice();
        let v2 = b"second version, longer".as_slice();
        let mut s = Stats::default();
        apply(&patch(&[chunk(b"ADIR", &path_body("data/01")), etry("data\\01\\00.DAT", MODE_ADD, ZERO, Some(v1))])[..], &root, &mut s).unwrap();
        assert_eq!(fs::read(root.join("data/01/00.DAT")).unwrap(), v1);
        let src: [u8; 20] = Sha1::digest(v1).into();
        apply(&patch(&[etry("data/01/00.DAT", MODE_MODIFY, src, Some(v2))])[..], &root, &mut s).unwrap();
        assert_eq!(fs::read(root.join("data/01/00.DAT")).unwrap(), v2);
        assert_eq!(s.base_verified, 1);
        apply(&patch(&[etry("data/01/00.DAT", MODE_DELETE, ZERO, None), chunk(b"DELD", &path_body("data/01"))])[..], &root, &mut s).unwrap();
        assert!(!root.join("data/01").exists());
        assert_eq!((s.written, s.deleted, s.dirs_removed), (2, 1, 1));
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn rejects_bad_hash_and_escape() {
        let root = tmpdir("bad");
        let mut bad = etry("x.bin", MODE_ADD, ZERO, Some(b"payload"));
        // flip a byte of dst_sha1 (after size+tag, path, count, mode, src)
        let at = 8 + 4 + 5 + 4 + 4 + 20;
        bad[at] ^= 0xFF;
        assert!(apply(&patch(&[bad])[..], &root, &mut Stats::default()).is_err());
        let escape = etry("../outside.bin", MODE_ADD, ZERO, Some(b"payload"));
        assert!(apply(&patch(&[escape])[..], &root, &mut Stats::default()).is_err());
        assert!(!root.parent().unwrap().join("outside.bin").exists());
        fs::remove_dir_all(&root).unwrap();
    }
}
