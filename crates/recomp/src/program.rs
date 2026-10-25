//! The image and its function map: the metadata's functions plus the
//! entries the metadata misses, found from relocations and direct branches.
use crate::disasm::Disasm;
use crate::pe::{py_slice, Pe};
use crate::Error;
use indexmap::IndexMap;
use serde_json::Value;
use std::collections::{HashMap, HashSet};

pub type Ranges = Vec<(u64, u64)>;

/// The C library's setjmp and longjmp (MSVC's _setjmp3 and longjmp), found by their code. A call to
/// _setjmp3 also leaves a host landing (RT_SETJMP); longjmp, which would end in a jump into the
/// middle of a translated function, is translated as a return to it (rt_longjmp, runtime.c).
const SETJMP3_CODE: [u8; 31] = [
    0x8b, 0x54, 0x24, 0x04, 0x89, 0x2a, 0x89, 0x5a, 0x04, 0x89, 0x7a, 0x08, 0x89, 0x72, 0x0c, 0x89, 0x62, 0x10, 0x8b, 0x04,
    0x24, 0x89, 0x42, 0x14, 0xc7, 0x42, 0x20, 0x30, 0x32, 0x43, 0x56,
];
/// ebx = jmp_buf; ebp; the SEH registration
const LONGJMP_HEAD: [u8; 16] = [0x8b, 0x5c, 0x24, 0x04, 0x8b, 0x2b, 0x8b, 0x73, 0x18, 0x64, 0x3b, 0x35, 0x00, 0x00, 0x00, 0x00];
/// mov esp, [edx+0x10]; add esp, 4; jmp [edx+0x14]
const LONGJMP_TAIL: [u8; 9] = [0x8b, 0x62, 0x10, 0x83, 0xc4, 0x04, 0xff, 0x62, 0x14];

pub struct Program {
    pub base: u64,
    pub image: Vec<u8>,
    /// translated function names: <prefix>XXXXXXXX (f_)
    pub prefix: String,
    /// entry -> ranges, in insertion order (Python dict order matters for add_branch_entries)
    pub functions: IndexMap<u64, Ranges>,
    pub entries: HashSet<u64>,
    pub switches: HashMap<u64, Vec<u64>>,
    /// address -> name (--hooks)
    pub hooks: HashMap<u64, String>,
    pub relocs: HashSet<u64>,
    /// entries that sit inside another function's body
    pub inner: HashSet<u64>,
    /// the entries that are the C library's _setjmp3, and its longjmp
    pub setjmps: HashSet<u64>,
    pub longjmps: HashSet<u64>,
    text: (u64, u64),
    tables: HashSet<u64>,
    boundaries: HashMap<u64, HashSet<u64>>,
}

/// A host's instructions sorted by address: their addresses (for bisect) and (address, imm_offset).
type Decoded = (Vec<u64>, Vec<(u64, u8)>);

/// One (lo, hi, entry) span of a function range, and the sorted starts for bisect.
struct Spans {
    spans: Vec<(u64, u64, u64)>,
    starts: Vec<u64>,
}

impl Spans {
    fn of(functions: &IndexMap<u64, Ranges>) -> Spans {
        let mut spans: Vec<(u64, u64, u64)> =
            functions.iter().flat_map(|(&e, r)| r.iter().map(move |&(lo, hi)| (lo, hi, e))).collect();
        spans.sort();
        let starts = spans.iter().map(|s| s.0).collect();
        Spans { spans, starts }
    }

    /// bisect_right(starts, a) - 1
    fn index(&self, a: u64) -> Option<usize> {
        self.starts.partition_point(|&s| s <= a).checked_sub(1)
    }

    fn span_of(&self, a: u64) -> Option<(u64, u64, u64)> {
        self.index(a).map(|i| self.spans[i]).filter(|s| s.0 <= a && a < s.1)
    }
}

fn int(v: &Value, what: &str) -> Result<u64, Error> {
    v.as_u64().ok_or_else(|| Error::Meta(format!("metadata: {} is not an integer", what)))
}

fn u32_at(image: &[u8], off: u64) -> Result<u32, Error> {
    let o = off as usize;
    image.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
        .ok_or_else(|| Error::Translate(format!("unpack_from requires a buffer of at least {} bytes", o + 4)))
}

fn u16_at(image: &[u8], off: u64) -> Result<u16, Error> {
    let o = off as usize;
    image.get(o..o + 2).map(|b| u16::from_le_bytes([b[0], b[1]]))
        .ok_or_else(|| Error::Translate(format!("unpack_from requires a buffer of at least {} bytes", o + 2)))
}

impl Program {
    pub fn new(meta: &Value, pe: &Pe, prefix: &str, hooks: HashMap<u64, String>) -> Result<Program, Error> {
        let base = pe.image_base as u64;
        let meta_base = int(meta.get("image_base").unwrap_or(&Value::Null), "image_base")?;
        if base != meta_base {
            return Err(Error::Meta(format!("image base {:#x} does not match metadata {:#x}", base, meta_base)));
        }
        let arr = |k: &str| meta.get(k).and_then(|v| v.as_array()).ok_or_else(|| Error::Meta(format!("metadata: no {}", k)));
        let mut functions: IndexMap<u64, Ranges> = IndexMap::new();
        for f in arr("functions")? {
            let entry = int(&f["entry"], "entry")?;
            let mut ranges = Vec::new();
            for r in f["ranges"].as_array().ok_or_else(|| Error::Meta("metadata: ranges".into()))? {
                ranges.push((int(&r[0], "range")?, int(&r[1], "range")?));
            }
            functions.insert(entry, ranges);
        }
        let mut switches = HashMap::new();
        let mut tables = HashSet::new();
        for s in arr("switches")? {
            if let Some(t) = s.get("table").and_then(|t| t.as_u64()) {
                tables.insert(t);
            }
            let targets: Vec<u64> = s["targets"].as_array().map(|a| a.iter().filter_map(|t| t.as_u64()).collect()).unwrap_or_default();
            if !targets.is_empty() {
                switches.insert(int(&s["at"], "at")?, targets);
            }
        }
        let text = arr("text")?;
        let text = (int(&text[0], "text")?, int(&text[1], "text")?);
        let mut p = Program {
            base, image: pe.memory_mapped_image(), prefix: prefix.to_string(), entries: functions.keys().copied().collect(),
            functions, switches, hooks, relocs: HashSet::new(), inner: HashSet::new(), setjmps: HashSet::new(),
            longjmps: HashSet::new(), text, tables, boundaries: HashMap::new(),
        };
        p.relocs = p.text_relocations(pe)?;
        let mut md = Disasm::new(true);
        let mut lite = Disasm::new(false);
        p.inner = p.add_data_entries(pe, &mut md, &mut lite)?;
        let more = p.add_branch_entries(&mut lite)?;
        p.inner.extend(more);
        p.boundaries.clear();
        let (setjmps, longjmps) = p.c_library_jumps();
        p.setjmps = setjmps;
        p.longjmps = longjmps;
        Ok(p)
    }

    /// The entries that are the C library's _setjmp3, and its longjmp.
    fn c_library_jumps(&self) -> (HashSet<u64>, HashSet<u64>) {
        let setjmps = self.entries.iter().copied().filter(|&e| self.read(e, SETJMP3_CODE.len() as u64) == SETJMP3_CODE).collect();
        let longjmps = self
            .entries
            .iter()
            .copied()
            .filter(|&e| {
                self.read(e, LONGJMP_HEAD.len() as u64) == LONGJMP_HEAD
                    && self.read(e, 0x80).windows(LONGJMP_TAIL.len()).any(|w| w == LONGJMP_TAIL)
            })
            .collect();
        (setjmps, longjmps)
    }

    pub fn read(&self, va: u64, n: u64) -> &[u8] {
        let off = va as i64 - self.base as i64;
        py_slice(&self.image, off, Some(off + n as i64))
    }

    fn in_text(&self, a: u64) -> bool {
        self.text.0 <= a && a < self.text.1
    }

    /// Code the image's data points at (vtables, callbacks, exception handlers) is called
    /// indirectly, so each such address must be an entry. The metadata misses some: Ghidra folds a
    /// small function into the one that tail-jumps to it (a vtable slot pointing into another
    /// function's range), and leaves a few out altogether. A folded one becomes an entry with its
    /// host's ranges (the translation starts at the entry, and every branch back into the host
    /// stays inside it); one outside every range, when it decodes as code up to a ret. Returns the entries that sit inside another
    /// function's body (the Windows loader must not put a 5-byte jmp there).
    fn add_data_entries(&mut self, pe: &Pe, md: &mut Disasm, lite: &mut Disasm) -> Result<HashSet<u64>, Error> {
        let spans = Spans::of(&self.functions);
        let mut inner = HashSet::new();
        let mut targets: Vec<u64> = Vec::new();
        for (_, entries) in pe.base_relocations() {
            for e in entries {
                if e.ty == 3 {
                    targets.push(u32_at(&self.image, e.rva)? as u64); // IMAGE_REL_BASED_HIGHLOW only
                }
            }
        }
        if targets.is_empty() {
            targets = self.table_pointers(pe)?;
        }
        {
            for target in targets {
                if !self.in_text(target) || self.functions.contains_key(&target) {
                    continue;
                }
                match spans.index(target).map(|i| spans.spans[i]) {
                    Some((lo, hi, host)) if lo <= target && target < hi => {
                        if !self.on_boundary(lite, host, target) {
                            continue; // inside one of the host's instructions: a table in .text, not code
                        }
                        let r = self.functions[&host].clone();
                        self.functions.insert(target, r);
                        inner.insert(target);
                    }
                    _ => {
                        let end = self.code_until_ret(lite, target);
                        if end != 0 {
                            self.functions.insert(target, vec![(target, end)]);
                        }
                    }
                }
            }
        }
        self.add_code_pointer_entries(&spans, md, lite)?;
        self.entries = self.functions.keys().copied().collect();
        Ok(inner)
    }

    /// A fixed-base image has no relocations to say which data holds code addresses: dwords in
    /// the sections other than .text that point into .text, when a neighbour does too (init
    /// tables, vtables, SEH scope tables, callback arrays; not ASCII that looks like an address).
    fn table_pointers(&self, pe: &Pe) -> Result<Vec<u64>, Error> {
        let mut out = Vec::new();
        for sec in &pe.sections {
            if sec.name == b".text" {
                continue;
            }
            let lo = sec.virtual_address as u64;
            let hi = lo + (sec.virtual_size.min(sec.size_of_raw_data.max(sec.virtual_size)) as u64);
            let hi = hi.min(self.image.len() as u64) & !3;
            let at = |o: u64| -> u64 { if o + 4 <= hi && o >= lo { u32_at(&self.image, o).unwrap_or(0) as u64 } else { 0 } };
            let mut o = lo;
            while o + 4 <= hi {
                let v = at(o);
                if self.in_text(v) && (self.in_text(at(o.wrapping_sub(4))) || self.in_text(at(o + 4))) {
                    out.push(v);
                }
                o += 4;
            }
        }
        out.sort();
        out.dedup();
        Ok(out)
    }

    /// The same for code that takes a function's address as an immediate - a callback stored
    /// into a structure, an exception handler pushed - when the function lies outside every range
    /// (a callback stored into a structure and called through it later). Those addresses are in .text's own relocations. Only immediates
    /// count: an address used as a displacement is a table read (switch tables, byte maps), and
    /// tables decode as code.
    fn add_code_pointer_entries(&mut self, spans: &Spans, md: &mut Disasm, lite: &mut Disasm) -> Result<(), Error> {
        let mut sites: Vec<u64> = self.relocs.iter().copied().collect();
        sites.sort();
        // host entry -> (instruction addresses, (address, imm_offset)) sorted by address
        let mut decoded: HashMap<u64, Decoded> = HashMap::new();
        for site in sites {
            let Some(host) = spans.span_of(site) else {
                continue; // a table in a gap, not an instruction
            };
            let target = u32_at(&self.image, site - self.base)? as u64;
            if !self.in_text(target) || self.functions.contains_key(&target) || self.tables.contains(&target)
                || spans.span_of(target).is_some()
            {
                continue;
            }
            let entry = host.2;
            if let std::collections::hash_map::Entry::Vacant(e) = decoded.entry(entry) {
                let mut ins: Vec<(u64, u8)> = Vec::new();
                for &(lo, hi) in &self.functions[&entry] {
                    md.for_each(self.read(lo, hi.saturating_sub(lo)), lo, |i| {
                        ins.push((i.address, i.imm_offset));
                        true
                    });
                }
                ins.sort_by_key(|i| i.0);
                e.insert((ins.iter().map(|i| i.0).collect(), ins));
            }
            let (addrs, ins) = &decoded[&entry];
            let Some(k) = addrs.partition_point(|&a| a <= site).checked_sub(1) else { continue };
            let (address, imm_offset) = ins[k];
            if imm_offset == 0 || address + imm_offset as u64 != site {
                continue; // a displacement: data
            }
            let end = self.code_until_ret(lite, target);
            if end != 0 {
                self.functions.insert(target, vec![(target, end)]);
            }
        }
        Ok(())
    }

    /// The same for direct branches: a call to an address that is not an entry, or a jump
    /// into another function's body. Each target on one of the host's instruction boundaries becomes an entry,
    /// so the branch is a call or tail call into its own translation; one inside an instruction
    /// comes from data decoded as code (a "jump" into the operand of a call) and is left.
    /// Returns the entries inside another function's body.
    fn add_branch_entries(&mut self, lite: &mut Disasm) -> Result<HashSet<u64>, Error> {
        let spans = Spans::of(&self.functions);
        let mut targets: IndexMap<u64, bool> = IndexMap::new();
        let mut bad: Option<String> = None;
        for (_, ranges) in &self.functions {
            for &(lo, hi) in ranges {
                lite.lite(self.read(lo, hi.saturating_sub(lo)), lo, |l| {
                    let mn = l.mnemonic.as_str();
                    if mn != "call" && !mn.starts_with('j') && !matches!(mn, "loop" | "loope" | "loopne") {
                        return;
                    }
                    let Some(h) = l.op_str.strip_prefix("0x") else { return };
                    let Ok(t) = u64::from_str_radix(h, 16) else {
                        bad.get_or_insert_with(|| format!("invalid literal for int() with base 16: '{}'", l.op_str));
                        return;
                    };
                    if self.functions.contains_key(&t) || !self.in_text(t) {
                        return;
                    }
                    if mn == "call" || !ranges.iter().any(|&(a, b)| a <= t && t < b) {
                        targets.entry(t).or_insert(mn == "call");
                    }
                });
            }
        }
        if let Some(b) = bad {
            return Err(Error::Translate(b));
        }
        let mut sorted: Vec<(u64, bool)> = targets.into_iter().collect();
        sorted.sort();
        let mut inner = HashSet::new();
        for (t, is_call) in sorted {
            let s = spans.span_of(t);
            if let Some((_, _, host)) = s {
                if host != t {
                    if !self.on_boundary(lite, host, t) {
                        continue; // inside one of the host's instructions: the branch is data decoded as code
                    }
                    let r = self.functions[&host].clone();
                    self.functions.insert(t, r);
                    inner.insert(t);
                }
            } else if is_call {
                let end = self.code_until_ret(lite, t);
                if end != 0 {
                    self.functions.insert(t, vec![(t, end)]);
                }
            }
        }
        self.entries = self.functions.keys().copied().collect();
        Ok(inner)
    }

    /// Whether a starts an instruction of host's ranges, decoded linearly.
    fn on_boundary(&mut self, lite: &mut Disasm, host: u64, a: u64) -> bool {
        if !self.boundaries.contains_key(&host) {
            let mut set = HashSet::new();
            for &(lo, hi) in &self.functions[&host] {
                lite.addresses(self.read(lo, hi.saturating_sub(lo)), lo, |x| {
                    set.insert(x);
                });
            }
            self.boundaries.insert(host, set);
        }
        self.boundaries[&host].contains(&a)
    }

    /// The end of straight-line code from va to its first ret, or 0 if it does not look like
    /// code (it runs out, or reads ports: data tables inside .text decode as in/ins).
    fn code_until_ret(&self, lite: &mut Disasm, va: u64) -> u64 {
        let mut end = 0;
        lite.lite_while(self.read(va, 0x1000), va, |l| {
            if matches!(l.mnemonic.as_str(), "in" | "out" | "insb" | "insd" | "outsb" | "outsd" | "hlt" | "cli" | "sti") {
                return false;
            }
            if l.mnemonic.starts_with("ret") {
                end = l.address + l.size as u64;
                return false;
            }
            true
        });
        end
    }

    /// Every location in .text that holds an absolute image address.
    ///
    /// The base-relocation blocks at the start of .reloc, up to the first block past .text or of
    /// size 0.
    fn text_relocations(&self, pe: &Pe) -> Result<HashSet<u64>, Error> {
        // A fixed-base image (ffxivgame.exe) has no .reloc: nothing moves, and every address
        // constant is used as is.
        let Ok(reloc) = pe.section(b".reloc") else { return Ok(HashSet::new()) };
        let text = pe.section(b".text")?;
        let text_end = text.virtual_address as u64 + text.virtual_size as u64;
        let mut off = reloc.virtual_address as u64;
        let mut out = HashSet::new();
        loop {
            let page = u32_at(&self.image, off)? as u64;
            let size = u32_at(&self.image, off + 4)? as u64;
            if size == 0 || page >= text_end {
                break;
            }
            for k in 0..size.saturating_sub(8) / 2 {
                let e = u16_at(&self.image, off + 8 + 2 * k)?;
                if e >> 12 == 3 {
                    // IMAGE_REL_BASED_HIGHLOW
                    out.insert(self.base + page + (e & 0xFFF) as u64);
                }
            }
            off += size;
        }
        Ok(out)
    }
}
