//! Resumable, SHA-1-verified downloads.

use std::fs::{self, File, OpenOptions};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use sha1::{Digest, Sha1};

use crate::progress::Counter;

/// A file to fetch, with the size and SHA-1 its host publishes.
#[derive(Debug, Clone)]
pub struct Source {
    pub name: &'static str,
    pub url: &'static str,
    /// Built-in fallback copies (see sources.rs for the links actually used).
    pub mirrors: &'static [&'static str],
    pub size: u64,
    pub sha1: &'static str,
}

const RETRIES: u32 = 20;
/// Failures in a row before moving on to the next copy.
const SWITCH_AFTER: u32 = 3;

fn verified_marker(path: &Path) -> PathBuf {
    path.with_extension(format!("{}.verified", path.extension().and_then(|e| e.to_str()).unwrap_or("")))
}

/// Feeds `path`'s bytes into `h` (used to resume hashing a partial download).
fn hash_existing(path: &Path, h: &mut Sha1, counter: &Counter) -> Result<u64> {
    let mut f = File::open(path)?;
    let mut buf = vec![0u8; 1 << 20];
    let mut n_total = 0u64;
    loop {
        let n = f.read(&mut buf)?;
        if n == 0 {
            return Ok(n_total);
        }
        h.update(&buf[..n]);
        counter.add(n as u64);
        n_total += n as u64;
    }
}

/// Downloads `src` into `dir` from the first of `urls` that works (resuming a
/// partial download, moving on to the next link when one keeps failing), checks
/// its size and SHA-1, and returns the path. A file verified earlier is reused.
pub fn fetch(src: &Source, urls: &[String], dir: &Path, counter: &Counter) -> Result<PathBuf> {
    fs::create_dir_all(dir)?;
    let path = dir.join(src.name);
    let marker = verified_marker(&path);
    if path.metadata().map(|m| m.len()) .ok() == Some(src.size) && fs::read_to_string(&marker).ok().as_deref() == Some(src.sha1) {
        counter.add(src.size);
        return Ok(path);
    }

    let part = path.with_extension(format!("{}.part", path.extension().and_then(|e| e.to_str()).unwrap_or("")));
    let mut hasher = Sha1::new();
    let mut have = if part.exists() { hash_existing(&part, &mut hasher, counter)? } else { 0 };
    if have > src.size {
        fs::remove_file(&part)?;
        hasher = Sha1::new();
        have = 0;
    }

    let agent = ureq::AgentBuilder::new()
        .timeout_connect(Duration::from_secs(30))
        .timeout_read(Duration::from_secs(60))
        .build();
    if urls.is_empty() {
        bail!("no download link for {}", src.name);
    }
    let (mut which, mut attempt, mut in_a_row) = (0usize, 0u32, 0u32);
    while have < src.size {
        match fetch_range(&agent, &urls[which], &part, have, &mut hasher, counter) {
            Ok(n) => {
                have += n;
                attempt = 0;
                in_a_row = 0;
            }
            Err(e) => {
                attempt += 1;
                in_a_row += 1;
                if attempt > RETRIES {
                    return Err(e.context(format!("downloading {} failed after {RETRIES} retries", src.name)));
                }
                if in_a_row >= SWITCH_AFTER && urls.len() > 1 {
                    which = (which + 1) % urls.len();
                    in_a_row = 0;
                }
                std::thread::sleep(Duration::from_secs(u64::from(in_a_row.min(10)) * 3));
            }
        }
    }

    let got = hex::encode(hasher.finalize());
    if got != src.sha1 {
        fs::remove_file(&part)?;
        bail!("{} is corrupt (SHA-1 {got}, expected {}); deleted, try again", src.name, src.sha1);
    }
    fs::rename(&part, &path)?;
    fs::write(&marker, src.sha1)?;
    Ok(path)
}

/// Requests bytes from `from` on and appends them to `part`. Returns how many
/// bytes were added (may be fewer than asked if the connection drops).
fn fetch_range(agent: &ureq::Agent, url: &str, part: &Path, from: u64, hasher: &mut Sha1, counter: &Counter) -> Result<u64> {
    let mut req = agent.get(url);
    if from > 0 {
        req = req.set("Range", &format!("bytes={from}-"));
    }
    let resp = req.call().with_context(|| format!("requesting {url}"))?;
    if from > 0 && resp.status() != 206 {
        bail!("server ignored the resume request (status {})", resp.status());
    }
    // A web page instead of the file (a host's warning or sign-in page).
    if resp.content_type().starts_with("text/html") {
        bail!("{url} answered with a web page, not the file");
    }
    let mut out = OpenOptions::new().create(true).append(true).open(part)?;
    let mut body = resp.into_reader();
    let mut buf = vec![0u8; 1 << 20];
    let mut added = 0u64;
    loop {
        let n = match body.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => n,
            Err(e) if e.kind() == io::ErrorKind::Interrupted => continue,
            Err(e) => {
                out.flush()?;
                if added > 0 {
                    return Ok(added); // keep what arrived; the caller resumes
                }
                return Err(e.into());
            }
        };
        out.write_all(&buf[..n])?;
        hasher.update(&buf[..n]);
        counter.add(n as u64);
        added += n as u64;
    }
    out.flush()?;
    Ok(added)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    /// Serves `body` with `content_type` to every request on a local port.
    fn serve(body: &'static [u8], content_type: &'static str) -> String {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let addr = l.local_addr().unwrap();
        std::thread::spawn(move || {
            for mut s in l.incoming().flatten() {
                let mut req = [0u8; 2048];
                let _ = s.read(&mut req);
                let head = format!("HTTP/1.1 200 OK\r\nContent-Type: {content_type}\r\nContent-Length: {}\r\nConnection: close\r\n\r\n", body.len());
                let _ = s.write_all(head.as_bytes());
                let _ = s.write_all(body);
            }
        });
        format!("http://{addr}/file")
    }

    #[test]
    fn falls_back_to_a_mirror() {
        const BODY: &[u8] = b"the patches, honest";
        let page = serve(b"<html>quota exceeded</html>", "text/html");
        let good = serve(BODY, "application/octet-stream");
        let src = Source {
            name: "test.bin",
            url: "",
            mirrors: &[],
            size: BODY.len() as u64,
            sha1: Box::leak(hex::encode(Sha1::digest(BODY)).into_boxed_str()),
        };
        let urls = ["http://127.0.0.1:1/unreachable".to_string(), page, good];
        let dir = std::env::temp_dir().join(format!("xivlegacy-dl-test-{}", std::process::id()));
        let path = fetch(&src, &urls, &dir, &Counter::default()).unwrap();
        assert_eq!(fs::read(&path).unwrap(), BODY);
        let _ = fs::remove_dir_all(&dir);
    }
}
