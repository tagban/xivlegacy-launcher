//! Starting ffxivgame.exe against the chosen server (Windows).
//!
//! The game is created suspended and two things are changed in its memory before
//! it runs; the files on disk stay as installed:
//! - the lobby host slot (RVA 0xB90110, 0x14 bytes with the NUL), which holds
//!   `lobby01.ffxiv.com`, gets the server's host name;
//! - the call that reads the clock for the login's time check (RVA 0x9A15E3)
//!   becomes `mov eax, 1356916754`, a moment in late 2012, so today's date
//!   doesn't look like the far future of the build's SERVER_UTC.
//!
//! The game is also held to at most 15 processor cores; with more it crashes
//! at start-up. (These facts follow Seventh Umbral and Garlemald Client.)

use std::path::Path;

use anyhow::{Result, bail};

#[cfg_attr(not(windows), allow(dead_code))]
pub const LOBBY_HOST_RVA: u32 = 0xB90110;
pub const LOBBY_HOST_SLOT: usize = 0x14;
#[cfg_attr(not(windows), allow(dead_code))]
pub const CLOCK_PATCH_RVA: u32 = 0x9A15E3;
#[cfg_attr(not(windows), allow(dead_code))]
pub const CLOCK_PATCH: [u8; 5] = [0xB8, 0x12, 0xE8, 0xE0, 0x50];
#[cfg_attr(not(windows), allow(dead_code))]
const RETAIL_LOBBY: &[u8] = b"lobby01.ffxiv.com\0";

/// The bytes written into the lobby host slot.
pub fn host_slot(host: &str) -> Result<Vec<u8>> {
    if host.is_empty() || host.len() >= LOBBY_HOST_SLOT || !host.bytes().all(|b| b.is_ascii_graphic()) {
        bail!("the game can only reach a server name of up to {} characters (\"{host}\")", LOBBY_HOST_SLOT - 1);
    }
    let mut v = host.as_bytes().to_vec();
    v.push(0);
    Ok(v)
}

/// Whether `dir` looks like a playable install (the program and the right version).
pub fn installed(dir: &Path) -> bool {
    dir.join("ffxivgame.exe").is_file()
        && std::fs::read_to_string(dir.join("game.ver")).is_ok_and(|v| v.trim() == "2012.09.19.0001")
}

/// Whether this system's client is our own: the host plus a game module made on this machine.
/// Always on macOS; on Windows unless XIVLEGACY_STOCK=1 asks for the stock ffxivgame.exe.
pub fn uses_module() -> bool {
    cfg!(target_os = "macos") || (cfg!(windows) && std::env::var_os("XIVLEGACY_STOCK").is_none())
}

/// The host program: in the app bundle's Helpers on macOS, beside XIVLegacy.exe on Windows.
fn host_program() -> Result<std::path::PathBuf> {
    let exe = std::env::current_exe()?;
    let dir = exe.parent().map(Path::to_path_buf).unwrap_or_default();
    let p = if cfg!(target_os = "macos") {
        dir.parent().map(|contents| contents.join("Helpers/xivlegacy-host")).unwrap_or_default()
    } else {
        dir.join(if cfg!(windows) { "xivlegacy-host.exe" } else { "xivlegacy-host" })
    };
    if p.is_file() {
        return Ok(p);
    }
    if let Some(unpacked) = unpack_embedded_host()? {
        return Ok(unpacked);
    }
    bail!("this copy of XIVLegacy is missing its game host ({})", p.display())
}

mod embedded {
    include!(concat!(env!("OUT_DIR"), "/host_files.rs"));
}

/// Windows release builds carry the host inside XIVLegacy.exe: it is written once per version to
/// the settings folder (bin-<version>) and run from there.
fn unpack_embedded_host() -> Result<Option<std::path::PathBuf>> {
    if embedded::HOST_FILES.is_empty() {
        return Ok(None);
    }
    let dir = crate::config::dir().join(format!("bin-{}", env!("CARGO_PKG_VERSION")));
    std::fs::create_dir_all(&dir)?;
    for (name, bytes) in embedded::HOST_FILES {
        let p = dir.join(name);
        if p.metadata().map(|m| m.len()).ok() != Some(bytes.len() as u64) {
            std::fs::write(&p, bytes)?;
        }
    }
    Ok(Some(dir.join(embedded::HOST_FILES[0].0)))
}

/// Starts our own client: the host with the game module this machine made. It takes the game
/// folder and the session id, builds the game's launch argument itself, and sends the lobby to
/// XIVLEGACY_LOBBY. Reports a game that closes within 30 seconds.
pub fn launch_native(game_dir: &Path, host: &str, session_id: &str, module: &Path) -> Result<()> {
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};

    use anyhow::Context;

    crate::sqex::launch_arg(session_id, 0)?; // the same session id check as the stock path
    let helper = host_program()?;
    let support = crate::config::dir();
    let user = support.join("user");
    std::fs::create_dir_all(&user)?;
    let logs = if cfg!(target_os = "macos") {
        dirs::home_dir().unwrap_or_default().join("Library/Logs/XIVLegacy")
    } else {
        support.join("logs")
    };
    std::fs::create_dir_all(&logs)?;
    // the last run's log survives one more start, so a crash report isn't lost to a quick retry
    let _ = std::fs::rename(logs.join("game.log"), logs.join("game.prev.log"));
    let log = std::fs::File::create(logs.join("game.log"))?;
    let mut cmd = Command::new(&helper);
    cmd.arg("--module")
        .arg(module)
        .arg(game_dir)
        .arg(session_id)
        .current_dir(&support)
        .env("XIVLEGACY_USER", &user)
        .env("XIVLEGACY_LOBBY", host)
        .stdout(Stdio::from(log.try_clone()?))
        .stderr(Stdio::from(log));
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0800_0000); // CREATE_NO_WINDOW: no console behind the game
    }
    let mut child = cmd.spawn().with_context(|| format!("starting {}", helper.display()))?;
    crate::config::log(&format!("game started (pid {}) for {host}", child.id()));
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(30) {
        if let Some(status) = child.try_wait()? {
            crate::config::log(&format!("game closed early: {status}"));
            // kept apart from game.log, which the next start (often an immediate retry) replaces
            let crash = logs.join("game-crash.log");
            let shown = if std::fs::copy(logs.join("game.log"), &crash).is_ok() { crash } else { logs.join("game.log") };
            bail!("The game closed right after starting ({status}). Its log: {}", shown.display());
        }
        std::thread::sleep(Duration::from_millis(250));
    }
    Ok(())
}

#[cfg(windows)]
pub fn launch(game_dir: &Path, host: &str, session_id: &str, module: Option<&Path>) -> Result<()> {
    if let Some(m) = module {
        return launch_native(game_dir, host, session_id, m);
    }
    use std::ffi::c_void;
    use std::os::windows::ffi::OsStrExt;
    use std::ptr::null;

    use anyhow::Context;
    use windows_sys::Win32::Foundation::{CloseHandle, HANDLE};
    use windows_sys::Win32::System::Diagnostics::Debug::{
        ReadProcessMemory, WOW64_CONTEXT, WOW64_CONTEXT_INTEGER, Wow64GetThreadContext, WriteProcessMemory,
    };
    use windows_sys::Win32::System::Memory::{PAGE_READWRITE, VirtualProtectEx};
    use windows_sys::Win32::System::SystemInformation::GetTickCount;
    use windows_sys::Win32::System::Threading::{
        CREATE_SUSPENDED, CreateProcessW, GetExitCodeProcess, GetProcessAffinityMask, PROCESS_INFORMATION,
        ResumeThread, STARTUPINFOW, SetProcessAffinityMask, TerminateProcess, WaitForSingleObject,
    };
    use windows_sys::Win32::Foundation::WAIT_OBJECT_0;

    let slot = host_slot(host)?;
    let exe = game_dir.join("ffxivgame.exe");
    let arg = crate::sqex::launch_arg(session_id, unsafe { GetTickCount() })?;
    let wide = |s: &std::ffi::OsStr| s.encode_wide().chain([0]).collect::<Vec<u16>>();
    let mut cmd = wide(format!("\"{}\" {arg}", exe.display()).as_ref());
    let cwd = wide(game_dir.as_os_str());

    let mut si: STARTUPINFOW = unsafe { std::mem::zeroed() };
    si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
    let mut pi: PROCESS_INFORMATION = unsafe { std::mem::zeroed() };
    let ok = unsafe {
        CreateProcessW(null(), cmd.as_mut_ptr(), null(), null(), 0, CREATE_SUSPENDED, null(), cwd.as_ptr(), &si, &mut pi)
    };
    if ok == 0 {
        bail!("couldn't start {}: {}", exe.display(), std::io::Error::last_os_error());
    }

    let read = |h: HANDLE, at: u32, len: usize| -> Result<Vec<u8>> {
        let mut buf = vec![0u8; len];
        let mut got = 0usize;
        if unsafe { ReadProcessMemory(h, at as usize as *const c_void, buf.as_mut_ptr().cast(), len, &mut got) } == 0
            || got != len
        {
            bail!("reading the game's memory at {at:#x}: {}", std::io::Error::last_os_error());
        }
        Ok(buf)
    };
    let write = |h: HANDLE, at: u32, bytes: &[u8]| -> Result<()> {
        let addr = at as usize as *const c_void;
        let mut old = 0u32;
        if unsafe { VirtualProtectEx(h, addr, bytes.len(), PAGE_READWRITE, &mut old) } == 0 {
            bail!("unprotecting {at:#x}: {}", std::io::Error::last_os_error());
        }
        let mut done = 0usize;
        let r = unsafe { WriteProcessMemory(h, addr, bytes.as_ptr().cast(), bytes.len(), &mut done) };
        unsafe { VirtualProtectEx(h, addr, bytes.len(), old, &mut old) };
        if r == 0 || done != bytes.len() {
            bail!("writing the game's memory at {at:#x}: {}", std::io::Error::last_os_error());
        }
        Ok(())
    };

    let patched = (|| -> Result<()> {
        // The 32-bit PEB is in EBX of the suspended thread; the image base is at PEB+8.
        let mut ctx: WOW64_CONTEXT = unsafe { std::mem::zeroed() };
        ctx.ContextFlags = WOW64_CONTEXT_INTEGER;
        if unsafe { Wow64GetThreadContext(pi.hThread, &mut ctx) } == 0 {
            bail!("reading the game's start-up state: {}", std::io::Error::last_os_error());
        }
        let base_bytes = read(pi.hProcess, ctx.Ebx + 8, 4)?;
        let base = u32::from_le_bytes(base_bytes.try_into().unwrap());
        if read(pi.hProcess, base + LOBBY_HOST_RVA, RETAIL_LOBBY.len())? != RETAIL_LOBBY {
            bail!("this ffxivgame.exe isn't the 1.23b build (the lobby slot doesn't match)");
        }
        if read(pi.hProcess, base + CLOCK_PATCH_RVA, 1)? != [0xE8] {
            bail!("this ffxivgame.exe isn't the 1.23b build (the clock call doesn't match)");
        }
        let mut s = vec![0u8; LOBBY_HOST_SLOT];
        s[..slot.len()].copy_from_slice(&slot);
        write(pi.hProcess, base + LOBBY_HOST_RVA, &s)?;
        write(pi.hProcess, base + CLOCK_PATCH_RVA, &CLOCK_PATCH)?;

        let (mut mask, mut system) = (0usize, 0usize);
        if unsafe { GetProcessAffinityMask(pi.hProcess, &mut mask, &mut system) } != 0 && mask.count_ones() > 15 {
            let mut keep = 0usize;
            for bit in 0..usize::BITS {
                if mask & (1 << bit) != 0 && keep.count_ones() < 15 {
                    keep |= 1 << bit;
                }
            }
            unsafe { SetProcessAffinityMask(pi.hProcess, keep) };
        }
        Ok(())
    })()
    .context("preparing the game");

    unsafe {
        if patched.is_err() {
            TerminateProcess(pi.hProcess, 1);
        } else {
            ResumeThread(pi.hThread);
        }
        CloseHandle(pi.hThread);
    }
    if let Err(e) = patched {
        unsafe { CloseHandle(pi.hProcess) };
        return Err(e);
    }
    crate::config::log(&format!("game started (pid {}) for {host}", pi.dwProcessId));
    // A game that closes within its first half minute failed to start: say how.
    let early = unsafe { WaitForSingleObject(pi.hProcess, EARLY_EXIT_MS) } == WAIT_OBJECT_0;
    let mut code = 0u32;
    if early {
        unsafe { GetExitCodeProcess(pi.hProcess, &mut code) };
    }
    unsafe { CloseHandle(pi.hProcess) };
    if early {
        crate::config::log(&format!("game closed early, exit code {code:#010x}"));
        bail!("The game closed right after starting (code {code:#010X}: {}).", exit_meaning(code));
    }
    Ok(())
}

#[cfg(windows)]
const EARLY_EXIT_MS: u32 = 30_000;


/// What a Windows exit code from a game that closed at start-up means.
#[cfg_attr(not(windows), allow(dead_code))]
pub fn exit_meaning(code: u32) -> &'static str {
    match code {
        0 => "it exited on its own",
        0xC0000005 => "it crashed (access violation)",
        0xC0000135 => "a DLL it needs is missing",
        0xC0000142 => "a DLL it needs failed to start",
        0xC000007B => "a DLL it found is the wrong kind (32/64-bit)",
        0xC0000409 => "it stopped itself after a fatal error",
        0xE06D7363 => "an unhandled C++ error",
        0x80000003 => "it hit a breakpoint (an assertion failed)",
        0xC000001D => "an instruction this processor doesn't support",
        _ => "an unknown error",
    }
}

/// macOS: always our own client.
#[cfg(target_os = "macos")]
pub fn launch(game_dir: &Path, host: &str, session_id: &str, module: Option<&Path>) -> Result<()> {
    use anyhow::Context;
    launch_native(game_dir, host, session_id, module.context("no game module")?)
}

#[cfg(not(any(windows, target_os = "macos")))]
pub fn launch(_game_dir: &Path, host: &str, session_id: &str, _module: Option<&Path>) -> Result<()> {
    host_slot(host)?;
    crate::sqex::launch_arg(session_id, 0)?;
    bail!("Starting the game isn't supported on this system yet.")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_names() {
        assert_eq!(host_slot("xiv.ffxi.cc").unwrap(), b"xiv.ffxi.cc\0");
        assert!(host_slot("a-very-long-server-name.example").is_err());
        assert!(host_slot("").is_err());
    }
}
