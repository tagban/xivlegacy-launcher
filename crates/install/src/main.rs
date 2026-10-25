use xivlegacy_install::{directx, disc, progress, setup, sources, udf, verify};

use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
use std::time::Instant;

use anyhow::{Result, bail};
use sha1::{Digest, Sha1};

use progress::{Phase, Report};
use setup::{DiscSource, PatchSource, Plan};

const USAGE: &str = "\
usage:
  xivlegacy-install setup <new install folder> [--iso <file.iso|file.7z>] [--patches <zip|folder|.patch>...] [--cache <folder>]
      Anything not given is downloaded from archive.org.
  xivlegacy-install build <file.iso|file.7z> <zip|folder> <new install folder>
      Same as setup with both given.
  xivlegacy-install check <existing game folder> [--full]
      Is this folder a complete 1.23b install? (--full hashes every file.)
  xivlegacy-install sources
      Which download links setup will use, and where they came from (sources.json
      beside the program, else the repository's copy, else built in).
debugging:
  xivlegacy-install iso-ls <iso> [dir] | iso-sha1 <iso> <path> | disc-extract <iso> <out> [skip] | dx-extract <iso> <out> | dx-installer <iso> <out>";

/// One updating terminal line: step, percent, bytes, speed, time left.
struct Terminal {
    state: Mutex<Option<(Phase, Instant, u64)>>,
}

fn gb(b: u64) -> String {
    format!("{:.2} GB", b as f64 / 1e9)
}

impl Report for Terminal {
    fn report(&self, phase: Phase, done: u64, total: u64, detail: &str) {
        let mut st = self.state.lock().unwrap();
        let (start, base) = match *st {
            Some((p, t, b)) if p == phase => (t, b),
            _ => {
                if st.is_some() {
                    eprintln!();
                }
                *st = Some((phase, Instant::now(), done));
                (Instant::now(), done)
            }
        };
        let secs = start.elapsed().as_secs_f64();
        let rate = if secs > 1.0 { done.saturating_sub(base) as f64 / secs } else { 0.0 };
        let eta = if rate > 0.0 && total > done {
            let s = ((total - done) as f64 / rate) as u64;
            format!("ETA {}m{:02}s", s / 60, s % 60)
        } else {
            String::new()
        };
        let pct = if total > 0 { done as f64 * 100.0 / total as f64 } else { 0.0 };
        let (n, of) = phase.step();
        let line = format!(
            "[{n}/{of}] {:<26} {pct:5.1}%  {} / {}  {:>6.1} MB/s  {eta}  {detail}",
            phase.label(),
            gb(done),
            gb(total),
            rate / 1e6
        );
        eprint!("\r{line:<130.130}");
        let _ = std::io::stderr().flush();
    }
}

fn run_setup(out: &Path, disc: DiscSource, patches: PatchSource, cache: Option<PathBuf>) -> Result<()> {
    let plan = Plan { cache: cache.unwrap_or_else(|| setup::default_cache(out)), out: out.to_path_buf(), disc, patches };
    let start = Instant::now();
    setup::run(&plan, Arc::new(Terminal { state: Mutex::new(None) }))?;
    eprintln!("\nFINAL FANTASY XIV 1.23b is ready in {} ({:.0?}).", out.display(), start.elapsed());
    eprintln!("Downloads and any unpacked disc image are in {} (safe to delete).", plan.cache.display());
    Ok(())
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(String::as_str) {
        Some("setup") => {
            let Some(out) = args.get(2) else { bail!(USAGE) };
            let (mut iso, mut patches, mut cache) = (None, Vec::new(), None);
            let mut flag: Option<&str> = None;
            for v in &args[3..] {
                match v.as_str() {
                    "--iso" | "--patches" | "--cache" => flag = Some(v.as_str()),
                    _ => match flag {
                        Some("--iso") => iso = Some(PathBuf::from(v)),
                        Some("--patches") => patches.push(PathBuf::from(v)),
                        Some("--cache") => cache = Some(PathBuf::from(v)),
                        _ => bail!("unexpected {v}\n{USAGE}"),
                    },
                }
            }
            let disc = iso.map_or(DiscSource::Download, DiscSource::File);
            let patches = if patches.is_empty() { PatchSource::Download } else { PatchSource::Files(patches) };
            run_setup(Path::new(out), disc, patches, cache)?;
        }
        Some("build") if args.len() == 5 => {
            run_setup(
                Path::new(&args[4]),
                DiscSource::File(PathBuf::from(&args[2])),
                PatchSource::Files(vec![PathBuf::from(&args[3])]),
                None,
            )?;
        }
        Some("check") => {
            let Some(folder) = args.get(2) else { bail!(USAGE) };
            let full = args.iter().any(|a| a == "--full");
            let counter = progress::Counter::default();
            let total = verify::total_size()?;
            let term: Arc<dyn Report> = Arc::new(Terminal { state: Mutex::new(None) });
            let done = Arc::new(std::sync::atomic::AtomicBool::new(false));
            let ticker = {
                let (c, t, d) = (counter.clone(), term.clone(), done.clone());
                std::thread::spawn(move || {
                    while !d.load(std::sync::atomic::Ordering::Relaxed) {
                        t.report(Phase::Verify, c.get().min(total), total, if full { "every file" } else { "sizes + programs" });
                        std::thread::sleep(std::time::Duration::from_millis(250));
                    }
                })
            };
            let result = verify::check(Path::new(folder), full, &counter);
            done.store(true, std::sync::atomic::Ordering::Relaxed);
            let _ = ticker.join();
            term.report(Phase::Verify, total, total, "");
            eprintln!();
            let c = result?;
            let version = c.game_version.as_deref().unwrap_or("no game.ver");
            eprintln!("game.ver: {version}");
            for (label, list) in [("missing", &c.missing), ("wrong size or contents", &c.wrong)] {
                if !list.is_empty() {
                    eprintln!("{} {label}:", list.len());
                    for p in list.iter().take(15) {
                        eprintln!("  {p}");
                    }
                    if list.len() > 15 {
                        eprintln!("  …");
                    }
                }
            }
            if c.ok() {
                eprintln!(
                    "OK: complete FFXIV 1.23b ({} files{}). It can be used as is.",
                    c.expected_files,
                    if c.full { ", every file hashed" } else { "; run with --full to hash every file" }
                );
            } else {
                bail!("{} is not a complete 1.23b install", folder);
            }
        }
        Some("sources") => {
            let s = sources::Sources::load();
            println!("links from: {}", s.origin);
            for (what, urls) in [("disc", &s.disc), ("patches", &s.patches)] {
                for (i, u) in urls.iter().enumerate() {
                    println!("  {what} {}: {u}", if i == 0 { "main" } else { "fallback" });
                }
            }
        }
        Some("iso-ls") => {
            let mut udf = udf::Udf::open(Path::new(&args[2]))?;
            let dir = udf.find(args.get(3).map(String::as_str).unwrap_or("/"))?;
            for e in udf.list(&dir)? {
                println!("{}{:>14} {}", if e.is_dir { "d" } else { "-" }, e.size, e.name);
            }
        }
        Some("iso-sha1") => {
            let mut udf = udf::Udf::open(Path::new(&args[2]))?;
            let e = udf.find(&args[3])?;
            let mut f = udf.open_file(&e);
            let mut h = Sha1::new();
            let mut buf = vec![0u8; 1 << 20];
            loop {
                let n = f.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                h.update(&buf[..n]);
            }
            println!("{:x}  {}", h.finalize(), args[3]);
        }
        Some("dx-installer") => {
            directx::copy_installer(Path::new(&args[2]), Path::new(&args[3]))?;
            eprintln!("DirectX installer copied to {}", args[3]);
        }
        Some("dx-extract") => {
            directx::install(Path::new(&args[2]), Path::new(&args[3]))?;
            eprintln!("{}", directx::file_names().collect::<Vec<_>>().join(", "));
        }
        Some("disc-extract") => {
            let d = disc::Disc::open(Path::new(&args[2]))?;
            let skip = args.get(4).map(|s| s.parse()).transpose()?.unwrap_or(0);
            let n = d.install(Path::new(&args[3]), skip, &progress::Counter::default())?;
            eprintln!("{n} files");
        }
        _ => bail!(USAGE),
    }
    Ok(())
}
