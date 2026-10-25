//! The FINAL FANTASY XIV icon for the launcher's window and taskbar, read from the player's own
//! ffxivgame.exe (its RT_ICON resources: a 256 px PNG and 48/32/16 px bitmaps). It is Square
//! Enix's artwork, so XIVLegacy never carries it; until the game is installed the window keeps the
//! default icon.

use std::path::Path;

/// (RGBA pixels, width, height) of the largest icon in `exe`.
pub fn from_exe(exe: &Path) -> Option<(Vec<u8>, u32, u32)> {
    let d = std::fs::read(exe).ok()?;
    let mut best: Option<(Vec<u8>, u32, u32)> = None;
    for data in icon_resources(&d)? {
        let decoded = if data.starts_with(b"\x89PNG") { decode_png(data) } else { decode_dib(data) };
        if let Some(icon) = decoded {
            if best.as_ref().is_none_or(|b| icon.1 > b.1) {
                best = Some(icon);
            }
        }
    }
    best
}

fn u16_at(d: &[u8], o: usize) -> Option<u32> {
    Some(u16::from_le_bytes(d.get(o..o + 2)?.try_into().ok()?) as u32)
}
fn u32_at(d: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(d.get(o..o + 4)?.try_into().ok()?))
}

/// The data of every RT_ICON resource in a PE file.
fn icon_resources(d: &[u8]) -> Option<Vec<&[u8]>> {
    let pe = u32_at(d, 0x3c)? as usize;
    let sections = u16_at(d, pe + 6)? as usize;
    let opt_size = u16_at(d, pe + 20)? as usize;
    let magic = u16_at(d, pe + 24)?;
    let dirs = pe + 24 + if magic == 0x10b { 96 } else { 112 };
    let res_rva = u32_at(d, dirs + 2 * 8)?;
    let off = |rva: u32| -> Option<usize> {
        (0..sections).find_map(|i| {
            let s = pe + 24 + opt_size + i * 40;
            let (vsize, va, rsize, raw) = (u32_at(d, s + 8)?, u32_at(d, s + 12)?, u32_at(d, s + 16)?, u32_at(d, s + 20)?);
            (va <= rva && rva < va + vsize.max(rsize)).then(|| (rva - va + raw) as usize)
        })
    };
    let base = off(res_rva)?;
    let entries = |o: usize| -> Vec<(u32, u32)> {
        let n = u16_at(d, o + 12).unwrap_or(0) + u16_at(d, o + 14).unwrap_or(0);
        (0..n as usize).filter_map(|i| Some((u32_at(d, o + 16 + 8 * i)?, u32_at(d, o + 20 + 8 * i)?))).collect()
    };
    let mut out = Vec::new();
    for (ty, ptr) in entries(base) {
        if ty != 3 {
            continue; // RT_ICON
        }
        for (_, p2) in entries(base + (ptr & 0x7fff_ffff) as usize) {
            for (_, p3) in entries(base + (p2 & 0x7fff_ffff) as usize) {
                let leaf = base + p3 as usize;
                let (rva, size) = (u32_at(d, leaf)?, u32_at(d, leaf + 4)? as usize);
                let o = off(rva)?;
                out.push(d.get(o..o + size)?);
            }
        }
    }
    Some(out)
}

fn decode_png(data: &[u8]) -> Option<(Vec<u8>, u32, u32)> {
    let mut dec = png::Decoder::new(std::io::Cursor::new(data));
    dec.set_transformations(png::Transformations::normalize_to_color8() | png::Transformations::ALPHA);
    let mut reader = dec.read_info().ok()?;
    let mut buf = vec![0; reader.output_buffer_size()];
    let info = reader.next_frame(&mut buf).ok()?;
    let rgba = match info.color_type {
        png::ColorType::Rgba => buf[..info.buffer_size()].to_vec(),
        png::ColorType::GrayscaleAlpha => buf[..info.buffer_size()].chunks(2).flat_map(|p| [p[0], p[0], p[0], p[1]]).collect(),
        _ => return None,
    };
    Some((rgba, info.width, info.height))
}

/// A 32-bit icon bitmap: a BITMAPINFOHEADER (height doubled for the mask), bottom-up BGRA rows.
fn decode_dib(data: &[u8]) -> Option<(Vec<u8>, u32, u32)> {
    let header = u32_at(data, 0)? as usize;
    let w = u32_at(data, 4)?;
    let h = u32_at(data, 8)? / 2;
    if u16_at(data, 14)? != 32 || w == 0 || h == 0 || w > 512 {
        return None;
    }
    let pixels = data.get(header..header + (w * h * 4) as usize)?;
    let mut rgba = vec![0u8; (w * h * 4) as usize];
    for y in 0..h as usize {
        let src = &pixels[(h as usize - 1 - y) * w as usize * 4..][..w as usize * 4];
        for (x, p) in src.chunks(4).enumerate() {
            rgba[(y * w as usize + x) * 4..][..4].copy_from_slice(&[p[2], p[1], p[0], p[3]]);
        }
    }
    Some((rgba, w, h))
}

#[cfg(test)]
mod tests {
    /// With an install at XIV_TEST_GAME: the 256 px icon decodes.
    #[test]
    #[ignore]
    fn reads_the_game_icon() {
        let dir = std::env::var("XIV_TEST_GAME").unwrap();
        let (rgba, w, h) = super::from_exe(&std::path::Path::new(&dir).join("ffxivgame.exe")).unwrap();
        println!("{w}x{h}");
        assert_eq!((rgba.len() as u32), w * h * 4);
        assert!(w >= 48);
    }
}
