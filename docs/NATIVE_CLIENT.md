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
| `meta/ffxivgame.<build>.patches.json` | instruction patches applied before translation (the 2012 clock pin and two crash guards, as Garlemald Client and Seventh Umbral patch the exe; the action bar shown outside active mode, a more solid chat window and the profanity filter off by default; the camera distance clamp compares against the camera's own float limit, so Monster Mode can scale it) |
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
    --out generated/c --all --patches meta/ffxivgame.2012.09.19.0001.patches.json --hooks streamlog=0x4e7290,replace_lua_protected=0xdcf140,actor_lookup=0x610d55,camera_follow=0x6163a6
python3 tools/build.py boot
build/boot "<1.23b install>" <56-character session id>
```

Run the recompiler again whenever `meta/*.patches.json` or the launcher's hooks change: a client built
on an older `generated/c` lacks those patches. `table.c` records the patches recomp applied (a stamp)
and the hooks it was given, and `tools/build.py` stops when they don't match `meta/` and
`crates/launcher/src/make.rs` (`XIVLEGACY_STALE_TRANSLATION=1` builds anyway). Test clients built on a
translation from before the 0x616F93 camera patch flew the camera to the world's origin after warps in
Ul'dah for a week (2026-11-24).

The graphics back end is chosen at build time: `XIVLEGACY_GFX=wgpu python3 tools/build.py boot` (or
`host`) builds on wgpu-native (`brew install wgpu-native`, or `WGPU_NATIVE=<prefix>`); macOS defaults to
Metal, Windows and Linux always use wgpu. `tools/wgsl_check.c` translates a shader dump
(`XIVLEGACY_DUMP_SHADERS`) to WGSL and has wgpu build every pair; `XIVLEGACY_WGSLDUMP=<dir>` saves each
pair's WGSL from a running game.

A session id comes from the server's web login (`POST /login` on :54993 answers with
`ffxiv://login_success?sessionId=...`). The lobby host the game asks for (`lobby01.ffxiv.com`) is mapped
to `XIVLEGACY_LOBBY` (default 127.0.0.1). The game's Documents folder is `XIVLEGACY_USER` (default
`./xiv-user`).

A server that stops answering without closing the connection (the map server behind the world server
dies, a link drops) counts as a lost connection after `XIVLEGACY_NET_TIMEOUT` seconds (default 45, 0 =
never) of silence while the game keeps sending: the client closes it, and the game shows its own game
server error box (code 30002) and returns to the character list, as it does when a server closes the
connection.

The title screen logs in to the lobby by itself; Start opens "A Word to Our Players", whose default
button is No.

## Settings and muted test clients

The launcher's Settings screen (the footer's Settings…, or Start on a gamepad) is applied at each
Join, through what the game and the client already read:

| Setting | How it reaches the game |
|---|---|
| Resolution; Window, Borderless window | the game's own `Documents\My Games\FINAL FANTASY XIV\config.sys` (below) |
| Full screen | config.sys's full-screen flag plus `XIVLEGACY_FULLSCREEN=1`: the frameless window covers the display at any resolution, the picture scaled; the display's mode is never changed |
| Sound off | `XIVLEGACY_NOAUDIO=1`: no audio device; the engine runs on the clock, so the game behaves the same |
| Volume | `XIVLEGACY_VOLUME=0`-`100`: the gain of the client's one output stream |
| VSync | `XIVLEGACY_VSYNC=0` / `1`, over the game's own present interval (Metal and wgpu) |
| Graphics API (Windows, Linux) | `XIVLEGACY_WGPU_BACKEND` |
| Test switches | one row each for `FRAMESTATS`, `WGPU_STATS`, `WGPU_PRESENT`, `MONSTERCAM`, `ACTORLOOKUP` and the speed-test switches `PARALLEL`, `CPUSETS`, `NOPBUF`, `SPINNAP`, `LOCKWAKE` (all `XIVLEGACY_`); the first choice of each unsets the variable for the game, even one set with `setx`. A switch a client does not know is ignored |
| Extra switches | any `NAME=value` lines, set for the game after the ones above; `NAME=` clears one set with `setx`. `set`, `setx` and `export` lines are taken as they are |

The launcher logs what it set (`settings:` and `game switches:` in launcher.log). Sound On unsets
`XIVLEGACY_NOAUDIO` (the client mutes on any value but `0`, so a value left by `setx` is cleared).

**Launch profiles.** The drop-down beside Join (left and right on a gamepad) picks switches for the
next Join only; it goes back to Normal once the game starts. Speed test A is the defaults, B to G add
one switch each (B `PARALLEL=1`, C `CPUSETS=0`, D `NOPBUF=1`, E `SPINNAP=0`, F `LOCKWAKE=all`,
G `WGPU_BACKEND=vulkan`, Windows, where Direct3D 12 is the default), and every speed test adds `XIVLEGACY_FRAMESTATS=1`. A profile wins
over Settings for its switches. Under the Join row the launcher shows every switch that Join sets.
A speed test also sets `XIVLEGACY_BUG_TAG=speed <letter>`, so the in-game report (Ctrl+F12, or both
sticks held in) opens the chat line with `!bug speed <letter> ` and the report says which run it was.

config.sys is what ffxivconfig.exe writes; ffxivgame.exe reads it at start-up (0x405C90, version
0x20120419, 0xE0 bytes of little-endian u32s and two UTF-16 strings). +0x10 is 0 for a window with a
frame, otherwise full screen (a frameless window at 0,0); +0x14 and +0x18 are the resolution, which
the game raises to at least 1024x720. Without the file the game uses 1280x720 in a window. The
launcher changes only those three fields of a file that is there. The game's own full screen is, on
our client, a frameless window at the resolution at the display's top left (Borderless window); it
covers the display only at the display's own size, or with `XIVLEGACY_FULLSCREEN=1`.

**Starting muted.** Test clients and scripts:

- the host: `xivlegacy-host --mute --module ...`, or `XIVLEGACY_NOAUDIO=1` (any value but `0`);
- the launcher: `XIVLegacy --mute` (on a Mac,
  `/Applications/XIVLegacy.app/Contents/MacOS/XIVLegacy --mute`) mutes the games it starts in that run,
  whatever Settings say. `XIVLEGACY_NOAUDIO=1` in the launcher's environment reaches the game as well.

## Switches for debugging

| Variable | |
|---|---|
| `XIVLEGACY_CONTROL=<fifo>` | drive a running game: `key <name>`, `hold <key> <ms>`, `click <x> <y>`, `move <x> <y>`, `type <text>`, `snap <png>`, `stack`, `findf <x> <y> <z> [tol [hex from]]` (guest addresses holding a float triple), `findnan` (three NaN floats in a row), `luatest` (the next Lua protected call first checks that a Lua error lands back in it: `[lua] self-test` in the log), `look <button> <dx> <dy> <steps> [ms [still ms]]` (hold a mouse button and drag: mouse-look), `keydown <key>` / `keyup <key>`, `wheel <notches> [count]`, `stall <ms>` (hold the guest lock: a hitch), `dumpf <hex addr> [count]` (guest floats), `trace` (the next frame's targets, clears, copies and draws), `snaptex <id> <png>` (a texture or render target by its trace id; float formats scaled), `gfxdbg <n>` (1 wireframe, 2 gray textures, 3 gray render targets), `skipvs <id>`, `vsc` (current vertex constants), `cpuvs <id>` (the next draw with that vertex shader against the CPU interpreter), `onlyvs <id>` (draw only that shader in full-size passes), `dumpvs <id> [n]` (draw inputs), `watch <hex addr> [bytes]` / `unwatch` (log the translated functions that write there) |
| `XIVLEGACY_KEYS="12:return,15:left,17:click@575x515"` | scripted input at seconds after start |
| `XIVLEGACY_SNAP=<dir>`, `XIVLEGACY_SNAP_EVERY=<n>` | the back buffer as PNGs |
| `XIVLEGACY_PROFILE=<seconds>:<delay>` | in-process sampling profiler (self/inclusive by function, per thread). On Windows (host_win.c) self time only, translated code named `f_<guest address>` through rt_table; launch profile "Speed test P" sets `120:60` |
| `XIVLEGACY_PROFILE_FOCUS=<name>` / `=><name>` | with the profiler: the call stacks above the functions matching `name` / what they call |
| `XIVLEGACY_FRAMESTATS` | fps, draws and clears a frame |
| `XIVLEGACY_GAMELOG` | the game's own log lines (its log callbacks, and the stream log via the `streamlog` hook) |
| `XIVLEGACY_MSLDUMP=<dir>` | each translated shader pair's Metal source, named by the ids traces print |
| `XIVLEGACY_VBCHECK` | log indexed draws whose indices reach past the vertex data supplied |
| `XIVLEGACY_DUMP_SHADERS=<dir>` | every shader's bytecode, plus the pairs drawn with (`tools/msl_check.m` compiles them all) |
| `XIVLEGACY_RECOMP_TRACE=<substring>` / `=1`, `XIVLEGACY_RECOMP_FIRSTCALLS=1` | import calls, with the calling thread (`=1` also logs failed file opens) |
| `XIVLEGACY_SLOW_LOADING=1`, `XIVLEGACY_LOADINGLOG=1` | load at the game's own pace (no speedups behind the loading screen or after a tap, to compare); log them switching on and off |
| `XIVLEGACY_LOCKSTATS=1` | every 2 s, per thread: time holding the guest lock, time waiting for it, times taken (`[lockstats]`) |
| `XIVLEGACY_VISLOG=1` | each frame's change from the one before, with its time, read back without a stall (`[vis]`): how long the screen takes to answer input |
| `XIVLEGACY_TRACE_WINDOW=<from>:<to>` | with `XIVLEGACY_RECOMP_TRACE`: only between those seconds after start |
| `XIVLEGACY_LONGCALLS=<ms>` | each import call that took at least that long, with its thread and call site (what the game waits in when it looks idle) |
| `XIVLEGACY_FILES_TRACE=1` | every file the game opens, with its handle |
| `XIVLEGACY_FILELOG`, `XIVLEGACY_MSGLOG`, `XIVLEGACY_SENDSTACK=<len>`, `XIVLEGACY_FIBERLOG` | files opened, window messages, the stack of sends of a length, fiber switches |
| `XIVLEGACY_CASTCHECK=1` | as each drain starts, the game's own `dynamic_cast<CharaActor>` of every queued actor compared with its RTTI (`[actors] dynamic_cast...` on a mismatch) |
| `XIVLEGACY_ACTORLOG=1` | every actor the deferred-delete guard drops from a delete batch (`[actors]`), the actor's own slot too, and queued actors at an address freed once (a hint only); without it only the drops and cleared effect controllers that matter are logged |
| `XIVLEGACY_NOAUDIO`, `XIVLEGACY_NOGFX` | run without the audio device / without Metal (`XIVLEGACY_NOAUDIO=0` keeps the sound) |
| `XIVLEGACY_PARALLEL=1` | the main loop runs beside the guest lock, as on Windows (experimental: `docs/PARALLEL.md`); control FIFO `par 0`/`par 1` switches it, `mark <text>` logs a marker |
| `XIVLEGACY_SHIMCOUNT=1` | calls per shim every 5 s, the drawing thread's and the others' |
| `XIVLEGACY_VOLUME=<0-100>`, `XIVLEGACY_VSYNC=0` / `1`, `XIVLEGACY_FULLSCREEN=1` | the sound's loudness; VSync over the game's choice; the game's full-screen window covers the display (Settings) |
| `XIVLEGACY_BUG_TAG=<words>` | the bug-report chat line opens with `!bug <words> ` instead of `!bug ` (launch profiles set `speed A`-`speed I` and `speed P`) |
| `MTL_DEBUG_LAYER=1` | Metal's API validation |
| `XIVLEGACY_MONSTERCAM=0` / `=2` | Monster Mode camera: off (a person's distances for any body) / log each body and its scale (`[monstercam]`) |
| `XIVLEGACY_ACTORLOOKUP=0` | Leave the camera's actor-by-id lookups unchecked (the `[actors] id lookup` check, `actor_lookup` hook): to rule it out |
| `XIVLEGACY_MONSTERICONS=0` / `=<id>:<icon>,...` | Monster Mode hotbar icons: keep the game's "Undefined" placeholder / set icons by hand, to try one (`[monstericons]`) |
| `XIVLEGACY_MONSTERPOINTS=0` / `=<id>` | Monster Points and the monster vendors' shop pack 5124: leave the money and shop sheets alone / name another unused money id instead of 1000124, to try one (`[monsterpoints]`) |

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
- macOS put the drawing thread on efficiency cores 30% of the time at the default QoS class: it and the
  main loop ask for the user-interactive class (plat_thread_interactive), 22 -> 28 fps in the world.
- Translated functions pass the general registers in host registers (guest.h RtRegs, swiftcall: four
  packed 64-bit values in and out); through Guest each call was ~45 loads and stores. Each function
  has a fast body f_X_r and an entry f_X(Guest*) for the runtime; indirect calls go through
  rt_fastcall.h. 28 -> 34 fps. Flags still travel through Guest at returns: dropping them broke the
  title screen.
- The drawing thread runs ~340 million instructions a frame at about 3 per cycle; three quarters of
  the guest memory references in the translation are stack traffic (push/pop, [esp+d]). Fewer
  instructions alone (folded addresses, ThinLTO) did not change the frame rate.
- Actors are deleted a frame late: the world's remove-actor pushes them onto a vector (0x134DFA0)
  that 0x7D2C40 drains in two batches through 0x7D27A0 (pass 1: `dynamic_cast<CharaActor>` and
  0x65DD20, which calls through the RaptureSchEffectController at +0x12F0; pass 2: 0x65DD40; pass
  3: delete). The common crash there (FATAL / SIGBUS reported in 0x65DD20, 0x65DD40 or
  "f_007d27a0 + 872", the return address of the pass-1 call) was a translation bug, not the game's:
  the recompiler's saved-register pass (`save_slots`, crates/recomp) took the switch cases Ghidra
  splits off 0x855790 for functions of their own, so a `push edi` there that is an argument
  became a C local. 0x8553F0 then wrote its 4x4 matrix to whatever the stale stack slot held, the
  address of RaptureActor's type descriptor, every frame: the matrix covers the type names of
  RaptureActor and CharaActor. Whenever both names start with the same bytes, `__RTDynamicCast`
  matches any RaptureActor (a BgPlateActor, a WindowActor) as a CharaActor, and passes 1 and 2
  read and write CharaActor fields past the end of it. The pass now skips switch cases and pushes
  of a register that isn't its incoming value (171 functions changed). The runtime checks those two
  names on each critical section and logs and restores a change (`[actors] type name at ...`).
- Separately, a CharaActor is sometimes queued for delete again after its destructor ran (the
  block freed by 0x669E3C seconds before; cause not found yet). Its controller is gone by then, so
  the drain drops it (`[actors] ... destroyed already, not deleted again`), and also takes
  non-objects, duplicates and actors added back to their world's list out of the queue
  (`xiv_crashlog.c`). The game frees most objects through its own allocator (0x40DF70, which
  enters a critical section first), not HeapFree, and allocates through paths the runtime doesn't
  all see: a table of freed blocks can't tell a dangling pointer from a reused address.
- Actors are also found by id: the world (a RaptureActorContainer, actor+0x118) keeps a
  `std::map<id, Actor*>` in its actor manager (world+0xA8; 0x7D2A50 finds in it). 0x610D30 looks
  up the id at +0x134 of the world's +0x70 object there for the per-frame updater 0x7F1BD0. Once
  (Platinum Mirage, about two minutes after a same-zone warp, Tab-targeting and walking) that found
  an actor whose memory was already given back: SIGBUS in 0x663CE0, reading +0x1350 of it. Not
  reproduced in about 100 warps since, so the stale entry's source is unknown. The recompiler's
  hook at 0x610D55 (`--hooks actor_lookup=0x610d55`, module ABI 5) has the runtime check what the
  lookup found: anything but a whole, live CharaActor comes back as not found, which the updater
  skips (`[actors] id lookup for <id> found <addr> (<class>) ...`). All nine callers of 0x610D30
  are camera contexts that read the result as a CharaActor. On 2026-11-12 the lookup returned a
  live BgObjActor after `!goto 1001424` across Ul'dah: the screen went black (camera locked on it),
  then the same SIGBUS at +0x1350. The id looked up was 0xC0000000, the game's "no actor" id, which
  the map also holds for local actors with no server id (WeaponActor, BgObjActor). The camera's
  "no lock target" test 0x616F90 (SetContext 0x618650 and the update at 0x619BC2 pick a lock context
  when it's false) inlined the same lookup without the check, so on Windows 0.1.26 it found the
  BgObjActor while the lock context's own lookup found nothing: the camera stopped and the player ran
  on. patches.json makes 0x616F90 call 0x610D30, so the check covers both. An actor freed while still in
  that map is logged with the stack that frees it (`[actors] ... freed while still in its world's
  id map`).
- A FATAL or crash also logs who freed what the registers point into, the drain's batches and the
  last game packets. Crash frames name the return address, not the faulting function: check which
  call sits before "+ N" in the host disassembly. `watch <hex address> [bytes]` on the control fifo
  logs each translated function that writes there.
- The CRT's x87 math dispatch jumps through unaligned tables (`discovery/CrtDispatch.py`); the
  MSSMIXER section holds a CPU check outside `.text` (a native stand-in in `xiv_misc.c`).

- Monster Mode (the server gives the local player a monster's model): the third-person camera aims
  between the skeleton's EID_CAMINT_HI and EID_CAMINT_LOW points (CharaActor+0x16C0/+0x1700, set up by
  0x664890; 0x664100 aims), but its distances (camera actor +0x3F0..+0x400, default 4.5 and limits
  1.5..10 in the floats at 0xFB70B8/0xFB70C0/0xFB70BC) are a person's for any body. Monster skeletons
  lack the crafting camera point EID_CAM_CFT (+0x16A8 is -1), which tells them from player races.
  The hook at 0x6163A6, the end of 0x616380 that returns the chara the camera follows
  (`--hooks camera_follow=0x6163a6`, module ABI 6), scales the distances by the body's nameplate
  height (EID_NAME times the actor's scale) over a person's (`xiv_monster_cam.c`). The clamp setter
  0x611350 compared against a double 10.0 at 0xF67848 shared with other code; a patch at 0x611372
  makes it compare against the float at 0xFB70BC.
- Monster moves (command ids 23011-23645) already have English names and recast times in the client's
  sheets; only their icon is the placeholder 30000, a picture of a red cross and "Undefined"
  (data/01/03/04/A6.DAT, 26-byte rows, u16 icon at +0; A7.DAT holds the id runs; A8 the offsets, by
  id minus the block's first id). `xiv_monster_moves.c` swaps in a player icon by the move's kind
  from the server's data as the game reads the file. The command sheets' names don't line up with
  their icon pictures (30146, named Fire, is an ice picture): pick icons by looking at them.
- Monster Points (money item 1000124, a placeholder "[en]" row) and the monster vendors' shop pack 5124
  come from `xiv_monster_points.c`, which serves changed copies of the money text sheets
  (data/0B/45/03/36 English, 02/D9, 03/93, 03/F0) and the shop sheets (packs 01/03/02/BF-C1, goods
  01/03/05/19-1B) from a cache folder through `CreateFileA`; the list is
  `xiv_monster_points_table.h`. A pack row is its first and last goods slot; a goods slot is item id,
  a flag byte and the price, and the shop window shows that price with the currency's icon. The game
  only reads goods slots inside the sheet's id span and a header of the size it had: slots appended
  after 2027211, or given a run of their own, leave the window closed (openShopBuy returns and
  selectShopBuy waits forever), so the 21 slots fill the hole 2027111-2027131 and the run before them
  grows. The server's shopRange for 5124 must be the same slots, in the same order. A client without
  the patch crashes (c000000d at 0x9D22C0) when the server opens pack 5124. The description shows in
  the currency list, but the list and the shop still name the currency "****": the name there does
  not come from the sheet's name fields (patching 1000123's row changed only its description).

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
