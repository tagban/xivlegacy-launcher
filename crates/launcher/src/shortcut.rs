//! Windows: XIVLegacy shortcuts on the Desktop and in the Start menu that show FINAL FANTASY
//! XIV's icon. The icon stays in the player's own ffxivgame.exe (the shortcut points at it), so
//! XIVLegacy.exe carries none of Square Enix's artwork; pinning a shortcut to the taskbar keeps it.
//! Linux: an applications menu entry, its icon drawn on this computer from the same exe.

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

/// Linux: an entry in the applications menu (~/.local/share/applications/xivlegacy.desktop) with
/// the game's icon, drawn from the player's own ffxivgame.exe into the settings folder.
#[cfg(target_os = "linux")]
pub fn make(game_dir: &Path) -> usize {
    let (Ok(exe), Some(data)) = (std::env::current_exe(), dirs::data_dir()) else {
        return 0;
    };
    let mut entry = String::from(
        "[Desktop Entry]\nType=Application\nName=XIVLegacy\nComment=FINAL FANTASY XIV 1.23b on private servers\n\
         Categories=Game;\nStartupWMClass=xivlegacy\nTerminal=false\n",
    );
    // Exec quoting (the Desktop Entry spec): in double quotes, \ " ` $ take a backslash
    let quoted: String = exe.display().to_string().chars().flat_map(|c| {
        let esc = matches!(c, '\\' | '"' | '`' | '$');
        esc.then_some('\\').into_iter().chain(std::iter::once(c))
    }).collect();
    entry += &format!("Exec=\"{quoted}\"\n");
    if let Some((rgba, w, h)) = crate::icon::from_exe(&game_dir.join("ffxivgame.exe")) {
        let png = crate::config::dir().join("xivlegacy.png");
        let written = std::fs::File::create(&png).ok().is_some_and(|f| {
            let mut enc = png::Encoder::new(std::io::BufWriter::new(f), w, h);
            enc.set_color(png::ColorType::Rgba);
            enc.set_depth(png::BitDepth::Eight);
            enc.write_header().and_then(|mut wr| wr.write_image_data(&rgba)).is_ok()
        });
        if written {
            entry += &format!("Icon={}\n", png.display());
        }
    }
    let apps = data.join("applications");
    let path = apps.join("xivlegacy.desktop");
    if std::fs::create_dir_all(&apps).and_then(|_| std::fs::write(&path, entry)).is_ok() {
        1
    } else {
        crate::config::log(&format!("couldn't write {}", path.display()));
        0
    }
}

#[cfg(not(any(windows, target_os = "linux")))]
pub fn make(_game_dir: &Path) -> usize {
    0
}
