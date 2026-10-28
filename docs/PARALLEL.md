# The main loop beside the guest lock (XIVLEGACY_PARALLEL)

On Windows, FFXIV 1.23b's main loop (the thread that owns the window: input, the camera's controls,
game logic) and its drawing thread run at the same time. In the native client every guest thread
takes turns under one guest lock (`runtime/portable/gthread.c`), and the main loop got about 15
ticks a second in the world. The mouse camera turned in steps: the main loop reads the cursor,
and the drawing thread moves the camera toward it.

`XIVLEGACY_PARALLEL=1` runs the main loop's translated code without the guest lock, beside
whichever thread holds it. Every other thread (the drawing thread, loaders, sound) shares the guest
lock as before. This is an experiment: off by default.

## What the guest lock protected

| | protected by | with XIVLEGACY_PARALLEL |
|---|---|---|
| the runtime's own state: windows and messages, handles, files, sockets, registry, fibers, XAudio2 | the guest lock | the **runtime lock**: held while a thread runs a shim (`thunk_dispatch`) or other runtime code, never while it runs translated code |
| the guest heap (`gheap_*`) | the guest lock | its own lock (`gwin.c`) |
| kernel objects (`kobj.c`) | already their own lock | unchanged |
| critical sections | the guest lock (plain read-modify-write) | an atomic compare-and-swap of the owner; no lock on the fast path |
| `Interlocked*` | the guest lock | atomics (`guest.h` `rt_xadd32`...) |
| `lock xadd`, `lock cmpxchg`, `xchg` with memory (41 + 130 sites in the exe) | the guest lock: the recompiler dropped `lock` | atomic, sequentially consistent (`crates/recomp`, `guest.h`) |
| the Direct3D 9 device | the guest lock | the guest lock's group: the drawing thread calls it without the runtime lock; the main loop's thread takes the guest lock for a device call (rare) |
| TLS, last error, the TEB | per thread | unchanged (no lock) |
| the game's own data shared between threads | the guest lock **and** the game's own locks | the game's own locks only, as on Windows |
| x86 memory ordering (TSO) | the guest lock's hand-off was the only cross-core ordering | **not provided** for plain loads and stores: see below |

Lock order: guest lock, then runtime lock. A thread holding the guest lock that enters runtime code
holds both, and a shim's blocking section (`gt_unlock`/`gt_lock` around a wait) gives up and takes
back both. Translated code called from a shim (a window procedure) runs without the runtime lock.

Shims the drawing thread calls about 1.5 million times a second do not take the runtime lock
(`lock_class_for` in `thunk.c`): shader constants and other device state, draws, `D3DPERF_*`,
critical sections, `Interlocked*`, TLS, last error and clocks. Through one lock, that lock's cache
line went back and forth between the two cores on every call, and fps fell by 10%.

## Measurements

Same session, same spot (the inn), `par 1` / `par 0` from the control FIFO, 3 rounds each, Apple M4,
fps and main loop ticks (`GetCursorPos` calls a second, in the `XIVLEGACY_FRAMESTATS` line):

| | fps standing | main loop | fps turning (mouse) | main loop |
|---|---|---|---|---|
| main loop under the guest lock (PR #6) | 20.7 | 16.9/s | 20.9 | 15.0/s |
| main loop beside it | 20.9 | 28.4/s | 22.0 | 28.4/s |

The drawing thread's waits for the locks: 3.5 ms a frame under the guest lock → 0.17 ms beside it.
The main loop now runs at its own frame limiter's rate (~29/s, the same as on the lobby screens)
and sleeps 87% of the time. A first version with every shim under the runtime lock also reached 29/s
but cost 1-3 fps.

Without `XIVLEGACY_PARALLEL` the build behaves as before (22 fps, main loop 16/s at that spot).

Soak: 24 rounds of walking with mouse turns and flicks plus the Main Menu opening and closing, and
four A/B sessions: no crash, hang or NaN camera.

## Memory ordering: the open risk

The translation is C, compiled for arm64. Apple Silicon is weakly ordered, while x86 (TSO) keeps a
thread's stores in order as seen by other cores. With the main loop on its own core, a plain store
publication with no lock in between (write an object, then a pointer or flag to it, read on the
other thread) can be seen out of order on arm64. Everything that goes through a critical section,
an `Interlocked*` call, a locked instruction or any shim is ordered: they are all full barriers now.
The compiler can also keep a guest value in a register across a spin loop that calls nothing.

Options:

- **Hardware TSO** (ACTLR_EL1.TSOEN, what Rosetta 2 uses): macOS gives it only to Rosetta's
  translated processes. A native process needs a kernel extension (TSOEnabler: SIP off), or must run
  in a Linux VM whose kernel exposes it. Not usable for a shipped client. On Linux on Apple hardware
  (Asahi) a prctl was proposed upstream but has not been merged.
- **Barriers in the recompiler**: acquire loads (`ldapr`) and release stores (`stlr`) for every
  guest memory access off the stack (the stack is per thread), as FEX does. This gives TSO in
  software. The cost has not been measured here: it means rebuilding all of `generated/c` with
  `rd*`/`wr*` changed. FEX and Box64 report it as large, and the drawing thread is already CPU
  bound. Cheaper variants are "barriers only in functions both threads run" (needs a profile of
  shared functions) and Box64-style heuristics.
- **Limit the parallelism to one pair** (this prototype): only the main loop leaves the guest lock.
  Its exposure is the data it shares with the drawing thread and the loaders without a lock. On
  Windows, x86 hid such races; MSVC 2005-era code often relies on `volatile` flags. No failure seen
  so far.

## Recommended path

1. Ship it behind the switch for testers (macOS), and keep PR #6's 3 ms turn as the default. The
   gain is the camera: 15 → 29 main loop ticks, at no fps cost.
2. Find what the main loop and the drawing thread share outside locks: the `watch` control
   command and a tool listing guest addresses written by one thread and read by the other in a
   short run. Put targeted barriers (or a recompiler "strong" list of functions) on those.
3. Then make it the default. Roughly 1-2 weeks for 2-3, mostly measuring.

The frame rate itself (under 30 in town) is the drawing thread's own work: in a profile it spends
about 63% in the game's translated scene and render code and 13% in DrawIndexedPrimitive → Metal,
and waits 2-5%. Threading does not raise it any further: that work is translated code quality and
the per-draw cost.
