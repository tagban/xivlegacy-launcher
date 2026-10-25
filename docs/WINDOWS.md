# XIVLegacy on Windows: building, running, measuring

The Windows client is the same as the Mac one (see [NATIVE_CLIENT.md](NATIVE_CLIENT.md)): a
**host** (`xivlegacy-host.exe`: the runtime, graphics through wgpu, SDL3 for the window, input
and audio) and a **game module** (`xivlegacy-game.dll`: the player's own `ffxivgame.exe`
translated to C and compiled on their machine). `XIVLegacy.exe` carries the host and makes the
module on the first Join.

## Where things are on a player's PC

| What | Where |
| --- | --- |
| Settings, logs | `%APPDATA%\XIVLegacy\` (`launcher.json`, `launcher.log`, `logs\game.log`) |
| The host, unpacked from XIVLegacy.exe | `%APPDATA%\XIVLegacy\bin-<version>\` |
| The game module | `%APPDATA%\XIVLegacy\game\xivlegacy-game-<key>.dll` |
| zig (the compiler, downloaded once) | `%APPDATA%\XIVLegacy\zig-0.15.1\` |
| The game | `%LOCALAPPDATA%\XIVLegacy\FINAL FANTASY XIV 1.23b\` (default) |

## Running the host by hand

From `%APPDATA%\XIVLegacy`, with a session id from signing in (the launcher logs the steps):

```
bin-<version>\xivlegacy-host.exe --module game\xivlegacy-game-<key>.dll "<game folder>" <56-character session id>
```

`XIVLEGACY_LOBBY=<server>` picks the server (name or IPv4; default 127.0.0.1). For measuring the
title screen, point it at a server that answers (`XIVLEGACY_LOBBY=xiv.ffxi.cc`): with no lobby at
all the game gives up and quits after about 30 frames.

## Switches worth knowing

| Variable | Does |
| --- | --- |
| `XIVLEGACY_FRAMESTATS=1` | frame time statistics in the log |
| `XIVLEGACY_WAITPROF=1` | every 2 s: frames, and where the drawing thread spent its time per import (`[waitprof]`, `API time`). On a Mac at the title screen: 120 frames, Sleep ~650 ms, Present ~60 ms, timeGetTime ~800k calls |
| `XIVLEGACY_WGPU_BACKEND=dx12` / `vulkan` | which API wgpu drives (default: wgpu's pick, Vulkan on most PCs) |
| `XIVLEGACY_WGPU_PRESENT=fifo` / `mailbox` / `immediate` | present mode (vsync or not) |
| `XIVLEGACY_WGPU_STATS=1` | per-frame wgpu costs (passes, uploads) |
| `XIVLEGACY_NOAUDIO=1` | no audio device (to tell audio stalls from graphics ones) |
| `XIVLEGACY_STOCK=1` (launcher) | start the 2010 ffxivgame.exe instead of our client |

Audio: `[xaudio2] N of M passes late (the device ran dry)` counts the times the engine fed the
device late; `voice passes short of data` the times the game fed its voices late. The engine
keeps more queued when the device runs dry (`keeping N ms queued now`).

## Building

Everything is cross-built from a Mac with zig (`python3 tools/build.py host --windows`, `module
--windows`, and the launcher with Rust's `x86_64-pc-windows-gnu` target). Building on Windows
directly would need clang or `zig cc` as CC, the SDL3 MinGW development package
(`XIVLEGACY_SDL3_MINGW`) and wgpu-native's `wgpu-windows-x86_64-gnu-release.zip`
(`XIVLEGACY_WGPU_WIN`); see `tools/build.py`.

## Open questions (October 2026)

- **Frame rate** was 11.4 fps on an RTX 4060 through 0.1.13: the game's drawing thread slept ~76 ms
  per 2 ms Sleep. The cause was the sleep timer in `plat_win.c`, meant to be one per thread but
  shared by all of them (MinGW compilers ignore `__declspec(thread)`; the code uses `RT_TLS` now, and
  the Windows build makes unknown attributes an error). A waitable timer wakes one waiter, so each
  thread woke on whichever thread's time came up first. Fixed in 0.1.14; measure again with
  `XIVLEGACY_WAITPROF=1`.
- **Audio** ran dry 1 pass in 8 on that PC with 0.1.7 (fine on the Mac); 0.1.8 asks for 10 ms
  device pulls and grows its queue when the device runs dry.
