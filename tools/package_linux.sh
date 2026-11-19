#!/bin/bash
# package_linux.sh [destination folder]: the Linux x86_64 release, cross-built from a Mac (or any machine
# with zig and cargo-zigbuild): the host (tools/build.py host --linux) with SDL3 and wgpu-native goes
# inside the launcher, which then is one program, as on Windows. Writes to the destination (default
# dist/linux):
#   XIVLegacy-linux-x86_64          the program alone: the asset "Update now" downloads (update.rs)
#   XIVLegacy-linux-x86_64.tar.gz   the same program as XIVLegacy/XIVLegacy, for players to unpack
#   SHA256SUMS-linux.txt            both checksums, to merge into the release's SHA256SUMS.txt
# docs/LINUX.md says how to get SDL3 and wgpu-native for the host build.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:-$root/dist/linux}
target=x86_64-unknown-linux-gnu
strip=${LLVM_STRIP:-$(command -v llvm-strip || echo /opt/homebrew/opt/llvm/bin/llvm-strip)}

(cd "$root" && python3 tools/build.py host --linux)
host=$(mktemp -d -t xivlegacy-linux-host.XXXXXX)
trap 'rm -rf "$host"' EXIT
# symbols for crash backtraces stay; debug information goes
"$strip" --strip-debug -o "$host/xivlegacy-host" "$root/build/linux/xivlegacy-host"
"$strip" --strip-unneeded -o "$host/libSDL3.so.0" "$root/build/linux/libSDL3.so.0"
cp "$root/build/linux/libwgpu_native.so" "$host/libwgpu_native.so"

(cd "$root" && XIVLEGACY_LINUX_HOST_DIR="$host" cargo zigbuild --release -p xivlegacy-launcher --target "$target.2.28")
out=${CARGO_TARGET_DIR:-$root/target}/$target/release/XIVLegacy

mkdir -p "$dest"
pack=$(mktemp -d -t xivlegacy-linux-pack.XXXXXX)
trap 'rm -rf "$host" "$pack"' EXIT
install -m 755 "$out" "$dest/XIVLegacy-linux-x86_64"
mkdir -p "$pack/XIVLegacy"
install -m 755 "$out" "$pack/XIVLegacy/XIVLegacy"
# no extended attributes or Mac owner in the archive
COPYFILE_DISABLE=1 tar -czf "$dest/XIVLegacy-linux-x86_64.tar.gz" --no-xattrs --no-mac-metadata --uid 0 --gid 0 --uname root --gname root -C "$pack" XIVLegacy 2>/dev/null \
  || COPYFILE_DISABLE=1 tar -czf "$dest/XIVLegacy-linux-x86_64.tar.gz" -C "$pack" XIVLegacy
(cd "$dest" && shasum -a 256 XIVLegacy-linux-x86_64 XIVLegacy-linux-x86_64.tar.gz > SHA256SUMS-linux.txt)
ls -l "$dest"
