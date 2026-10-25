//! Windows: XIVLegacy shortcuts on the Desktop and in the Start menu that show FINAL FANTASY
//! XIV's icon. The icon stays in the player's own ffxivgame.exe (the shortcut points at it), so
//! XIVLegacy.exe carries none of Square Enix's artwork; pinning a shortcut to the taskbar keeps it.

use std::path::Path;

/// Make (or refresh) the shortcuts to this program with the icon of `game_dir`'s ffxivgame.exe.
/// Returns how many were written.
#[cfg(windows)]
pub fn make(game_dir: &Path) -> usize {
    use std::os::windows::process::CommandExt;
    let (Ok(exe), Some(desktop), Some(appdata)) = (std::env::current_exe(), dirs::desktop_dir(), dirs::config_dir()) else {
        return 0;
    };
    let icon = game_dir.join("ffxivgame.exe");
    if !icon.exists() {
        return 0;
    }
    let start = appdata.join(r"Microsoft\Windows\Start Menu\Programs");
    // PowerShell's single-quoted strings: a quote is doubled, nothing else is special
    let q = |p: &Path| format!("'{}'", p.display().to_string().replace('\'', "''"));
    let mut written = 0;
    for folder in [desktop, start] {
        let lnk = folder.join("XIVLegacy.lnk");
        let script = format!(
            "$s = (New-Object -ComObject WScript.Shell).CreateShortcut({lnk}); $s.TargetPath = {exe}; \
             $s.WorkingDirectory = {dir}; $s.IconLocation = {icon} + ',0'; \
             $s.Description = 'FINAL FANTASY XIV 1.23b on private servers'; $s.Save()",
            lnk = q(&lnk),
            exe = q(&exe),
            dir = q(exe.parent().unwrap_or(Path::new("."))),
            icon = q(&icon),
        );
        let ok = std::process::Command::new("powershell")
            .args(["-NoProfile", "-NonInteractive", "-Command", &script])
            .creation_flags(0x0800_0000) // CREATE_NO_WINDOW
            .status()
            .is_ok_and(|s| s.success());
        if ok {
            written += 1;
        } else {
            crate::config::log(&format!("couldn't write the shortcut {}", lnk.display()));
        }
    }
    written
}

#[cfg(not(windows))]
pub fn make(_game_dir: &Path) -> usize {
    0
}
