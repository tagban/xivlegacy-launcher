/* SSE/SSE2/MMX semantics for translated x86 code.
 *
 * The translator (crates/recomp/src/x86c_sse.rs) emits, per instruction,
 *     { X128 d = <dst>, s = <src>; d = sse_<op>(d, s[, imm]); <dst> = d; }
 * with XMM registers in g->xmm[n], MMX registers in g->mm[n] (widened to X128 for the
 * shared integer helpers, low 64 bits kept), and memory read and written through the guest's
 * rd/wr accessors. Plain C, no host intrinsics: the host may be ARM or x86-64.
 *
 * Rounding: conversions that round (cvtss2si, cvtps2dq, ...) honour MXCSR.RC; the
 * arithmetic itself runs in the host's default round-to-nearest, which is what the game sets.
 * Denormal/flush-to-zero modes are not modelled.
 */
#ifndef XIV_SSE_H
#define XIV_SSE_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef RT_INLINE
#define RT_INLINE static inline __attribute__((always_inline))
#endif

typedef union X128 {
    float f[4];
    double d[2];
    uint64_t q[2];
    int64_t sq[2];
    uint32_t u[4];
    int32_t i[4];
    uint16_t w[8];
    int16_t sw[8];
    uint8_t b[16];
    int8_t sb[16];
} X128;

RT_INLINE X128 x_zero(void) { X128 r; memset(&r, 0, sizeof r); return r; }
RT_INLINE X128 x_q(uint64_t lo) { X128 r = x_zero(); r.q[0] = lo; return r; }
RT_INLINE X128 x_u(uint32_t lo) { X128 r = x_zero(); r.u[0] = lo; return r; }

RT_INLINE X128 x_q2(uint64_t lo, uint64_t hi) { X128 r; r.q[0] = lo; r.q[1] = hi; return r; }

/* 128-bit guest memory, through the guest's rd64/wr64 (defined by guest.h before use). */
#define x_rd128(a) x_q2(rd64(a), rd64((uint32_t)((a) + 8u)))
#define x_wr128(a, v) do { X128 v_ = (v); wr64((a), v_.q[0]); wr64((uint32_t)((a) + 8u), v_.q[1]); } while (0)

#define MXCSR_DEFAULT 0x1F80u

/* ---------------------------------------------------------------- arithmetic */

#define SSE_PS(name, expr) \
    RT_INLINE X128 sse_##name##ps(X128 d, X128 s) { for (int k = 0; k < 4; k++) { float a = d.f[k], b = s.f[k]; (void)a; d.f[k] = (expr); } return d; } \
    RT_INLINE X128 sse_##name##ss(X128 d, X128 s) { float a = d.f[0], b = s.f[0]; (void)a; d.f[0] = (expr); return d; }
#define SSE_PD(name, expr) \
    RT_INLINE X128 sse_##name##pd(X128 d, X128 s) { for (int k = 0; k < 2; k++) { double a = d.d[k], b = s.d[k]; (void)a; d.d[k] = (expr); } return d; } \
    RT_INLINE X128 sse_##name##sd(X128 d, X128 s) { double a = d.d[0], b = s.d[0]; (void)a; d.d[0] = (expr); return d; }

SSE_PS(add, a + b)
SSE_PS(sub, a - b)
SSE_PS(mul, a * b)
SSE_PS(div, a / b)
/* MINPS/MAXPS: the second operand when either is NaN or both are zero. */
SSE_PS(min, a < b ? a : b)
SSE_PS(max, a > b ? a : b)
SSE_PS(sqrt, sqrtf(b))
SSE_PS(rcp, 1.0f / b)
SSE_PS(rsqrt, 1.0f / sqrtf(b))
SSE_PD(add, a + b)
SSE_PD(sub, a - b)
SSE_PD(mul, a * b)
SSE_PD(div, a / b)
SSE_PD(min, a < b ? a : b)
SSE_PD(max, a > b ? a : b)
SSE_PD(sqrt, sqrt(b))

/* ---------------------------------------------------------------- logic (bitwise on 128 bits) */

RT_INLINE X128 sse_and(X128 d, X128 s) { d.q[0] &= s.q[0]; d.q[1] &= s.q[1]; return d; }
RT_INLINE X128 sse_andn(X128 d, X128 s) { d.q[0] = ~d.q[0] & s.q[0]; d.q[1] = ~d.q[1] & s.q[1]; return d; }
RT_INLINE X128 sse_or(X128 d, X128 s) { d.q[0] |= s.q[0]; d.q[1] |= s.q[1]; return d; }
RT_INLINE X128 sse_xor(X128 d, X128 s) { d.q[0] ^= s.q[0]; d.q[1] ^= s.q[1]; return d; }

/* ---------------------------------------------------------------- compares */

/* CMPPS/CMPSS predicate imm: 0 eq, 1 lt, 2 le, 3 unord, 4 neq, 5 nlt, 6 nle, 7 ord. */
RT_INLINE int cmp_pred_f(double a, double b, int p) {
    int un = a != a || b != b;
    switch (p & 7) {
    case 0: return !un && a == b;
    case 1: return !un && a < b;
    case 2: return !un && a <= b;
    case 3: return un;
    case 4: return un || a != b;
    case 5: return un || !(a < b);
    case 6: return un || !(a <= b);
    default: return !un;
    }
}
RT_INLINE X128 sse_cmpps(X128 d, X128 s, int p) { for (int k = 0; k < 4; k++) d.u[k] = cmp_pred_f(d.f[k], s.f[k], p) ? 0xFFFFFFFFu : 0; return d; }
RT_INLINE X128 sse_cmpss(X128 d, X128 s, int p) { d.u[0] = cmp_pred_f(d.f[0], s.f[0], p) ? 0xFFFFFFFFu : 0; return d; }
RT_INLINE X128 sse_cmppd(X128 d, X128 s, int p) { for (int k = 0; k < 2; k++) d.q[k] = cmp_pred_f(d.d[k], s.d[k], p) ? ~0ull : 0; return d; }
RT_INLINE X128 sse_cmpsd(X128 d, X128 s, int p) { d.q[0] = cmp_pred_f(d.d[0], s.d[0], p) ? ~0ull : 0; return d; }

/* COMISS/UCOMISS/COMISD/UCOMISD into ZF PF CF (unordered sets all three); OF SF AF cleared. */
#define SSE_COMI(a, b) do { double a_ = (a), b_ = (b); \
    if (a_ != a_ || b_ != b_) { zf = pf = cf = 1; } else { zf = a_ == b_; pf = 0; cf = a_ < b_; } \
    of = sf = af = 0; } while (0)

/* ---------------------------------------------------------------- conversions */

/* MXCSR.RC (bits 13-14): 0 nearest-even, 1 down, 2 up, 3 toward zero. */
RT_INLINE double sse_round(uint32_t mxcsr, double x) {
    switch ((mxcsr >> 13) & 3u) {
    case 0: return nearbyint(x) ; /* host default is nearest-even */
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
/* Out of range or NaN gives the "integer indefinite" 0x80000000. */
RT_INLINE int32_t sse_i32(double x) { return (x != x || x >= 2147483648.0 || x < -2147483648.0) ? INT32_MIN : (int32_t)x; }
RT_INLINE int32_t sse_cvt_i32(uint32_t mxcsr, double x) { return sse_i32(sse_round(mxcsr, x)); }
RT_INLINE int32_t sse_cvtt_i32(double x) { return sse_i32(trunc(x)); }

RT_INLINE X128 sse_cvtps2pd(X128 d, X128 s) { (void)d; X128 r; r.d[0] = s.f[0]; r.d[1] = s.f[1]; return r; }
RT_INLINE X128 sse_cvtpd2ps(X128 d, X128 s) { (void)d; X128 r = x_zero(); r.f[0] = (float)s.d[0]; r.f[1] = (float)s.d[1]; return r; }
RT_INLINE X128 sse_cvtss2sd(X128 d, X128 s) { d.d[0] = s.f[0]; return d; }
RT_INLINE X128 sse_cvtsd2ss(X128 d, X128 s) { d.f[0] = (float)s.d[0]; return d; }
RT_INLINE X128 sse_cvtdq2ps(X128 d, X128 s) { (void)d; for (int k = 0; k < 4; k++) d.f[k] = (float)s.i[k]; return d; }
RT_INLINE X128 sse_cvtdq2pd(X128 d, X128 s) { (void)d; X128 r; r.d[0] = s.i[0]; r.d[1] = s.i[1]; return r; }
RT_INLINE X128 sse_cvtps2dq(uint32_t mx, X128 d, X128 s) { (void)d; for (int k = 0; k < 4; k++) d.i[k] = sse_cvt_i32(mx, s.f[k]); return d; }
RT_INLINE X128 sse_cvttps2dq(X128 d, X128 s) { (void)d; for (int k = 0; k < 4; k++) d.i[k] = sse_cvtt_i32(s.f[k]); return d; }
/* CVTPI2PS: two int32 from an MMX register / m64 into the low two floats; the high two stay. */
RT_INLINE X128 sse_cvtpi2ps(X128 d, X128 s) { d.f[0] = (float)s.i[0]; d.f[1] = (float)s.i[1]; return d; }
/* CVTTPS2PI: the low two floats, truncated, into an MMX register. */
RT_INLINE X128 sse_cvttps2pi(X128 d, X128 s) { (void)d; X128 r = x_zero(); r.i[0] = sse_cvtt_i32(s.f[0]); r.i[1] = sse_cvtt_i32(s.f[1]); return r; }

/* ---------------------------------------------------------------- shuffles and unpacks */

RT_INLINE X128 sse_shufps(X128 d, X128 s, int m) {
    X128 r; r.f[0] = d.f[m & 3]; r.f[1] = d.f[(m >> 2) & 3]; r.f[2] = s.f[(m >> 4) & 3]; r.f[3] = s.f[(m >> 6) & 3]; return r;
}
RT_INLINE X128 sse_shufpd(X128 d, X128 s, int m) { X128 r; r.d[0] = d.d[m & 1]; r.d[1] = s.d[(m >> 1) & 1]; return r; }
RT_INLINE X128 sse_pshufd(X128 d, X128 s, int m) {
    (void)d; X128 r; for (int k = 0; k < 4; k++) r.u[k] = s.u[(m >> (2 * k)) & 3]; return r;
}
/* PSHUFW (MMX): four words of the 64-bit source. */
RT_INLINE X128 sse_pshufw(X128 d, X128 s, int m) {
    (void)d; X128 r = x_zero(); for (int k = 0; k < 4; k++) r.w[k] = s.w[(m >> (2 * k)) & 3]; return r;
}
RT_INLINE X128 sse_unpcklps(X128 d, X128 s) { X128 r; r.f[0] = d.f[0]; r.f[1] = s.f[0]; r.f[2] = d.f[1]; r.f[3] = s.f[1]; return r; }
RT_INLINE X128 sse_unpckhps(X128 d, X128 s) { X128 r; r.f[0] = d.f[2]; r.f[1] = s.f[2]; r.f[2] = d.f[3]; r.f[3] = s.f[3]; return r; }
RT_INLINE X128 sse_unpcklpd(X128 d, X128 s) { X128 r; r.q[0] = d.q[0]; r.q[1] = s.q[0]; return r; }
RT_INLINE X128 sse_unpckhpd(X128 d, X128 s) { X128 r; r.q[0] = d.q[1]; r.q[1] = s.q[1]; return r; }
RT_INLINE X128 sse_movlhps(X128 d, X128 s) { d.q[1] = s.q[0]; return d; }
RT_INLINE X128 sse_movhlps(X128 d, X128 s) { d.q[0] = s.q[1]; return d; }

/* PUNPCKL*: interleave the low halves. `half` is the register's byte width / 2 (8 for XMM,
 * 4 for MMX); for MMX only the low 8 result bytes are kept by the caller. */
#define SSE_PUNPCK(name, T, lane, lo) \
    RT_INLINE X128 sse_##name(X128 d, X128 s, int half) { \
        X128 r = x_zero(); int n = half / (int)sizeof(T); int base = (lo) ? 0 : n; \
        for (int k = 0; k < n; k++) { r.lane[2 * k] = d.lane[base + k]; r.lane[2 * k + 1] = s.lane[base + k]; } \
        return r; }
SSE_PUNPCK(punpcklbw, uint8_t, b, 1)
SSE_PUNPCK(punpcklwd, uint16_t, w, 1)
SSE_PUNPCK(punpckldq, uint32_t, u, 1)
SSE_PUNPCK(punpcklqdq, uint64_t, q, 1)
SSE_PUNPCK(punpckhbw, uint8_t, b, 0)
SSE_PUNPCK(punpckhwd, uint16_t, w, 0)
SSE_PUNPCK(punpckhdq, uint32_t, u, 0)
SSE_PUNPCK(punpckhqdq, uint64_t, q, 0)

RT_INLINE int16_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (int16_t)v; }
RT_INLINE int8_t sat8(int32_t v) { return v > 127 ? 127 : v < -128 ? -128 : (int8_t)v; }
RT_INLINE uint8_t satu8(int32_t v) { return v > 255 ? 255 : v < 0 ? 0 : (uint8_t)v; }
RT_INLINE uint16_t satu16(int32_t v) { return v > 65535 ? 65535 : v < 0 ? 0 : (uint16_t)v; }

/* PACKSSDW: dwords of d then s, saturated to words. `bytes` 16 (XMM) or 8 (MMX). */
RT_INLINE X128 sse_packssdw(X128 d, X128 s, int bytes) {
    X128 r = x_zero(); int n = bytes / 4;
    for (int k = 0; k < n; k++) { r.sw[k] = sat16(d.i[k]); r.sw[n + k] = sat16(s.i[k]); }
    return r;
}
RT_INLINE X128 sse_packsswb(X128 d, X128 s, int bytes) {
    X128 r = x_zero(); int n = bytes / 2;
    for (int k = 0; k < n; k++) { r.sb[k] = sat8(d.sw[k]); r.sb[n + k] = sat8(s.sw[k]); }
    return r;
}
RT_INLINE X128 sse_packuswb(X128 d, X128 s, int bytes) {
    X128 r = x_zero(); int n = bytes / 2;
    for (int k = 0; k < n; k++) { r.b[k] = satu8(d.sw[k]); r.b[n + k] = satu8(s.sw[k]); }
    return r;
}

/* ---------------------------------------------------------------- integer lanes */

#define SSE_LANES(name, T, lane, expr) \
    RT_INLINE X128 sse_##name(X128 d, X128 s) { \
        for (int k = 0; k < (int)(16 / sizeof(T)); k++) { T a = d.lane[k], b = s.lane[k]; d.lane[k] = (T)(expr); } return d; }
SSE_LANES(paddb, uint8_t, b, a + b)
SSE_LANES(paddw, uint16_t, w, a + b)
SSE_LANES(paddd, uint32_t, u, a + b)
SSE_LANES(paddq, uint64_t, q, a + b)
SSE_LANES(psubb, uint8_t, b, a - b)
SSE_LANES(psubw, uint16_t, w, a - b)
SSE_LANES(psubd, uint32_t, u, a - b)
SSE_LANES(psubq, uint64_t, q, a - b)
SSE_LANES(paddsb, int8_t, sb, sat8(a + b))
SSE_LANES(paddsw, int16_t, sw, sat16(a + b))
SSE_LANES(psubsb, int8_t, sb, sat8(a - b))
SSE_LANES(psubsw, int16_t, sw, sat16(a - b))
SSE_LANES(paddusb, uint8_t, b, satu8(a + b))
SSE_LANES(paddusw, uint16_t, w, satu16(a + b))
SSE_LANES(psubusb, uint8_t, b, satu8(a - b))
SSE_LANES(psubusw, uint16_t, w, satu16(a - b))
SSE_LANES(pmaxsw, int16_t, sw, a > b ? a : b)
SSE_LANES(pminsw, int16_t, sw, a < b ? a : b)
SSE_LANES(pmaxub, uint8_t, b, a > b ? a : b)
SSE_LANES(pminub, uint8_t, b, a < b ? a : b)
SSE_LANES(pmullw, uint16_t, w, a * b)
SSE_LANES(pcmpeqb, uint8_t, b, a == b ? 0xFF : 0)
SSE_LANES(pcmpeqw, uint16_t, w, a == b ? 0xFFFF : 0)
SSE_LANES(pcmpeqd, uint32_t, u, a == b ? 0xFFFFFFFFu : 0)
SSE_LANES(pcmpgtb, int8_t, sb, a > b ? -1 : 0)
SSE_LANES(pcmpgtw, int16_t, sw, a > b ? -1 : 0)
SSE_LANES(pcmpgtd, int32_t, i, a > b ? -1 : 0)
SSE_LANES(pavgb, uint8_t, b, (a + b + 1) >> 1)
SSE_LANES(pavgw, uint16_t, w, (a + b + 1) >> 1)

/* PMULUDQ: low dword of each qword, unsigned, to a qword. */
RT_INLINE X128 sse_pmuludq(X128 d, X128 s) {
    X128 r; r.q[0] = (uint64_t)d.u[0] * s.u[0]; r.q[1] = (uint64_t)d.u[2] * s.u[2]; return r;
}
RT_INLINE X128 sse_pmulhw(X128 d, X128 s) { for (int k = 0; k < 8; k++) d.sw[k] = (int16_t)(((int32_t)d.sw[k] * s.sw[k]) >> 16); return d; }

/* Shifts by a count (an immediate or the low qword of an operand); counts past the lane
 * width give 0 (logical) or the sign (arithmetic). */
#define SSE_SHIFT(name, T, lane, bits, op, arith) \
    RT_INLINE X128 sse_##name(X128 d, uint64_t c) { \
        for (int k = 0; k < (int)(16 / sizeof(T)); k++) { \
            if (c >= bits) d.lane[k] = (arith) ? (T)(d.lane[k] < 0 ? -1 : 0) : 0; \
            else d.lane[k] = (T)(d.lane[k] op (int)c); } \
        return d; }
SSE_SHIFT(psllw, uint16_t, w, 16, <<, 0)
SSE_SHIFT(pslld, uint32_t, u, 32, <<, 0)
SSE_SHIFT(psllq, uint64_t, q, 64, <<, 0)
SSE_SHIFT(psrlw, uint16_t, w, 16, >>, 0)
SSE_SHIFT(psrld, uint32_t, u, 32, >>, 0)
SSE_SHIFT(psrlq, uint64_t, q, 64, >>, 0)
SSE_SHIFT(psraw, int16_t, sw, 16, >>, 1)
SSE_SHIFT(psrad, int32_t, i, 32, >>, 1)

/* PSLLDQ/PSRLDQ: the whole XMM register by bytes. */
RT_INLINE X128 sse_pslldq(X128 d, uint64_t n) {
    X128 r = x_zero(); if (n > 15) return r; for (int k = (int)n; k < 16; k++) r.b[k] = d.b[k - n]; return r;
}
RT_INLINE X128 sse_psrldq(X128 d, uint64_t n) {
    X128 r = x_zero(); if (n > 15) return r; for (int k = 0; k < 16 - (int)n; k++) r.b[k] = d.b[k + n]; return r;
}

/* ---------------------------------------------------------------- to general registers */

RT_INLINE uint32_t sse_movmskps(X128 s) { return (s.u[0] >> 31) | ((s.u[1] >> 31) << 1) | ((s.u[2] >> 31) << 2) | ((s.u[3] >> 31) << 3); }
RT_INLINE uint32_t sse_movmskpd(X128 s) { return (uint32_t)(s.q[0] >> 63) | ((uint32_t)(s.q[1] >> 63) << 1); }
/* PMOVMSKB: `bytes` 16 (XMM) or 8 (MMX). */
RT_INLINE uint32_t sse_pmovmskb(X128 s, int bytes) { uint32_t m = 0; for (int k = 0; k < bytes; k++) m |= (uint32_t)(s.b[k] >> 7) << k; return m; }

#endif
