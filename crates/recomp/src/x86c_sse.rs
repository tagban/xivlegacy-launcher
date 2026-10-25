//! SSE / SSE2 / MMX translation (runtime/sse.h has the semantics).
//!
//! XMM registers live in g->xmm[n] (X128), MMX registers in g->mm[n] (uint64_t), MXCSR in
//! g->mxcsr. Most instructions become
//!     { X128 d = <dst>, s = <src>; d = sse_<op>(d, s); <dst> = d; }
//! where an MMX operand is widened to X128 (high half zero) and only its low 64 bits are stored
//! back, so the integer helpers serve both register files.
use super::*;

#[derive(Clone, Copy)]
enum Loc {
    Xmm(u8),
    Mm(u8),
    Mem(u32),
    Gpr,
    Imm,
}

fn reg_num(op: &Op, prefix: &str) -> Option<u8> {
    if op.ty != OP_REG {
        return None;
    }
    let n = disasm::reg_name(op.reg)?;
    let rest = n.strip_prefix(prefix)?;
    rest.parse::<u8>().ok().filter(|&k| k < 8)
}

fn loc(op: &Op) -> Loc {
    if let Some(n) = reg_num(op, "xmm") {
        Loc::Xmm(n)
    } else if let Some(n) = reg_num(op, "mm") {
        Loc::Mm(n)
    } else {
        match op.ty {
            OP_MEM => Loc::Mem(bits_of(op)),
            OP_IMM => Loc::Imm,
            _ => Loc::Gpr,
        }
    }
}

/// Instructions handled here even without an XMM/MMX operand.
/// (cvt*2si from memory has no XMM operand either.)
const STANDALONE: [&str; 13] = ["ldmxcsr", "stmxcsr", "emms", "prefetchnta", "prefetcht0", "prefetcht1", "prefetcht2",
                                "sfence", "lfence", "cvttss2si", "cvttsd2si", "cvtss2si", "cvtsd2si"];

/// CMPPS-family pseudo-op predicate names, as capstone prints them (cmpltps, cmpnlesd, ...).
const CMP_PRED: [&str; 8] = ["eq", "lt", "le", "unord", "neq", "nlt", "nle", "ord"];

fn cmp_pseudo(mn: &str) -> Option<(u32, &'static str)> {
    let rest = mn.strip_prefix("cmp")?;
    for suffix in ["ps", "ss", "pd", "sd"] {
        if let Some(pred) = rest.strip_suffix(suffix) {
            if let Some(p) = CMP_PRED.iter().position(|&x| x == pred) {
                return Some((p as u32, ["cmpps", "cmpss", "cmppd", "cmpsd"][["ps", "ss", "pd", "sd"].iter().position(|&s| s == suffix).unwrap()]));
            }
        }
    }
    None
}

impl<'a> FunctionTranslator<'a> {
    /// The operand as an X128 expression.
    fn xload(&self, ins: &Insn, op: &Op) -> R<String> {
        Ok(match loc(op) {
            Loc::Xmm(n) => format!("g->xmm[{}]", n),
            Loc::Mm(n) => format!("x_q(g->mm[{}])", n),
            Loc::Mem(128) => "x_rd128(A)".into(),
            Loc::Mem(64) => "x_q(rd64(A))".into(),
            Loc::Mem(32) => "x_u(rd32(A))".into(),
            Loc::Mem(16) => "x_u(rd16(A))".into(),
            Loc::Mem(b) => return unsup(format!("sse memory operand of {} bits", b)),
            Loc::Gpr | Loc::Imm => format!("x_u({})", self.rd(ins, op, Some(32))?),
        })
    }

    /// Stores X128 `v` (a C lvalue name) to the operand.
    fn xstore(&self, op: &Op, v: &str) -> R<String> {
        Ok(match loc(op) {
            Loc::Xmm(n) => format!("g->xmm[{}] = {};", n, v),
            Loc::Mm(n) => format!("g->mm[{}] = {}.q[0];", n, v),
            Loc::Mem(128) => format!("x_wr128(A, {});", v),
            Loc::Mem(64) => format!("wr64(A, {}.q[0]);", v),
            Loc::Mem(32) => format!("wr32(A, {}.u[0]);", v),
            Loc::Mem(b) => return unsup(format!("sse store of {} bits", b)),
            Loc::Gpr => self.wr(op, &format!("{}.u[0]", v))?,
            Loc::Imm => return unsup("sse store to an immediate"),
        })
    }

    /// `{ X128 d = dst, s = src; d = <call>; dst = d; }`
    fn xbinary(&self, ins: &Insn, d: &Op, s: &Op, call: &str) -> R<Vec<String>> {
        Ok(vec![format!(
            "{{ X128 d = {}, s = {}; d = {}; {} }}",
            self.xload(ins, d)?,
            self.xload(ins, s)?,
            call,
            self.xstore(d, "d")?
        )])
    }

    fn imm8(op: &Op) -> R<u64> {
        if op.ty != OP_IMM {
            return unsup("sse: expected an immediate");
        }
        Ok(op.imm as u64 & 0xFF)
    }

    /// Translates `ins` if it is an SSE/SSE2/MMX instruction; None otherwise.
    pub(super) fn sse(&self, ins: &Insn, mn: &str) -> R<Option<Vec<String>>> {
        let ops = &ins.operands;
        let vector = ops.iter().any(|o| matches!(loc(o), Loc::Xmm(_) | Loc::Mm(_)));
        if !vector && !STANDALONE.contains(&mn) {
            return Ok(None);
        }
        let mut out = self.mem_prologue(ins)?;
        out.extend(self.sse_body(ins, mn, ops)?);
        Ok(Some(out))
    }

    fn sse_body(&self, ins: &Insn, mn: &str, ops: &[Op]) -> R<Vec<String>> {
        // MMX operands are 8 bytes wide, XMM 16.
        let width = |op: &Op| if matches!(loc(op), Loc::Mm(_)) { 8 } else { 16 };

        if let Some((pred, base)) = cmp_pseudo(mn) {
            let (d, s) = ops2(ops)?;
            return self.xbinary(ins, d, s, &format!("sse_{}(d, s, {})", base, pred));
        }

        match mn {
            "emms" | "prefetchnta" | "prefetcht0" | "prefetcht1" | "prefetcht2" | "sfence" | "lfence" => Ok(vec![]),
            "ldmxcsr" => Ok(vec!["g->mxcsr = rd32(A);".into()]),
            "stmxcsr" => Ok(vec!["wr32(A, g->mxcsr);".into()]),

            // ---------------------------------------------------------------- moves
            "movaps" | "movups" | "movapd" | "movupd" | "movdqa" | "movdqu" | "lddqu" | "movntps" | "movntpd"
            | "movntdq" => {
                let (d, s) = ops2(ops)?;
                Ok(vec![format!("{{ X128 d = {}; {} }}", self.xload(ins, s)?, self.xstore(d, "d")?)])
            }
            "movss" | "movsd" => {
                let (d, s) = ops2(ops)?;
                let (lane, rd, wr) = if mn == "movss" { ("u[0]", "x_u(rd32(A))", "wr32") } else { ("q[0]", "x_q(rd64(A))", "wr64") };
                Ok(vec![match (loc(d), loc(s)) {
                    (Loc::Xmm(a), Loc::Xmm(b)) => format!("g->xmm[{}].{} = g->xmm[{}].{};", a, lane, b, lane),
                    (Loc::Xmm(a), Loc::Mem(_)) => format!("g->xmm[{}] = {};", a, rd),
                    (Loc::Mem(_), Loc::Xmm(b)) => format!("{}(A, g->xmm[{}].{});", wr, b, lane),
                    _ => return unsup(format!("{} form", mn)),
                }])
            }
            "movq" | "movntq" | "movq2dq" | "movdq2q" => {
                let (d, s) = ops2(ops)?;
                let src64 = match loc(s) {
                    Loc::Xmm(n) => format!("g->xmm[{}].q[0]", n),
                    Loc::Mm(n) => format!("g->mm[{}]", n),
                    Loc::Mem(64) => "rd64(A)".into(),
                    _ => return unsup("movq source"),
                };
                Ok(vec![match loc(d) {
                    Loc::Xmm(n) => format!("g->xmm[{}] = x_q({});", n, src64),
                    Loc::Mm(n) => format!("g->mm[{}] = {};", n, src64),
                    Loc::Mem(64) => format!("wr64(A, {});", src64),
                    _ => return unsup("movq destination"),
                }])
            }
            "movd" => {
                let (d, s) = ops2(ops)?;
                Ok(vec![match (loc(d), loc(s)) {
                    (Loc::Xmm(n), Loc::Gpr | Loc::Mem(32)) => format!("g->xmm[{}] = x_u({});", n, self.rd(ins, s, Some(32))?),
                    (Loc::Mm(n), Loc::Gpr | Loc::Mem(32)) => format!("g->mm[{}] = (uint64_t){};", n, self.rd(ins, s, Some(32))?),
                    (Loc::Gpr | Loc::Mem(32), Loc::Xmm(n)) => self.wr(d, &format!("g->xmm[{}].u[0]", n))?,
                    (Loc::Gpr | Loc::Mem(32), Loc::Mm(n)) => self.wr(d, &format!("(uint32_t)g->mm[{}]", n))?,
                    _ => return unsup("movd form"),
                }])
            }
            "movlps" | "movlpd" | "movhps" | "movhpd" => {
                let (d, s) = ops2(ops)?;
                let lane = if mn.starts_with("movl") { 0 } else { 1 };
                Ok(vec![match (loc(d), loc(s)) {
                    (Loc::Xmm(n), Loc::Mem(_)) => format!("g->xmm[{}].q[{}] = rd64(A);", n, lane),
                    (Loc::Mem(_), Loc::Xmm(n)) => format!("wr64(A, g->xmm[{}].q[{}]);", n, lane),
                    _ => return unsup(format!("{} form", mn)),
                }])
            }
            "movlhps" | "movhlps" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s)", mn))
            }

            // ---------------------------------------------------------------- arithmetic
            "addps" | "addss" | "addpd" | "addsd" | "subps" | "subss" | "subpd" | "subsd" | "mulps" | "mulss"
            | "mulpd" | "mulsd" | "divps" | "divss" | "divpd" | "divsd" | "minps" | "minss" | "minpd" | "minsd"
            | "maxps" | "maxss" | "maxpd" | "maxsd" | "sqrtps" | "sqrtss" | "sqrtpd" | "sqrtsd" | "rcpps"
            | "rcpss" | "rsqrtps" | "rsqrtss" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s)", mn))
            }

            // ---------------------------------------------------------------- logic
            "andps" | "andpd" | "pand" => self.logic(ins, ops, "and"),
            "andnps" | "andnpd" | "pandn" => self.logic(ins, ops, "andn"),
            "orps" | "orpd" | "por" => self.logic(ins, ops, "or"),
            "xorps" | "xorpd" | "pxor" => self.logic(ins, ops, "xor"),

            // ---------------------------------------------------------------- compares
            "cmpps" | "cmpss" | "cmppd" | "cmpsd" => {
                let (d, s, i) = ops3(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s, {})", mn, Self::imm8(i)? & 7))
            }
            "comiss" | "ucomiss" | "comisd" | "ucomisd" => {
                let (d, s) = ops2(ops)?;
                let lane = if mn.ends_with("ss") { "f[0]" } else { "d[0]" };
                Ok(vec![format!("SSE_COMI(({}).{}, ({}).{});", self.xload(ins, d)?, lane, self.xload(ins, s)?, lane)])
            }

            // ---------------------------------------------------------------- conversions
            "cvtps2pd" | "cvtpd2ps" | "cvtss2sd" | "cvtsd2ss" | "cvtdq2ps" | "cvtdq2pd" | "cvttps2dq" | "cvtpi2ps"
            | "cvttps2pi" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s)", mn))
            }
            "cvtps2dq" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, "sse_cvtps2dq(g->mxcsr, d, s)")
            }
            "cvtsi2ss" | "cvtsi2sd" => {
                let (d, s) = ops2(ops)?;
                let Loc::Xmm(n) = loc(d) else { return unsup(format!("{} destination", mn)) };
                let (lane, ty) = if mn == "cvtsi2ss" { ("f[0]", "float") } else { ("d[0]", "double") };
                Ok(vec![format!("g->xmm[{}].{} = ({})(int32_t){};", n, lane, ty, self.rd(ins, s, Some(32))?)])
            }
            "cvttss2si" | "cvttsd2si" | "cvtss2si" | "cvtsd2si" => {
                let (d, s) = ops2(ops)?;
                let lane = if mn.contains("ss2") { "f[0]" } else { "d[0]" };
                let v = format!("({}).{}", self.xload(ins, s)?, lane);
                let conv = if mn.starts_with("cvtt") { format!("sse_cvtt_i32({})", v) } else { format!("sse_cvt_i32(g->mxcsr, {})", v) };
                Ok(vec![self.wr(d, &format!("(uint32_t){}", conv))?])
            }

            // ---------------------------------------------------------------- shuffles
            "shufps" | "shufpd" | "pshufd" | "pshufw" => {
                let (d, s, i) = ops3(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s, {})", mn, Self::imm8(i)?))
            }
            "unpcklps" | "unpckhps" | "unpcklpd" | "unpckhpd" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s)", mn))
            }
            "punpcklbw" | "punpcklwd" | "punpckldq" | "punpcklqdq" | "punpckhbw" | "punpckhwd" | "punpckhdq"
            | "punpckhqdq" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s, {})", mn, width(d) / 2))
            }
            "packssdw" | "packsswb" | "packuswb" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s, {})", mn, width(d)))
            }

            // ---------------------------------------------------------------- integer lanes
            "paddb" | "paddw" | "paddd" | "paddq" | "psubb" | "psubw" | "psubd" | "psubq" | "paddsb" | "paddsw"
            | "psubsb" | "psubsw" | "paddusb" | "paddusw" | "psubusb" | "psubusw" | "pmaxsw" | "pminsw" | "pmaxub"
            | "pminub" | "pmullw" | "pmulhw" | "pmuludq" | "pcmpeqb" | "pcmpeqw" | "pcmpeqd" | "pcmpgtb"
            | "pcmpgtw" | "pcmpgtd" | "pavgb" | "pavgw" => {
                let (d, s) = ops2(ops)?;
                self.xbinary(ins, d, s, &format!("sse_{}(d, s)", mn))
            }
            "psllw" | "pslld" | "psllq" | "psrlw" | "psrld" | "psrlq" | "psraw" | "psrad" => {
                let (d, s) = ops2(ops)?;
                let count = match loc(s) {
                    Loc::Imm => format!("{}u", Self::imm8(s)?),
                    _ => format!("({}).q[0]", self.xload(ins, s)?),
                };
                Ok(vec![format!("{{ X128 d = {}; d = sse_{}(d, {}); {} }}", self.xload(ins, d)?, mn, count, self.xstore(d, "d")?)])
            }
            "pslldq" | "psrldq" => {
                let (d, s) = ops2(ops)?;
                Ok(vec![format!("{{ X128 d = {}; d = sse_{}(d, {}u); {} }}", self.xload(ins, d)?, mn, Self::imm8(s)?, self.xstore(d, "d")?)])
            }

            // ---------------------------------------------------------------- to and from general registers
            "movmskps" | "movmskpd" => {
                let (d, s) = ops2(ops)?;
                Ok(vec![self.wr(d, &format!("sse_{}({})", mn, self.xload(ins, s)?))?])
            }
            "pmovmskb" => {
                let (d, s) = ops2(ops)?;
                Ok(vec![self.wr(d, &format!("sse_pmovmskb({}, {})", self.xload(ins, s)?, width(s)))?])
            }
            "pextrw" => {
                let (d, s, i) = ops3(ops)?;
                let lanes = width(s) / 2;
                Ok(vec![self.wr(d, &format!("(uint32_t)({}).w[{}]", self.xload(ins, s)?, Self::imm8(i)? % lanes))?])
            }
            "pinsrw" => {
                let (d, s, i) = ops3(ops)?;
                let lanes = width(d) / 2;
                Ok(vec![format!(
                    "{{ X128 d = {}; d.w[{}] = (uint16_t){}; {} }}",
                    self.xload(ins, d)?,
                    Self::imm8(i)? % lanes,
                    self.rd(ins, s, Some(if s.ty == OP_MEM { 16 } else { 32 }))?,
                    self.xstore(d, "d")?
                )])
            }
            _ => unsup(format!("sse {}", mn)),
        }
    }

    fn logic(&self, ins: &Insn, ops: &[Op], f: &str) -> R<Vec<String>> {
        let (d, s) = ops2(ops)?;
        // xorps x, x / pxor x, x: the zeroing idiom.
        if f == "xor" || f == "andn" {
            if let (Loc::Xmm(a), Loc::Xmm(b)) = (loc(d), loc(s)) {
                if a == b {
                    return Ok(vec![format!("g->xmm[{}] = x_zero();", a)]);
                }
            }
            if let (Loc::Mm(a), Loc::Mm(b)) = (loc(d), loc(s)) {
                if a == b {
                    return Ok(vec![format!("g->mm[{}] = 0;", a)]);
                }
            }
        }
        self.xbinary(ins, d, s, &format!("sse_{}(d, s)", f))
    }
}
