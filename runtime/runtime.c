/* Runtime for generated code: indirect-call dispatch, traps, CPU identification. */
#include "runtime.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* rt_table (table.c, generated): every translated function, sorted by guest address. */
int rt_index(uint32_t addr)
{
    unsigned lo = 0, hi = rt_table_count;
    while (lo < hi)
    {
        unsigned mid = (lo + hi) / 2;
        if (rt_table[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (lo < rt_table_count && rt_table[lo].addr == addr) ? (int)lo : -1;
}

GuestFn rt_lookup(uint32_t addr)
{
    int i = rt_index(addr);
    return i >= 0 ? rt_table[i].fn : NULL;
}

static RtNativeHandler g_native = NULL;

void rt_set_native_handler(RtNativeHandler h) { g_native = h; }

uint32_t rt_reloc_delta;
uint32_t rt_image_lo, rt_image_hi;

void rt_set_image(uint32_t runtime_base)
{
    rt_reloc_delta = runtime_base - rt_image_base;
    rt_image_lo = runtime_base;
    rt_image_hi = runtime_base + rt_image_size;
}

static GuestFn lookup_uncached(uint32_t target)
{
    /* Only addresses inside the image can be translated functions; anything else is native, and
     * must not be mistaken for one after subtracting a relocation delta. */
    if (target >= rt_image_lo && target < rt_image_hi)
        return rt_lookup(target - rt_reloc_delta);
    return NULL;
}

/* A direct-mapped cache of indirect-call targets, one per thread (no locking): C++ code makes a
 * virtual call every few instructions, and the binary search over the table is 17 steps of cache
 * misses. */
#define CALL_CACHE_BITS 15
typedef struct
{
    uint32_t addr;
    GuestFn fn;
} CallCacheEnt;
static _Thread_local CallCacheEnt t_call_cache[1u << CALL_CACHE_BITS];

GuestFn rt_lookup_any(uint32_t target)
{
    CallCacheEnt* e = &t_call_cache[(target ^ (target >> CALL_CACHE_BITS)) & ((1u << CALL_CACHE_BITS) - 1)];
    if (e->addr == target && e->fn)
        return e->fn;
    GuestFn fn = lookup_uncached(target);
    if (fn)
        e->addr = target, e->fn = fn;
    return fn;
}

void rt_call_indirect(Guest* g, uint32_t target)
{
    GuestFn fn = rt_lookup_any(target);
    if (fn)
    {
        fn(g);
        return;
    }
    /* Not a translated function: an import (IAT slot holding a host API address) or a bug.
     * The platform layer decides; without one this is fatal. */
    if (g_native && g_native(g, target))
        return;
    rt_fatal(g, target, "indirect call/jump to an address with no translation");
}

/* Nonzero while a guest thread waits for the guest lock; the platform bridge maintains it and
 * installs the yield. Without a bridge (tests/difftest.c) there is one thread and no lock. */
volatile uint32_t rt_lock_contended;
static void (*g_yield)(void);

void rt_set_yield(void (*yield)(void)) { g_yield = yield; }

void rt_safepoint(void)
{
    if (g_yield)
        g_yield();
}

/* --- setjmp and longjmp (guest.h) --------------------------------------------------------------------
 * The landings a guest thread's _setjmp3 calls left, by jmp_buf address: a jmp_buf is filled and
 * jumped to on one thread, and a function rarely has more than one in play, so a few per thread,
 * reused oldest first, are plenty. */
#if defined(_MSC_VER)
#define RT_THREAD_LOCAL __declspec(thread)
#else
#define RT_THREAD_LOCAL _Thread_local
#endif
#define RT_JMP_SLOTS 16
typedef struct JmpSlot
{
    uint32_t guest; /* the guest jmp_buf */
    _Alignas(16) unsigned char host[RT_JMP_BYTES]; /* the landing: a jmp_buf of the translation's C library */
} JmpSlot;
static RT_THREAD_LOCAL JmpSlot g_jmp[RT_JMP_SLOTS];
static RT_THREAD_LOCAL unsigned g_jmp_next;

void* rt_setjmp_buf(Guest* g)
{
    uint32_t jb = rd32(g->esp + 4);
    for (unsigned i = 0; i < RT_JMP_SLOTS; ++i)
        if (g_jmp[i].guest == jb)
            return g_jmp[i].host;
    JmpSlot* s = &g_jmp[g_jmp_next++ % RT_JMP_SLOTS];
    s->guest = jb;
    return s->host;
}

void* rt_longjmp_regs(Guest* g)
{
    /* longjmp(jmp_buf, value): its return address at esp. The MSVC jmp_buf: ebp, ebx, edi, esi, esp
     * (at the _setjmp3 call's return address), eip, the SEH registration, ... */
    uint32_t jb = rd32(g->esp + 4), value = rd32(g->esp + 8);
    for (unsigned i = 0; i < RT_JMP_SLOTS; ++i)
        if (g_jmp[i].guest == jb)
        {
            /* what the C library's longjmp leaves: the SEH chain as it was at the setjmp (the
             * frames in between are not unwound: their handlers do not run), the callee-saved
             * registers, the stack as after the call returned, and the value (0 comes back as 1) */
            wr32(g->fs_base, rd32(jb + 0x18));
            g->ebp = rd32(jb);
            g->ebx = rd32(jb + 4);
            g->edi = rd32(jb + 8);
            g->esi = rd32(jb + 0xc);
            g->esp = rd32(jb + 0x10) + 4;
            g->eax = value ? value : 1;
            rt_log("[recomp] longjmp: back to the setjmp that returns to %08x\n", rd32(jb + 0x14));
            return g_jmp[i].host;
        }
    rt_fatal(g, jb, "longjmp to a jmp_buf no translated setjmp filled");
}

static FILE* g_log;
static RtFatalHook g_fatal_hook;

void rt_set_log(FILE* f) { g_log = f; }
void rt_set_fatal_hook(RtFatalHook h) { g_fatal_hook = h; }

void rt_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (g_log)
    {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

void rt_fatal(Guest* g, uint32_t addr, const char* what)
{
    rt_log("\n[recomp] FATAL at %08x: %s\n", addr, what);
    rt_log("  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
        g->eax, g->ecx, g->edx, g->ebx, g->esp, g->ebp, g->esi, g->edi);
    /* The guest stack, as return addresses give it: which guest functions led here. */
    rt_log("  guest stack:");
    for (int i = 0; i < 24; ++i)
        rt_log(" %08x", rd32(g->esp + 4u * i));
    rt_log("\n");
    if (g_fatal_hook)
        g_fatal_hook(addr, what);
    abort();
}

/* The CPU the game is told it runs on: the host's CPUID with every SIMD extension removed
 * (MMX, SSE*, 3DNow!), so the game and D3DX take their x87 paths. The 203 functions that use
 * SIMD are not translated; if one is ever reached it traps (RT_UNIMPL), which is the test of
 * this choice. */
void rt_cpuid(Guest* g)
{
#if defined(_M_IX86)
    int r[4];
    uint32_t leaf = g->eax;
    __cpuidex(r, (int)leaf, (int)g->ecx);
    if (leaf == 1)
    {
        r[3] &= ~((1 << 23) | (1 << 25) | (1 << 26)); /* EDX: MMX, SSE, SSE2 */
        r[2] = 0;                                     /* ECX: SSE3 and later */
    }
    else if (leaf == 7)
    {
        r[1] = r[2] = r[3] = 0; /* AVX2, AVX-512 ... */
    }
    else if (leaf == 0x80000001u)
    {
        r[3] &= ~((1u << 31) | (1 << 30) | (1 << 23) | (1 << 22)); /* 3DNow!, 3DNow!+, MMX, MMX+ */
        r[2] = 0;
    }
    g->eax = (uint32_t)r[0];
    g->ebx = (uint32_t)r[1];
    g->ecx = (uint32_t)r[2];
    g->edx = (uint32_t)r[3];
#else
    /* Every other host (R3: x64 and arm64) reports one fixed CPU, the same everywhere: a
     * Pentium III-class GenuineIntel (family 6, model 8) with FPU, TSC, CX8 and CMOV, and no MMX,
     * SSE, FXSR or extended leaves. */
    uint32_t leaf = g->eax;
    g->eax = g->ebx = g->ecx = g->edx = 0;
    if (leaf == 0)
    {
        g->eax = 1;
        g->ebx = 0x756E6547u; /* "Genu" */
        g->edx = 0x49656E69u; /* "ineI" */
        g->ecx = 0x6C65746Eu; /* "ntel" */
    }
    else if (leaf == 1)
    {
        g->eax = 0x00000683u;
        g->edx = (1u << 0) | (1u << 4) | (1u << 8) | (1u << 15); /* FPU, TSC, CX8, CMOV */
    }
    else if (leaf == 0x80000000u)
        g->eax = 0x80000000u; /* no extended leaves */
#endif
}

void rt_rdtsc(Guest* g)
{
#if defined(_M_IX86)
    unsigned long long t = __rdtsc();
#else
    unsigned long long t = rt_monotonic_ns() * 2; /* a 2 GHz TSC */
#endif
    g->eax = (uint32_t)t;
    g->edx = (uint32_t)(t >> 32);
}
