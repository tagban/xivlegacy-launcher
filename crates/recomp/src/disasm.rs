//! Capstone, x86-32, as the Python pipeline sees it through the `capstone` module.
//!
//! capstone-sys 0.18 bundles capstone 5.0.6; the Python pipeline runs 5.0.7. Between the two only
//! SStream's overflow checks and skipdata copies changed (arch/X86 is byte-identical), so mnemonics,
//! operand strings and details are the same. One handle per thread: handles are not shared.
use capstone_sys as cs;
use std::ffi::CStr;

pub const OP_REG: u32 = cs::x86_op_type::X86_OP_REG as u32;
pub const OP_IMM: u32 = cs::x86_op_type::X86_OP_IMM as u32;
pub const OP_MEM: u32 = cs::x86_op_type::X86_OP_MEM as u32;
pub const REG_CS: u32 = cs::x86_reg::X86_REG_CS;
pub const REG_DS: u32 = cs::x86_reg::X86_REG_DS;
pub const REG_ES: u32 = cs::x86_reg::X86_REG_ES;
pub const REG_FS: u32 = cs::x86_reg::X86_REG_FS;
pub const REG_SS: u32 = cs::x86_reg::X86_REG_SS;

#[derive(Clone, Copy, Debug, Default)]
pub struct Mem {
    pub segment: u32,
    pub base: u32,
    pub index: u32,
    pub scale: i32,
    pub disp: i64,
}

/// One operand (capstone's cs_x86_op): `reg`, `imm` or `mem` by `ty`.
#[derive(Clone, Copy, Debug)]
pub struct Op {
    pub ty: u32,
    pub size: u8,
    pub reg: u32,
    pub imm: i64,
    pub mem: Mem,
}

/// A decoded instruction; `operands`, `disp_offset` and `imm_offset` only with detail on.
#[derive(Clone, Debug)]
pub struct Insn {
    pub address: u64,
    pub size: u16,
    pub bytes: Vec<u8>,
    pub mnemonic: String,
    pub op_str: String,
    pub operands: Vec<Op>,
    pub disp_offset: u8,
    pub imm_offset: u8,
}

/// (address, size, mnemonic, op_str): Python's disasm_lite.
pub struct Lite {
    pub address: u64,
    pub size: u16,
    pub mnemonic: String,
    pub op_str: String,
}

/// cs_reg_name for every x86 register id, shared by all threads (the table is fixed).
pub fn reg_name(r: u32) -> Option<&'static str> {
    static NAMES: std::sync::OnceLock<Vec<Option<String>>> = std::sync::OnceLock::new();
    NAMES.get_or_init(|| Disasm::new(false).reg_names().to_vec()).get(r as usize).and_then(|n| n.as_deref())
}

pub struct Disasm {
    handle: cs::csh,
    insn: *mut cs::cs_insn,
    detail: bool,
    reg_names: Vec<Option<String>>,
}

// A handle is used by one thread at a time; it may move between threads.
unsafe impl Send for Disasm {}

fn cstr(p: &[core::ffi::c_char]) -> String {
    // SAFETY: capstone NUL-terminates mnemonic and op_str inside their arrays.
    unsafe { CStr::from_ptr(p.as_ptr()) }.to_string_lossy().into_owned()
}

impl Disasm {
    pub fn new(detail: bool) -> Disasm {
        let mut handle: cs::csh = 0;
        // SAFETY: plain capstone C API calls on a handle we own.
        unsafe {
            let err = cs::cs_open(cs::cs_arch::CS_ARCH_X86, cs::cs_mode::CS_MODE_32, &mut handle);
            assert!(err == cs::cs_err::CS_ERR_OK, "cs_open failed: {}", err as u32);
            if detail {
                cs::cs_option(handle, cs::cs_opt_type::CS_OPT_DETAIL, cs::cs_opt_value::CS_OPT_ON as usize);
            }
            let insn = cs::cs_malloc(handle);
            let reg_names = (0..cs::x86_reg::X86_REG_ENDING)
                .map(|r| {
                    let p = cs::cs_reg_name(handle, r);
                    if p.is_null() { None } else { Some(CStr::from_ptr(p).to_string_lossy().into_owned()) }
                })
                .collect();
            Disasm { handle, insn, detail, reg_names }
        }
    }

    /// cs_reg_name, or None (Python's None) for an invalid id.
    pub fn reg_name(&self, r: u32) -> Option<&str> {
        self.reg_names.get(r as usize).and_then(|n| n.as_deref())
    }

    pub fn reg_names(&self) -> &[Option<String>] {
        &self.reg_names
    }

    /// Steps through `code` at `address` until it runs out or hits an invalid instruction, like
    /// Python's `md.disasm` (count 0 = no limit). `f` returns false to stop early.
    fn each(&mut self, code: &[u8], address: u64, count: usize, mut f: impl FnMut(&cs::cs_insn) -> bool) {
        let mut p = code.as_ptr();
        let mut n = code.len();
        let mut a = address;
        let mut k = 0;
        // SAFETY: the buffer outlives the loop; self.insn is capstone's own one-instruction cache.
        unsafe {
            while cs::cs_disasm_iter(self.handle, &mut p, &mut n, &mut a, self.insn) {
                k += 1;
                if !f(&*self.insn) || (count != 0 && k >= count) {
                    break;
                }
            }
        }
    }

    fn full(&self, i: &cs::cs_insn) -> Insn {
        let mut ins = Insn {
            address: i.address,
            size: i.size,
            bytes: i.bytes[..i.size as usize].to_vec(),
            mnemonic: cstr(&i.mnemonic),
            op_str: cstr(&i.op_str),
            operands: Vec::new(),
            disp_offset: 0,
            imm_offset: 0,
        };
        if self.detail && !i.detail.is_null() {
            // SAFETY: detail is valid with CS_OPT_DETAIL on; the x86 member is the live one.
            let x = unsafe { &(*i.detail).__bindgen_anon_1.x86 };
            ins.disp_offset = x.encoding.disp_offset;
            ins.imm_offset = x.encoding.imm_offset;
            for o in &x.operands[..x.op_count as usize] {
                let ty = o.type_ as u32;
                // SAFETY: the union member read is the one the operand type names.
                let (reg, imm, mem) = unsafe {
                    match ty {
                        OP_REG => (o.__bindgen_anon_1.reg, 0, Mem::default()),
                        OP_IMM => (0, o.__bindgen_anon_1.imm, Mem::default()),
                        OP_MEM => {
                            let m = o.__bindgen_anon_1.mem;
                            (0, 0, Mem { segment: m.segment, base: m.base, index: m.index,
                                         scale: m.scale, disp: m.disp })
                        }
                        _ => (0, 0, Mem::default()),
                    }
                };
                ins.operands.push(Op { ty, size: o.size, reg, imm, mem });
            }
        }
        ins
    }

    /// Python `list(md.disasm(code, address, count))`.
    pub fn disasm(&mut self, code: &[u8], address: u64, count: usize) -> Vec<Insn> {
        let mut out = Vec::new();
        let mut p = code.as_ptr();
        let mut n = code.len();
        let mut a = address;
        // SAFETY: as in `each`.
        unsafe {
            while cs::cs_disasm_iter(self.handle, &mut p, &mut n, &mut a, self.insn) {
                out.push(self.full(&*self.insn));
                if count != 0 && out.len() >= count {
                    break;
                }
            }
        }
        out
    }

    /// Python `md.disasm` consumed one instruction at a time: `f` sees each in full and returns
    /// false to stop.
    pub fn for_each(&mut self, code: &[u8], address: u64, mut f: impl FnMut(Insn) -> bool) {
        let mut p = code.as_ptr();
        let mut n = code.len();
        let mut a = address;
        // SAFETY: as in `each`.
        unsafe {
            while cs::cs_disasm_iter(self.handle, &mut p, &mut n, &mut a, self.insn) {
                if !f(self.full(&*self.insn)) {
                    break;
                }
            }
        }
    }

    /// Python `md.disasm_lite(code, address)`.
    pub fn lite(&mut self, code: &[u8], address: u64, mut f: impl FnMut(Lite)) {
        self.each(code, address, 0, |i| {
            f(Lite { address: i.address, size: i.size, mnemonic: cstr(&i.mnemonic), op_str: cstr(&i.op_str) });
            true
        });
    }

    /// disasm_lite consumed until `f` returns false.
    pub fn lite_while(&mut self, code: &[u8], address: u64, mut f: impl FnMut(Lite) -> bool) {
        self.each(code, address, 0, |i| f(Lite { address: i.address, size: i.size, mnemonic: cstr(&i.mnemonic), op_str: cstr(&i.op_str) }));
    }

    /// Addresses only (disasm_lite's first field), without building strings.
    pub fn addresses(&mut self, code: &[u8], address: u64, mut f: impl FnMut(u64)) {
        self.each(code, address, 0, |i| {
            f(i.address);
            true
        });
    }
}

impl Drop for Disasm {
    fn drop(&mut self) {
        // SAFETY: frees what new() allocated, once.
        unsafe {
            cs::cs_free(self.insn, 1);
            cs::cs_close(&mut self.handle);
        }
    }
}
