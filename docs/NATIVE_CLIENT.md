# The native client

`ffxivgame.exe` (1.23b, 2012.09.19.0001), recompiled from 32-bit x86 to C on the player's own machine
and run against a portable Windows runtime, with Direct3D 9 drawn through Metal. No Wine, nothing of
Square Enix's shipped: the translation is made from the player's install.

Status (2026-10-22): logs in to a local [Garlemald](https://github.com/tagban/Garlemald-Server) server,
creates a character through the real UI, zones in (the Limsa Lominsa boat) and walks around, with
sound, on an Apple M4 at about 39 fps in the world (60 on the title and lobby screens). Correct
rendering; performance is the work now.

## Pieces

| Where | What |
|---|---|
| `discovery/`, `tools/discover.py` | Ghidra (11.3, Jython) finds every function: the per-build metadata in `meta/` (addresses only) |
| `meta/ffxivgame.<build>.patches.json` | instruction patches applied before translation (the 2012 clock pin, two crash guards: as Garlemald Client and Seventh Umbral patch the exe) |
| `crates/recomp` | the recompiler: x86 + x87 + SSE/SSE2/MMX to C, `generated/c/` |
| `runtime/portable/` | the Windows the game sees: kernel32, user32 (SDL3 windows, input, COMCTL32 subclassing), ws2_32, ole32, registry, file system mounts, fibers, XAudio2 2.4 (SDL3 audio), DirectInput/XInput, WinHTTP, D3D9 + D3DX |
| `runtime/portable/d3d9.c` | Direct3D 9: objects, state, state blocks, resources; hands draws to `gfx9.h` |
| `runtime/portable/gfx9_metal.m` | the Metal back end: pipelines, depth-stencil and sampler states, a per-frame ring for vertex/index/constant data, passes, present |
| `runtime/portable/sm3.c`, `sm3_msl.c` | D3D9 shader bytecode parser and the SM2/SM3 to Metal Shading Language translator (per vertex/pixel shader pair) |
| `runtime/portable/gfx9_wgpu.c`, `sm3_wgsl.c` | the wgpu back end (wgpu-native: Direct3D 12 on Windows, Vulkan on Linux, Metal on macOS) and the SM2/SM3 to WGSL translator; `XIVLEGACY_GFX=wgpu` |
| `tests/boot.c` | the test host: mounts the install, builds the launch argument, runs the game |

## Building and running

```sh
python3 tools/discover.py --game "<1.23b install>"            # once per build: meta/*.meta.json
target/release/xivlegacy-recomp recomp --meta meta/ffxivgame.2012.09.19.0001.meta.json \
    --image generated/images/2012.09.19.0001/ffxivgame.exe --retail generated/images/2012.09.19.0001/ffxivgame.exe \
    --out generated/c --all --patches meta/ffxivgame.2012.09.19.0001.patches.json --hooks streamlog=0x4e7290
python3 tools/build.py boot
build/boot "<1.23b install>" <56-character session id>
```

The graphics back end is chosen at build time: `XIVLEGACY_GFX=wgpu python3 tools/build.py boot` (or
`host`) builds on wgpu-native (`brew install wgpu-native`, or `WGPU_NATIVE=<prefix>`); macOS defaults to
Metal, Windows and Linux always use wgpu. `tools/wgsl_check.c` translates a shader dump
(`XIVLEGACY_DUMP_SHADERS`) to WGSL and has wgpu build every pair; `XIVLEGACY_WGSLDUMP=<dir>` saves each
pair's WGSL from a running game.

A session id comes from the server's web login (`POST /login` on :54993 answers with
`ffxiv://login_success?sessionId=...`). The lobby host the game asks for (`lobby01.ffxiv.com`) is mapped
to `XIVLEGACY_LOBBY` (default 127.0.0.1). The game's Documents folder is `XIVLEGACY_USER` (default
`./xiv-user`).

The title screen logs in to the lobby by itself; Start opens "A Word to Our Players", whose default
button is No.

## Switches for debugging

| Variable | |
|---|---|
| `XIVLEGACY_CONTROL=<fifo>` | drive a running game: `key <name>`, `hold <key> <ms>`, `click <x> <y>`, `move <x> <y>`, `type <text>`, `snap <png>`, `stack`, `findf <x> <y> <z> [tol [hex from]]` (guest addresses holding a float triple), `findnan` (three NaN floats in a row), `look <button> <dx> <dy> <steps> [ms [still ms]]` (hold a mouse button and drag: mouse-look), `keydown <key>` / `keyup <key>`, `wheel <notches> [count]`, `stall <ms>` (hold the guest lock: a hitch), `dumpf <hex addr> [count]` (guest floats), `trace` (the next frame's targets, clears, copies and draws), `snaptex <id> <png>` (a texture or render target by its trace id; float formats scaled), `gfxdbg <n>` (1 wireframe, 2 gray textures, 3 gray render targets), `skipvs <id>`, `vsc` (current vertex constants), `cpuvs <id>` (the next draw with that vertex shader against the CPU interpreter), `onlyvs <id>` (draw only that shader in full-size passes), `dumpvs <id> [n]` (draw inputs), `watch <hex addr> [bytes]` / `unwatch` (log the translated functions that write there) |
| `XIVLEGACY_KEYS="12:return,15:left,17:click@575x515"` | scripted input at seconds after start |
| `XIVLEGACY_SNAP=<dir>`, `XIVLEGACY_SNAP_EVERY=<n>` | the back buffer as PNGs |
| `XIVLEGACY_PROFILE=<seconds>:<delay>` | in-process sampling profiler (self/inclusive by function, per thread) |
| `XIVLEGACY_PROFILE_FOCUS=<name>` / `=><name>` | with the profiler: the call stacks above the functions matching `name` / what they call |
| `XIVLEGACY_FRAMESTATS` | fps, draws and clears a frame |
| `XIVLEGACY_GAMELOG` | the game's own log lines (its log callbacks, and the stream log via the `streamlog` hook) |
| `XIVLEGACY_MSLDUMP=<dir>` | each translated shader pair's Metal source, named by the ids traces print |
| `XIVLEGACY_VBCHECK` | log indexed draws whose indices reach past the vertex data supplied |
| `XIVLEGACY_DUMP_SHADERS=<dir>` | every shader's bytecode, plus the pairs drawn with (`tools/msl_check.m` compiles them all) |
| `XIVLEGACY_RECOMP_TRACE=<substring>` / `=1`, `XIVLEGACY_RECOMP_FIRSTCALLS=1` | import calls, with the calling thread (`=1` also logs failed file opens) |
| `XIVLEGACY_SLOW_LOADING=1`, `XIVLEGACY_LOADINGLOG=1` | load at the game's own pace (no loading-screen speedups, to compare); log the loading screen going up and down |
| `XIVLEGACY_LONGCALLS=<ms>` | each import call that took at least that long, with its thread and call site (what the game waits in when it looks idle) |
| `XIVLEGACY_FILES_TRACE=1` | every file the game opens, with its handle |
| `XIVLEGACY_FILELOG`, `XIVLEGACY_MSGLOG`, `XIVLEGACY_SENDSTACK=<len>`, `XIVLEGACY_FIBERLOG` | files opened, window messages, the stack of sends of a length, fiber switches |
| `XIVLEGACY_NOAUDIO`, `XIVLEGACY_NOGFX` | run without the audio device / without Metal |
| `MTL_DEBUG_LAYER=1` | Metal's API validation |

## Lessons the game taught

- Input reaches the game through `SetWindowSubclass` (COMCTL32 ordinals 410-413) on its window; its
  window procedure ignores keys and the mouse. `GetCursorPos` is how it places the pointer.
- Folders above a mount point (`C:\users`) must exist, or the game cannot make `My Games\FINAL FANTASY XIV`.
- The lobby needs the clock pinned near 2012-12-31 (the patch at 0xDA15E3).
- Metal must not get a negative `baseVertex`: the base vertex is applied to the indices.
- Text fields need WM_CHAR for Backspace, Tab, Enter and Escape (TranslateMessage's job; SDL's text
  input leaves them out).
- D3D9 pixel centres are a half pixel off Metal's: positions move by (+1/width, -1/height) times w.
- Threads share one guest lock. The thread that draws must keep it for long turns (50 ms): it gives
  it up whenever its frame Sleeps, and each forced yield also hands a turn to every other queued
  thread. 8 ms turns held the world at 26 fps, 50 ms at 40. For the same reason the XAudio2 engine
  fills its device queue in batches (50-80 ms) when the lock comes free, and asks for it urgently
  (`gt_lock_urgent`) only when the queue runs low.
- The game's main loop (the thread that owns its window) reads the mouse with `GetCursorPos` and
  sets the camera's target; the drawing thread moves the camera toward it each frame. With the
  1 ms turn other threads get, each main-loop tick (about 3 ms of work) was cut into pieces with a
  drawing-thread turn between them, and a zero-timeout `select` on its socket each tick gave the lock
  up again: the loop ran at 11 Hz in the world and the mouse camera turned in steps. It now gets
  3 ms turns outside loading and keeps the lock through a poll: ~24 Hz, about once a frame (fps -10%).
- The CRT's x87 math dispatch jumps through unaligned tables (`discovery/CrtDispatch.py`); the
  MSSMIXER section holds a CPU check outside `.text` (a native stand-in in `xiv_misc.c`).

## Next

- Performance: the render thread runs the game's own renderer in translated code; flags liveness and
  code quality in the translator, persistent Metal buffers for static vertex/index buffers, fewer
  per-draw copies.
- A launcher (window, sign-in, server choice) in place of `tests/boot.c`.
- Audio checks in the world, gamepad support (the controls screen greys the pad out).
- Windows, after macOS: a `plat_win.c` under the same `plat_*` calls; D3D9
  could go straight to the system's own d3d9 there instead of through `gfx9.h`.
- A Clear must take effect even when no draw follows it before the targets change: the Metal back end
  turns a whole-target clear into the next pass's load action, and used to drop it when the game
  switched targets first. Cutscenes then kept last frame's pixels wherever a frame drew nothing - a
  moving camera smeared into radial streaks and doubled characters (`flush_clear`).
- `cpuvs <id>` checks a draw's vertex shader against a CPU interpreter of the original bytecode
  (`sm3_interp.c`), per vertex and instance, from the positions the Metal shader writes back.
- A cutscene replays with Garlemald's `playTime` set to 0 (Gridania) or the opening quest's sequence
  set back (the boat's exit door).
- Tried and backed out:- Tried and backed out: `ret` without storing the flags crashed at the title screen (something reads
  flags a callee left).
