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
 * Every 2 ms each of the process's other threads is paused and its instruction pointer read. A
 * pointer inside the game module is named by the translated function holding it: the nearest
 * function start below it in rt_table (the module's own symbols are hidden). Others are named by
 * their module. The result, per busy thread, goes to the log, which the bug-report key sends. */
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

typedef struct
{
    DWORD tid;
    uint32_t key; /* guest address, or 0 with a module name */
    char mod[24];
    uint32_t n;
} ProfHit;

static DWORD WINAPI profiler_thread(LPVOID arg)
{
    const char* spec = (const char*)arg;
    double secs = atof(spec), delay = 0;
    const char* c = strchr(spec, ':');
    if (c)
        delay = atof(c + 1);
    Sleep((DWORD)(delay * 1000));
    prof_build_starts();
    /* the game module's range: the module holding a translated function */
    uintptr_t lo = 0, hi = 0;
    if (g_nstarts)
    {
        HMODULE m = NULL;
        MODULEENTRY32 me;
        me.dwSize = sizeof me;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)g_starts[0].start, &m);
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
        if (snap != INVALID_HANDLE_VALUE)
        {
            for (BOOL ok = Module32First(snap, &me); ok; ok = Module32Next(snap, &me))
                if (me.hModule == m)
                    lo = (uintptr_t)me.modBaseAddr, hi = lo + me.modBaseSize;
            CloseHandle(snap);
        }
    }
    enum { MAXH = 16384 };
    ProfHit* hits = (ProfHit*)calloc(MAXH, sizeof *hits);
    size_t nh = 0;
    DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    DWORD tids[256];
    uint32_t tcount[256] = { 0 };
    int nt = 0;
    printf("[profile] sampling for %.0f s (%zu translated functions)\n", secs, g_nstarts / 2);
    uint64_t end = rt_monotonic_ns() + (uint64_t)(secs * 1e9);
    while (hits && rt_monotonic_ns() < end)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te;
        te.dwSize = sizeof te;
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
                continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!h)
                continue;
            CONTEXT ctx;
            memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            uintptr_t pc = 0;
            if (SuspendThread(h) != (DWORD)-1)
            {
                if (GetThreadContext(h, &ctx))
                    pc = (uintptr_t)ctx.Rip;
                ResumeThread(h);
            }
            CloseHandle(h);
            if (!pc)
                continue;
            ProfHit k;
            memset(&k, 0, sizeof k);
            k.tid = te.th32ThreadID;
            k.key = prof_guest_of(pc, lo, hi);
            if (!k.key)
            {
                HMODULE m = NULL;
                char path[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &m))
                    GetModuleFileNameA(m, path, sizeof path);
                const char* base = strrchr(path, '\\');
                snprintf(k.mod, sizeof k.mod, "%s", base ? base + 1 : path);
            }
            int t = 0;
            while (t < nt && tids[t] != k.tid)
                t++;
            if (t == nt && nt < 256)
                tids[nt++] = k.tid;
            if (t < 256)
                tcount[t]++;
            size_t i = 0;
            while (i < nh && !(hits[i].tid == k.tid && hits[i].key == k.key && !strcmp(hits[i].mod, k.mod)))
                i++;
            if (i == nh)
            {
                if (nh == MAXH)
                    continue;
                hits[nh++] = k;
            }
            hits[i].n++;
        }
        CloseHandle(snap);
        Sleep(2);
    }
    /* busy threads first: those whose samples are not all in system waits are the interesting ones */
    for (int t = 0; t < nt; ++t)
    {
        uint32_t tr = 0;
        for (size_t i = 0; i < nh; ++i)
            if (hits[i].tid == tids[t] && hits[i].key)
                tr += hits[i].n;
        if (tcount[t] < 50)
            continue;
        printf("[profile] thread %lu: %u samples, %.1f%% in translated code\n", (unsigned long)tids[t], tcount[t], 100.0 * tr / tcount[t]);
        for (int r = 0; r < 25; ++r)
        {
            size_t best = (size_t)-1;
            for (size_t i = 0; i < nh; ++i)
                if (hits[i].tid == tids[t] && hits[i].n && (best == (size_t)-1 || hits[i].n > hits[best].n))
                    best = i;
            if (best == (size_t)-1 || hits[best].n * 200 < tcount[t]) /* down to 0.5% */
                break;
            if (hits[best].key)
                printf("[profile]   %5.1f%%  f_%08x%s\n", 100.0 * hits[best].n / tcount[t], hits[best].key & 0x7fffffffu,
                    hits[best].key & 0x80000000u ? "_r" : "");
            else
                printf("[profile]   %5.1f%%  %s\n", 100.0 * hits[best].n / tcount[t], hits[best].mod);
            hits[best].n = 0;
        }
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
