// The recompiler's per-build metadata (8 MB of JSON) goes into the launcher gzipped.
use std::io::Write;

/// The Windows host, its SDL3 and wgpu-native, built by `tools/build.py host --windows`, go inside
/// XIVLegacy.exe when XIVLEGACY_WIN_HOST_DIR names their folder (release builds), so players still
/// get one file. Linux likewise with XIVLEGACY_LINUX_HOST_DIR (`tools/build.py host --linux`).
/// Without it the launcher looks for the host beside itself. The host comes first.
const WIN_HOST_FILES: &[&str] = &["xivlegacy-host.exe", "SDL3.dll", "wgpu_native.dll"];
const LINUX_HOST_FILES: &[&str] = &["xivlegacy-host", "libSDL3.so.0", "libwgpu_native.so"];

fn embed_host() {
    println!("cargo:rerun-if-env-changed=XIVLEGACY_WIN_HOST_DIR");
    println!("cargo:rerun-if-env-changed=XIVLEGACY_LINUX_HOST_DIR");
    println!("cargo::rustc-check-cfg=cfg(embedded_host)");
    let out = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).to_path_buf();
    let (var, files) = match std::env::var("CARGO_CFG_TARGET_OS").as_deref() {
        Ok("windows") => ("XIVLEGACY_WIN_HOST_DIR", WIN_HOST_FILES),
        Ok("linux") => ("XIVLEGACY_LINUX_HOST_DIR", LINUX_HOST_FILES),
        _ => ("", &[][..]),
    };
    let dir = std::env::var(var).ok().filter(|_| !files.is_empty());
    let mut list = String::from("pub const HOST_FILES: &[(&str, &[u8])] = &[\n");
    if let Some(dir) = dir {
        for &name in files {
            let src = std::path::Path::new(&dir).join(name);
            println!("cargo:rerun-if-changed={}", src.display());
            std::fs::copy(&src, out.join(name)).unwrap_or_else(|e| panic!("{}: {e}", src.display()));
            list += &format!("    (\"{name}\", include_bytes!(concat!(env!(\"OUT_DIR\"), \"/{name}\"))),\n");
        }
        println!("cargo:rustc-cfg=embedded_host");
    }
    list += "];\n";
    std::fs::write(out.join("host_files.rs"), list).unwrap();
}

fn main() {
    embed_host();
    let src = "../../meta/ffxivgame.2012.09.19.0001.meta.json";
    println!("cargo:rerun-if-changed={src}");
    let json = std::fs::read(src).expect("meta json");
    let out = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).join("meta.json.gz");
    let mut gz = flate2::write::GzEncoder::new(std::fs::File::create(out).unwrap(), flate2::Compression::best());
    gz.write_all(&json).unwrap();
    gz.finish().unwrap();
}
