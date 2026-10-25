# XIVLegacy Launcher

FINAL FANTASY XIV 1.23b (the original 2010 game, not A Realm Reborn) as a
self-contained program for macOS and Windows that connects to the private
server you choose, and lives alongside retail FFXIV without touching it.

For private servers only, such as
[Garlemald Server](https://github.com/tagban/Garlemald-Server). This project
ships no Square Enix files: you supply the game, or let the installer fetch
the preserved copies from archive.org.

## Play on Windows

1. Download **XIVLegacy.exe** from the
   [releases page](https://github.com/tagban/xivlegacy-launcher/releases)
   and run it. There is nothing to install first.
2. **First run: game files.** XIVLegacy looks for the disc image (`.iso`, or
   the `.7z` it is preserved in) and `ffxiv_patches.zip` in your Downloads,
   Desktop and Documents folders and on every drive, and downloads whatever it
   doesn't find from archive.org (about 11 GB), switching to a backup copy
   if archive.org is down. It then builds a clean 1.23b
   install (about 12 GB) in `%LOCALAPPDATA%\XIVLegacy`. Already have a 1.23b
   folder? Choose **I already have the game…** instead.
3. **First Join: making the game.** XIVLegacy runs its own client, not the
   2010 `ffxivgame.exe`: it translates your copy of the game's code into a
   modern program and compiles it for your computer (a few minutes, once; it
   downloads the zig compiler, about 90 MB, the first time). Nothing of the
   game is downloaded.
4. **After that: pick a server and Join.** The list starts with
   `xiv.ffxi.cc`; **Edit…** adds others. **Create Account** makes an account
   on the selected server with the name and password you typed.

XIVLegacy checks for a newer version when it starts and offers **Update
now**, which replaces itself and restarts. **Repair…** (bottom right) reinstalls
DirectX if needed and checks every game file; if any are missing or damaged it
offers to reinstall the game files.

Windows may warn that the program is from an unknown publisher (it isn't
code-signed yet): choose **More info → Run anyway**.

What it changes: nothing outside its own folders. Its client draws with
Direct3D 12 or Vulkan (through wgpu) and needs none of 2010's DirectX. The server address is written into the
game's memory when it starts; the game files on disk stay as installed.
Passwords are kept only if you tick **Remember me**, in Windows Credential
Manager.

## The installer on its own

`xivlegacy-install` is the same setup as a command-line tool (macOS, Windows,
Linux):

```bash
cargo build --release

# Everything from archive.org (about 11 GB to download, 12 GB installed):
./target/release/xivlegacy-install setup "FINAL FANTASY XIV 1.23b"

# Or from files you already have — any mix; whatever you leave out is downloaded:
./target/release/xivlegacy-install setup "FINAL FANTASY XIV 1.23b" \
  --iso "Final Fantasy XIV Online (USA, Japan) (En,Ja,Fr,De).iso" \
  --patches ffxiv_patches.zip

# Check an existing game folder instead (--full hashes every file):
./target/release/xivlegacy-install check "/path/to/FINAL FANTASY XIV"
```

- **Disc:** a `.iso`, or the `.7z` it is preserved in (archive.org item
  `Nova_FFXIV_JPN`).
- **Patches:** `ffxiv_patches.zip`, a folder, or individual `.patch` files
  (archive.org item `ffxiv_patches`). All 52, boot `2010.09.18.0000` through
  `2012.09.19.0001`, are required; it says which are missing before doing any
  work.
- **Checks:** downloads resume and are checked against archive.org's SHA-1s;
  every disc file is MD5-checked; every patch and every file a patch writes is
  SHA-1-checked. `check` compares a folder with a built-in list of all 188,697
  files of a correct install (paths, sizes and hashes; no game data).

## Download links (`sources.json`)

Where setup downloads the disc and patches from is kept in
[`sources.json`](sources.json): a list of links for each, tried in order (the
first is the main copy, the rest are fallbacks used when it keeps failing).
Setup reads `sources.json` next to the program if there is one, else this
repository's copy, else the links built into the program, so changing the
repository's file changes the links for everyone without a new release.
`xivlegacy-install sources` shows which links would be used.

Only links come from the file: the expected size and SHA-1 of each download
are built into the program, so a download from any link must still be the
exact archive.org file.

## Native macOS client

`crates/recomp` and `runtime/` are a native macOS client (no Wine): the game's
x86 code statically translated to C, with Direct3D 9 drawn through Metal. See
[docs/NATIVE_CLIENT.md](docs/NATIVE_CLIENT.md). It isn't packaged for players
yet.

## License

AGPL-3.0-or-later. The static recompiler and the portable Windows runtime
derive from the author's (John Leighow) earlier native FINAL FANTASY XI
client. The InstallShield reader follows
[unshield](https://github.com/twogood/unshield) (MIT); the ZIPATCH format is
as documented by Seventh Umbral and Garlemald Client.
