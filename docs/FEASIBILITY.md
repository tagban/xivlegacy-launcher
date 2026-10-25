# XIVLegacy: native FFXIV 1.23b client — feasibility notes

Goal: FINAL FANTASY XIV 1.23b as a self-contained native program for macOS
(first) and modern Windows, that connects to any server you name, and lives
alongside retail FFXIV without touching its files, registry or settings.
No Wine.

Server side: [Garlemald Server](https://github.com/swstegall/Garlemald-Server)
(Rust port of Project Meteor), our fork at
[tagban/Garlemald-Server](https://github.com/tagban/Garlemald-Server).

## Game files

Inputs the player supplies (nothing from Square Enix is shipped by us):

- the retail disc image, `Final Fantasy XIV Online (USA, Japan) (En,Ja,Fr,De).iso`
  (UDF, July 2010, InstallShield 15 cabinets `data1.hdr` + `data1..3.cab`);
- the 52 ZIPATCH files from `D2010.09.18.0000` (boot) through
  `D2012.09.19.0001` (game), e.g. archive.org's `ffxiv_patches.zip`.
  `2012.09.19.0001` is the version Garlemald requires.

### Disc layout

The installer's file groups map straight onto the install:

| InstallShield group | install folder |
|---|---|
| `NewComponent1` (37,819 files) | `client/` |
| `NewComponent2` (99,941 files) | `data/` |
| `ファイル` ("files", 5 files) | install root (`ffxivboot.exe`, `ffxivconfig.exe`, `ffxivupdater.exe`, …) |

9.7 GB, 137,789 files. Everything else in the cabinets is InstallShield's own.
**There is no `ffxivgame.exe` on the disc**; the first game patch adds it.

### ZIPATCH

PNG-style chunks after a 12-byte `\x91ZIPATCH\r\n\x1a\n` signature:
`size u32 BE, tag[4], body, crc32 BE`.

- `FHDR`, `APLY`, `APFS`: metadata.
- `ADIR` / `DELD`: create / delete a directory (`len u32 BE, path`).
- `ETRY`: `len u32 BE, path, count u32 BE`, then `count` items of
  `mode u32 LE, src_sha1[20], dst_sha1[20], comp u32 LE, csize, old_size, new_size (u32 BE)`
  and, for the last item only, `csize` bytes of data.
  - mode `0x41` add, `0x4D` modify, `0x44` delete (no data);
  - comp `0x5A` zlib, `0x4E` stored;
  - **data is the whole new file**, not a diff; `dst_sha1` is its SHA-1 and,
    for modify, `src_sha1` is the SHA-1 of the file it replaces.

Across all 52 patches: 308,090 entries (199k modify, 86k add, 15,454 delete),
10,400 `ADIR`, 1,131 `DELD`. 97,496 distinct paths: 72,437 under `data/`,
25,053 under `client/`, 6 at the root.

`dst_sha1` proves every file the builder writes. `src_sha1` is not a reliable
link to the previous version: in 112,072 modify entries it differs from the
`dst_sha1` the previous patch wrote for that file, and 1,670 first changes
don't match the disc's copy. Since every entry carries the whole new file,
the result is still exact (disc files are MD5-checked by the cabinet reader);
the builder only counts these.

The patches don't touch `boot.ver` / `game.ver` (the disc's say
`2010.07.10.0000`). The retail updater wrote the last patch applied:
`2010.09.18.0000` and `2012.09.19.0001`; the client reports `game.ver`, and
Garlemald expects `2012.09.19.0001`. The builder writes both.

InstallShield stores identical files once: later copies link back to an
earlier file, sometimes through a chain (e.g. file 137832 → 137831 → 126919,
the one with data).

Result: 188,697 files, 12 GB (disc 137,765 files / 9.7 GB, then 292,636
writes, 15,454 file deletes and 273 directory deletes).

## ffxivgame.exe (2012.09.19.0001)

- 15,996,808 bytes, SHA-1 `04fd1c65eb0ab2e5d37d814dac1298ae2940e86a`,
  written by `D2012.09.19.0001.patch`.
- PE32 i386, Visual Studio 2005 (linker 8.0), built 2012-09-11.
- Image base 0x400000, **no relocations**, TLS directory, **not packed**.
- `.text` 0xB3B56D bytes (11.8 MB of code), `.rdata`
  3.2 MB, `.data` 1.1 MB. A leftover section named `MSSMIXER`.
- Statically linked: Lua 5.1.4, OpenSSL 1.0.0, zlib 1.2.3 (named by
  meteor-decomp's Function ID pass).
- About 4.0 M instructions by linear sweep, including ~227 k SSE/SSE2 and
  ~49 k x87.

### Imports (391)

| DLL | count | notes |
|---|---:|---|
| KERNEL32 | 185 | includes **fibers** (`ConvertThreadToFiber`, `CreateFiberEx`, `SwitchToFiber`) |
| USER32 | 67 | |
| d3d9 | 4 | `Direct3DCreate9` (one call site) |
| d3dx9_41 | 11 | textures, surface/volume loads, `D3DXCompileShader` |
| WS2_32 | 30 | plain BSD sockets, `select` |
| WINHTTP | 12 | |
| DINPUT8 / XINPUT1_3 | 1 / 3 | |
| IMM32 | 12 | Japanese input |
| WINTRUST / CRYPT32 | 1 / 7 | signature check; stub |
| VERSION, SHELL32, ole32, OLEAUT32, WINMM, GDI32, ADVAPI32, iphlpapi, COMCTL32 | | |

Loaded at run time: `XAudio2_0.dll` (sound is XAudio2 via COM),
`nvapi.dll`, `mscoree.dll`, `NETAPI32.DLL`.

### Shaders

The main renderer loads shader code from data resources
(`ShaderCodeResourceNode`). `D3DXCompileShader` has a single call site
(0x43F037) behind a flag, with a bytecode path on the other branch. Which one
runs is to be confirmed at run time. If HLSL compiling is needed,
vkd3d-shader compiles SM2/SM3 HLSL.

## Approach

Recompile `ffxivgame.exe` to C on the player's machine at install time and
run it on portable Win32 stand-ins: D3D9 → Metal, XAudio2 → SDL3 audio,
DirectInput/XInput → SDL3 gamepads, ws2 → host sockets.

| piece | work for XIV |
|---|---|
| x86 → C translator (`crates/recomp/src/x86c.rs`) | x87 and integer code; **needs SSE/SSE2** |
| PE loading, TLS | simple here (no relocations) |
| KERNEL32 / USER32 stand-ins | fibers, W-variants, console no-ops |
| graphics | a D3D9 front end over a Metal back end; SM2/3 bytecode → MSL |
| sound | XAudio2 stand-in |
| input | DirectInput8 and XInput |
| network | ws2; redirect the lobby host at the socket layer (the in-exe slot holds only 19 characters) |
| WinHTTP, VERSION, CRYPT32/WINTRUST | stubs |

## Launch handoff

What `ffxivboot` normally passes (from Garlemald Client / Seventh Umbral):
`ffxivgame.exe sqex0002<b64>!////`, where `<b64>` is URL-safe base64 of
`" T =<tick> /LANG =en-us /REGION =2 /SERVER_UTC =1356916742 /SESSION_ID =<56 chars>"`
(NUL-terminated), Blowfish-encrypted with the key `hex8(tick & ~0xFFFF)`.
The lobby host string lives at RVA 0xB90110 (max 0x14 bytes with the NUL);
a server-UTC immediate is patched at RVA 0x9A15E3.

The slot holds the default `lobby01.ffxiv.com`, sitting next to the config
key names `net_lobby_host` and `net_lobby_port` (one code reference, at
0x4B9A37). A native client does not need to patch it: the `getaddrinfo`
stand-in maps `lobby01.ffxiv.com` to the server the player chose, with no
length limit. The exe also names `secure.square-enix.com` (login) and the
registry values `InstallDir` / `Repository`; the registry stand-in keeps
those inside our own folder, away from a retail install.

## Progress (2026-10-22)

- **Discovery** (`tools/discover.py`, Ghidra 11.3.2 headless + the scripts in `discovery/`): 10 min of
  auto-analysis, then the fixpoint passes. The gate needed a handful of manual verdicts
  (`discovery/verdicts.py`): false function starts seeded by ASCII strings in data, OpenSSL's
  aes-586 tables inside `.text` (0xE4A0C0, 0xE4B740), a 1,982-entry jump table after the switch
  at 0x57ED4E, and 7 switch case counts. Exported `meta/ffxivgame.2012.09.19.0001.meta.json`:
  **122,492 functions, 1,116 switches, 48,558 tail jumps** (addresses only).
- **Translation** (`crates/recomp`, with SSE/SSE2/MMX added in `x86c_sse.rs` + `runtime/sse.h`):
  **123,249 functions, 0 unimplemented instructions**, in 6 seconds; 519 MB of C that passes
  `clang -fsyntax-only`. A fixed-base image needs no relocation handling (every address constant
  is used as is).
- **Boot test** (`tests/boot.c`, `tools/build.py boot`): the portable runtime
  (guest window, loader, thunks, kernel32/registry/files/OLE stand-ins) loading the exe and
  running `mainCRTStartup`.

## Risks

1. SSE support in the translator: mechanical but large.
2. Fibers: recompiled code needs host stack switching (ucontext or a small
   assembly switcher). Contained: every fiber call is in one small wrapper
   at 0xDCEF00–0xDCF030 (1 `CreateFiberEx`, 1 `ConvertThreadToFiber`,
   3 `SwitchToFiber`). The TLS directory has no callbacks.
3. D3D9 SM3 shader translation and whatever fixed-function state remains.
4. Size: 11.8 MB of code; recompile time and binary size on the player's
   machine.
