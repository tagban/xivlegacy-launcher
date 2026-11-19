/* The game host on Windows: Winsock, a crash report, then the shared start (xiv_boot.c).
 *
 *   xivlegacy-host.exe --module <xivlegacy-game.dll> <1.23b install folder> [session id]
 *
 * The macOS and Linux host is tests/boot.c, which adds POSIX debugging tools (the control FIFO,
 * the profilers) around the same start. XIVLEGACY_PROFILE works here too (below). */
#include <winsock2.h>
#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <stdlib.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_boot.h"

static LONG WINAPI on_crash(EXCEPTION_POINTERS* e)
{
    fprintf(stderr, "[host] crash: exception %#lx at %p\n", (unsigned long)e->ExceptionRecord->ExceptionCode,
        e->ExceptionRecord->ExceptionAddress);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* XIVLEGACY_PROFILE=<seconds>[:<delay>]: a sampling profiler (the Windows side of tests/boot.c's).
 * Every 2 ms each of the process's other threads is paused, its instruction pointer read and the
 * top of its stack copied. A pointer inside the game module is named by the translated function
 * holding it: the nearest function start below it in rt_table (the module's own symbols are
 * hidden). Others are named by module and export ("ntdll.dll!NtWaitForSingleObject"), or by
 * offset in the host (named afterwards with xivlegacy-host.pdb), together with the nearest return
 * address into our code found on the copied stack: who called into the system. The result, per
 * busy thread, goes to the log, which the bug-report key sends. */
typedef struct
{
    uintptr_t start;
    uint32_t guest;
} ProfStart;
static ProfStart* g_starts;
static size_t g_nstarts;
static int start_cmp(const void* a, const void* b)
{
    uintptr_t x = ((const ProfStart*)a)->start, y = ((const ProfStart*)b)->start;
    return x < y ? -1 : x > y;
}
static void prof_build_starts(void)
{
    g_starts = (ProfStart*)malloc(sizeof *g_starts * 2 * (size_t)rt_table_count);
    if (!g_starts)
        return;
    for (unsigned i = 0; i < rt_table_count; ++i)
    {
        if (rt_table[i].fn)
            g_starts[g_nstarts].start = (uintptr_t)rt_table[i].fn, g_starts[g_nstarts++].guest = rt_table[i].addr;
        if (rt_table[i].fast)
            g_starts[g_nstarts].start = (uintptr_t)rt_table[i].fast, g_starts[g_nstarts++].guest = rt_table[i].addr | 0x80000000u;
    }
    qsort(g_starts, g_nstarts, sizeof *g_starts, start_cmp);
}
/* the translated function holding pc (bit 31: its fast-call form), or 0 */
static uint32_t prof_guest_of(uintptr_t pc, uintptr_t lo, uintptr_t hi)
{
    if (pc < lo || pc >= hi || !g_nstarts || pc < g_starts[0].start)
        return 0;
    size_t a = 0, b = g_nstarts;
    while (b - a > 1)
    {
        size_t m = (a + b) / 2;
        if (g_starts[m].start <= pc)
            a = m;
        else
            b = m;
    }
    return g_starts[a].guest;
}

/* a module's name and the exported function at or below pc ("ntdll.dll!NtWaitForSingleObject"),
 * or for a module without exports its offset ("xivlegacy-host.exe+0x1234", named later with the
 * build's .pdb) */
static void prof_name_pc(uintptr_t pc, char* out, size_t cap)
{
    HMODULE m = NULL;
    char path[MAX_PATH] = "?";
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m))
    {
        snprintf(out, cap, "?");
        return;
    }
    GetModuleFileNameA(m, path, sizeof path);
    const char* base = strrchr(path, '\\');
    base = base ? base + 1 : path;
    const unsigned char* img = (const unsigned char*)m;
    uintptr_t rva = pc - (uintptr_t)m;
    const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)(img + ((const IMAGE_DOS_HEADER*)img)->e_lfanew);
    const IMAGE_DATA_DIRECTORY* dd = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const char* best = NULL;
    uint32_t best_rva = 0;
    if (dd->VirtualAddress && dd->Size)
    {
        const IMAGE_EXPORT_DIRECTORY* ed = (const IMAGE_EXPORT_DIRECTORY*)(img + dd->VirtualAddress);
        const DWORD* fns = (const DWORD*)(img + ed->AddressOfFunctions);
        const DWORD* names = (const DWORD*)(img + ed->AddressOfNames);
        const WORD* ords = (const WORD*)(img + ed->AddressOfNameOrdinals);
        for (DWORD i = 0; i < ed->NumberOfNames; ++i)
        {
            uint32_t f = fns[ords[i]];
            if (f <= rva && f >= best_rva && (f < dd->VirtualAddress || f >= dd->VirtualAddress + dd->Size))
                best_rva = f, best = (const char*)(img + names[i]);
        }
    }
    /* an export far below pc is a different, unexported function: show the offset as well */
    if (best && rva - best_rva < 0x4000)
        snprintf(out, cap, "%s!%s+0x%x", base, best, (unsigned)(rva - best_rva));
    else
        snprintf(out, cap, "%s+0x%x", base, (unsigned)rva);
    /* the exact offset inside a system function doesn't matter: fold it */
    if (best && rva - best_rva < 0x4000 && _strnicmp(base, "xivlegacy", 9))
    {
        char* plus = strrchr(out, '+');
        if (plus)
            *plus = 0;
    }
}

/* is v a return address: inside [lo,hi) and just after a call instruction */
static int prof_is_ret(uintptr_t v, uintptr_t lo, uintptr_t hi)
{
    if (v < lo + 8 || v >= hi)
        return 0;
    const unsigned char* p = (const unsigned char*)v;
    if (p[-5] == 0xE8)
        return 1; /* call rel32 */
    if (p[-2] == 0xFF && (p[-1] & 0x38) == 0x10)
        return 1; /* call reg / [reg] */
    if (p[-3] == 0xFF && (p[-2] & 0x38) == 0x10)
        return 1; /* call [reg+d8] */
    if (p[-6] == 0xFF && (p[-5] & 0x38) == 0x10)
        return 1; /* call [rip+d32] / [reg+d32] */
    if (p[-7] == 0xFF && (p[-6] & 0x38) == 0x10)
        return 1; /* call [reg+reg*s+d32] */
    if (p[-4] == 0xFF && (p[-3] & 0x38) == 0x10)
        return 1; /* call [reg+reg*s+d8] */
    return 0;
}

typedef struct
{
    DWORD tid;
    uint32_t n;
    char where[64]; /* "f_00dd58b0_r" or a module name */
    char from[64];  /* the nearest caller in our code, or "" */
} ProfHit;

enum { PROF_HASH = 1 << 16 };
static ProfHit* g_hits;
static size_t g_nh;
static ProfHit* prof_slot(DWORD tid, const char* where, const char* from)
{
    uint32_t h = 2166136261u ^ tid;
    for (const char* c = where; *c; ++c)
        h = (h ^ (unsigned char)*c) * 16777619u;
    for (const char* c = from; *c; ++c)
        h = (h ^ (unsigned char)*c) * 16777619u;
    for (uint32_t i = 0; i < PROF_HASH; ++i)
    {
        ProfHit* s = &g_hits[(h + i) & (PROF_HASH - 1)];
        if (!s->n)
        {
            if (g_nh * 4 >= PROF_HASH * 3)
                return NULL;
            g_nh++;
            s->tid = tid;
            snprintf(s->where, sizeof s->where, "%s", where);
            snprintf(s->from, sizeof s->from, "%s", from);
            return s;
        }
        if (s->tid == tid && !strcmp(s->where, where) && !strcmp(s->from, from))
            return s;
    }
    return NULL;
}

static void prof_name_ours(uintptr_t pc, uintptr_t lo, uintptr_t hi, char* out, size_t cap)
{
    uint32_t g = prof_guest_of(pc, lo, hi);
    if (g)
        snprintf(out, cap, "f_%08x%s", g & 0x7fffffffu, g & 0x80000000u ? "_r" : "");
    else
        prof_name_pc(pc, out, cap);
}

/* the top entries of one thread: by where the thread was, and by where-and-caller */
static void prof_print(DWORD tid, uint32_t total, int by_caller, int rows)
{
    typedef struct { const char* where; const char* from; uint32_t n; } Row;
    Row* r = (Row*)calloc(g_nh + 1, sizeof *r);
    size_t nr = 0;
    if (!r)
        return;
    for (size_t i = 0; i < PROF_HASH; ++i)
    {
        ProfHit* s = &g_hits[i];
        if (!s->n || s->tid != tid)
            continue;
        const char* from = by_caller ? s->from : "";
        size_t j = 0;
        while (j < nr && !(!strcmp(r[j].where, s->where) && !strcmp(r[j].from, from)))
            j++;
        if (j == nr)
            r[nr].where = s->where, r[nr].from = from, nr++;
        r[j].n += s->n;
    }
    for (int k = 0; k < rows; ++k)
    {
        size_t best = (size_t)-1;
        for (size_t j = 0; j < nr; ++j)
            if (r[j].n && (best == (size_t)-1 || r[j].n > r[best].n))
                best = j;
        if (best == (size_t)-1 || r[best].n * 200 < total) /* down to 0.5% */
            break;
        if (by_caller && r[best].from[0])
            printf("[profile]   %5.1f%%  %s  <- %s\n", 100.0 * r[best].n / total, r[best].where, r[best].from);
        else
            printf("[profile]   %5.1f%%  %s\n", 100.0 * r[best].n / total, r[best].where);
        r[best].n = 0;
    }
    free(r);
}

static DWORD WINAPI profiler_thread(LPVOID arg)
{
    const char* spec = (const char*)arg;
    double secs = atof(spec), delay = 0;
    const char* c = strchr(spec, ':');
    if (c)
        delay = atof(c + 1);
    Sleep((DWORD)(delay * 1000));
    prof_build_starts();
    /* the game module's range (the module holding a translated function) and the host's */
    uintptr_t lo = 0, hi = 0, hlo = 0, hhi = 0;
    {
        HMODULE gm = NULL, hm = GetModuleHandleA(NULL);
        MODULEENTRY32 me;
        me.dwSize = sizeof me;
        if (g_nstarts)
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)g_starts[0].start, &gm);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
            {
                if (gm && me.hModule == gm)
                    lo = (uintptr_t)me.modBaseAddr, hi = lo + me.modBaseSize;
                if (me.hModule == hm)
                    hlo = (uintptr_t)me.modBaseAddr, hhi = hlo + me.modBaseSize;
            }
            CloseHandle(snap);
        }
        printf("[profile] host %p, game module %p\n", (void*)hlo, (void*)lo);
    }
    g_hits = (ProfHit*)calloc(PROF_HASH, sizeof *g_hits);
    enum { STACK_COPY = 32768 };
    unsigned char* stack = (unsigned char*)malloc(STACK_COPY);
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    DWORD tids[256];
    uint32_t tcount[256] = { 0 }, tours[256] = { 0 };
    int nt = 0;
    uint64_t rounds = 0, t_start = rt_monotonic_ns();
    printf("[profile] sampling for %.0f s (%zu translated function entries)\n", secs, g_nstarts);
    uint64_t end = rt_monotonic_ns() + (uint64_t)(secs * 1e9);
    /* the thread list and handles are refreshed once a second, not every round: a snapshot and
     * OpenThread per thread every 2 ms cost the game more than the sampling itself */
    struct { DWORD tid; HANDLE h; } th[256];
    int nth = 0;
    uint64_t next_list = 0;
    while (g_hits && stack && rt_monotonic_ns() < end)
    {
        rounds++;
        if (rt_monotonic_ns() >= next_list)
        {
            for (int i = 0; i < nth; ++i)
                CloseHandle(th[i].h);
            nth = 0;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te;
            te.dwSize = sizeof te;
            for (BOOL ok = Thread32First(snap, &te); ok && nth < 256; ok = Thread32Next(snap, &te))
            {
                if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                    continue;
                HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (h)
                    th[nth].tid = te.th32ThreadID, th[nth++].h = h;
            }
            CloseHandle(snap);
            next_list = rt_monotonic_ns() + 1000000000ull;
        }
        for (int q = 0; q < nth; ++q)
        {
            HANDLE h = th[q].h;
            THREADENTRY32 te;
            te.th32ThreadID = th[q].tid;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            uintptr_t pc = 0, sp = 0;
            size_t ncopy = 0;
            if (SuspendThread(h) != (DWORD)-1)
            {
                if (GetThreadContext(h, &ctx))
                {
                    pc = (uintptr_t)ctx.Rip, sp = (uintptr_t)ctx.Rsp;
                    /* copy the top of its stack while it is stopped (only plain memory reads here:
                     * the stopped thread may hold any lock) */
                    MEMORY_BASIC_INFORMATION mbi;
                    if (sp && VirtualQuery((LPCVOID)sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT)
                    {
                        uintptr_t top = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
                        ncopy = top - sp < STACK_COPY ? (size_t)(top - sp) : STACK_COPY;
                        memcpy(stack, (const void*)sp, ncopy);
                    }
                }
                ResumeThread(h);
            }
            if (!pc)
                continue;
            int t = 0;
            while (t < nt && tids[t] != te.th32ThreadID)
                t++;
            if (t == nt && nt < 256)
                tids[nt++] = te.th32ThreadID;
            if (t >= 256)
                continue;
            tcount[t]++;
            char where[64], from[64] = "";
            int ours = prof_guest_of(pc, lo, hi) != 0 || (pc >= hlo && pc < hhi);
            if (prof_guest_of(pc, lo, hi))
                tours[t]++;
            prof_name_ours(pc, lo, hi, where, sizeof where);
            /* outside our code: the nearest return address into the game module or the host */
            if (!ours)
                for (size_t o = 0; o + 8 <= ncopy; o += 8)
                {
                    uintptr_t v;
                    memcpy(&v, stack + o, 8);
                    if (prof_is_ret(v, lo, hi) || prof_is_ret(v, hlo, hhi))
                    {
                        prof_name_ours(v, lo, hi, from, sizeof from);
                        break;
                    }
                }
            ProfHit* s = prof_slot(te.th32ThreadID, where, from);
            if (s)
                s->n++;
        }
        Sleep(2);
    }
    for (int i = 0; i < nth; ++i)
        CloseHandle(th[i].h);
    double el = (double)(rt_monotonic_ns() - t_start) * 1e-9;
    printf("[profile] %llu rounds in %.0f s (%.1f ms a round)\n", (unsigned long long)rounds, el, rounds ? el * 1000 / rounds : 0);
    for (int t = 0; t < nt; ++t)
    {
        if (tcount[t] < 50)
            continue;
        /* a thread that only ever sat in one place is idle: one line for it */
        uint32_t top = 0;
        for (size_t i = 0; i < PROF_HASH; ++i)
            if (g_hits[i].n && g_hits[i].tid == tids[t])
                top = g_hits[i].n > top ? g_hits[i].n : top;
        printf("[profile] thread %lu: %u samples, %.1f%% in translated code\n", (unsigned long)tids[t], tcount[t],
            100.0 * tours[t] / tcount[t]);
        if (top * 100 >= tcount[t] * 98u)
        {
            prof_print(tids[t], tcount[t], 1, 1);
            continue;
        }
        printf("[profile]  where:\n");
        prof_print(tids[t], tcount[t], 0, 30);
        printf("[profile]  where, and our nearest caller:\n");
        prof_print(tids[t], tcount[t], 1, 30);
    }
    printf("[profile] done\n");
    return 0;
}

int main(int argc, char** argv)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    /* Timeouts on Windows round up to the system timer's tick, 15.6 ms by default, and since
     * Windows 10 2004 each program sets its own: the game's 1 ms waits (WaitForSingleObject and
     * the rest, through WaitOnAddress) take 15.6 without this, enough to hold the frame rate down
     * and make the audio engine late. */
    timeBeginPeriod(1);
    plat_no_throttling();
    {
        char d[256];
        plat_sleep_diag(d, sizeof d);
        printf("[host] sleep check: %s\n", d);
    }
    /* a check that lands in the log: what a 2 ms sleep costs here */
    {
        uint64_t t0 = rt_monotonic_ns();
        for (int i = 0; i < 5; ++i)
            plat_sleep_ms(2);
        printf("[host] 5 sleeps of 2 ms took %.1f ms\n", (double)(rt_monotonic_ns() - t0) * 1e-6);
    }
    SetUnhandledExceptionFilter(on_crash);
    setvbuf(stdout, NULL, _IONBF, 0);
    int rc = xiv_boot_prepare(&argc, argv);
    if (rc)
        return rc;
    if (getenv("XIVLEGACY_PROFILE"))
        CloseHandle(CreateThread(NULL, 0, profiler_thread, (LPVOID)getenv("XIVLEGACY_PROFILE"), 0, NULL));
    uint32_t r = xiv_boot_run();
    printf("mainCRTStartup returned %u\n", r);
    return 0;
}
