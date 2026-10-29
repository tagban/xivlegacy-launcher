//! Making the game for this computer (runtime/xiv_game.h): the first time a
//! player joins, the launcher translates their own ffxivgame.exe to C with the
//! recompiler and compiles it into the game module the host loads. Nothing of
//! the game is ever downloaded; the module never leaves this machine.
//!
//! The compiler is clang when the machine has one (on a Mac, Xcode's command
//! line tools), else zig, downloaded once from ziglang.org and checked against
//! its published SHA-256. Modules are kept per exe and per translation inputs,
//! so a new launcher remakes the game only when the translation changed.

use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Mutex;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use sha2::{Digest, Sha256};

/// What the translation needs besides the player's exe: the per-build metadata
/// (gzip, from build.rs), the instruction patches, and the runtime headers the
/// generated C includes.
const META_GZ: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/meta.json.gz"));
const PATCHES: &str = include_str!("../../../meta/ffxivgame.2012.09.19.0001.patches.json");
const HEADERS: [(&str, &str); 6] = [
    ("guest.h", include_str!("../../../runtime/guest.h")),
    ("runtime.h", include_str!("../../../runtime/runtime.h")),
    ("rt_fastcall.h", include_str!("../../../runtime/rt_fastcall.h")),
    ("sse.h", include_str!("../../../runtime/sse.h")),
    ("xiv_game.h", include_str!("../../../runtime/xiv_game.h")),
    ("xiv_module.c", include_str!("../../../runtime/xiv_module.c")),
];
const HOOKS: [(&str, u64); 1] = [("streamlog", 0x4e7290)];

#[cfg(target_os = "macos")]
const EXT: &str = "dylib";
#[cfg(windows)]
const EXT: &str = "dll";
#[cfg(not(any(windows, target_os = "macos")))]
const EXT: &str = "so";

/// Flags the module must share with the host it is loaded into (tools/build.py).
fn cflags() -> Vec<&'static str> {
    let mut f = vec!["-std=c11", "-O2", "-fno-strict-aliasing", "-DRT_GUEST_WINDOW", "-fPIC", "-fvisibility=hidden", "-w"];
    if cfg!(target_os = "macos") {
        f.push("-DRT_GUEST_FIXED_BASE=0x10000000000ull"); // tools/build.py says why
    }
    f
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Step {
    Compiler,
    Translate,
    Compile,
    Link,
}

impl Step {
    pub fn label(self) -> &'static str {
        match self {
            Step::Compiler => "Getting a compiler",
            Step::Translate => "Translating the game for this computer",
            Step::Compile => "Compiling the game",
            Step::Link => "Finishing",
        }
    }
}

/// A process with no console window: on Windows a windowed program that starts a console program
/// otherwise gets a new console for each one (one per compiler running at once).
fn quiet(program: impl AsRef<std::ffi::OsStr>) -> Command {
    #[allow(unused_mut)]
    let mut c = Command::new(program);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        c.creation_flags(0x0800_0000); // CREATE_NO_WINDOW
    }
    c
}

fn games_dir() -> PathBuf {
    crate::config::dir().join("game")
}

/// The module for this game folder's ffxivgame.exe and this launcher's
/// translation inputs.
pub fn module_path(game_dir: &Path) -> Result<PathBuf> {
    let exe = std::fs::read(game_dir.join("ffxivgame.exe")).context("reading ffxivgame.exe")?;
    let mut h = Sha256::new();
    h.update(&exe);
    h.update(META_GZ);
    h.update(PATCHES.as_bytes());
    for (name, text) in HEADERS {
        h.update(name.as_bytes());
        h.update(text.as_bytes());
    }
    h.update(env!("CARGO_PKG_VERSION").as_bytes()); // the recompiler ships with the launcher
    for flag in cflags() {
        h.update(flag.as_bytes()); // the guest window's base among them
    }
    let key = &hex::encode(h.finalize())[..16];
    Ok(games_dir().join(format!("xivlegacy-game-{key}.{EXT}")))
}

/// The module, made now if this computer doesn't have it yet. `progress` gets
/// (step, done, total).
pub fn ensure(game_dir: &Path, progress: &(dyn Fn(Step, usize, usize) + Sync)) -> Result<PathBuf> {
    let out = module_path(game_dir)?;
    if out.is_file() {
        return Ok(out);
    }
    std::fs::create_dir_all(games_dir())?;
    let work = games_dir().join("work");
    let _ = std::fs::remove_dir_all(&work);
    let (inc, csrc, obj) = (work.join("include"), work.join("gen"), work.join("obj"));
    for d in [&inc, &csrc, &obj] {
        std::fs::create_dir_all(d)?;
    }
    for (name, text) in HEADERS {
        std::fs::write(inc.join(name), text)?;
    }

    progress(Step::Compiler, 0, 1);
    let cc = compiler(progress)?;
    crate::config::log(&format!("making the game with {}", cc.join(" ")));

    progress(Step::Translate, 0, 1);
    let meta = work.join("meta.json");
    let mut json = Vec::new();
    std::io::Read::read_to_end(&mut flate2::read::GzDecoder::new(META_GZ), &mut json)?;
    std::fs::write(&meta, json)?;
    let patches = work.join("patches.json");
    std::fs::write(&patches, PATCHES)?;
    let exe = game_dir.join("ffxivgame.exe");
    let mut opts = xivlegacy_recomp::recomp::Options::new(meta, exe.clone(), exe, csrc.clone());
    opts.all = true;
    opts.patches = Some(patches);
    opts.hooks = HOOKS.iter().map(|(n, a)| (n.to_string(), *a)).collect();
    opts.threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    xivlegacy_recomp::recomp::run(&opts, &mut |e| {
        if let xivlegacy_recomp::Event::Progress { done, total, .. } = e {
            progress(Step::Translate, done, total);
        }
    })
    .map_err(|e| anyhow::anyhow!("translating ffxivgame.exe: {e}"))?;

    let mut sources: Vec<PathBuf> = std::fs::read_dir(&csrc)?
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "c"))
        .collect();
    sources.sort();
    sources.push(inc.join("xiv_module.c"));
    let total = sources.len();
    let next = AtomicUsize::new(0);
    let done = AtomicUsize::new(0);
    let failed: Mutex<Option<String>> = Mutex::new(None);
    progress(Step::Compile, 0, total);
    std::thread::scope(|s| {
        for _ in 0..std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4) {
            s.spawn(|| {
                loop {
                    let i = next.fetch_add(1, Ordering::Relaxed);
                    if i >= total || failed.lock().unwrap().is_some() {
                        return;
                    }
                    let src = &sources[i];
                    let o = obj.join(format!("{}.o", src.file_stem().unwrap().to_string_lossy()));
                    let r = quiet(&cc[0])
                        .args(&cc[1..])
                        .arg("-c")
                        .args(cflags())
                        .arg("-I")
                        .arg(&inc)
                        .arg("-I")
                        .arg(&csrc)
                        .arg(src)
                        .arg("-o")
                        .arg(&o)
                        .output();
                    match r {
                        Ok(out) if out.status.success() => {
                            let n = done.fetch_add(1, Ordering::Relaxed) + 1;
                            progress(Step::Compile, n, total);
                        }
                        Ok(out) => {
                            let msg = String::from_utf8_lossy(&out.stderr).lines().take(5).collect::<Vec<_>>().join("\n");
                            *failed.lock().unwrap() = Some(format!("{}: {msg}", src.display()));
                        }
                        Err(e) => *failed.lock().unwrap() = Some(format!("running the compiler: {e}")),
                    }
                }
            });
        }
    });
    if let Some(e) = failed.into_inner().unwrap() {
        bail!("compiling the game failed: {e}");
    }

    progress(Step::Link, 0, 1);
    let objs: Vec<String> = std::fs::read_dir(&obj)?
        .flatten()
        .map(|e| e.path().to_string_lossy().replace('\\', "/"))
        .filter(|p| p.ends_with(".o"))
        .collect();
    let rsp = work.join("objects.rsp");
    std::fs::write(&rsp, objs.iter().map(|o| format!("\"{o}\"")).collect::<Vec<_>>().join("\n"))?;
    let tmp = out.with_extension(format!("{EXT}.part"));
    let mut link = quiet(&cc[0]);
    link.args(&cc[1..]).arg(format!("@{}", rsp.display())).arg("-o").arg(&tmp);
    if cfg!(target_os = "macos") {
        // C library symbols resolve from the host process when the module loads (zig's macOS
        // stubs lack a few, such as __fpclassifyd).
        link.arg("-dynamiclib")
            .arg(format!("-Wl,-install_name,@rpath/{}", out.file_name().unwrap().to_string_lossy()))
            .arg("-Wl,-undefined,dynamic_lookup");
    } else {
        link.arg("-shared");
        if !cfg!(windows) {
            link.arg("-lm");
        }
    }
    let r = link.output().context("running the linker")?;
    if !r.status.success() {
        bail!("linking the game failed: {}", String::from_utf8_lossy(&r.stderr).lines().take(5).collect::<Vec<_>>().join("\n"));
    }
    std::fs::rename(&tmp, &out)?;
    let _ = std::fs::remove_dir_all(&work);
    crate::config::log(&format!("made {}", out.display()));
    progress(Step::Link, 1, 1);
    Ok(out)
}

/// clang if this machine has one, else zig (downloaded once).
fn compiler(progress: &(dyn Fn(Step, usize, usize) + Sync)) -> Result<Vec<String>> {
    if std::env::var_os("XIVLEGACY_USE_ZIG").is_some() {
        let zig = zig(progress)?;
        return Ok(vec![zig.to_string_lossy().into_owned(), "cc".into()]);
    }
    let works = |cmd: &[&str]| quiet(cmd[0]).args(&cmd[1..]).arg("--version").output().is_ok_and(|o| o.status.success());
    if cfg!(target_os = "macos") {
        // Only ask xcrun when the command line tools are installed; otherwise macOS offers to install them.
        if quiet("xcode-select").arg("-p").output().is_ok_and(|o| o.status.success()) && works(&["xcrun", "clang"]) {
            return Ok(vec!["xcrun".into(), "clang".into()]);
        }
    } else if !cfg!(windows) && works(&["clang"]) {
        return Ok(vec!["clang".into()]);
    }
    let zig = zig(progress)?;
    Ok(vec![zig.to_string_lossy().into_owned(), "cc".into()])
}

struct ZigBuild {
    url: &'static str,
    sha256: &'static str,
    size: u64,
}

/// zig 0.15.1 from ziglang.org/download/index.json.
fn zig_build() -> Option<ZigBuild> {
    let (arch, os) = (std::env::consts::ARCH, std::env::consts::OS);
    Some(match (arch, os) {
        ("aarch64", "macos") => ZigBuild { url: "https://ziglang.org/download/0.15.1/zig-aarch64-macos-0.15.1.tar.xz", sha256: "c4bd624d901c1268f2deb9d8eb2d86a2f8b97bafa3f118025344242da2c54d7b", size: 50644996 },
        ("x86_64", "macos") => ZigBuild { url: "https://ziglang.org/download/0.15.1/zig-x86_64-macos-0.15.1.tar.xz", sha256: "9919392e0287cccc106dfbcbb46c7c1c3fa05d919567bb58d7eb16bca4116184", size: 55791880 },
        ("x86_64", "windows") => ZigBuild { url: "https://ziglang.org/download/0.15.1/zig-x86_64-windows-0.15.1.zip", sha256: "91e69e887ca8c943ce9a515df3af013d95a66a190a3df3f89221277ebad29e34", size: 92612958 },
        ("aarch64", "windows") => ZigBuild { url: "https://ziglang.org/download/0.15.1/zig-aarch64-windows-0.15.1.zip", sha256: "1f1bf16228b0ffcc882b713dc5e11a6db4219cb30997e13c72e8e723c2104ec6", size: 88458549 },
        ("x86_64", "linux") => ZigBuild { url: "https://ziglang.org/download/0.15.1/zig-x86_64-linux-0.15.1.tar.xz", sha256: "c61c5da6edeea14ca51ecd5e4520c6f4189ef5250383db33d01848293bfafe05", size: 53734456 },
        ("aarch64", "linux") => ZigBuild { url: "https://ziglang.org/download/0.15.1/zig-aarch64-linux-0.15.1.tar.xz", sha256: "bb4a8d2ad735e7fba764c497ddf4243cb129fece4148da3222a7046d3f1f19fe", size: 49493872 },
        _ => return None,
    })
}

fn zig(progress: &(dyn Fn(Step, usize, usize) + Sync)) -> Result<PathBuf> {
    let b = zig_build().context("no compiler is available for this computer")?;
    let dir = crate::config::dir().join("zig-0.15.1");
    let exe = dir.join(if cfg!(windows) { "zig.exe" } else { "zig" });
    if exe.is_file() {
        return Ok(exe);
    }
    let archive = crate::config::dir().join(b.url.rsplit('/').next().unwrap());
    let agent = ureq::AgentBuilder::new().timeout_read(Duration::from_secs(60)).build();
    let mut body = agent.get(b.url).call().context("downloading zig")?.into_reader();
    let mut file = std::fs::File::create(&archive)?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 20];
    let mut got = 0u64;
    loop {
        let n = std::io::Read::read(&mut body, &mut buf)?;
        if n == 0 {
            break;
        }
        std::io::Write::write_all(&mut file, &buf[..n])?;
        h.update(&buf[..n]);
        got += n as u64;
        progress(Step::Compiler, got as usize, b.size as usize);
    }
    drop(file);
    if hex::encode(h.finalize()) != b.sha256 {
        let _ = std::fs::remove_file(&archive);
        bail!("the zig download didn't match its published checksum; try again");
    }
    // Every archive holds one top folder, zig-<arch>-<os>-0.15.1/; unpack, then name it zig-0.15.1.
    let unpack = crate::config::dir().join("zig-unpack");
    let _ = std::fs::remove_dir_all(&unpack);
    std::fs::create_dir_all(&unpack)?;
    if b.url.ends_with(".zip") {
        let mut zip = zip::ZipArchive::new(std::fs::File::open(&archive)?)?;
        zip.extract(&unpack)?;
    } else {
        let st = quiet("tar").arg("-xJf").arg(&archive).arg("-C").arg(&unpack).status()?;
        if !st.success() {
            bail!("unpacking zig failed");
        }
    }
    let top = std::fs::read_dir(&unpack)?.flatten().map(|e| e.path()).find(|p| p.is_dir()).context("empty zig archive")?;
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::rename(&top, &dir)?;
    let _ = std::fs::remove_dir_all(&unpack);
    let _ = std::fs::remove_file(&archive);
    Ok(exe)
}
