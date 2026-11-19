# The Linux client

The same client as on Windows and macOS: the launcher (crates/launcher) and the host
(runtime/portable on plat_posix.c, started by tests/boot.c) draw with wgpu on Vulkan; the game module
is made on the player's PC from their own ffxivgame.exe. x86_64 only for now.

## What a player's PC needs

glibc 2.28 or newer, a Vulkan driver, X11 or Wayland. The release is one program,
`XIVLegacy-linux-x86_64`: the launcher with the host, SDL3 and wgpu-native inside it, written to
`~/.local/share/XIVLegacy/bin-<version>/` on the first Join (the host finds its libraries beside
itself, `$ORIGIN`). SDL3 and the launcher window load X11, Wayland, libGL/EGL, xkbcommon, PipeWire,
PulseAudio, ALSA and libudev at run time, so none is linked. The game module is compiled with zig
(downloaded once) aimed at glibc 2.28, which brings its own C library headers: no development
packages are needed. Passwords go to the Secret Service (pure Rust, over D-Bus) or, without one, to
`passwords.json` (mode 0600).

Folders: `$XDG_DATA_HOME/XIVLegacy` (`~/.local/share/XIVLegacy`) holds `launcher.json`,
`launcher.log`, `logs/game.log`, the game (`FINAL FANTASY XIV 1.23b/`), `game/` (modules), `zig-0.15.1/`
and the applications menu icon; the menu entry is `~/.local/share/applications/xivlegacy.desktop`.

## Building it (cross, from a Mac)

`tools/package_linux.sh` does all of it once the two libraries are in place:

- **wgpu-native**: `wgpu-linux-x86_64-release.zip` of the release the other platforms use
  (v29.0.1.1), unpacked at `XIVLEGACY_WGPU_LINUX` (default
  `/Volumes/AppStorage/ffxiv-scratch/deps/wgpu-linux`). The release's .so has no SONAME, so the host
  would name the build machine's path for it; give it one once:
  `patchelf --set-soname libwgpu_native.so lib/libwgpu_native.so`.
- **SDL3**: there is no official Linux binary, so build it with the same zig target as the host, on a
  Debian or Ubuntu machine (or VM) that has the X11, Wayland, audio and udev development headers for
  amd64 (any architecture works: zig cross-compiles):

  ```bash
  sudo dpkg --add-architecture amd64 && sudo apt update
  sudo apt install cmake ninja-build pkg-config wayland-protocols libwayland-bin \
    libx11-dev:amd64 libxext-dev:amd64 libxcursor-dev:amd64 libxrandr-dev:amd64 libxi-dev:amd64 \
    libxfixes-dev:amd64 libxss-dev:amd64 libxtst-dev:amd64 libxkbcommon-dev:amd64 libwayland-dev:amd64 \
    libdecor-0-dev:amd64 libdbus-1-dev:amd64 libasound2-dev:amd64 libpulse-dev:amd64 \
    libpipewire-0.3-dev:amd64 libudev-dev:amd64 libdrm-dev:amd64 libgbm-dev:amd64 libegl-dev:amd64
  printf '#!/bin/sh\nexec zig cc -target x86_64-linux-gnu.2.28 "$@"\n' > zcc && chmod +x zcc
  cat > tc.cmake <<EOF
  set(CMAKE_SYSTEM_NAME Linux)
  set(CMAKE_SYSTEM_PROCESSOR x86_64)
  set(CMAKE_C_COMPILER $PWD/zcc)
  set(CMAKE_C_FLAGS_INIT "-idirafter /usr/include")
  set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)
  EOF
  PKG_CONFIG_LIBDIR=/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
    cmake -S SDL3-3.4.16 -B sdl-build -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/tc.cmake \
    -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DCMAKE_INSTALL_PREFIX=$PWD/sdl-x64
  ninja -C sdl-build install
  ```

  (`-idirafter` lets zig's own glibc headers win over the system's, which keeps the 2.28 floor; the
  system's are used only for X11, Wayland and the rest.) Copy `sdl-x64/include` and
  `sdl-x64/lib/libSDL3.so*` to `XIVLEGACY_SDL3_LINUX` (default
  `/Volumes/AppStorage/ffxiv-scratch/deps/sdl3-linux-x64`).

Then `tools/package_linux.sh` builds the host (`tools/build.py host --linux`), strips it and the
libraries, and builds the launcher with them inside (`XIVLEGACY_LINUX_HOST_DIR`, crates/launcher/build.rs).
A launcher built without that variable looks for `xivlegacy-host` beside itself.

## Testing without a Linux PC

A Lima VM on an Apple silicon Mac runs the x86_64 build through Rosetta (`vmType: vz`,
`rosetta: {enabled: true, binfmt: true}`, Debian 13 with `dpkg --add-architecture amd64` and the amd64
runtime: `libvulkan1 mesa-vulkan-drivers libx11-6 libxkbcommon0 libgl1 libegl1 ...`). Software Vulkan
(lavapipe) and Xvfb are enough to reach the title screen; it is slow. The VM's own `clang` or headers
are arm64 and of no use to the x86_64 build, which is one more reason the launcher always uses zig
with its bundled headers on Linux.
