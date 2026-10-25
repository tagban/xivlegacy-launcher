//! The part of pefile the pipeline uses, with pefile's own rules (so the mapped image, section
//! data and relocation blocks come out the same): PE32 headers, sections sorted by address,
//! `get_data`, `get_memory_mapped_image` and the base relocation directory.
use crate::Error;

/// pefile.MAX_SECTIONS
const MAX_SECTIONS: usize = 0x800;

#[derive(Clone, Debug)]
pub struct Section {
    pub name: Vec<u8>, // NULs stripped from the right
    pub virtual_size: u32,
    pub virtual_address: u32,
    pub size_of_raw_data: u32,
    pub pointer_to_raw_data: u32,
    pub characteristics: u32,
    /// file offset of this section's header
    pub header_offset: usize,
    next_va: Option<u32>,
    va_adj: u64,
    ptr_adj: u64,
}

pub struct Pe {
    pub data: Vec<u8>,
    pub timestamp: u32,
    pub opt_offset: usize,
    pub entry: u32,
    pub image_base: u32,
    pub section_alignment: u32,
    pub file_alignment: u32,
    pub size_of_image: u32,
    pub dirs: Vec<(u32, u32)>, // DATA_DIRECTORY (VirtualAddress, Size)
    pub sections: Vec<Section>,
    header_len: usize,
}

/// Relocation block entry (pefile RelocationData): type and the rva it applies to.
pub struct Reloc {
    pub ty: u16,
    pub rva: u64,
}

fn u16_at(d: &[u8], o: usize) -> Option<u16> {
    d.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]))
}

fn u32_at(d: &[u8], o: usize) -> Option<u32> {
    d.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

/// Python's `data[start:end]` (negative indices count from the end, then clamped).
pub fn py_slice(d: &[u8], start: i64, end: Option<i64>) -> &[u8] {
    let n = d.len() as i64;
    let fix = |i: i64| if i < 0 { (i + n).max(0) } else { i.min(n) };
    let s = fix(start);
    let e = end.map(fix).unwrap_or(n);
    if s >= e { &[] } else { &d[s as usize..e as usize] }
}

/// pefile cache_adjust_SectionAlignment
fn adjust_section_alignment(val: u32, sa: u32, fa: u32) -> u64 {
    let sa = if sa < 0x1000 { fa } else { sa };
    if sa != 0 && !val.is_multiple_of(sa) {
        return sa as u64 * (val as f64 / sa as f64).trunc() as u64;
    }
    val as u64
}

impl Pe {
    pub fn parse(data: Vec<u8>) -> Result<Pe, Error> {
        let bad = |what: &str| Error::Pe(what.to_string());
        if data.get(0..2) != Some(b"MZ") {
            return Err(bad("DOS Header magic not found"));
        }
        let lfanew = u32_at(&data, 0x3c).ok_or_else(|| bad("truncated DOS header"))? as usize;
        if data.get(lfanew..lfanew + 4) != Some(b"PE\0\0") {
            return Err(bad("invalid NT Headers signature"));
        }
        let fh = lfanew + 4;
        let nsections = u16_at(&data, fh + 2).ok_or_else(|| bad("truncated file header"))? as usize;
        let timestamp = u32_at(&data, fh + 4).ok_or_else(|| bad("truncated file header"))?;
        let opt_size = u16_at(&data, fh + 16).ok_or_else(|| bad("truncated file header"))? as usize;
        let opt = fh + 20;
        if u16_at(&data, opt) != Some(0x10b) {
            return Err(bad("not a PE32 image"));
        }
        let field = |o: usize| u32_at(&data, opt + o).ok_or_else(|| bad("truncated optional header"));
        let entry = field(16)?;
        let image_base = field(28)?;
        let section_alignment = field(32)?;
        let file_alignment = field(36)?;
        let size_of_image = field(56)?;
        let nrva = field(92)? & 0x7FFF_FFFF;
        // at most 16 directories fit in a normally sized optional header (pefile stops there)
        let mut dirs = Vec::new();
        let mut off = opt + 96;
        for _ in 0..nrva {
            if data.len() == off {
                break;
            }
            let mut e = py_slice(&data, off as i64, Some(off as i64 + 8)).to_vec();
            e.resize(8, 0);
            dirs.push((u32::from_le_bytes([e[0], e[1], e[2], e[3]]), u32::from_le_bytes([e[4], e[5], e[6], e[7]])));
            off += 8;
            if off >= opt + 96 + 8 * 16 {
                break;
            }
        }
        let mut pe = Pe { data, timestamp, opt_offset: opt, entry, image_base, section_alignment, file_alignment,
                          size_of_image, dirs, sections: Vec::new(), header_len: 0 };
        let end = pe.parse_sections(opt + opt_size, nsections);
        let lowest = pe.sections.iter().filter(|s| s.pointer_to_raw_data > 0).map(|s| s.ptr_adj).min();
        pe.header_len = match lowest {
            Some(l) if l != 0 && l as usize >= end => l as usize,
            _ => end,
        }
        .min(pe.data.len());
        Ok(pe)
    }

    fn parse_sections(&mut self, offset: usize, n: usize) -> usize {
        let len = self.data.len() as u64;
        for i in 0..n {
            if i >= MAX_SECTIONS {
                break;
            }
            let so = offset + 40 * i;
            let raw = py_slice(&self.data, so as i64, Some(so as i64 + 40));
            if raw.len() == 40 && raw.iter().all(|&b| b == 0) {
                break; // "Contents are null-bytes"
            }
            if raw.len() < 40 {
                break;
            }
            let name: Vec<u8> = {
                let mut n = raw[..8].to_vec();
                while n.last() == Some(&0) {
                    n.pop();
                }
                n
            };
            let g = |o: usize| u32::from_le_bytes([raw[o], raw[o + 1], raw[o + 2], raw[o + 3]]);
            let (vs, va, srd, prd, ch) = (g(8), g(12), g(16), g(20), g(36));
            let mut errors = 0;
            if srd as u64 + prd as u64 > len {
                errors += 1;
            }
            if (prd & !0x1FF) as u64 > len {
                errors += 1;
            }
            if vs > 0x1000_0000 {
                errors += 1;
            }
            if adjust_section_alignment(va, self.section_alignment, self.file_alignment) > 0x1000_0000 {
                errors += 1;
            }
            if self.file_alignment != 0 && prd % self.file_alignment != 0 {
                errors += 1;
            }
            if errors >= 3 {
                break; // "Too many warnings parsing section. Aborting."
            }
            let mut ptr_adj = (prd & !0x1FF) as u64;
            if self.section_alignment < 0x1000 && prd == va {
                ptr_adj = va as u64;
            }
            self.sections.push(Section {
                name, virtual_size: vs, virtual_address: va, size_of_raw_data: srd, pointer_to_raw_data: prd,
                characteristics: ch, header_offset: so, next_va: None,
                va_adj: adjust_section_alignment(va, self.section_alignment, self.file_alignment), ptr_adj,
            });
        }
        self.sections.sort_by_key(|s| s.virtual_address);
        for k in 0..self.sections.len() {
            self.sections[k].next_va = self.sections.get(k + 1).map(|s| s.virtual_address);
        }
        if n > 0 && !self.sections.is_empty() { offset + 40 * n } else { offset }
    }

    /// The first section (in address order) with this name.
    pub fn section(&self, name: &[u8]) -> Result<&Section, Error> {
        self.sections.iter().find(|s| s.name == name)
            .ok_or_else(|| Error::Pe(format!("no {} section", String::from_utf8_lossy(name))))
    }

    /// SectionStructure.contains_rva
    fn contains(&self, s: &Section, rva: u64) -> bool {
        let mut size = if (self.data.len() as i64 - s.ptr_adj as i64) < s.size_of_raw_data as i64 {
            s.virtual_size as u64
        } else {
            s.size_of_raw_data.max(s.virtual_size) as u64
        };
        if let Some(next) = s.next_va {
            if next > s.virtual_address && s.va_adj + size > next as u64 {
                size = next as u64 - s.va_adj;
            }
        }
        s.va_adj <= rva && rva < s.va_adj + size
    }

    /// SectionStructure.get_data(start, length); start None = the section's raw data.
    pub fn section_data(&self, s: &Section, start: Option<u64>, length: Option<i64>) -> &[u8] {
        let offset = match start {
            None => s.ptr_adj as i64,
            Some(st) => st as i64 - s.va_adj as i64 + s.ptr_adj as i64,
        };
        let mut end = match length {
            Some(l) => offset + l,
            None => offset + s.size_of_raw_data as i64,
        };
        let limit = s.pointer_to_raw_data as i64 + s.size_of_raw_data as i64;
        if end > limit {
            end = limit;
        }
        py_slice(&self.data, offset, Some(end))
    }

    /// Overwrites the bytes at an RVA inside a section's raw data (an instruction patch before
    /// translation).
    pub fn patch(&mut self, rva: u64, bytes: &[u8]) -> Result<(), Error> {
        let s = self.sections.iter().find(|s| self.contains(s, rva))
            .ok_or_else(|| Error::Pe(format!("patch at RVA {:#x}: not in a section", rva)))?;
        let off = (rva - s.va_adj + s.ptr_adj as u64) as usize;
        let limit = s.pointer_to_raw_data as usize + s.size_of_raw_data as usize;
        if off + bytes.len() > limit || off + bytes.len() > self.data.len() {
            return Err(Error::Pe(format!("patch at RVA {:#x}: past the section's data", rva)));
        }
        self.data[off..off + bytes.len()].copy_from_slice(bytes);
        Ok(())
    }

    /// PE.get_data(rva, length)
    pub fn get_data(&self, rva: u64, length: Option<i64>) -> Result<&[u8], Error> {
        let end = length.map(|l| rva as i64 + l);
        if let Some(s) = self.sections.iter().find(|s| self.contains(s, rva)) {
            return Ok(self.section_data(s, Some(rva), length));
        }
        if (rva as usize) < self.header_len {
            return Ok(py_slice(&self.data[..self.header_len], rva as i64, end));
        }
        if (rva as usize) < self.data.len() {
            return Ok(py_slice(&self.data, rva as i64, end));
        }
        Err(Error::Pe("data at RVA can't be fetched. Corrupt header?".into()))
    }

    /// get_memory_mapped_image(): the header, then each section's raw data at its address.
    pub fn memory_mapped_image(&self) -> Vec<u8> {
        let len = self.data.len() as u64;
        let mut m = self.data[..self.header_len].to_vec();
        for s in &self.sections {
            if s.virtual_size == 0 && s.size_of_raw_data == 0 {
                continue;
            }
            let (srd, prd) = (s.size_of_raw_data as u64, s.ptr_adj);
            if srd > len || prd > len || srd + prd > len || s.va_adj >= 0x1000_0000 {
                continue;
            }
            let va = s.va_adj as usize;
            if va > m.len() {
                m.resize(va, 0);
            } else if va < m.len() {
                m.truncate(va);
            }
            m.extend_from_slice(self.section_data(s, None, None));
        }
        m
    }

    /// The base relocation directory's blocks (page rva, entries), with pefile's sanity stops.
    pub fn base_relocations(&self) -> Vec<(u32, Vec<Reloc>)> {
        let mut out = Vec::new();
        let Some(&(dir_rva, dir_size)) = self.dirs.get(5) else { return out };
        if dir_rva == 0 {
            return out;
        }
        let mut rva = dir_rva as i64;
        let end = dir_rva as i64 + dir_size as i64;
        while rva < end {
            let Ok(h) = self.get_data(rva as u64, Some(8)) else { break };
            if h.len() < 8 {
                break;
            }
            let page = u32::from_le_bytes([h[0], h[1], h[2], h[3]]);
            let size = u32::from_le_bytes([h[4], h[5], h[6], h[7]]);
            if page > self.size_of_image || size > self.size_of_image {
                break;
            }
            let mut entries = Vec::new();
            if let Ok(d) = self.get_data((rva + 8) as u64, Some(size as i64 - 8)) {
                let mut seen = std::collections::HashSet::new();
                for k in 0..d.len() / 2 {
                    let w = u16::from_le_bytes([d[2 * k], d[2 * k + 1]]);
                    if !seen.insert((w & 0xFFF, w >> 12)) {
                        break; // "Overlapping offsets in relocation data"
                    }
                    entries.push(Reloc { ty: w >> 12, rva: page as u64 + (w & 0xFFF) as u64 });
                }
            }
            out.push((page, entries));
            if size == 0 {
                break;
            }
            rva += size as i64;
        }
        out
    }
}
