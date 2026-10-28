/* Indirect calls and jumps from translated code with the general registers in host registers
 * (guest.h RT_CALLI): a translation's fast body directly, anything else (an import, a shim, code
 * outside the image) with the registers in g through rt_call_indirect. Included by what holds the
 * translation: runtime.c in one program, xiv_module.c in the game module. */
#pragma once
#include "runtime.h"

/* not a translation: an import's thunk (a shim) or a stand-in, with the registers in g */
#ifndef RT_FAST_NATIVE
#define RT_FAST_NATIVE(g, t) rt_call_indirect((g), (t))
#endif

/* Targets seen, direct-mapped and shared by every thread: each entry one 64-bit word, the target
 * and its index in rt_table + 1, so a thread never pairs one target with another's translation.
 * (The thread-local cache rt_lookup_any keeps cost a call into the TLS resolver each time.) */
#define FAST_CACHE_BITS 15
static volatile uint64_t g_fast_cache[1u << FAST_CACHE_BITS];

static int fast_find(uint32_t addr)
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

static inline int fast_index(uint32_t target)
{
    volatile uint64_t* e = &g_fast_cache[(target ^ (target >> FAST_CACHE_BITS)) & ((1u << FAST_CACHE_BITS) - 1)];
    uint64_t v = *e;
    if ((uint32_t)v == target && (v >> 32))
        return (int)(v >> 32) - 1;
    /* only addresses inside the image can be translations (as lookup_uncached) */
    uint32_t base = rt_image_base + rt_reloc_delta;
    int i = target - base < rt_image_size ? fast_find(target - rt_reloc_delta) : -1;
    if (i >= 0)
        *e = (uint64_t)target | (uint64_t)(uint32_t)(i + 1) << 32;
    return i;
}

RtRegs RT_FASTCC rt_call_indirect_r(Guest* g, uint32_t target, uint64_t r_a, uint64_t r_b, uint64_t r_c, uint64_t r_d)
{
    int i = fast_index(target);
    if (i >= 0)
        return rt_table[i].fast(g, r_a, r_b, r_c, r_d);
    RT_REGS_TO_G;
    RT_FAST_NATIVE(g, target);
    return RT_REGS_G;
}
