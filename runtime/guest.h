/* Guest state and primitives shared by the runtime and every generated function.
 *
 * Generated code keeps registers and flags in C locals (see REGS_DECL/REGS_LOAD/REGS_STORE) and
 * writes them back to Guest only around calls and returns, so the C compiler can keep them in
 * host registers and drop flag computations nobody reads.
 *
 * Guest memory is a 4 GB window at GUEST_BASE. On the R2 target (32-bit Windows) GUEST_BASE is 0:
 * guest addresses are host addresses, and the game's own pointers work unchanged.
 */
#pragma once

#include <math.h>
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#if defined(RT_GUEST_WINDOW)
/* 64-bit hosts (R3: the Windows x64 target and arm64 macOS): guest memory is a reserved 4 GB
 * window somewhere in the host address space; runtime/portable/gwin.c sets its base. */
extern unsigned char* rt_guest_base;
#if defined(RT_GUEST_FIXED_BASE)
/* the window at a fixed address (gwin.c reserves exactly there): every guest access is a constant
 * plus the address. Through the variable, each guest store (which may alias anything, under
 * -fno-strict-aliasing) made the next access reload the base. */
#define GUEST_PTR(a) ((unsigned char*)(uintptr_t)((uint64_t)(RT_GUEST_FIXED_BASE) + (uint32_t)(a)))
#else
#define GUEST_PTR(a) (rt_guest_base + (uint32_t)(a))
#endif
#else
#ifndef GUEST_BASE
#define GUEST_BASE 0u
#endif
#define GUEST_PTR(a) ((unsigned char*)(uintptr_t)(GUEST_BASE + (uint32_t)(a)))
#endif

/* The helpers below are a few instructions each and run in the hottest translated code (an x87
 * push per float load). MSVC inlines to a budget per function, and a large translated function
 * spends it early, leaving these as calls; clang inlines them without being told. */
#if defined(_MSC_VER)
#define RT_INLINE static __forceinline
#else
#define RT_INLINE static inline
#endif

RT_INLINE uint8_t rd8(uint32_t a) { return *GUEST_PTR(a); }
RT_INLINE uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, GUEST_PTR(a), 2); return v; }
RT_INLINE uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, GUEST_PTR(a), 4); return v; }
RT_INLINE uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, GUEST_PTR(a), 8); return v; }
RT_INLINE void wr8(uint32_t a, uint8_t v) { *GUEST_PTR(a) = v; }
RT_INLINE void wr16(uint32_t a, uint16_t v) { memcpy(GUEST_PTR(a), &v, 2); }
RT_INLINE void wr32(uint32_t a, uint32_t v) { memcpy(GUEST_PTR(a), &v, 4); }
RT_INLINE void wr64(uint32_t a, uint64_t v) { memcpy(GUEST_PTR(a), &v, 8); }
RT_INLINE float rdf32(uint32_t a) { float v; memcpy(&v, GUEST_PTR(a), 4); return v; }
RT_INLINE double rdf64(uint32_t a) { double v; memcpy(&v, GUEST_PTR(a), 8); return v; }
RT_INLINE void wrf32(uint32_t a, float v) { memcpy(GUEST_PTR(a), &v, 4); }
RT_INLINE void wrf64(uint32_t a, double v) { memcpy(GUEST_PTR(a), &v, 8); }

/* through a host pointer into the guest window (the translation's [reg + disp] operands) */
RT_INLINE uint8_t rdp8(const unsigned char* p) { return *p; }
RT_INLINE uint16_t rdp16(const unsigned char* p) { uint16_t v; memcpy(&v, p, 2); return v; }
RT_INLINE uint32_t rdp32(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
RT_INLINE uint64_t rdp64(const unsigned char* p) { uint64_t v; memcpy(&v, p, 8); return v; }
RT_INLINE void wrp8(unsigned char* p, uint8_t v) { *p = v; }
RT_INLINE void wrp16(unsigned char* p, uint16_t v) { memcpy(p, &v, 2); }
RT_INLINE void wrp32(unsigned char* p, uint32_t v) { memcpy(p, &v, 4); }
RT_INLINE void wrp64(unsigned char* p, uint64_t v) { memcpy(p, &v, 8); }
RT_INLINE float rdpf32(const unsigned char* p) { float v; memcpy(&v, p, 4); return v; }
RT_INLINE double rdpf64(const unsigned char* p) { double v; memcpy(&v, p, 8); return v; }
RT_INLINE void wrpf32(unsigned char* p, float v) { memcpy(p, &v, 4); }
RT_INLINE void wrpf64(unsigned char* p, double v) { memcpy(p, &v, 8); }

#include "sse.h" /* X128 and the SSE/MMX helpers; needs rd64/wr64 above */

typedef struct Guest
{
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, pf, af, zf, sf, of, df;
    uint32_t fs_base; /* guest TEB: fs:[0] is the guest's SEH chain, never the host's */

    /* x87: values held as double. Precision control is honoured by rounding results to float
     * when PC selects 24 bits (Direct3D 8 sets that on its thread unless FPU_PRESERVE). */
    double st[8];
    uint32_t top;
    uint16_t fcw;
    uint8_t c0, c1, c2, c3;

    /* SSE/SSE2 and MMX (ffxivgame.exe uses both; see sse.h). MMX is kept apart from the x87
     * stack: the game issues EMMS between them, so the aliasing is never observed. */
    X128 xmm[8];
    uint64_t mm[8];
    uint32_t mxcsr; /* MXCSR_DEFAULT at thread start */
} Guest;

typedef void (*GuestFn)(Guest*);

/* Where the game image really is. The translation is made at the image's preferred base; an
 * image with relocations may be mapped elsewhere. Every address constant the image's relocation
 * table covers is emitted as `constant + RD`; jump-table entries and indirect-call targets are
 * turned back into static addresses before lookup. RD is 0 when the image sits at its preferred
 * base (always, for ffxivgame.exe: it has no relocations). */
extern uint32_t rt_reloc_delta;
extern uint32_t rt_image_lo, rt_image_hi; /* runtime range of the image */
#define RD rt_reloc_delta

#define RT_GB_DECL (void)0
#define REGS_DECL                                                                                \
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;                                             \
    unsigned cf, pf, af, zf, sf, of, df;                                                         \
    RT_GB_DECL;                                                                                  \
    (void)af
#define REGS_LOAD                                                                                \
    eax = g->eax; ecx = g->ecx; edx = g->edx; ebx = g->ebx;                                      \
    esp = g->esp; ebp = g->ebp; esi = g->esi; edi = g->edi;                                      \
    cf = g->cf; pf = g->pf; af = g->af; zf = g->zf; sf = g->sf; of = g->of; df = g->df
#define REGS_STORE                                                                               \
    g->eax = eax; g->ecx = ecx; g->edx = edx; g->ebx = ebx;                                      \
    g->esp = esp; g->ebp = ebp; g->esi = esi; g->edi = edi;                                      \
    g->cf = (uint8_t)cf; g->pf = (uint8_t)pf; g->af = (uint8_t)af; g->zf = (uint8_t)zf;          \
    g->sf = (uint8_t)sf; g->of = (uint8_t)of; g->df = (uint8_t)df

/* Between translated functions the general registers travel in host registers: each function has a
 * fast body f_X_r taking and returning them packed two to a 64-bit value (swiftcall returns four
 * such in registers), and an entry f_X(Guest*) for the runtime with them in g. Through g each call
 * was ~45 loads and stores, half the drawing thread's time in translated code. The flags and DF
 * still go through g (REGS_STORE_R at returns and tail jumps, REGS_IN at entry), as do the x87 and
 * XMM registers. */
typedef struct
{
    uint64_t a, b, c, d; /* eax|edx, ecx|esp, ebx|ebp, esi|edi */
} RtRegs;
#if defined(__has_attribute)
#if __has_attribute(swiftcall)
#define RT_FASTCC __attribute__((swiftcall))
#endif
#endif
#ifndef RT_FASTCC
#define RT_FASTCC
#endif
#define RT_PACK(lo, hi) ((uint64_t)(uint32_t)(lo) | (uint64_t)(uint32_t)(hi) << 32)
#define RT_FAST_PARAMS Guest* g, uint64_t r_a, uint64_t r_b, uint64_t r_c, uint64_t r_d
#define RT_UNPACK(a_, b_, c_, d_)                                                                  \
    eax = (uint32_t)(a_); edx = (uint32_t)((a_) >> 32); ecx = (uint32_t)(b_); esp = (uint32_t)((b_) >> 32); \
    ebx = (uint32_t)(c_); ebp = (uint32_t)((c_) >> 32); esi = (uint32_t)(d_); edi = (uint32_t)((d_) >> 32)
#define REGS_IN                                                                                  \
    RT_UNPACK(r_a, r_b, r_c, r_d);                                                               \
    cf = g->cf; pf = g->pf; af = g->af; zf = g->zf; sf = g->sf; of = g->of; df = g->df
#define RT_REGS_OUT ((RtRegs){ RT_PACK(eax, edx), RT_PACK(ecx, esp), RT_PACK(ebx, ebp), RT_PACK(esi, edi) })
#define RT_REGS_G ((RtRegs){ RT_PACK(g->eax, g->edx), RT_PACK(g->ecx, g->esp), RT_PACK(g->ebx, g->ebp), RT_PACK(g->esi, g->edi) })
#define RT_REGS_TO_G                                                                             \
    g->eax = (uint32_t)r_a; g->edx = (uint32_t)(r_a >> 32); g->ecx = (uint32_t)r_b; g->esp = (uint32_t)(r_b >> 32); \
    g->ebx = (uint32_t)r_c; g->ebp = (uint32_t)(r_c >> 32); g->esi = (uint32_t)r_d; g->edi = (uint32_t)(r_d >> 32)
#define REGS_STORE_R                                                                             \
    g->cf = (uint8_t)cf; g->pf = (uint8_t)pf; g->af = (uint8_t)af; g->zf = (uint8_t)zf;          \
    g->sf = (uint8_t)sf; g->of = (uint8_t)of; g->df = (uint8_t)df
#define REGS_STORE_NF_R g->df = (uint8_t)df
/* a return stores the flags too: dropping them (only DF) made the game fail at its title screen,
 * so some translated code reads flags a callee left, through a function entry (REGS_IN) */
#define REGS_STORE_RET REGS_STORE_R
#define REGS_LOAD_NF_R df = g->df
#define RT_CALL(fn)                                                                              \
    do { RtRegs r_ = fn(g, RT_PACK(eax, edx), RT_PACK(ecx, esp), RT_PACK(ebx, ebp), RT_PACK(esi, edi)); \
         RT_UNPACK(r_.a, r_.b, r_.c, r_.d); } while (0)
#define RT_TAIL(fn) return fn(g, RT_PACK(eax, edx), RT_PACK(ecx, esp), RT_PACK(ebx, ebp), RT_PACK(esi, edi))
typedef RtRegs(RT_FASTCC* RtFastFn)(RT_FAST_PARAMS);
/* a call or jump through a pointer (rt_fastcall.h) */
RtRegs RT_FASTCC rt_call_indirect_r(Guest* g, uint32_t target, uint64_t r_a, uint64_t r_b, uint64_t r_c, uint64_t r_d);
#define RT_CALLI(t)                                                                              \
    do { RtRegs r_ = rt_call_indirect_r(g, (t), RT_PACK(eax, edx), RT_PACK(ecx, esp), RT_PACK(ebx, ebp), RT_PACK(esi, edi)); \
         RT_UNPACK(r_.a, r_.b, r_.c, r_.d); } while (0)
#define RT_JUMPI(t) return rt_call_indirect_r(g, (t), RT_PACK(eax, edx), RT_PACK(ecx, esp), RT_PACK(ebx, ebp), RT_PACK(esi, edi))
#define RT_WRAP(fn)                                                                              \
    do { RtRegs r_ = fn(g, RT_PACK(g->eax, g->edx), RT_PACK(g->ecx, g->esp), RT_PACK(g->ebx, g->ebp), RT_PACK(g->esi, g->edi)); \
         g->eax = (uint32_t)r_.a; g->edx = (uint32_t)(r_.a >> 32); g->ecx = (uint32_t)r_.b; g->esp = (uint32_t)(r_.b >> 32); \
         g->ebx = (uint32_t)r_.c; g->ebp = (uint32_t)(r_.c >> 32); g->esi = (uint32_t)r_.d; g->edi = (uint32_t)(r_.d >> 32); } while (0)

/* Around calls: the arithmetic flags are not carried across a call (x86 code never relies on them
 * there), only the registers and DF. Function entries and tail jumps keep REGS_LOAD / REGS_STORE:
 * code discovery may split functions where flags flow on. */
#define REGS_LOAD_NF                                                                             \
    eax = g->eax; ecx = g->ecx; edx = g->edx; ebx = g->ebx;                                      \
    esp = g->esp; ebp = g->ebp; esi = g->esi; edi = g->edi; df = g->df
#define REGS_STORE_NF                                                                            \
    g->eax = eax; g->ecx = ecx; g->edx = edx; g->ebx = ebx;                                      \
    g->esp = esp; g->ebp = ebp; g->esi = esi; g->edi = edi; g->df = (uint8_t)df

/* PF: set when the low byte has an even number of 1 bits. 0x6996 is the parity of each nibble. */
#define PARITY(v) ((unsigned)(!((0x6996u >> ((((uint32_t)(v) & 0xFFu) ^ (((uint32_t)(v) & 0xFFu) >> 4)) & 0xFu)) & 1u)))

/* --- runtime entry points (runtime.c) -------------------------------------------------------- */

void rt_call_indirect(Guest* g, uint32_t target);
#if defined(_MSC_VER)
__declspec(noreturn)
#elif defined(__GNUC__)
__attribute__((noreturn))
#endif
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void rt_fatal(Guest* g, uint32_t addr, const char* what);
void rt_cpuid(Guest* g);
void rt_rdtsc(Guest* g);

/* setjmp and longjmp: the game's C library's _setjmp3 and longjmp (recomp.py finds them by their
 * code). The C library's longjmp ends in a jump to the setjmp's return address, the middle of a
 * translated function, which no translation can take. So a call to _setjmp3 also leaves a host
 * landing there (RT_SETJMP, in the translated caller's own frame), and longjmp is translated as
 * rt_longjmp: the guest registers the jmp_buf holds are put back, as the C library would, and the
 * host returns to the landing, where the translation goes on after the call with eax = the value.
 *
 * rt_setjmp_buf: storage for the landing of the jmp_buf the call is given ([esp + 4], its return
 * address at esp), per guest thread. rt_longjmp_regs: puts the registers back and returns the
 * landing's storage (never returns when there is none). The host's setjmp and longjmp are called
 * here, in the translation, so both are the same C library's even if the translation and
 * the host were built by different compilers (Windows: MSVC and MinGW jmp_bufs differ). */
#define RT_JMP_BYTES 512 /* room for any C library's jmp_buf */
void* rt_setjmp_buf(Guest* g);
void* rt_longjmp_regs(Guest* g);
#if defined(_WIN32)
#define RT_SETJMP(g) setjmp(*(jmp_buf*)rt_setjmp_buf(g))
#define RT_HOST_LONGJMP(b) longjmp(*(jmp_buf*)(b), 1)
#else
#define RT_SETJMP(g) _setjmp(*(jmp_buf*)rt_setjmp_buf(g)) /* no signal mask to save */
#define RT_HOST_LONGJMP(b) _longjmp(*(jmp_buf*)(b), 1)
#endif
typedef char rt_jmp_bytes_fit[sizeof(jmp_buf) <= RT_JMP_BYTES ? 1 : -1];
static inline void rt_longjmp(Guest* g)
{
    RT_HOST_LONGJMP(rt_longjmp_regs(g));
}

/* The guest-wide lock: one guest thread runs
 * translated code at a time. The platform bridge releases it around every native call and takes
 * it on every host->guest entry; a loop that never calls out would starve the other guest threads,
 * so every backward jump is a safepoint that yields when another thread is waiting. The same
 * hand-off is what gives arm64 x86's memory ordering for the guest. */
#ifdef XIVLEGACY_SPLIT
/* the flag is the game module's (xiv_game.h), so its translation reads it directly */
extern volatile uint32_t* rt_lock_contended_ptr;
#define rt_lock_contended (*rt_lock_contended_ptr)
#else
extern volatile uint32_t rt_lock_contended;
#endif
void rt_safepoint(void);
#define RT_SAFEPOINT do { if (rt_lock_contended) rt_safepoint(); } while (0)

#define RT_UNIMPL(addr, text) do { REGS_STORE; rt_fatal(g, (addr), "unimplemented: " text); } while (0)
#define RT_BADJUMP(addr) do { REGS_STORE; rt_fatal(g, (addr), "jump to an address with no translation"); } while (0)
#define RT_DIVIDE(addr) do { REGS_STORE; rt_fatal(g, (addr), "divide error (#DE)"); } while (0)
#define RT_TRAP(addr) do { REGS_STORE; rt_fatal(g, (addr), "int3 reached"); } while (0)

/* --- x87 ------------------------------------------------------------------------------------- */

#define ST(i) (g->st[(g->top + (i)) & 7u])

RT_INLINE void fpush(Guest* g, double v)
{
    g->top = (g->top - 1) & 7u;
    g->st[g->top] = v;
}

RT_INLINE double fpop(Guest* g)
{
    double v = g->st[g->top];
    g->top = (g->top + 1) & 7u;
    return v;
}

/* Precision control: PC=00 (24-bit) rounds every result to single precision. */
RT_INLINE double fr_cw(unsigned cw, double x)
{
    return ((cw >> 8) & 3u) == 0 ? (double)(float)x : x;
}
RT_INLINE double fr(Guest* g, double x) { return fr_cw(g->fcw, x); }

/* FCOM/FUCOM/FTST/FICOM: C3 C2 C0 = 000 greater, 001 less, 100 equal, 111 unordered. */
RT_INLINE void fcom(Guest* g, double a, double b)
{
    if (a != a || b != b) { g->c3 = 1; g->c2 = 1; g->c0 = 1; }
    else if (a > b) { g->c3 = 0; g->c2 = 0; g->c0 = 0; }
    else if (a < b) { g->c3 = 0; g->c2 = 0; g->c0 = 1; }
    else { g->c3 = 1; g->c2 = 0; g->c0 = 0; }
    g->c1 = 0;
}

RT_INLINE uint16_t fstsw_top(Guest* g, unsigned top)
{
    return (uint16_t)((g->c3 << 14) | ((top & 7u) << 11) | (g->c2 << 10) | (g->c1 << 9) | (g->c0 << 8));
}
RT_INLINE uint16_t fstsw(Guest* g) { return fstsw_top(g, g->top); }

/* Round per the control word's RC field (00 nearest-even, 01 down, 10 up, 11 truncate). */
RT_INLINE double frnd_cw(unsigned cw, double x)
{
    switch ((cw >> 10) & 3u)
    {
    case 0: return nearbyint(x); /* host default rounding mode is nearest-even */
    case 1: return floor(x);
    case 2: return ceil(x);
    default: return trunc(x);
    }
}
RT_INLINE double frnd(Guest* g, double x) { return frnd_cw(g->fcw, x); }

/* FIST/FISTP: out of range or NaN stores the "integer indefinite" value. */
RT_INLINE int16_t fist16_cw(unsigned cw, double x)
{
    double r = frnd_cw(cw, x);
    return (r != r || r < -32768.0 || r > 32767.0) ? (int16_t)0x8000 : (int16_t)r;
}
RT_INLINE int32_t fist32_cw(unsigned cw, double x)
{
    double r = frnd_cw(cw, x);
    return (r != r || r < -2147483648.0 || r > 2147483647.0) ? (int32_t)0x80000000u : (int32_t)r;
}
RT_INLINE int64_t fist64_cw(unsigned cw, double x)
{
    double r = frnd_cw(cw, x);
    return (r != r || r < -9223372036854775808.0 || r >= 9223372036854775808.0) ? (int64_t)0x8000000000000000ull : (int64_t)r;
}
RT_INLINE int16_t fist16(Guest* g, double x) { return fist16_cw(g->fcw, x); }
RT_INLINE int32_t fist32(Guest* g, double x) { return fist32_cw(g->fcw, x); }
RT_INLINE int64_t fist64(Guest* g, double x) { return fist64_cw(g->fcw, x); }

/* --- x87 in locals: what the recompiler emits for functions with x87 code (recomp/x86c.py) ------
 * g->st[(g->top + i) & 7] as the stack cannot stay in registers: every guest store goes through a
 * byte pointer that may alias g, so each ST(i) is a reload and an index computation. In these
 * functions ST(i) is the local x87_s<i> instead, always ST(0) first. A push or pop renames them
 * (the compiler's copy propagation makes the rotation free), and a pop moves the old ST(0) to
 * ST(7), as the physical register stack does, so the state written back is exact. g has it at
 * every REGS_STORE and REGS_LOAD, and around the few instructions that take all of it. */
#define X87_DECL                                                                                 \
    double x87_s0, x87_s1, x87_s2, x87_s3, x87_s4, x87_s5, x87_s6, x87_s7, x87_t;                \
    unsigned x87_top, x87_cw;                                                                    \
    (void)x87_t
#define X87_LOAD                                                                                 \
    x87_top = g->top & 7u; x87_cw = g->fcw;                                                      \
    x87_s0 = g->st[x87_top]; x87_s1 = g->st[(x87_top + 1) & 7u];                                 \
    x87_s2 = g->st[(x87_top + 2) & 7u]; x87_s3 = g->st[(x87_top + 3) & 7u];                      \
    x87_s4 = g->st[(x87_top + 4) & 7u]; x87_s5 = g->st[(x87_top + 5) & 7u];                      \
    x87_s6 = g->st[(x87_top + 6) & 7u]; x87_s7 = g->st[(x87_top + 7) & 7u]
#define X87_STORE                                                                                \
    g->top = x87_top; g->fcw = (uint16_t)x87_cw;                                                 \
    g->st[x87_top] = x87_s0; g->st[(x87_top + 1) & 7u] = x87_s1;                                 \
    g->st[(x87_top + 2) & 7u] = x87_s2; g->st[(x87_top + 3) & 7u] = x87_s3;                      \
    g->st[(x87_top + 4) & 7u] = x87_s4; g->st[(x87_top + 5) & 7u] = x87_s5;                      \
    g->st[(x87_top + 6) & 7u] = x87_s6; g->st[(x87_top + 7) & 7u] = x87_s7
#define X87_PUSH(v)                                                                              \
    do                                                                                           \
    {                                                                                            \
        double x87_v_ = (v);                                                                     \
        x87_s7 = x87_s6; x87_s6 = x87_s5; x87_s5 = x87_s4; x87_s4 = x87_s3;                      \
        x87_s3 = x87_s2; x87_s2 = x87_s1; x87_s1 = x87_s0; x87_s0 = x87_v_;                      \
        x87_top = (x87_top - 1) & 7u;                                                            \
    } while (0)
#define X87_POP()                                                                                \
    (x87_t = x87_s0, x87_s0 = x87_s1, x87_s1 = x87_s2, x87_s2 = x87_s3, x87_s3 = x87_s4,         \
        x87_s4 = x87_s5, x87_s5 = x87_s6, x87_s6 = x87_s7, x87_s7 = x87_t,                       \
        x87_top = (x87_top + 1) & 7u, x87_t)

/* FXAM: C3 C2 C0 = 001 NaN, 010 normal, 011 infinity, 100 zero, 110 denormal; C1 = sign. */
RT_INLINE void fxam(Guest* g)
{
    double x = ST(0);
    g->c1 = signbit(x) ? 1 : 0;
    switch (fpclassify(x))
    {
    case FP_NAN: g->c3 = 0; g->c2 = 0; g->c0 = 1; break;
    case FP_INFINITE: g->c3 = 0; g->c2 = 1; g->c0 = 1; break;
    case FP_ZERO: g->c3 = 1; g->c2 = 0; g->c0 = 0; break;
    case FP_SUBNORMAL: g->c3 = 1; g->c2 = 1; g->c0 = 0; break;
    default: g->c3 = 0; g->c2 = 1; g->c0 = 0; break;
    }
}

/* FNSTENV/FLDENV (28-byte protected-mode environment) and FNSAVE/FRSTOR (environment + the eight
 * registers as 80-bit, ST(0) first). Tags are not modelled; a save/restore pair round-trips. */
RT_INLINE void wrf80(uint32_t a, double v);
RT_INLINE double rdf80(uint32_t a);
RT_INLINE void fenv_store(Guest* g, uint32_t a)
{
    wr32(a + 0, 0xFFFF0000u | g->fcw);
    wr32(a + 4, 0xFFFF0000u | fstsw(g));
    wr32(a + 8, 0xFFFFFFFFu);
    for (uint32_t k = 12; k < 28; k += 4)
        wr32(a + k, 0);
}
RT_INLINE void fenv_load(Guest* g, uint32_t a)
{
    uint16_t sw = rd16(a + 4);
    g->fcw = rd16(a + 0);
    g->top = (sw >> 11) & 7u;
    g->c0 = (sw >> 8) & 1u; g->c1 = (sw >> 9) & 1u; g->c2 = (sw >> 10) & 1u; g->c3 = (sw >> 14) & 1u;
}
RT_INLINE void fsave(Guest* g, uint32_t a)
{
    fenv_store(g, a);
    for (uint32_t i = 0; i < 8; ++i)
        wrf80(a + 28 + 10 * i, ST(i));
    g->fcw = 0x037F; g->top = 0; g->c0 = g->c1 = g->c2 = g->c3 = 0; /* FNSAVE ends with FNINIT */
}
RT_INLINE void frstor(Guest* g, uint32_t a)
{
    fenv_load(g, a);
    for (uint32_t i = 0; i < 8; ++i)
        ST(i) = rdf80(a + 28 + 10 * i);
}

/* 80-bit extended <-> double. */
RT_INLINE double rdf80(uint32_t a)
{
    uint64_t m;
    uint16_t se;
    memcpy(&m, GUEST_PTR(a), 8);
    memcpy(&se, GUEST_PTR(a) + 8, 2);
    int s = se >> 15, e = se & 0x7FFF;
    double v;
    if (e == 0 && m == 0) v = 0.0;
    else if (e == 0x7FFF) v = (m << 1) ? NAN : INFINITY;
    else v = ldexp((double)m, e - 16383 - 63);
    return s ? -v : v;
}

RT_INLINE void wrf80(uint32_t a, double v)
{
    uint64_t m = 0;
    uint16_t se = signbit(v) ? 0x8000 : 0;
    if (v != v) { se |= 0x7FFF; m = 0xC000000000000000ull; }
    else if (isinf(v)) { se |= 0x7FFF; m = 0x8000000000000000ull; }
    else if (v != 0.0)
    {
        int e;
        double f = frexp(fabs(v), &e); /* v = f * 2^e, f in [0.5, 1) */
        m = (uint64_t)ldexp(f, 64);
        se |= (uint16_t)(e - 1 + 16383);
    }
    memcpy(GUEST_PTR(a), &m, 8);
    memcpy(GUEST_PTR(a) + 8, &se, 2);
}

/* Translated functions (RT_TRANSLATION, set by the translation's funcs.h): the window's base is a
 * local the compiler cannot see through, so it stays in a register and an access [reg + disp] is
 * one load or store with the displacement folded in. With the base a constant, clang formed each
 * address anew (add, orr with the base, then the access). The helpers above keep the plain form. */
#if defined(RT_TRANSLATION) && defined(RT_GUEST_WINDOW) && defined(RT_GUEST_FIXED_BASE) && (defined(__clang__) || defined(__GNUC__))
RT_INLINE unsigned char* rt_opaque_base(void)
{
    unsigned char* p = (unsigned char*)(uintptr_t)(RT_GUEST_FIXED_BASE);
    __asm__("" : "+r"(p));
    return p;
}
#undef RT_GB_DECL
#define RT_GB_DECL unsigned char* const rt_gb = rt_opaque_base()
#undef GUEST_PTR
#define GUEST_PTR(a) (rt_gb + (uint32_t)(a))
#define rd8(a) rdp8(GUEST_PTR(a))
#define rd16(a) rdp16(GUEST_PTR(a))
#define rd32(a) rdp32(GUEST_PTR(a))
#define rd64(a) rdp64(GUEST_PTR(a))
#define wr8(a, v) wrp8(GUEST_PTR(a), (v))
#define wr16(a, v) wrp16(GUEST_PTR(a), (v))
#define wr32(a, v) wrp32(GUEST_PTR(a), (v))
#define wr64(a, v) wrp64(GUEST_PTR(a), (v))
#define rdf32(a) rdpf32(GUEST_PTR(a))
#define rdf64(a) rdpf64(GUEST_PTR(a))
#define wrf32(a, v) wrpf32(GUEST_PTR(a), (v))
#define wrf64(a, v) wrpf64(GUEST_PTR(a), (v))
#endif
