//! DirectX audio for the game (Windows).
//!
//! ffxivgame.exe creates XAudio2 2.4 through COM, which exists only once
//! Microsoft's DirectX installer has registered it; without it the game closes
//! as soon as its window opens. Before starting the game, the launcher checks
//! for it and, if it's missing, runs DXSETUP silently (Windows asks the player
//! for permission once). The installer comes from the disc, kept beside the
//! game folder by setup, or, for installs made before that, Microsoft's own
//! June 2010 redistributable (downloaded, SHA-256 checked).

#[cfg(windows)]
pub use imp::ensure;

#[cfg(not(windows))]
pub fn ensure(_game_dir: &std::path::Path, _say: &dyn Fn(&str)) -> anyhow::Result<()> {
    Ok(())
}

#[cfg(windows)]
mod imp {
    use std::io::{Read, Write};
    use std::os::windows::ffi::OsStrExt;
    use std::path::{Path, PathBuf};
    use std::time::Duration;

    use anyhow::{Context, Result, bail};
    use sha2::{Digest, Sha256};
    use windows_sys::Win32::Foundation::{CloseHandle, ERROR_CANCELLED, GetLastError};
    use windows_sys::Win32::System::Registry::{
        HKEY, HKEY_CLASSES_ROOT, KEY_READ, KEY_WOW64_32KEY, RegCloseKey, RegOpenKeyExW,
    };
    use windows_sys::Win32::System::Threading::{GetExitCodeProcess, INFINITE, WaitForSingleObject};
    use windows_sys::Win32::UI::Shell::{SEE_MASK_NOCLOSEPROCESS, SHELLEXECUTEINFOW, ShellExecuteExW};
    use windows_sys::Win32::UI::WindowsAndMessaging::SW_HIDE;

    /// XAudio2 2.4 (DirectX March 2009), the version the game creates.
    const XAUDIO2_4: &str = r"CLSID\{03219E78-5BC3-44D1-B92E-F63D89CC6526}\InprocServer32";
    const REDIST_URL: &str =
        "https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/directx_Jun2010_redist.exe";
    const REDIST_SIZE: u64 = 100_275_120;
    const REDIST_SHA256: &str = "053f76dcbb28802e23341b6a787e3b0791c0fa5c8d4d011b1044172dbf89c73b";

    fn wide(s: &std::ffi::OsStr) -> Vec<u16> {
        s.encode_wide().chain([0]).collect()
    }

    /// Whether XAudio2 2.4 is registered for 32-bit programs.
    fn xaudio_ok() -> bool {
        let key = wide(XAUDIO2_4.as_ref());
        let mut h: HKEY = std::ptr::null_mut();
        let r = unsafe { RegOpenKeyExW(HKEY_CLASSES_ROOT, key.as_ptr(), 0, KEY_READ | KEY_WOW64_32KEY, &mut h) };
        if r == 0 {
            unsafe { RegCloseKey(h) };
        }
        r == 0
    }

    /// Runs `exe args`, elevated if `admin`, and waits; its exit code.
    fn run(exe: &Path, args: &str, admin: bool) -> Result<u32> {
        let verb = wide(if admin { "runas" } else { "open" }.as_ref());
        let file = wide(exe.as_os_str());
        let params = wide(args.as_ref());
        let dir = wide(exe.parent().unwrap_or(Path::new(".")).as_os_str());
        let mut info: SHELLEXECUTEINFOW = unsafe { std::mem::zeroed() };
        info.cbSize = std::mem::size_of::<SHELLEXECUTEINFOW>() as u32;
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = verb.as_ptr();
        info.lpFile = file.as_ptr();
        info.lpParameters = params.as_ptr();
        info.lpDirectory = dir.as_ptr();
        info.nShow = SW_HIDE;
        if unsafe { ShellExecuteExW(&mut info) } == 0 {
            if unsafe { GetLastError() } == ERROR_CANCELLED {
                bail!("DirectX wasn't installed because permission was declined. Join again and choose Yes when Windows asks.");
            }
            bail!("couldn't start {}: {}", exe.display(), std::io::Error::last_os_error());
        }
        let mut code = 1u32;
        unsafe {
            WaitForSingleObject(info.hProcess, INFINITE);
            GetExitCodeProcess(info.hProcess, &mut code);
            CloseHandle(info.hProcess);
        }
        Ok(code)
    }

    /// Microsoft's redistributable, downloaded and unpacked into `dir`.
    fn microsoft_installer(dir: &Path, say: &dyn Fn(&str)) -> Result<PathBuf> {
        std::fs::create_dir_all(dir)?;
        let exe = dir.join("directx_Jun2010_redist.exe");
        let ok = |p: &Path| -> Result<bool> {
            if p.metadata().map(|m| m.len()).ok() != Some(REDIST_SIZE) {
                return Ok(false);
            }
            let mut h = Sha256::new();
            std::io::copy(&mut std::fs::File::open(p)?, &mut h)?;
            Ok(hex::encode(h.finalize()) == REDIST_SHA256)
        };
        if !ok(&exe)? {
            say("Downloading DirectX from Microsoft (100 MB)…");
            let agent = ureq::AgentBuilder::new().timeout_read(Duration::from_secs(60)).build();
            let mut body = agent.get(REDIST_URL).call().context("downloading DirectX")?.into_reader();
            let mut out = std::fs::File::create(&exe)?;
            let mut buf = vec![0u8; 1 << 20];
            loop {
                let n = body.read(&mut buf)?;
                if n == 0 {
                    break;
                }
                out.write_all(&buf[..n])?;
            }
            drop(out);
            if !ok(&exe)? {
                let _ = std::fs::remove_file(&exe);
                bail!("the DirectX download from Microsoft didn't match its expected checksum; try again");
            }
        }
        let unpacked = dir.join("files");
        if !unpacked.join("DXSETUP.exe").is_file() {
            say("Unpacking DirectX…");
            run(&exe, &format!("/Q /T:\"{}\"", unpacked.display()), false)?;
        }
        Ok(unpacked.join("DXSETUP.exe"))
    }

    pub fn ensure(game_dir: &Path, say: &dyn Fn(&str)) -> Result<()> {
        if xaudio_ok() {
            return Ok(());
        }
        let disc = xivlegacy_install::directx::installer_dir(game_dir).join("DXSETUP.exe");
        let setup = if disc.is_file() {
            disc
        } else {
            microsoft_installer(&game_dir.with_file_name("DirectX (Microsoft)"), say)?
        };
        say("Installing DirectX audio (Windows will ask for permission)…");
        let code = run(&setup, "/silent", true)?;
        if !xaudio_ok() {
            bail!("DirectX setup finished (code {code}) but XAudio2 still isn't available; install Microsoft's DirectX End-User Runtime by hand");
        }
        Ok(())
    }
}
