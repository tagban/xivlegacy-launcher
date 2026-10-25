//! x86-32 -> C translation of one guest function (recomp/x86c.py).
//!
//! Model (see runtime/guest.h):
//!   * registers and flags are C locals, loaded from Guest at entry and stored around calls/returns;
//!   * guest memory is accessed through rd8/16/32/64 and wr8/16/32/64 on 32-bit guest addresses;
//!   * every instruction that is a branch target gets a label; jumps are gotos;
//!   * direct calls push the return address on the guest stack and call the callee's C function,
//!     `ret` pops and returns; a jump to another function's entry is a tail call;
//!   * switches (`jmp [reg*4+table]`) read the table from guest memory and dispatch with a C switch
//!     over the targets the metadata lists;
//!   * x87 instructions are decoded from their opcode bytes (not the mnemonic: disassemblers
//!     disagree on FSUBP/FSUBRP naming) and run on doubles with the control word's RC and PC.
//!
//! Anything not handled becomes RT_UNIMPL(addr, "text"), which traps at run time; the recompiler
//! counts them so coverage can be measured over the whole binary.
//!
//! The emitted text must stay byte-identical to x86c.py's: the code follows it branch for branch.
use crate::disasm::{self, Disasm, Insn, Op, OP_IMM, OP_MEM, OP_REG, REG_CS, REG_DS, REG_ES, REG_FS, REG_SS};
use crate::program::Program;
use std::collections::HashSet;

#[path = "x86c_sse.rs"]
mod sse;

/// Why an instruction was not translated. Unsup is x86c.py's Unsupported (becomes RT_UNIMPL);
/// Fatal is what would crash the Python (a KeyError or a bad unpack): the whole run stops.
enum Tx {
    Unsup(String),
    Fatal(String),
}

type R<T> = Result<T, Tx>;

fn unsup<T>(why: impl Into<String>) -> R<T> {
    Err(Tx::Unsup(why.into()))
}

const R32: [&str; 8] = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"];

fn r16(n: &str) -> Option<&'static str> {
    Some(match n {
        "ax" => "eax", "cx" => "ecx", "dx" => "edx", "bx" => "ebx",
        "sp" => "esp", "bp" => "ebp", "si" => "esi", "di" => "edi",
        _ => return None,
    })
}

fn r8l(n: &str) -> Option<&'static str> {
    Some(match n { "al" => "eax", "cl" => "ecx", "dl" => "edx", "bl" => "ebx", _ => return None })
}

fn r8h(n: &str) -> Option<&'static str> {
    Some(match n { "ah" => "eax", "ch" => "ecx", "dh" => "edx", "bh" => "ebx", _ => return None })
}

fn ut(bits: u32) -> R<&'static str> {
    Ok(match bits {
        8 => "uint8_t", 16 => "uint16_t", 32 => "uint32_t", 64 => "uint64_t",
        _ => return Err(Tx::Fatal(format!("KeyError: {} (unsigned type)", bits))),
    })
}

fn st(bits: u32) -> R<&'static str> {
    Ok(match bits {
        8 => "int8_t", 16 => "int16_t", 32 => "int32_t", 64 => "int64_t",
        _ => return Err(Tx::Fatal(format!("KeyError: {} (signed type)", bits))),
    })
}

/// Condition codes: the C test of each x86 condition suffix.
fn cc(s: &str) -> Option<&'static str> {
    Some(match s {
        "o" => "of", "no" => "!of", "b" => "cf", "ae" => "!cf", "e" => "zf", "ne" => "!zf",
        "be" => "(cf || zf)", "a" => "(!cf && !zf)", "s" => "sf", "ns" => "!sf", "p" => "pf", "np" => "!pf",
        "l" => "(sf != of)", "ge" => "(sf == of)", "le" => "(zf || sf != of)", "g" => "(!zf && sf == of)",
        _ => return None,
    })
}

fn mask(bits: u32) -> u64 {
    if bits >= 64 { u64::MAX } else { (1u64 << bits) - 1 }
}

fn hexu(v: u64, bits: u32) -> String {
    format!("0x{:X}u", v & mask(bits))
}

fn hex32(v: u64) -> String {
    hexu(v, 32)
}

fn bits_of(op: &Op) -> u32 {
    op.size as u32 * 8
}

// Emitted x87 statements that take the whole x87 state from g (runtime/guest.h): in a function with
// the stack in locals, g is brought up to date before them and read back after.
const X87_WHOLE: [&str; 6] = ["fenv_load(", "fenv_store(", "fsave(", "frstor(", "fxam(", "g->top"];

/// re.sub(r'\bST\(([0-7])\)', r'x87_s\1', s)
fn sub_st(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = String::with_capacity(s.len());
    let mut i = 0;
    let mut last = 0;
    while i + 5 <= b.len() {
        if &b[i..i + 3] == b"ST(" && (b'0'..=b'7').contains(&b[i + 3]) && b[i + 4] == b')'
            && (i == 0 || !(b[i - 1].is_ascii_alphanumeric() || b[i - 1] == b'_'))
        {
            out.push_str(&s[last..i]);
            out.push_str("x87_s");
            out.push(b[i + 3] as char);
            i += 5;
            last = i;
        } else {
            i += 1;
        }
    }
    out.push_str(&s[last..]);
    out
}

/// A function's text with its x87 stack in locals (guest.h, "x87 in locals") instead of g->st.
fn x87_locals(lines: Vec<String>) -> Vec<String> {
    let mut out = Vec::with_capacity(lines.len());
    for line in lines {
        let code = line.trim_start_matches(char::is_whitespace); // str.lstrip()
        if !code.starts_with("/*") && X87_WHOLE.iter().any(|k| code.contains(k)) {
            let indent = &line[..line.len() - code.len()];
            out.push(format!("{}X87_STORE; {} X87_LOAD;", indent, code));
            continue;
        }
        let mut s = line.replace("REGS_DECL;", "REGS_DECL; X87_DECL;").replace("REGS_LOAD;", "REGS_LOAD; X87_LOAD;");
        s = s.replace("REGS_STORE;", "REGS_STORE; X87_STORE;");
        s = s.replace("REGS_LOAD_NF;", "REGS_LOAD_NF; X87_LOAD;").replace("REGS_STORE_NF;", "REGS_STORE_NF; X87_STORE;");
        s = sub_st(&s);
        s = s.replace("fpush(g, ", "X87_PUSH(").replace("fpop(g)", "X87_POP()").replace("fr(g, ", "fr_cw(x87_cw, ");
        for f in ["frnd", "fist16", "fist32", "fist64"] {
            s = s.replace(&format!("{}(g, ", f), &format!("{}_cw(x87_cw, ", f));
        }
        s = s.replace("fstsw(g)", "fstsw_top(g, x87_top)").replace("g->fcw", "x87_cw");
        out.push(s);
    }
    out
}

/// The XMM registers a function uses, as C locals (xmm_N) rather than g->xmm[N]: loaded where the
/// general registers are (REGS_LOAD: entry, after calls) and stored where they are (REGS_STORE:
/// before calls, jumps out, returns). In memory, every guest store (-fno-strict-aliasing) could
/// alias them, and each SSE instruction went through memory.
fn xmm_locals(lines: Vec<String>) -> Vec<String> {
    let mut used = [false; 8];
    for line in &lines {
        let mut rest = line.as_str();
        while let Some(i) = rest.find("g->xmm[") {
            let tail = &rest[i + 7..];
            if let Some(d) = tail.chars().next().and_then(|c| c.to_digit(10)) {
                if d < 8 && tail[1..].starts_with(']') {
                    used[d as usize] = true;
                }
            }
            rest = &rest[i + 7..];
        }
    }
    if !used.iter().any(|&u| u) {
        return lines;
    }
    let regs: Vec<usize> = (0..8).filter(|&n| used[n]).collect();
    let decl = format!("X128 {};", regs.iter().map(|n| format!("xmm_{}", n)).collect::<Vec<_>>().join(", "));
    let load = regs.iter().map(|n| format!("xmm_{} = g->xmm[{}];", n, n)).collect::<Vec<_>>().join(" ");
    let store = regs.iter().map(|n| format!("g->xmm[{}] = xmm_{};", n, n)).collect::<Vec<_>>().join(" ");
    let mut out = Vec::with_capacity(lines.len());
    for line in lines {
        let mut s = line;
        for n in &regs {
            s = s.replace(&format!("g->xmm[{}]", n), &format!("xmm_{}", n));
        }
        s = s.replace("REGS_DECL;", &format!("REGS_DECL; {}", decl));
        s = s.replace("REGS_LOAD;", &format!("REGS_LOAD; {}", load));
        s = s.replace("REGS_STORE;", &format!("REGS_STORE; {}", store));
        s = s.replace("REGS_LOAD_NF;", &format!("REGS_LOAD_NF; {}", load));
        s = s.replace("REGS_STORE_NF;", &format!("REGS_STORE_NF; {}", store));
        out.push(s);
    }
    out
}

/// What one translation produces besides its text.
pub struct Translated {
    pub lines: Vec<String>,
    /// (address, mnemonic, why)
    pub unimpl: Vec<(u64, String, String)>,
    /// entries reached by direct call or tail jump (for --functions closures)
    pub referenced: Vec<u64>,
    /// hook addresses this function placed
    pub hooked: Vec<u64>,
}

pub struct FunctionTranslator<'a> {
    ctx: &'a Program,
    md: &'a mut Disasm,
    entry: u64,
    ranges: &'a [(u64, u64)],
    insns: Vec<Insn>,
    addrs: HashSet<u64>,
    labels: HashSet<u64>,
    unimpl: Vec<(u64, String, String)>,
    referenced: Vec<u64>,
    hooked: Vec<u64>,
    uses_x87: bool, // then the function keeps the x87 stack in locals (x87_locals)
}

enum Item {
    Label(usize), // index into insns
    Line(String),
}

impl<'a> FunctionTranslator<'a> {
    pub fn new(ctx: &'a Program, md: &'a mut Disasm, entry: u64, ranges: &'a [(u64, u64)]) -> Self {
        FunctionTranslator { ctx, md, entry, ranges, insns: Vec::new(), addrs: HashSet::new(), labels: HashSet::new(),
                             unimpl: Vec::new(), referenced: Vec::new(), hooked: Vec::new(), uses_x87: false }
    }

    // ------------------------------------------------------------------ decoding

    fn decode(&mut self) {
        for &(lo, hi) in self.ranges {
            let code = self.ctx.read(lo, hi.saturating_sub(lo));
            let found = self.md.disasm(code, lo, 0);
            self.insns.extend(found);
        }
        self.addrs = self.insns.iter().map(|i| i.address).collect();
        // The metadata's ranges come from Ghidra, which stops a body where another function begins.
        // Code this function jumps to beyond its ranges (not another function's entry) is part of
        // it: decode it by following control flow, so the translation never depends on those cuts
        // (e.g. 0x10320813 jumps to 0x10320917, past a function Ghidra started inside its body).
        let mut work: Vec<u64> = Vec::new();
        for i in &self.insns {
            self.jump_targets(i, &mut work);
        }
        while let Some(mut pc) = work.pop() {
            while !self.addrs.contains(&pc) && !self.ctx.entries.contains(&pc) {
                let code = self.ctx.read(pc, 16);
                let Some(ins) = self.md.disasm(code, pc, 1).into_iter().next() else { break };
                self.addrs.insert(pc);
                self.jump_targets(&ins, &mut work);
                let stop = ins.mnemonic == "ret" || ins.mnemonic == "jmp" || ins.mnemonic == "int3" || ins.mnemonic.starts_with("ret");
                let next = ins.address + ins.size as u64;
                self.insns.push(ins);
                if stop {
                    break;
                }
                pc = next;
            }
        }
        self.insns.sort_by_key(|i| i.address); // stable, as list.sort
    }

    /// Direct intra-procedural control transfers out of an instruction (not calls).
    fn jump_targets(&self, ins: &Insn, out: &mut Vec<u64>) {
        let mn = ins.mnemonic.as_str();
        if mn == "jmp" || (mn.starts_with('j') && cc(&mn[1..]).is_some()) || matches!(mn, "jecxz" | "loop" | "loope" | "loopne") {
            if let Some(op) = ins.operands.first().filter(|o| o.ty == OP_IMM) {
                out.push(op.imm as u64 & 0xFFFF_FFFF);
            } else if mn == "jmp" {
                if let Some(t) = self.ctx.switches.get(&ins.address) {
                    out.extend_from_slice(t);
                }
            }
        }
    }

    fn inside(&self, a: u64) -> bool {
        self.addrs.contains(&a)
    }

    // ------------------------------------------------------------------ operands

    fn reg(&self, r: u32) -> R<(&'static str, char, u32)> {
        let name = disasm::reg_name(r);
        if let Some(n) = name {
            if let Some(&b) = R32.iter().find(|&&x| x == n) {
                return Ok((b, 'd', 32));
            }
            if let Some(b) = r16(n) {
                return Ok((b, 'w', 16));
            }
            if let Some(b) = r8l(n) {
                return Ok((b, 'l', 8));
            }
            if let Some(b) = r8h(n) {
                return Ok((b, 'h', 8));
            }
        }
        unsup(format!("register {}", name.unwrap_or("None")))
    }

    fn addr(&self, ins: &Insn, op: &Op, lea: bool) -> R<String> {
        let m = &op.mem;
        let mut parts: Vec<String> = Vec::new();
        if m.base != 0 {
            let (b, _, bits) = self.reg(m.base)?;
            if bits != 32 {
                return unsup("16-bit addressing");
            }
            parts.push(b.to_string());
        }
        if m.index != 0 {
            let (i, _, _) = self.reg(m.index)?;
            parts.push(if m.scale != 1 { format!("{} * {}", i, m.scale) } else { i.to_string() });
        }
        if m.disp != 0 {
            parts.push(hex32(m.disp as u64) + if self.reloc_disp(ins) { " + RD" } else { "" });
        }
        if !lea && m.segment == REG_FS {
            parts.push("g->fs_base".into());
        } else if !lea && ![0, REG_DS, REG_ES, REG_SS, REG_CS].contains(&m.segment) {
            return unsup(format!("segment {}", disasm::reg_name(m.segment).unwrap_or("None")));
        }
        Ok(format!("(uint32_t)({})", if parts.is_empty() { "0u".to_string() } else { parts.join(" + ") }))
    }

    /// The displacement is an absolute image address (relocated at load time).
    fn reloc_disp(&self, ins: &Insn) -> bool {
        ins.disp_offset > 0 && self.ctx.relocs.contains(&(ins.address + ins.disp_offset as u64))
    }

    /// The immediate is an absolute image address (push offset f, mov eax, offset g, cmp r, offset h).
    fn reloc_imm(&self, ins: &Insn) -> bool {
        ins.imm_offset > 0 && self.ctx.relocs.contains(&(ins.address + ins.imm_offset as u64))
    }

    fn rd(&self, ins: &Insn, op: &Op, bits: Option<u32>) -> R<String> {
        let bits = match bits { Some(b) if b != 0 => b, _ => bits_of(op) };
        match op.ty {
            OP_REG => {
                let (b, kind, _) = self.reg(op.reg)?;
                Ok(match kind {
                    'd' => b.to_string(),
                    'w' => format!("(uint16_t){}", b),
                    'l' => format!("(uint8_t){}", b),
                    _ => format!("(uint8_t)({} >> 8)", b),
                })
            }
            OP_IMM => Ok(if self.reloc_imm(ins) {
                format!("({} + RD)", hexu(op.imm as u64, bits))
            } else {
                hexu(op.imm as u64, bits)
            }),
            OP_MEM => Ok(format!("rd{}(A)", bits)),
            t => unsup(format!("operand type {}", t)),
        }
    }

    fn wr(&self, op: &Op, expr: &str) -> R<String> {
        let bits = bits_of(op);
        match op.ty {
            OP_REG => {
                let (b, kind, _) = self.reg(op.reg)?;
                Ok(match kind {
                    'd' => format!("{} = (uint32_t)({});", b, expr),
                    'w' => format!("{} = ({} & 0xFFFF0000u) | (uint16_t)({});", b, b, expr),
                    'l' => format!("{} = ({} & 0xFFFFFF00u) | (uint8_t)({});", b, b, expr),
                    _ => format!("{} = ({} & 0xFFFF00FFu) | ((uint32_t)(uint8_t)({}) << 8);", b, b, expr),
                })
            }
            OP_MEM => Ok(format!("wr{}(A, ({})({}));", bits, ut(bits)?, expr)),
            t => unsup(format!("write to operand type {}", t)),
        }
    }

    fn mem_prologue(&self, ins: &Insn) -> R<Vec<String>> {
        for op in &ins.operands {
            if op.ty == OP_MEM {
                return Ok(vec![format!("uint32_t A = {};", self.addr(ins, op, false)?)]);
            }
        }
        Ok(Vec::new())
    }

    // ------------------------------------------------------------------ flags

    fn szp(r: &str, bits: u32) -> R<Vec<String>> {
        Ok(vec![format!("zf = ({}){} == 0;", ut(bits)?, r), format!("sf = (({}) >> {}) & 1u;", r, bits - 1),
                format!("pf = PARITY({});", r)])
    }

    fn flags_add(a: &str, b: &str, r: &str, bits: u32, carry: Option<&str>) -> R<Vec<String>> {
        let mut out = Vec::new();
        match carry {
            None => out.push(format!("cf = ({}){} < ({}){};", ut(bits)?, r, ut(bits)?, a)),
            Some(c) => out.push(format!("cf = (unsigned)((((uint64_t){} + (uint64_t){} + {}) >> {}) & 1u);", a, b, c, bits)),
        }
        out.push(format!("of = (((({}) ^ ({})) & (({}) ^ ({}))) >> {}) & 1u;", a, r, b, r, bits - 1));
        out.push(format!("af = ((({}) ^ ({}) ^ ({})) >> 4) & 1u;", a, b, r));
        out.extend(Self::szp(r, bits)?);
        Ok(out)
    }

    fn flags_sub(a: &str, b: &str, r: &str, bits: u32, borrow: Option<&str>) -> R<Vec<String>> {
        let mut out = Vec::new();
        match borrow {
            None => out.push(format!("cf = ({}){} < ({}){};", ut(bits)?, a, ut(bits)?, b)),
            Some(c) => out.push(format!("cf = (uint64_t){} < (uint64_t){} + {};", a, b, c)),
        }
        out.push(format!("of = (((({}) ^ ({})) & (({}) ^ ({}))) >> {}) & 1u;", a, b, a, r, bits - 1));
        out.push(format!("af = ((({}) ^ ({}) ^ ({})) >> 4) & 1u;", a, b, r));
        out.extend(Self::szp(r, bits)?);
        Ok(out)
    }

    fn flags_logic(r: &str, bits: u32) -> R<Vec<String>> {
        let mut out = vec!["cf = 0; of = 0; af = 0;".to_string()];
        out.extend(Self::szp(r, bits)?);
        Ok(out)
    }

    // ------------------------------------------------------------------ control flow helpers

    /// C statement(s) transferring control to a guest address from inside this function.
    fn goto(&mut self, target: u64, from_addr: Option<u64>) -> String {
        if self.inside(target) {
            self.labels.insert(target);
            if let Some(f) = from_addr {
                if target <= f {
                    // A backward jump closes a loop: a safepoint, so a guest thread spinning on a
                    // variable still lets the others run under the guest lock.
                    return format!("{{ RT_SAFEPOINT; goto L_{:08x}; }}", target);
                }
            }
            return format!("goto L_{:08x};", target);
        }
        if self.ctx.entries.contains(&target) {
            self.referenced.push(target);
            return format!("{{ REGS_STORE; {}{:08x}(g); return; }}", self.ctx.prefix, target);
        }
        format!("RT_BADJUMP({});", hex32(target))
    }

    fn call(&mut self, target: u64, ret_addr: u64) -> Vec<String> {
        self.referenced.push(target);
        let push = format!("esp -= 4; wr32(esp, {} + RD);", hex32(ret_addr));
        if self.ctx.setjmps.contains(&target) {
            // _setjmp3: a host landing here too, for longjmp to come back to (guest.h)
            return vec![push, format!("REGS_STORE_NF; if (!RT_SETJMP(g)) {}{:08x}(g); REGS_LOAD_NF;", self.ctx.prefix, target)];
        }
        vec![push, format!("REGS_STORE_NF; {}{:08x}(g); REGS_LOAD_NF;", self.ctx.prefix, target)]
    }

    // ------------------------------------------------------------------ translation

    pub fn translate(mut self) -> Result<Translated, crate::Error> {
        if self.ctx.longjmps.contains(&self.entry) {
            // the C library's longjmp: back to the landing its _setjmp3 call left (guest.h)
            let lines = vec![format!("void {}{:08x}(Guest* g)", self.ctx.prefix, self.entry), "{".into(), "    rt_longjmp(g);".into(), "}".into()];
            return Ok(Translated { lines, unimpl: Vec::new(), referenced: Vec::new(), hooked: Vec::new() });
        }
        self.decode();
        let insns = std::mem::take(&mut self.insns);
        let mut body: Vec<Item> = Vec::with_capacity(insns.len() * 6);
        let mut prev_falls_to: Option<u64> = None;
        for (k, ins) in insns.iter().enumerate() {
            if let Some(p) = prev_falls_to {
                if p != ins.address {
                    body.push(Item::Line(format!("    {}", self.goto(p, None))));
                }
            }
            body.push(Item::Label(k));
            let (stmts, falls) = match self.one(ins) {
                Ok(r) => r,
                Err(Tx::Unsup(why)) => {
                    let text = format!("{} {}", ins.mnemonic, ins.op_str);
                    self.unimpl.push((ins.address, ins.mnemonic.clone(), why));
                    (vec![format!("RT_UNIMPL({}, \"{}\");", hex32(ins.address), text.replace('"', "'"))], false)
                }
                Err(Tx::Fatal(why)) => {
                    return Err(crate::Error::Translate(format!("{:#x} ({} {}) in {}{:08x}: {}", ins.address, ins.mnemonic,
                                                               ins.op_str, self.ctx.prefix, self.entry, why)));
                }
            };
            let ctx = self.ctx;
            if let Some(hook) = ctx.hooks.get(&ins.address) {
                if !hook.is_empty() {
                    // A host hook point (--hooks): the host sees and may change the guest's
                    // state here, before this instruction runs; unset, it costs one test.
                    self.hooked.push(ins.address);
                    body.push(Item::Line(format!("    if (rt_hook_{}) {{ REGS_STORE; rt_hook_{}(g); REGS_LOAD; }}", hook, hook)));
                }
            }
            body.push(Item::Line(format!("    /* {:08x}: {} {} */", ins.address, ins.mnemonic, ins.op_str)));
            if !stmts.is_empty() {
                // each instruction in its own block, so its temporaries (A, t, ...) are local to it
                body.push(Item::Line("    {".into()));
                for s in stmts {
                    body.push(Item::Line(format!("        {}", s)));
                }
                body.push(Item::Line("    }".into()));
            }
            prev_falls_to = if falls { Some(ins.address + ins.size as u64) } else { None };
        }
        if let Some(p) = prev_falls_to {
            body.push(Item::Line(format!("    {}", self.goto(p, None))));
        }

        let mut out = vec![format!("void {}{:08x}(Guest* g)", self.ctx.prefix, self.entry), "{".into(),
                           "    REGS_DECL;".into(), "    REGS_LOAD;".into()];
        if let Some(first) = insns.first() {
            if first.address != self.entry {
                out.push(format!("    goto L_{:08x};", self.entry));
                self.labels.insert(self.entry);
            }
        }
        for item in body {
            match item {
                Item::Label(k) => {
                    let a = insns[k].address;
                    if self.labels.contains(&a) {
                        out.push(format!("L_{:08x}: ;", a));
                    }
                }
                Item::Line(s) => out.push(s),
            }
        }
        out.push("}".into());
        let lines = if self.uses_x87 { x87_locals(out) } else { out };
        let lines = xmm_locals(lines);
        Ok(Translated { lines, unimpl: self.unimpl, referenced: self.referenced, hooked: self.hooked })
    }

    /// (C statements, falls through)
    fn one(&mut self, ins: &Insn) -> R<(Vec<String>, bool)> {
        let full = ins.mnemonic.as_str();
        let (mut prefix, mn) = match full.split_once(' ') {
            Some((p, m)) => (Some(p), m),
            None => (None, full),
        };
        if prefix == Some("lock") {
            prefix = None; // one guest thread runs at a time (guest lock), so lock is implicit
        }
        let ops = &ins.operands;
        let nxt = ins.address + ins.size as u64;

        // x87 by opcode
        if let Some(opc) = x87_opcode(ins) {
            self.uses_x87 = true;
            return Ok((self.x87(ins, opc)?, true));
        }

        if matches!(mn, "nop" | "wait" | "fwait" | "pause") {
            return Ok((vec![], true));
        }
        if mn == "int3" {
            return Ok((vec![format!("RT_TRAP({});", hex32(ins.address))], false));
        }

        // string instructions
        if mn.len() == 5 && matches!(&mn[..4], "movs" | "stos" | "lods" | "scas" | "cmps") && "bwd".contains(&mn[4..])
            && ops.iter().all(|o| o.ty != OP_REG || self.reg(o.reg).is_ok())
        {
            return Ok((self.string(mn, prefix)?, true));
        }
        // SSE / SSE2 / MMX (x86c_sse.rs)
        if let Some(v) = self.sse(ins, mn)? {
            return Ok((v, true));
        }
        // `rep ret` (an AMD branch-predictor idiom) is a plain ret.
        if mn == "ret" && matches!(prefix, Some("rep" | "repz")) {
            prefix = None;
        }
        if mn == "hlt" {
            // Privileged; only reachable from code that checks for ring 0 first (OpenSSL).
            return Ok((vec![format!("RT_TRAP({});", hex32(ins.address))], false));
        }
        if let Some(p) = prefix {
            return unsup(format!("prefix {}", p));
        }

        let mut pro = self.mem_prologue(ins)?;

        macro_rules! with_pro {
            ($v:expr) => {{
                let v: Vec<String> = $v;
                pro.extend(v);
                return Ok((pro, true));
            }};
        }

        // mov r/m16, sreg: the selector values 32-bit Windows user code sees (the CRT copies them
        // into CONTEXT records for crash reports).
        if mn == "mov" {
            if let [d, s] = &ops[..] {
                let sel = match (s.ty == OP_REG).then(|| disasm::reg_name(s.reg)).flatten() {
                    Some("cs") => Some("0x1Bu"),
                    Some("ss" | "ds" | "es") => Some("0x23u"),
                    Some("fs") => Some("0x3Bu"),
                    Some("gs") => Some("0u"),
                    _ => None,
                };
                if let Some(v) = sel {
                    with_pro!(vec![self.wr(d, v)?]);
                }
            }
        }

        match mn {
            "mov" | "movzx" | "movsx" => {
                let (d, s) = ops2(ops)?;
                let (sb, db) = (bits_of(s), bits_of(d));
                let mut v = self.rd(ins, s, None)?;
                if mn == "movsx" {
                    v = format!("({})({})(({}){})", ut(db)?, st(db)?, st(sb)?, v);
                }
                with_pro!(vec![self.wr(d, &v)?]);
            }
            "lea" => {
                let (d, s) = ops2(ops)?;
                let a = self.addr(ins, s, true)?;
                return Ok((vec![self.wr(d, &a)?], true));
            }
            "xchg" => {
                let (a, b) = ops2(ops)?;
                let l1 = format!("{{ {} t = {};", ut(bits_of(a))?, self.rd(ins, a, None)?);
                let l2 = self.wr(a, &self.rd(ins, b, None)?)?;
                let l3 = self.wr(b, "t")? + " }";
                with_pro!(vec![l1, l2, l3]);
            }
            "add" | "adc" | "sub" | "sbb" | "cmp" | "and" | "or" | "xor" | "test" => {
                let (d, s) = ops2(ops)?;
                let bits = bits_of(d);
                let t = ut(bits)?;
                let mut v = vec!["{".to_string(),
                                 format!("{} a = {}; {} b = ({}){};", t, self.rd(ins, d, None)?, t, t, self.rd(ins, s, Some(bits))?)];
                if mn == "adc" || mn == "sbb" {
                    v.push("unsigned c = cf;".into());
                }
                let expr = match mn {
                    "add" => "a + b", "adc" => "a + b + c", "sub" => "a - b", "sbb" => "a - b - c", "cmp" => "a - b",
                    "and" => "a & b", "or" => "a | b", "xor" => "a ^ b", _ => "a & b",
                };
                v.push(format!("{} r = ({})({});", t, t, expr));
                match mn {
                    "add" | "adc" => v.extend(Self::flags_add("a", "b", "r", bits, if mn == "adc" { Some("c") } else { None })?),
                    "sub" | "sbb" | "cmp" => v.extend(Self::flags_sub("a", "b", "r", bits, if mn == "sbb" { Some("c") } else { None })?),
                    _ => v.extend(Self::flags_logic("r", bits)?),
                }
                if mn != "cmp" && mn != "test" {
                    v.push(self.wr(d, "r")?);
                }
                v.push("}".into());
                with_pro!(v);
            }
            "inc" | "dec" => {
                let d = ops1(ops)?;
                let bits = bits_of(d);
                let t = ut(bits)?;
                let sign = 1u64 << (bits - 1);
                let mut v = if mn == "inc" {
                    vec![format!("{{ {} a = {}; {} r = ({})(a + 1u);", t, self.rd(ins, d, None)?, t, t),
                         format!("of = r == {}; af = (r & 0xFu) == 0;", hexu(sign, bits))]
                } else {
                    vec![format!("{{ {} a = {}; {} r = ({})(a - 1u);", t, self.rd(ins, d, None)?, t, t),
                         format!("of = a == {}; af = (r & 0xFu) == 0xFu;", hexu(sign, bits))]
                };
                v.extend(Self::szp("r", bits)?);
                v.push(self.wr(d, "r")? + " }");
                with_pro!(v);
            }
            "neg" => {
                let d = ops1(ops)?;
                let bits = bits_of(d);
                let t = ut(bits)?;
                let mut v = vec![format!("{{ {} a = {}; {} r = ({})(0u - a);", t, self.rd(ins, d, None)?, t, t),
                                 format!("cf = a != 0; of = a == {}; af = (a & 0xFu) != 0;", hexu(1u64 << (bits - 1), bits))];
                v.extend(Self::szp("r", bits)?);
                v.push(self.wr(d, "r")? + " }");
                with_pro!(v);
            }
            "not" => {
                let d = ops1(ops)?;
                let e = format!("~{}", self.rd(ins, d, None)?);
                with_pro!(vec![self.wr(d, &e)?]);
            }
            "shl" | "sal" | "shr" | "sar" | "rol" | "ror" => with_pro!(self.shift(ins, mn)?),
            "rcl" | "rcr" => {
                let d = ops.first().ok_or_else(|| Tx::Fatal("IndexError: operands".into()))?;
                let bits = bits_of(d);
                let t = ut(bits)?;
                let cnt = if ops.len() > 1 { self.rd(ins, &ops[1], Some(8))? } else { "1u".to_string() };
                let (step, ofx) = if mn == "rcr" {
                    (format!("unsigned o = r & 1u; r = ({})((r >> 1) | ((uint32_t)cf << {})); cf = o;", t, bits - 1),
                     format!("of = ((r >> {}) ^ (r >> {})) & 1u;", bits - 1, bits - 2))
                } else {
                    (format!("unsigned o = (r >> {}) & 1u; r = ({})((r << 1) | cf); cf = o;", bits - 1, t),
                     format!("of = ((r >> {}) & 1u) ^ cf;", bits - 1))
                };
                with_pro!(vec![format!("{{ unsigned c = ((unsigned)({}) & 31u) % {}u; {} r = {}; unsigned i;", cnt, bits + 1, t,
                                       self.rd(ins, d, None)?),
                               format!("for (i = 0; i < c; i++) {{ {} }}", step), format!("if (c) {{ {} }}", ofx),
                               self.wr(d, "r")? + " }"]);
            }
            "shld" | "shrd" => with_pro!(self.shiftd(ins, mn)?),
            "mul" | "imul" | "div" | "idiv" => with_pro!(self.muldiv(ins, mn)?),
            "cdq" => return Ok((vec!["edx = (uint32_t)((int32_t)eax >> 31);".into()], true)),
            "cwde" => return Ok((vec!["eax = (uint32_t)(int32_t)(int16_t)eax;".into()], true)),
            "cbw" => return Ok((vec!["eax = (eax & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)eax;".into()], true)),
            "cwd" => return Ok((vec!["edx = (edx & 0xFFFF0000u) | ((eax & 0x8000u) ? 0xFFFFu : 0u);".into()], true)),
            "push" => {
                let s = ops1(ops)?;
                if bits_of(s) != 32 && s.ty != OP_IMM {
                    return unsup("16-bit push");
                }
                with_pro!(vec![format!("{{ uint32_t v = {}; esp -= 4; wr32(esp, v); }}", self.rd(ins, s, Some(32))?)]);
            }
            "pop" => {
                let d = ops1(ops)?;
                if bits_of(d) != 32 {
                    return unsup("16-bit pop");
                }
                if d.ty == OP_MEM {
                    // the address is computed with esp already incremented
                    return Ok((vec![format!("{{ uint32_t v = rd32(esp); esp += 4; uint32_t A = {}; wr32(A, v); }}",
                                            self.addr(ins, d, false)?)], true));
                }
                return Ok((vec![format!("{{ uint32_t v = rd32(esp); esp += 4; {} }}", self.wr(d, "v")?)], true));
            }
            "pushal" => return Ok((vec!["{ uint32_t s = esp; esp -= 32; wr32(esp + 28, eax); wr32(esp + 24, ecx); wr32(esp + 20, edx); \
                wr32(esp + 16, ebx); wr32(esp + 12, s); wr32(esp + 8, ebp); wr32(esp + 4, esi); wr32(esp, edi); }".into()], true)),
            "popal" => return Ok((vec!["edi = rd32(esp); esi = rd32(esp + 4); ebp = rd32(esp + 8); ebx = rd32(esp + 16); \
                edx = rd32(esp + 20); ecx = rd32(esp + 24); eax = rd32(esp + 28); esp += 32;".into()], true)),
            "pushfd" => return Ok((vec!["{ uint32_t f = cf | 2u | (pf << 2) | (af << 4) | (zf << 6) | (sf << 7) | (df << 10) | (of << 11); \
                esp -= 4; wr32(esp, f); }".into()], true)),
            "popfd" => return Ok((vec!["{ uint32_t f = rd32(esp); esp += 4; cf = f & 1u; pf = (f >> 2) & 1u; af = (f >> 4) & 1u; \
                zf = (f >> 6) & 1u; sf = (f >> 7) & 1u; df = (f >> 10) & 1u; of = (f >> 11) & 1u; }".into()], true)),
            "lahf" => return Ok((vec!["eax = (eax & 0xFFFF00FFu) | ((sf << 15) | (zf << 14) | (af << 12) | (pf << 10) | (1u << 9) | (cf << 8));".into()], true)),
            "sahf" => return Ok((vec!["{ uint32_t h = (eax >> 8) & 0xFFu; cf = h & 1u; pf = (h >> 2) & 1u; af = (h >> 4) & 1u; \
                zf = (h >> 6) & 1u; sf = (h >> 7) & 1u; }".into()], true)),
            "leave" => return Ok((vec!["esp = ebp; ebp = rd32(esp); esp += 4;".into()], true)),
            "clc" => return Ok((vec!["cf = 0;".into()], true)),
            "stc" => return Ok((vec!["cf = 1;".into()], true)),
            "cmc" => return Ok((vec!["cf ^= 1u;".into()], true)),
            "cld" => return Ok((vec!["df = 0;".into()], true)),
            "std" => return Ok((vec!["df = 1;".into()], true)),
            _ => {}
        }
        if let Some(c) = mn.strip_prefix("set").and_then(cc) {
            let d = ops1(ops)?;
            with_pro!(vec![self.wr(d, &format!("({}) ? 1u : 0u", c))?]);
        }
        if let Some(c) = mn.strip_prefix("cmov").and_then(cc) {
            let (d, s) = ops2(ops)?;
            let v = self.rd(ins, s, None)?;
            with_pro!(vec![format!("if ({}) {{ {} }}", c, self.wr(d, &v)?)]);
        }
        match mn {
            "bt" | "bts" | "btr" | "btc" => with_pro!(self.bittest(ins, mn)?),
            "bsf" | "bsr" => {
                let (d, s) = ops2(ops)?;
                if bits_of(d) != 32 {
                    return unsup("16-bit bsf/bsr");
                }
                let lp = if mn == "bsf" { "for (i = 0; !((v >> i) & 1u); i++) ;" } else { "for (i = 31; !((v >> i) & 1u); i--) ;" };
                with_pro!(vec![format!("{{ uint32_t v = {}; zf = v == 0; if (v) {{ unsigned i; {} {} }} }}",
                                       self.rd(ins, s, None)?, lp, self.wr(d, "i")?)]);
            }
            "bswap" => {
                let d = ops1(ops)?;
                let b = self.rd(ins, d, None)?;
                return Ok((vec![format!("{} = ({} >> 24) | (({} >> 8) & 0xFF00u) | (({} << 8) & 0xFF0000u) | ({} << 24);", b, b, b, b, b)], true));
            }
            "xadd" => {
                let (d, s) = ops2(ops)?;
                let bits = bits_of(d);
                let t = ut(bits)?;
                let mut v = vec![format!("{{ {} a = {}; {} b = {}; {} r = ({})(a + b);", t, self.rd(ins, d, None)?, t,
                                         self.rd(ins, s, None)?, t, t)];
                v.extend(Self::flags_add("a", "b", "r", bits, None)?);
                v.push(self.wr(s, "a")?);
                v.push(self.wr(d, "r")? + " }");
                with_pro!(v);
            }
            "cmpxchg" => {
                let (d, s) = ops2(ops)?;
                let bits = bits_of(d);
                let t = ut(bits)?;
                let (acc, acc_w) = match bits {
                    8 => ("(uint8_t)eax", "eax = (eax & 0xFFFFFF00u) | (uint8_t)v;"),
                    16 => ("(uint16_t)eax", "eax = (eax & 0xFFFF0000u) | (uint16_t)v;"),
                    32 => ("eax", "eax = v;"),
                    _ => return Err(Tx::Fatal(format!("KeyError: {} (cmpxchg)", bits))),
                };
                let mut v = vec![format!("{{ {} a = {}; {} v = {}; {} r = ({})(a - v);", t, acc, t, self.rd(ins, d, None)?, t, t)];
                v.extend(Self::flags_sub("a", "v", "r", bits, None)?);
                let w = self.wr(d, &self.rd(ins, s, None)?)?;
                v.push(format!("if (zf) {{ {} }} else {{ {} }} }}", w, acc_w));
                with_pro!(v);
            }
            "xlatb" => return Ok((vec!["eax = (eax & 0xFFFFFF00u) | rd8(ebx + (eax & 0xFFu));".into()], true)),
            "cpuid" => return Ok((vec!["REGS_STORE; rt_cpuid(g); REGS_LOAD;".into()], true)),
            "rdtsc" => return Ok((vec!["REGS_STORE; rt_rdtsc(g); REGS_LOAD;".into()], true)),
            _ => {}
        }

        // control flow
        if mn == "jmp" {
            let t = ops1(ops)?;
            if t.ty == OP_IMM {
                return Ok((vec![self.goto(t.imm as u64 & 0xFFFF_FFFF, Some(ins.address))], false));
            }
            let ctx = self.ctx;
            if let Some(sw) = ctx.switches.get(&ins.address).filter(|_| t.ty == OP_MEM) {
                let mut v = pro;
                v.push("switch (rd32(A) - RD) {".into());
                for &tgt in sw {
                    let g = self.goto(tgt, Some(ins.address));
                    v.push(format!("    case {}: {}", hex32(tgt), g));
                }
                v.push(format!("    default: RT_BADJUMP({});", hex32(ins.address)));
                v.push("}".into());
                return Ok((v, false));
            }
            pro.push(format!("{{ uint32_t t = {}; REGS_STORE; rt_call_indirect(g, t); return; }}", self.rd(ins, t, Some(32))?));
            return Ok((pro, false));
        }
        if let Some(c) = mn.strip_prefix('j').and_then(cc) {
            let t = ops1(ops)?;
            let g = self.goto(t.imm as u64 & 0xFFFF_FFFF, Some(ins.address));
            return Ok((vec![format!("if ({}) {}", c, g)], true));
        }
        if mn == "jecxz" {
            let t = ops1(ops)?;
            let g = self.goto(t.imm as u64 & 0xFFFF_FFFF, Some(ins.address));
            return Ok((vec![format!("if (ecx == 0) {}", g)], true));
        }
        if matches!(mn, "loop" | "loope" | "loopne") {
            let t = ops1(ops)?;
            let cond = match mn { "loop" => "ecx != 0", "loope" => "ecx != 0 && zf", _ => "ecx != 0 && !zf" };
            let g = self.goto(t.imm as u64 & 0xFFFF_FFFF, Some(ins.address));
            return Ok((vec!["ecx--;".into(), format!("if ({}) {}", cond, g)], true));
        }
        if mn == "call" {
            let t = ops1(ops)?;
            if t.ty == OP_IMM {
                let tgt = t.imm as u64 & 0xFFFF_FFFF;
                if tgt == nxt {
                    // call $+5 ; pop r  (get EIP)
                    return Ok((vec![format!("esp -= 4; wr32(esp, {} + RD);", hex32(nxt))], true));
                }
                if self.ctx.entries.contains(&tgt) {
                    return Ok((self.call(tgt, nxt), true));
                }
                // No translated function there (code outside .text, say): the runtime decides,
                // through its native handler, as for a call through a pointer.
                return Ok((vec![format!("{{ esp -= 4; wr32(esp, {} + RD); REGS_STORE_NF; rt_call_indirect(g, {}); REGS_LOAD_NF; }}",
                                        hex32(nxt), hex32(tgt))], true));
            }
            pro.push(format!("{{ uint32_t t = {}; esp -= 4; wr32(esp, {} + RD); REGS_STORE_NF; rt_call_indirect(g, t); REGS_LOAD_NF; }}",
                             self.rd(ins, t, Some(32))?, hex32(nxt)));
            return Ok((pro, true));
        }
        if mn == "ret" {
            let n = ops.first().map(|o| o.imm).unwrap_or(0);
            return Ok((vec![format!("esp += {}; REGS_STORE; return;", 4 + n)], false));
        }

        unsup(mn)
    }

    // ------------------------------------------------------------------ groups

    fn shift(&self, ins: &Insn, mn: &str) -> R<Vec<String>> {
        let d = ins.operands.first().ok_or_else(|| Tx::Fatal("IndexError: operands".into()))?;
        let bits = bits_of(d);
        let (t, s) = (ut(bits)?, st(bits)?);
        let cnt = if ins.operands.len() > 1 { self.rd(ins, &ins.operands[1], Some(8))? } else { "1u".into() };
        let mut v = vec![format!("{{ unsigned c = (unsigned)({}) & 31u; if (c) {{ {} a = {}; {} r;", cnt, t, self.rd(ins, d, None)?, t)];
        match mn {
            "shl" | "sal" => {
                v.push(format!("r = ({})((uint32_t)a << c);", t));
                v.push(format!("cf = c <= {} ? ((uint32_t)a >> ({} - c)) & 1u : 0u;", bits, bits));
                v.push(format!("of = ((r >> {}) & 1u) ^ cf;", bits - 1));
                v.extend(Self::szp("r", bits)?);
            }
            "shr" => {
                v.push(format!("r = ({})((uint32_t)a >> c);", t));
                v.push("cf = ((uint32_t)a >> (c - 1)) & 1u;".into());
                v.push(format!("of = (a >> {}) & 1u;", bits - 1));
                v.extend(Self::szp("r", bits)?);
            }
            "sar" => {
                v.push(format!("{{ unsigned k = c < {}u ? c : {}u; r = ({})((int32_t)({})a >> k);", bits, bits - 1, t, s));
                v.push(format!("cf = (unsigned)(((int32_t)({})a >> (k - (c < {}u ? 1u : 0u))) & 1); }}", s, bits));
                v.push("of = 0;".into());
                v.extend(Self::szp("r", bits)?);
            }
            "rol" => {
                v.push(format!("{{ unsigned k = c % {}u; r = k ? ({})(((uint32_t)a << k) | ((uint32_t)a >> ({} - k))) : a; }}", bits, t, bits));
                v.push(format!("cf = r & 1u; of = ((r >> {}) & 1u) ^ cf;", bits - 1));
            }
            _ => {
                v.push(format!("{{ unsigned k = c % {}u; r = k ? ({})(((uint32_t)a >> k) | ((uint32_t)a << ({} - k))) : a; }}", bits, t, bits));
                v.push(format!("cf = (r >> {}) & 1u; of = ((r >> {}) ^ (r >> {})) & 1u;", bits - 1, bits - 1, bits - 2));
            }
        }
        v.push(self.wr(d, "r")?);
        v.push("} }".into());
        Ok(v)
    }

    fn shiftd(&self, ins: &Insn, mn: &str) -> R<Vec<String>> {
        let (d, s, n) = ops3(&ins.operands)?;
        if bits_of(d) != 32 {
            return unsup("16-bit shld/shrd");
        }
        let mut v = vec![format!("{{ unsigned c = (unsigned)({}) & 31u; if (c) {{ uint32_t a = {}, b = {}, r;",
                                 self.rd(ins, n, Some(8))?, self.rd(ins, d, None)?, self.rd(ins, s, None)?)];
        if mn == "shld" {
            v.extend(["r = (a << c) | (b >> (32 - c));".into(), "cf = (a >> (32 - c)) & 1u;".into(), "of = ((r ^ a) >> 31) & 1u;".into()]);
        } else {
            v.extend(["r = (a >> c) | (b << (32 - c));".into(), "cf = (a >> (c - 1)) & 1u;".into(), "of = ((r ^ a) >> 31) & 1u;".into()]);
        }
        v.extend(Self::szp("r", 32)?);
        v.push(self.wr(d, "r")?);
        v.push("} }".into());
        Ok(v)
    }

    fn muldiv(&self, ins: &Insn, mn: &str) -> R<Vec<String>> {
        let ops = &ins.operands;
        if mn == "imul" && ops.len() >= 2 {
            let d = &ops[0];
            let bits = bits_of(d);
            let s = st(bits)?;
            let a = if ops.len() == 3 { self.rd(ins, &ops[1], None)? } else { self.rd(ins, d, None)? };
            let b = if ops.len() == 3 { self.rd(ins, &ops[2], Some(bits))? } else { self.rd(ins, &ops[1], Some(bits))? };
            return Ok(vec![format!("{{ int64_t p = (int64_t)({}){} * (int64_t)({}){}; {} r = ({})p;", s, a, s, b, s, s),
                           "cf = of = p != (int64_t)r;".into(), self.wr(d, &format!("({})r", ut(bits)?))? + " }"]);
        }
        let s = ops1(ops)?;
        let bits = bits_of(s);
        let v = self.rd(ins, s, None)?;
        let a = hex32(ins.address);
        Ok(vec![match (bits, mn) {
            (32, "mul") => format!("{{ uint64_t p = (uint64_t)eax * (uint64_t){}; eax = (uint32_t)p; edx = (uint32_t)(p >> 32); cf = of = edx != 0; }}", v),
            (32, "imul") => format!("{{ int64_t p = (int64_t)(int32_t)eax * (int64_t)(int32_t){}; eax = (uint32_t)p; edx = (uint32_t)((uint64_t)p >> 32); \
                cf = of = p != (int64_t)(int32_t)eax; }}", v),
            (32, "div") => format!("{{ uint64_t n = ((uint64_t)edx << 32) | eax; uint32_t d = {}; if (!d || n / d > 0xFFFFFFFFull) RT_DIVIDE({}); \
                eax = (uint32_t)(n / d); edx = (uint32_t)(n % d); }}", v, a),
            (32, _) => format!("{{ int64_t n = (int64_t)(((uint64_t)edx << 32) | eax); int32_t d = (int32_t){}; \
                if (!d || (n == INT64_MIN && d == -1)) RT_DIVIDE({}); {{ int64_t q = n / d; \
                if (q > INT32_MAX || q < INT32_MIN) RT_DIVIDE({}); eax = (uint32_t)(int32_t)q; edx = (uint32_t)(int32_t)(n % d); }} }}", v, a, a),
            (16, "mul") => format!("{{ uint32_t p = (uint32_t)(uint16_t)eax * (uint32_t){}; eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu); \
                edx = (edx & 0xFFFF0000u) | (p >> 16); cf = of = (p >> 16) != 0; }}", v),
            (16, "imul") => format!("{{ int32_t p = (int32_t)(int16_t)eax * (int32_t)(int16_t){}; eax = (eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFFu); \
                edx = (edx & 0xFFFF0000u) | (((uint32_t)p >> 16) & 0xFFFFu); cf = of = p != (int32_t)(int16_t)p; }}", v),
            (16, "div") => format!("{{ uint32_t n = ((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu); uint32_t d = {}; if (!d || n / d > 0xFFFFu) RT_DIVIDE({}); \
                eax = (eax & 0xFFFF0000u) | (n / d); edx = (edx & 0xFFFF0000u) | (n % d); }}", v, a),
            (16, _) => format!("{{ int32_t n = (int32_t)(((edx & 0xFFFFu) << 16) | (eax & 0xFFFFu)); int32_t d = (int16_t){}; \
                if (!d) RT_DIVIDE({}); {{ int32_t q = n / d; if (q > 32767 || q < -32768) RT_DIVIDE({}); \
                eax = (eax & 0xFFFF0000u) | ((uint32_t)q & 0xFFFFu); edx = (edx & 0xFFFF0000u) | ((uint32_t)(n % d) & 0xFFFFu); }} }}", v, a, a),
            // 8-bit
            (_, "mul") => format!("{{ uint32_t p = (uint32_t)(uint8_t)eax * (uint32_t){}; eax = (eax & 0xFFFF0000u) | (p & 0xFFFFu); cf = of = (p >> 8) != 0; }}", v),
            (_, "imul") => format!("{{ int32_t p = (int32_t)(int8_t)eax * (int32_t)(int8_t){}; eax = (eax & 0xFFFF0000u) | ((uint32_t)p & 0xFFFFu); \
                cf = of = p != (int32_t)(int8_t)p; }}", v),
            (_, "div") => format!("{{ uint32_t n = eax & 0xFFFFu; uint32_t d = {}; if (!d || n / d > 0xFFu) RT_DIVIDE({}); \
                eax = (eax & 0xFFFF0000u) | ((n % d) << 8) | (n / d); }}", v, a),
            (_, _) => format!("{{ int32_t n = (int16_t)eax; int32_t d = (int8_t){}; if (!d) RT_DIVIDE({}); {{ int32_t q = n / d; \
                if (q > 127 || q < -128) RT_DIVIDE({}); eax = (eax & 0xFFFF0000u) | (((uint32_t)(n % d) & 0xFFu) << 8) | ((uint32_t)q & 0xFFu); }} }}", v, a, a),
        }])
    }

    fn bittest(&self, ins: &Insn, mn: &str) -> R<Vec<String>> {
        let (d, s) = ops2(&ins.operands)?;
        if d.ty == OP_MEM && s.ty != OP_IMM {
            // Bit string: the register offset is signed and not limited to the operand, so it
            // addresses the byte at A + (offset >> 3), bit offset & 7 (byte granularity has the
            // same effect as the CPU's dword access). CRT character-set builders use this
            // (`bts dword ptr [esp], eax` at 0x10322c0b).
            let sb = bits_of(s);
            let off = format!("(int32_t)({}){}", st(sb)?, self.rd(ins, s, None)?);
            let mut v = vec![format!("{{ int32_t o = {}; uint32_t B = A + (uint32_t)(o >> 3); unsigned n = (unsigned)o & 7u; uint8_t v = rd8(B);", off),
                             "cf = (v >> n) & 1u;".to_string()];
            match mn {
                "bts" => v.push("wr8(B, (uint8_t)(v | (1u << n)));".into()),
                "btr" => v.push("wr8(B, (uint8_t)(v & ~(1u << n)));".into()),
                "btc" => v.push("wr8(B, (uint8_t)(v ^ (1u << n)));".into()),
                _ => {}
            }
            v.push("}".into());
            return Ok(v);
        }
        let bits = bits_of(d);
        let mut v = vec![format!("{{ unsigned n = (unsigned)({}) & {}u; {} v = {}; cf = (v >> n) & 1u;",
                                 self.rd(ins, s, Some(if s.ty == OP_IMM { 8 } else { bits }))?, bits as i64 - 1,
                                 ut(bits)?, self.rd(ins, d, None)?)];
        match mn {
            "bts" => v.push(self.wr(d, "v | (1u << n)")?),
            "btr" => v.push(self.wr(d, "v & ~(1u << n)")?),
            "btc" => v.push(self.wr(d, "v ^ (1u << n)")?),
            _ => {}
        }
        v.push("}".into());
        Ok(v)
    }

    fn string(&self, mn: &str, prefix: Option<&str>) -> R<Vec<String>> {
        let kind = &mn[..4];
        let w = match &mn[4..] { "b" => 8, "w" => 16, _ => 32 };
        let t = ut(w)?;
        let n = w / 8;
        let (acc, acc_w) = match w {
            8 => ("(uint8_t)eax", "eax = (eax & 0xFFFFFF00u) | (uint8_t)v;"),
            16 => ("(uint16_t)eax", "eax = (eax & 0xFFFF0000u) | (uint16_t)v;"),
            _ => ("eax", "eax = v;"),
        };
        let step = format!("(df ? (uint32_t)-{} : {}u)", n, n);
        let body = match kind {
            "movs" => format!("wr{}(edi, rd{}(esi)); esi += {}; edi += {};", w, w, step, step),
            "stos" => format!("wr{}(edi, ({}){}); edi += {};", w, t, acc, step),
            "lods" => format!("{{ {} v = rd{}(esi); {} }} esi += {};", t, w, acc_w, step),
            "scas" => format!("{{ {} a = {}, b = rd{}(edi), r = ({})(a - b); {} }} edi += {};", t, acc, w, t,
                              Self::flags_sub("a", "b", "r", w, None)?.join(" "), step),
            // cmps: [esi] - [edi]
            _ => format!("{{ {} a = rd{}(esi), b = rd{}(edi), r = ({})(a - b); {} }} esi += {}; edi += {};", t, w, w, t,
                         Self::flags_sub("a", "b", "r", w, None)?.join(" "), step, step),
        };
        match prefix {
            None => Ok(vec![body]),
            Some("rep") if matches!(kind, "movs" | "stos" | "lods") => Ok(vec![format!("while (ecx) {{ {} ecx--; }}", body)]),
            Some("rep" | "repe" | "repz") if matches!(kind, "scas" | "cmps") => Ok(vec![format!("while (ecx) {{ {} ecx--; if (!zf) break; }}", body)]),
            Some("repne" | "repnz") if matches!(kind, "scas" | "cmps") => Ok(vec![format!("while (ecx) {{ {} ecx--; if (zf) break; }}", body)]),
            Some(p) => unsup(format!("{} {}", p, mn)),
        }
    }

    // ------------------------------------------------------------------ x87

    fn x87(&self, ins: &Insn, (op, modrm): (u8, u8)) -> R<Vec<String>> {
        let (md, reg, rm) = (modrm >> 6, (modrm >> 3) & 7, modrm & 7);
        let memop = ins.operands.iter().find(|o| o.ty == OP_MEM);
        let a: Vec<String> = match memop {
            Some(m) => vec![format!("uint32_t A = {};", self.addr(ins, m, false)?)],
            None => vec![],
        };
        let arith = |r: u8, d: &str, s: &str| -> String {
            match r {
                0 => format!("{d} = fr(g, {d} + {s});"),
                1 => format!("{d} = fr(g, {d} * {s});"),
                4 => format!("{d} = fr(g, {d} - {s});"),
                5 => format!("{d} = fr(g, {s} - {d});"),
                6 => format!("{d} = fr(g, {d} / {s});"),
                _ => format!("{d} = fr(g, {s} / {d});"),
            }
        };
        // DC/DE register forms: ST(i) = ST(i) op ST(0) with FSUBR/FSUB and FDIVR/FDIV swapped
        let arith_rev = |r: u8, d: &str, s: &str| -> Option<String> {
            Some(match r {
                0 => format!("{d} = fr(g, {d} + {s});"),
                1 => format!("{d} = fr(g, {d} * {s});"),
                4 => format!("{d} = fr(g, {s} - {d});"),
                5 => format!("{d} = fr(g, {d} - {s});"),
                6 => format!("{d} = fr(g, {s} / {d});"),
                7 => format!("{d} = fr(g, {d} / {s});"),
                _ => return None,
            })
        };
        let with = |mut a: Vec<String>, v: &[&str]| -> Vec<String> {
            a.extend(v.iter().map(|s| s.to_string()));
            a
        };

        if md != 3 {
            // memory forms
            if matches!(op, 0xD8 | 0xDC | 0xDA | 0xDE) {
                let src = match op { 0xD8 => "rdf32(A)", 0xDC => "rdf64(A)", 0xDA => "(double)(int32_t)rd32(A)", _ => "(double)(int16_t)rd16(A)" };
                if reg == 2 || reg == 3 {
                    let mut v = a;
                    v.push(format!("fcom(g, ST(0), {});", src));
                    if reg == 3 {
                        v.push("fpop(g);".into());
                    }
                    return Ok(v);
                }
                let mut v = a;
                v.push(arith(reg, "ST(0)", src));
                return Ok(v);
            }
            let pick: Option<&[&str]> = match (op, reg) {
                (0xD9, 0) => Some(&["fpush(g, rdf32(A));"]),
                (0xD9, 2) => Some(&["wrf32(A, (float)ST(0));"]),
                (0xD9, 3) => Some(&["wrf32(A, (float)fpop(g));"]),
                (0xD9, 4) => Some(&["fenv_load(g, A);"]),
                (0xD9, 5) => Some(&["g->fcw = rd16(A);"]),
                (0xD9, 6) => Some(&["fenv_store(g, A); g->fcw |= 0x3Fu;"]),
                (0xD9, 7) => Some(&["wr16(A, g->fcw);"]),
                (0xDB, 0) => Some(&["fpush(g, (double)(int32_t)rd32(A));"]),
                (0xDB, 1) => Some(&["wr32(A, (uint32_t)(int32_t)trunc(fpop(g)));"]),
                (0xDB, 2) => Some(&["wr32(A, (uint32_t)fist32(g, ST(0)));"]),
                (0xDB, 3) => Some(&["wr32(A, (uint32_t)fist32(g, fpop(g)));"]),
                (0xDB, 5) => Some(&["fpush(g, rdf80(A));"]),
                (0xDB, 7) => Some(&["wrf80(A, fpop(g));"]),
                (0xDD, 0) => Some(&["fpush(g, rdf64(A));"]),
                (0xDD, 2) => Some(&["wrf64(A, ST(0));"]),
                (0xDD, 3) => Some(&["wrf64(A, fpop(g));"]),
                (0xDD, 4) => Some(&["frstor(g, A);"]),
                (0xDD, 6) => Some(&["fsave(g, A);"]),
                (0xDD, 7) => Some(&["wr16(A, fstsw(g));"]),
                (0xDF, 0) => Some(&["fpush(g, (double)(int16_t)rd16(A));"]),
                (0xDF, 2) => Some(&["wr16(A, (uint16_t)fist16(g, ST(0)));"]),
                (0xDF, 3) => Some(&["wr16(A, (uint16_t)fist16(g, fpop(g)));"]),
                (0xDF, 5) => Some(&["fpush(g, (double)(int64_t)rd64(A));"]),
                (0xDF, 7) => Some(&["wr64(A, (uint64_t)fist64(g, fpop(g)));"]),
                _ => None,
            };
            return match pick {
                Some(v) => Ok(with(a, v)),
                None => x87_bad(ins),
            };
        }

        let i = rm; // register forms: ST(i)
        match op {
            0xD8 => {
                if reg == 2 || reg == 3 {
                    let mut v = vec![format!("fcom(g, ST(0), ST({}));", i)];
                    if reg == 3 {
                        v.push("fpop(g);".into());
                    }
                    return Ok(v);
                }
                return Ok(vec![arith(reg, "ST(0)", &format!("ST({})", i))]);
            }
            0xDC => {
                // DC: ST(i) = ST(i) op ST(0); E0+i FSUBR (ST(i) = ST(0) - ST(i)), E8+i FSUB, F0+i FDIVR, F8+i FDIV
                return match arith_rev(reg, &format!("ST({})", i), "ST(0)") {
                    Some(s) => Ok(vec![s]),
                    None => x87_bad(ins),
                };
            }
            0xDE => {
                if modrm == 0xD9 {
                    return Ok(vec!["fcom(g, ST(0), ST(1)); fpop(g); fpop(g);".into()]);
                }
                return match arith_rev(reg, &format!("ST({})", i), "ST(0)") {
                    Some(s) => Ok(vec![s, "fpop(g);".into()]),
                    None => x87_bad(ins),
                };
            }
            0xD9 => {
                if reg == 0 {
                    return Ok(vec![format!("{{ double v = ST({}); fpush(g, v); }}", i)]);
                }
                if reg == 1 {
                    return Ok(vec![format!("{{ double t = ST(0); ST(0) = ST({}); ST({}) = t; }}", i, i)]);
                }
                let simple: &[&str] = match modrm {
                    0xD0 => &[],
                    0xE0 => &["ST(0) = -ST(0);"],
                    0xE1 => &["ST(0) = fabs(ST(0));"],
                    0xE4 => &["fcom(g, ST(0), 0.0);"],
                    0xE5 => &["fxam(g);"],
                    0xE8 => &["fpush(g, 1.0);"],
                    0xE9 => &["fpush(g, 3.3219280948873623);"],
                    0xEA => &["fpush(g, 1.4426950408889634);"],
                    0xEB => &["fpush(g, 3.141592653589793);"],
                    0xEC => &["fpush(g, 0.30102999566398120);"],
                    0xED => &["fpush(g, 0.69314718055994531);"],
                    0xEE => &["fpush(g, 0.0);"],
                    0xF0 => &["ST(0) = fr(g, exp2(ST(0)) - 1.0);"],
                    0xF1 => &["{ double x = fpop(g); ST(0) = fr(g, ST(0) * log2(x)); }"],
                    0xF2 => &["ST(0) = fr(g, tan(ST(0))); fpush(g, 1.0); g->c2 = 0;"],
                    0xF3 => &["{ double x = fpop(g); ST(0) = fr(g, atan2(ST(0), x)); }"],
                    0xF5 => &["ST(0) = fr(g, remainder(ST(0), ST(1))); g->c2 = 0;"],
                    0xF6 => &["g->top = (g->top - 1) & 7u;"],
                    0xF7 => &["g->top = (g->top + 1) & 7u;"],
                    0xF8 => &["ST(0) = fr(g, fmod(ST(0), ST(1))); g->c2 = 0;"],
                    0xF9 => &["{ double x = fpop(g); ST(0) = fr(g, ST(0) * log2(x + 1.0)); }"],
                    0xFA => &["ST(0) = fr(g, sqrt(ST(0)));"],
                    0xFB => &["{ double x = ST(0); ST(0) = fr(g, sin(x)); fpush(g, fr(g, cos(x))); g->c2 = 0; }"],
                    0xFC => &["ST(0) = frnd(g, ST(0));"],
                    0xFD => &["ST(0) = fr(g, ldexp(ST(0), (int)trunc(ST(1))));"],
                    0xFE => &["ST(0) = fr(g, sin(ST(0))); g->c2 = 0;"],
                    0xFF => &["ST(0) = fr(g, cos(ST(0))); g->c2 = 0;"],
                    _ => return x87_bad(ins),
                };
                return Ok(simple.iter().map(|s| s.to_string()).collect());
            }
            _ => {}
        }
        if op == 0xDA && modrm == 0xE9 {
            return Ok(vec!["fcom(g, ST(0), ST(1)); fpop(g); fpop(g);".into()]);
        }
        if op == 0xDB {
            if modrm == 0xE2 {
                return Ok(vec!["g->c0 = g->c1 = g->c2 = g->c3 = 0;".into()]);
            }
            if modrm == 0xE3 {
                return Ok(vec!["g->fcw = 0x037F; g->top = 0; g->c0 = g->c1 = g->c2 = g->c3 = 0;".into()]);
            }
            if reg == 5 || reg == 6 {
                return Ok(fcomi(i, false));
            }
            return x87_bad(ins);
        }
        if op == 0xDD {
            return match reg {
                0 => Ok(vec![]), // FFREE: tags are not modelled
                2 => Ok(vec![format!("ST({}) = ST(0);", i)]),
                3 => Ok(vec![format!("ST({}) = ST(0); fpop(g);", i)]),
                4 | 5 => {
                    let mut v = vec![format!("fcom(g, ST(0), ST({}));", i)];
                    if reg == 5 {
                        v.push("fpop(g);".into());
                    }
                    Ok(v)
                }
                _ => x87_bad(ins),
            };
        }
        if op == 0xDF && modrm == 0xE0 {
            return Ok(vec!["eax = (eax & 0xFFFF0000u) | fstsw(g);".into()]);
        }
        if op == 0xDF && (reg == 5 || reg == 6) {
            return Ok(fcomi(i, true));
        }
        x87_bad(ins)
    }
}

fn x87_opcode(ins: &Insn) -> Option<(u8, u8)> {
    let b = &ins.bytes;
    let mut i = 0;
    while i < b.len() && matches!(b[i], 0x9B | 0x66 | 0x67 | 0x26 | 0x2E | 0x36 | 0x3E | 0x64 | 0x65 | 0xF0 | 0xF2 | 0xF3) {
        i += 1;
    }
    if i + 1 < b.len() && (0xD8..=0xDF).contains(&b[i]) {
        return Some((b[i], b[i + 1]));
    }
    None
}

/// FCOMI/FUCOMI(P): compare ST(0) with ST(i) into ZF PF CF (unordered sets all three).
fn fcomi(i: u8, pop: bool) -> Vec<String> {
    let mut v = vec![format!("{{ double a = ST(0), b = ST({}); if (a != a || b != b) {{ zf = pf = cf = 1; }} \
        else {{ zf = a == b; pf = 0; cf = a < b; }} of = sf = af = 0; }}", i)];
    if pop {
        v.push("fpop(g);".into());
    }
    v
}

fn x87_bad<T>(ins: &Insn) -> R<T> {
    unsup(format!("x87 {}", ins.bytes.iter().map(|b| format!("{:02x}", b)).collect::<String>()))
}

fn ops1(ops: &[Op]) -> R<&Op> {
    match ops {
        [a] => Ok(a),
        _ => Err(Tx::Fatal(format!("ValueError: expected 1 operand, got {}", ops.len()))),
    }
}

fn ops2(ops: &[Op]) -> R<(&Op, &Op)> {
    match ops {
        [a, b] => Ok((a, b)),
        _ => Err(Tx::Fatal(format!("ValueError: expected 2 operands, got {}", ops.len()))),
    }
}

fn ops3(ops: &[Op]) -> R<(&Op, &Op, &Op)> {
    match ops {
        [a, b, c] => Ok((a, b, c)),
        _ => Err(Tx::Fatal(format!("ValueError: expected 3 operands, got {}", ops.len()))),
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn st_locals() {
        assert_eq!(super::sub_st("ST(0) = fr(g, ST(1) + XST(2)) ST(8)"), "x87_s0 = fr(g, x87_s1 + XST(2)) ST(8)");
        let out = super::x87_locals(vec!["    REGS_DECL;".into(), "        fxam(g);".into(), "        ST(0) = fr(g, ST(0) * ST(1));".into()]);
        assert_eq!(out, ["    REGS_DECL; X87_DECL;", "        X87_STORE; fxam(g); X87_LOAD;", "        x87_s0 = fr_cw(x87_cw, x87_s0 * x87_s1);"]);
    }
}
