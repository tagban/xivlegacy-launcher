// The recompiler's per-build metadata (8 MB of JSON) goes into the launcher gzipped.
use std::io::Write;

/// The Windows host, its SDL3 and wgpu-native, built by `tools/build.py host --windows`, go inside
/// XIVLegacy.exe when XIVLEGACY_WIN_HOST_DIR names their folder (release builds), so players still
/// get one file. Without it the launcher looks for the host beside itself.
const WIN_HOST_FILES: [&str; 3] = ["xivlegacy-host.exe", "SDL3.dll", "wgpu_native.dll"];

fn embed_windows_host() {
    println!("cargo:rerun-if-env-changed=XIVLEGACY_WIN_HOST_DIR");
    println!("cargo::rustc-check-cfg=cfg(embedded_host)");
    let out = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).to_path_buf();
    let windows = std::env::var("CARGO_CFG_TARGET_OS").as_deref() == Ok("windows");
    let dir = std::env::var("XIVLEGACY_WIN_HOST_DIR").ok().filter(|_| windows);
    let mut list = String::from("pub const HOST_FILES: &[(&str, &[u8])] = &[\n");
    if let Some(dir) = dir {
        for name in WIN_HOST_FILES {
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
    embed_windows_host();
    let src = "../../meta/ffxivgame.2012.09.19.0001.meta.json";
    println!("cargo:rerun-if-changed={src}");
    let json = std::fs::read(src).expect("meta json");
    let out = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).join("meta.json.gz");
    let mut gz = flate2::write::GzEncoder::new(std::fs::File::create(out).unwrap(), flate2::Compression::best());
    gz.write_all(&json).unwrap();
    gz.finish().unwrap();
}
