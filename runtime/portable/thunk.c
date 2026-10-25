/* Synthetic import addresses and shim dispatch. See thunk.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "thunk.h"

#if defined(_MSC_VER)
#define stricmp_ _stricmp
#else
#include <strings.h>
#define stricmp_ strcasecmp
#endif

typedef struct Thunk
{
    char name[96]; /* DLL!name */
    Shim shim;
    uint64_t prof_ns, prof_calls; /* the game thread's time in it since the last report */
    uint32_t calls;               /* XIVLEGACY_RECOMP_FIRSTCALLS */
} Thunk;

static Thunk* g_thunks;
static unsigned g_count, g_cap;
static const ShimDef* g_tables[32];
static unsigned g_ntables;

void thunk_register(const ShimDef* defs)
{
    if (g_ntables < sizeof g_tables / sizeof g_tables[0])
        g_tables[g_ntables++] = defs;
}

static Shim find_shim(const char* dll, const char* name)
{
    for (unsigned t = 0; t < g_ntables; ++t)
        for (const ShimDef* d = g_tables[t]; d->name; ++d)
            if (!strcmp(d->name, name) && (!d->dll || !stricmp_(d->dll, dll)))
                return d->fn;
    return NULL;
}

int thunk_has_shim(const char* dll, const char* name)
{
    return find_shim(dll, name) != NULL;
}

Shim thunk_find_shim(const char* dll, const char* name)
{
    return find_shim(dll, name);
}

uint32_t thunk_for(const char* dll, const char* name)
{
    char full[96];
    snprintf(full, sizeof full, "%s!%s", dll, name);
    for (unsigned i = 0; i < g_count; ++i)
        if (!stricmp_(g_thunks[i].name, full))
            return THUNK_BASE + i * THUNK_STRIDE;
    if (g_count == g_cap)
    {
        g_cap = g_cap ? g_cap * 2 : 512;
        if (g_cap > THUNK_MAX)
            return 0;
        g_thunks = (Thunk*)realloc(g_thunks, g_cap * sizeof *g_thunks);
    }
    Thunk* t = &g_thunks[g_count];
    memset(t, 0, sizeof *t);
    strcpy(t->name, full);
    t->shim = find_shim(dll, name);
    return THUNK_BASE + g_count++ * THUNK_STRIDE;
}

const char* thunk_name(uint32_t addr)
{
    if (addr < THUNK_BASE || (addr - THUNK_BASE) % THUNK_STRIDE)
        return NULL;
    uint32_t i = (addr - THUNK_BASE) / THUNK_STRIDE;
    return i < g_count ? g_thunks[i].name : NULL;
}

/* XIVLEGACY_RECOMP_TRACE=1: every shim call, with its call site, first four arguments and the result. */
static int g_trace = -1;

void (*thunk_timer)(uint64_t ns);
uint32_t thunk_prof_thread;
static _Thread_local int t_depth;

/* the shims the game thread spent the most time in since the last call, one line, then reset */
void thunk_prof_report(void)
{
    unsigned top[8] = { 0 }, n = 0;
    for (unsigned i = 0; i < g_count; ++i)
    {
        if (!g_thunks[i].prof_calls)
            continue;
        unsigned k = n < 8 ? n++ : 8;
        while (k > 0 && g_thunks[top[k - 1]].prof_ns < g_thunks[i].prof_ns)
        {
            if (k < 8)
                top[k] = top[k - 1];
            k--;
        }
        if (k < 8)
            top[k] = i;
    }
    if (!n)
        return;
    char line[1024];
    int o = snprintf(line, sizeof line, "[gfx]   API time (2 s):");
    for (unsigned k = 0; k < n && o < (int)sizeof line - 100; ++k)
    {
        const Thunk* t = &g_thunks[top[k]];
        const char* nm = strchr(t->name, '!');
        o += snprintf(line + o, sizeof line - (size_t)o, " %s %.0f ms/%llu", nm ? nm + 1 : t->name, (double)t->prof_ns * 1e-6,
            (unsigned long long)t->prof_calls);
    }
    rt_log("%s\n", line);
    for (unsigned i = 0; i < g_count; ++i)
        g_thunks[i].prof_ns = g_thunks[i].prof_calls = 0;
}

static struct
{
    uint32_t addr;
    Shim fn;
} g_at[16];
static unsigned g_nat;

void thunk_register_at(uint32_t addr, Shim fn)
{
    if (g_nat < 16)
        g_at[g_nat].addr = addr, g_at[g_nat++].fn = fn;
}

int thunk_dispatch(Guest* g, uint32_t target)
{
    for (unsigned i = 0; i < g_nat; ++i)
        if (g_at[i].addr == target)
        {
            g_at[i].fn(g);
            return 1;
        }
    const char* name = thunk_name(target);
    if (!name)
        return 0;
    Shim s = g_thunks[(target - THUNK_BASE) / THUNK_STRIDE].shim;
    if (!s)
    {
        char buf[160];
        snprintf(buf, sizeof buf, "no shim yet for %s (called from %08x)", name, rd32(g->esp));
        rt_fatal(g, target, buf);
    }
    /* XIVLEGACY_RECOMP_TRACE=1: every call; any other value: the calls whose name contains it (WS2_32 ...) */
    static const char* trace_filter;
    if (g_trace < 0)
    {
        const char* e = getenv("XIVLEGACY_RECOMP_TRACE");
        g_trace = e && *e;
        if (g_trace && strcmp(e, "1"))
            trace_filter = e;
    }
    static int first_calls = -1;
    if (first_calls < 0)
    {
        const char* e = getenv("XIVLEGACY_RECOMP_FIRSTCALLS");
        first_calls = e && *e == '1';
    }
    if (first_calls && !(g_trace && !trace_filter))
    {
        /* each import's first call: what the game starts doing at a given moment (a menu, an
         * error), in order, without a full trace's volume */
        Thunk* t = &g_thunks[(target - THUNK_BASE) / THUNK_STRIDE];
        uint32_t site = rd32(g->esp), a0 = rd32(g->esp + 4), a1 = rd32(g->esp + 8), a2 = rd32(g->esp + 12);
        t_depth++;
        s(g);
        t_depth--;
        if (t->calls++ == 0)
            rt_log("[first] %08x %-44s (%08x %08x %08x) = %08x\n", site, name, a0, a1, a2, g->eax);
        return 1;
    }
    if (g_trace && (!trace_filter || strstr(name, trace_filter)))
    {
        uint32_t site = rd32(g->esp), a0 = rd32(g->esp + 4), a1 = rd32(g->esp + 8), a2 = rd32(g->esp + 12), a3 = rd32(g->esp + 16);
        s(g);
        rt_log("[trace] %08x %-44s (%08x %08x %08x %08x) = %08x\n", site, name, a0, a1, a2, a3, g->eax);
        return 1;
    }
    if (thunk_timer && !t_depth)
    {
        uint64_t t0 = rt_monotonic_ns();
        t_depth++;
        s(g);
        t_depth--;
        uint64_t ns = rt_monotonic_ns() - t0;
        thunk_timer(ns);
        if (plat_thread_id() == thunk_prof_thread)
        {
            Thunk* t = &g_thunks[(target - THUNK_BASE) / THUNK_STRIDE];
            t->prof_ns += ns, t->prof_calls++;
        }
        return 1;
    }
    t_depth++;
    s(g);
    t_depth--;
    return 1;
}

unsigned thunk_report_missing(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < g_count; ++i)
        if (!g_thunks[i].shim)
        {
            rt_log("[recomp]   no shim: %s\n", g_thunks[i].name);
            n++;
        }
    rt_log("[recomp] %u bound imports have no shim yet\n", n);
    return n;
}
