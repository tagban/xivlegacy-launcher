// The recompiler's per-build metadata (8 MB of JSON) goes into the launcher gzipped.
use std::io::Write;

fn main() {
    let src = "../../meta/ffxivgame.2012.09.19.0001.meta.json";
    println!("cargo:rerun-if-changed={src}");
    let json = std::fs::read(src).expect("meta json");
    let out = std::path::Path::new(&std::env::var("OUT_DIR").unwrap()).join("meta.json.gz");
    let mut gz = flate2::write::GzEncoder::new(std::fs::File::create(out).unwrap(), flate2::Compression::best());
    gz.write_all(&json).unwrap();
    gz.finish().unwrap();
}
