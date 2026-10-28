/* Boot test: the translated ffxivgame.exe on the portable runtime, no x86 anywhere.
 *
 * Maps the retail ffxivgame.exe into the 4 GB guest window with our loader (every import a thunk
 * to a shim), mounts the install at a Windows path, and runs the CRT entry point
 * (mainCRTStartup) on the guest. Every API call the game makes goes through the thunks; the first
 * one with no shim stops the run and names itself, which is the to-do list for the stand-ins.
 *
 * usage: boot <1.23b install folder> [56-character session id]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <sys/stat.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <SDL3/SDL.h>

#include "dinput.h"
#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "pe.h"
#include "plat.h"
#include "sqex_args.h"
#include "thunk.h"
#include "user32.h"
#include "vfs.h"
#include "ws2.h"
#include "xiv_boot.h"

#define GAME_DIR "C:\\Program Files\\SquareEnix\\FINAL FANTASY XIV"
/* The user profile the game sees (Documents\\My Games\\..., AppData) lives in a folder of ours. */
#define USER_DIR "C:\\users\\Player"

/* A host crash (SIGBUS/SIGSEGV): where, and the native stack, before dying. */
#include <pthread.h>
static pthread_t g_main_thread;

/* SIGUSR2: this thread's native stack (translated functions are f_<guest address>) */
static void on_stack_request(int sig)
{
    (void)sig;
    /* one thread at a time: they all get the signal at once */
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    void* frames[64];
    int k = backtrace(frames, 64);
    char head[64];
    int n = snprintf(head, sizeof head, "[boot] stack of thread %u:\n", plat_thread_id());
    write(2, head, (size_t)n);
    backtrace_symbols_fd(frames, k, 2);
    pthread_mutex_unlock(&m);
}

/* XIVLEGACY_KEYS="8:return,10.5:down,12:click@640x517,20:stack": input at seconds after start, as SDL
 * events to the game's window, for unattended runs (keys by SDL name; click@x x y in window points;
 * stack: the main thread's native stack). */
static void scripted_input(void* arg)
{
    char* spec = strdup((const char*)arg);
    uint64_t t0 = rt_monotonic_ns();
    for (char* item = strtok(spec, ","); item; item = strtok(NULL, ","))
    {
        double at = atof(item);
        const char* what = strchr(item, ':');
        if (!what)
            continue;
        what++;
        uint64_t due = t0 + (uint64_t)(at * 1e9);
        while (rt_monotonic_ns() < due)
            plat_sleep_ms(10);
        int n = 0;
        SDL_Window** ws = SDL_GetWindows(&n);
        SDL_WindowID id = ws && n ? SDL_GetWindowID(ws[0]) : 0;
        SDL_free(ws);
        /* the game takes input only while its window is the foreground one: as if it were */
        {
            SDL_Event f;
            memset(&f, 0, sizeof f);
            f.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
            f.window.windowID = id;
            SDL_PushEvent(&f);
            plat_sleep_ms(100);
        }
        if (!strcmp(what, "stack"))
        {
            pthread_kill(g_main_thread, SIGUSR2);
            plat_sleep_ms(200);
            plat_signal_threads(SIGUSR2);
            continue;
        }
        if (!strncmp(what, "click@", 6))
        {
            float x = 0, y = 0;
            sscanf(what + 6, "%fx%f", &x, &y);
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.windowID = id, e.motion.x = x, e.motion.y = y;
            SDL_PushEvent(&e);
            plat_sleep_ms(50);
            for (int down = 1; down >= 0; --down)
            {
                memset(&e, 0, sizeof e);
                e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                e.button.windowID = id, e.button.button = SDL_BUTTON_LEFT, e.button.down = down, e.button.clicks = 1;
                e.button.x = x, e.button.y = y;
                SDL_PushEvent(&e);
                plat_sleep_ms(80);
            }
            printf("[boot] input: click at %.0f x %.0f\n", x, y);
            continue;
        }
        SDL_Scancode sc = SDL_GetScancodeFromName(what);
        if (sc == SDL_SCANCODE_UNKNOWN)
        {
            printf("[boot] input: no key named %s\n", what);
            continue;
        }
        for (int down = 1; down >= 0; --down)
        {
            SDL_Event e;
            memset(&e, 0, sizeof e);
            e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.windowID = id, e.key.scancode = sc, e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
            e.key.down = down;
            SDL_PushEvent(&e);
            plat_sleep_ms(100);
        }
        printf("[boot] input: %s\n", what);
    }
    free(spec);
}

void g9_snap_next(const char* path);
void g9_debug(int mode);
void g9_trace_next(void);
void g9_skip_vs(uint32_t id);
void g9_dump_vs(uint32_t id, uint32_t count);
void g9_cpu_check_vs(uint32_t id);
void g9_only_vs(uint32_t id);
void g9_snap_tex(uint32_t id, const char* path);

/* --- XIVLEGACY_PROFILE=<seconds>[:<start delay>]: a sampling profiler ----------------------------------
 * Every ~2 ms each thread is sent SIGPROF; its handler keeps a short native backtrace. At the end the
 * samples are counted by function (dladdr: translated code shows as f_<guest address>), self and
 * inclusive, per thread, and printed. */
#include <dlfcn.h>
#define PROF_DEPTH 12
#define PROF_MAX 200000
typedef struct
{
    uint32_t tid;
    uint8_t n;
    void* pc[PROF_DEPTH];
} ProfSample;
static ProfSample* g_prof;
static volatile uint32_t g_prof_n, g_prof_on;

/* XIVLEGACY_HITCHPROF: the drawing thread sampled every millisecond into a ring; a slow frame
 * (d3d9.c, with XIVLEGACY_FRAMESTATS) prints where its time went */
#define HP_RING 16384
typedef struct
{
    uint64_t t;
    uint8_t n;
    void* pc[PROF_DEPTH];
} HpSample;
static HpSample* g_hp;
static volatile uint32_t g_hp_head;
static volatile int g_hp_on;
extern uint32_t d3d9_drawer_tid;
extern pthread_t d3d9_drawer_thread;

static void hp_record(void)
{
    uint32_t i = plat_atomic_add32(&g_hp_head, 1) % HP_RING;
    void* frames[PROF_DEPTH + 2];
    int k = backtrace(frames, PROF_DEPTH + 2);
    HpSample* s = &g_hp[i];
    s->t = rt_monotonic_ns();
    s->n = 0;
    for (int j = 2; j < k && s->n < PROF_DEPTH; ++j)
        s->pc[s->n++] = frames[j];
}

static void on_prof(int sig)
{
    (void)sig;
    if (g_hp_on && plat_thread_id() == d3d9_drawer_tid)
    {
        hp_record();
        return;
    }
    if (!g_prof_on)
        return;
    uint32_t i = plat_atomic_add32(&g_prof_n, 1);
    if (i >= PROF_MAX)
        return;
    void* frames[PROF_DEPTH + 2];
    int k = backtrace(frames, PROF_DEPTH + 2);
    ProfSample* s = &g_prof[i];
    s->tid = plat_thread_id();
    s->n = 0;
    for (int j = 2; j < k && s->n < PROF_DEPTH; ++j) /* past this handler and the signal trampoline */
        s->pc[s->n++] = frames[j];
}

typedef struct
{
    const char* name;
    uint32_t self, incl;
} ProfFn;

static int prof_cmp(const void* a, const void* b)
{
    const ProfFn* x = (const ProfFn*)a;
    const ProfFn* y = (const ProfFn*)b;
    return (int)y->self - (int)x->self;
}

static const char* prof_name(void* pc)
{
    Dl_info di;
    if (dladdr(pc, &di) && di.dli_sname)
        return di.dli_sname;
    return "?";
}

/* XIVLEGACY_PROFILE_FOCUS=<substring>: the stacks under the functions whose names hold it, counted;
 * =><substring>: what they call */
static void prof_focus(uint32_t tid, uint32_t n, uint32_t total, const char* focus)
{
    enum { MAXS = 512 };
    char** keys = (char**)calloc(MAXS, sizeof *keys);
    uint32_t* counts = (uint32_t*)calloc(MAXS, sizeof *counts);
    int nk = 0;
    /* ">name": what name calls (the frames between it and the leaf, three deep) */
    int below = focus[0] == '>';
    focus += below;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (g_prof[i].tid != tid)
            continue;
        int at = -1;
        for (int j = 0; j < g_prof[i].n && at < 0; ++j)
            if (strstr(prof_name(g_prof[i].pc[j]), focus))
                at = j;
        if (at < 0)
            continue;
        char key[512];
        size_t len = 0;
        key[0] = 0;
        if (below) /* what it calls: the leaf up to it */
            for (int j = at - 1 > 2 ? 2 : at - 1; j >= 0; --j)
                len += (size_t)snprintf(key + len, sizeof key - len, "%s%s", len ? " > " : "", prof_name(g_prof[i].pc[j]));
        else
            for (int j = at; j < g_prof[i].n && j < at + 7; ++j)
                len += (size_t)snprintf(key + len, sizeof key - len, "%s%s", j > at ? " < " : "", prof_name(g_prof[i].pc[j]));
        if (!len)
            snprintf(key, sizeof key, "(self)");
        int k = 0;
        while (k < nk && strcmp(keys[k], key))
            k++;
        if (k == nk)
        {
            if (nk == MAXS)
                continue;
            keys[nk++] = strdup(key);
        }
        counts[k]++;
    }
    for (int a = 0; a < nk; ++a)
        for (int b = a + 1; b < nk; ++b)
            if (counts[b] > counts[a])
            {
                uint32_t c = counts[a];
                char* t = keys[a];
                counts[a] = counts[b], keys[a] = keys[b];
                counts[b] = c, keys[b] = t;
            }
    for (int k = 0; k < nk && k < 12; ++k)
        rt_log("[profile]   %5.1f%%  %s\n", 100.0 * counts[k] / total, keys[k]);
    for (int k = 0; k < nk; ++k)
        free(keys[k]);
    free(keys);
    free(counts);
}

static void profiler_thread(void* arg)
{
    const char* spec = (const char*)arg;
    double secs = atof(spec), delay = 0;
    const char* c = strchr(spec, ':');
    if (c)
        delay = atof(c + 1);
    g_prof = (ProfSample*)calloc(PROF_MAX, sizeof *g_prof);
    signal(SIGPROF, on_prof);
    plat_sleep_ms((uint32_t)(delay * 1000));
    rt_log("[profile] sampling for %.0f s\n", secs);
    g_prof_on = 1;
    uint64_t end = rt_monotonic_ns() + (uint64_t)(secs * 1e9);
    while (rt_monotonic_ns() < end && g_prof_n < PROF_MAX)
    {
        pthread_kill(g_main_thread, SIGPROF);
        plat_signal_threads(SIGPROF);
        plat_sleep_ms(2);
    }
    g_prof_on = 0;
    plat_sleep_ms(50);
    uint32_t n = g_prof_n < PROF_MAX ? g_prof_n : PROF_MAX;
    /* threads, by sample count */
    uint32_t tids[64] = { 0 }, tcount[64] = { 0 };
    int nt = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        int t = 0;
        while (t < nt && tids[t] != g_prof[i].tid)
            t++;
        if (t == nt && nt < 64)
            tids[nt++] = g_prof[i].tid;
        if (t < 64)
            tcount[t]++;
    }
    for (int t = 0; t < nt; ++t)
    {
        /* functions of this thread: name -> counts (names are dladdr's, stable pointers) */
        ProfFn* fns = (ProfFn*)calloc(8192, sizeof *fns);
        int nf = 0;
        for (uint32_t i = 0; i < n; ++i)
        {
            if (g_prof[i].tid != tids[t])
                continue;
            const char* seen[PROF_DEPTH];
            int ns = 0;
            for (int j = 0; j < g_prof[i].n; ++j)
            {
                Dl_info di;
                const char* name = "?";
                static char regions[64][24];
                static uintptr_t region_key[64];
                if (!dladdr(g_prof[i].pc[j], &di))
                {
                    /* outside every image: named by its 16 MB region */
                    uintptr_t key = (uintptr_t)g_prof[i].pc[j] >> 24;
                    int r = 0;
                    while (r < 64 && region_key[r] && region_key[r] != key)
                        r++;
                    if (r < 64)
                    {
                        if (!region_key[r])
                        {
                            region_key[r] = key;
                            snprintf(regions[r], sizeof regions[r], "?%lx000000", (unsigned long)key);
                        }
                        name = regions[r];
                    }
                }
                else
                {
                    if (di.dli_sname)
                        name = di.dli_sname;
                    else if (di.dli_fname)
                    {
                        const char* slash = strrchr(di.dli_fname, '/');
                        name = slash ? slash + 1 : di.dli_fname; /* no symbol: its module */
                    }
                }
                int dup = 0;
                for (int q = 0; q < ns; ++q)
                    dup |= seen[q] == name;
                if (dup)
                    continue;
                seen[ns++] = name;
                int f = 0;
                while (f < nf && fns[f].name != name)
                    f++;
                if (f == nf)
                {
                    if (nf == 8192)
                        continue;
                    fns[nf++].name = name;
                }
                fns[f].incl++;
                if (j == 0)
                    fns[f].self++;
            }
        }
        qsort(fns, (size_t)nf, sizeof *fns, prof_cmp);
        rt_log("[profile] thread %u: %u samples\n", tids[t], tcount[t]);
        for (int f = 0; f < nf && f < 25; ++f)
            rt_log("[profile]   %5.1f%% self %5.1f%% incl  %s\n", 100.0 * fns[f].self / tcount[t], 100.0 * fns[f].incl / tcount[t],
                fns[f].name);
        /* the heaviest inclusive ones too */
        for (int a = 0; a < nf; ++a)
            for (int b = a + 1; b < nf; ++b)
                if (fns[b].incl > fns[a].incl)
                {
                    ProfFn tmp = fns[a];
                    fns[a] = fns[b];
                    fns[b] = tmp;
                }
        for (int f = 0; f < nf && f < 25; ++f)
            rt_log("[profile]   incl %5.1f%%  %s\n", 100.0 * fns[f].incl / tcount[t], fns[f].name);
        free(fns);
        if (getenv("XIVLEGACY_PROFILE_FOCUS"))
            prof_focus(tids[t], n, tcount[t], getenv("XIVLEGACY_PROFILE_FOCUS"));
    }
}

static void find_floats(const float v[3], float tol);
static void watch_start(uint32_t a, uint32_t n);
static void watch_stop(void);

static void hitch_sampler(void* arg)
{
    (void)arg;
    for (;;)
    {
        plat_sleep_ms(1);
        if (d3d9_drawer_tid)
            pthread_kill(d3d9_drawer_thread, SIGPROF);
    }
}

int main(int argc, char** argv);

void xiv_on_hitch(uint64_t from_ns, uint64_t to_ns)
{
    /* counts by raw address (offsets from main, so `nm -n build/boot` names them afterwards): dladdr
     * on this binary costs milliseconds an address */
    if (!g_hp_on)
        return;
    typedef struct
    {
        uintptr_t pc;
        uint32_t self, incl;
    } Pc;
    static Pc pcs[4096];
    int np = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < HP_RING; ++i)
    {
        HpSample* s = &g_hp[i];
        if (s->t < from_ns || s->t > to_ns || !s->n)
            continue;
        total++;
        for (int j = 0; j < s->n; ++j)
        {
            uintptr_t pc = (uintptr_t)s->pc[j];
            int f = 0;
            while (f < np && pcs[f].pc != pc)
                f++;
            if (f == np)
            {
                if (np == 4096)
                    continue;
                pcs[np].pc = pc, pcs[np].self = pcs[np].incl = 0;
                np++;
            }
            pcs[f].incl++;
            if (j == 0)
                pcs[f].self++;
        }
    }
    if (!total)
        return;
    uintptr_t base = (uintptr_t)&main;
    for (int a = 0; a < np; ++a)
        for (int b = a + 1; b < np; ++b)
            if (pcs[b].self > pcs[a].self)
            {
                Pc t = pcs[a];
                pcs[a] = pcs[b], pcs[b] = t;
            }
    rt_log("[hitchprof] %u samples over %.0f ms; main at %lx; self:", total, (to_ns - from_ns) / 1e6, (unsigned long)base);
    for (int f = 0; f < np && f < 12; ++f)
        rt_log(" %lx:%u", (unsigned long)pcs[f].pc, pcs[f].self);
    for (int a = 0; a < np; ++a)
        for (int b = a + 1; b < np; ++b)
            if (pcs[b].incl > pcs[a].incl)
            {
                Pc t = pcs[a];
                pcs[a] = pcs[b], pcs[b] = t;
            }
    rt_log("\n[hitchprof]   inclusive:");
    for (int f = 0; f < np && f < 30; ++f)
        rt_log(" %lx:%u", (unsigned long)pcs[f].pc, pcs[f].incl);
    rt_log("\n");
}

static void push_key_mod(SDL_WindowID id, SDL_Scancode sc, SDL_Keymod mod)
{
    for (int down = 1; down >= 0; --down)
    {
        SDL_Event e;
        memset(&e, 0, sizeof e);
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.windowID = id, e.key.scancode = sc, e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
        e.key.down = down, e.key.mod = mod;
        SDL_PushEvent(&e);
        plat_sleep_ms(60);
    }
}

static void push_key(SDL_WindowID id, SDL_Scancode sc) { push_key_mod(id, sc, SDL_KMOD_NONE); }

static SDL_WindowID game_window(void)
{
    int n = 0;
    SDL_Window** ws = SDL_GetWindows(&n);
    SDL_WindowID id = ws && n ? SDL_GetWindowID(ws[0]) : 0;
    SDL_free(ws);
    SDL_Event f;
    memset(&f, 0, sizeof f);
    f.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    f.window.windowID = id;
    SDL_PushEvent(&f);
    return id;
}

/* XIVLEGACY_CONTROL=<fifo>: commands, one a line, for driving a running game by hand:
 *   key [ctrl+]<SDL key name>  hold <key> <ms>    click <x> <y>      move <x> <y>      type <text>
 *   snap <png path>         stack */
static void control_thread(void* arg)
{
    const char* path = (const char*)arg;
    for (;;)
    {
        FILE* f = fopen(path, "r"); /* blocks until a writer opens it */
        if (!f)
        {
            plat_sleep_ms(500);
            continue;
        }
        char line[512];
        while (fgets(line, sizeof line, f))
        {
            line[strcspn(line, "\r\n")] = 0;
            SDL_WindowID id = game_window();
            plat_sleep_ms(50);
            float x = 0, y = 0;
            if (!strncmp(line, "key ", 4))
            {
                /* key ctrl+<name>: with Ctrl held (the bug-report key is ctrl+F12) */
                int ctrl = !strncmp(line + 4, "ctrl+", 5);
                SDL_Scancode sc = SDL_GetScancodeFromName(line + 4 + (ctrl ? 5 : 0));
                if (sc != SDL_SCANCODE_UNKNOWN)
                    push_key_mod(id, sc, ctrl ? SDL_KMOD_LCTRL : SDL_KMOD_NONE);
            }
            else if ((!strncmp(line, "click ", 6) || !strncmp(line, "move ", 5)) && sscanf(strchr(line, ' ') + 1, "%f %f", &x, &y) == 2)
            {
                SDL_Event e;
                memset(&e, 0, sizeof e);
                e.type = SDL_EVENT_MOUSE_MOTION;
                e.motion.windowID = id, e.motion.x = x, e.motion.y = y;
                SDL_PushEvent(&e);
                plat_sleep_ms(80);
                for (int down = 1; line[0] == 'c' && down >= 0; --down)
                {
                    memset(&e, 0, sizeof e);
                    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                    e.button.windowID = id, e.button.button = SDL_BUTTON_LEFT, e.button.down = down, e.button.clicks = 1;
                    e.button.x = x, e.button.y = y;
                    SDL_PushEvent(&e);
                    plat_sleep_ms(80);
                }
            }
            else if (!strncmp(line, "type ", 5))
            {
                for (const char* p = line + 5; *p; ++p)
                {
                    char name[2] = { *p, 0 };
                    SDL_Scancode sc = *p == ' ' ? SDL_SCANCODE_SPACE : SDL_GetScancodeFromName(name);
                    int shift = *p >= 'A' && *p <= 'Z';
                    if (shift)
                    {
                        SDL_Event e;
                        memset(&e, 0, sizeof e);
                        e.type = SDL_EVENT_KEY_DOWN, e.key.windowID = id, e.key.scancode = SDL_SCANCODE_LSHIFT, e.key.down = 1;
                        e.key.key = SDLK_LSHIFT, e.key.mod = SDL_KMOD_LSHIFT;
                        SDL_PushEvent(&e);
                    }
                    if (sc != SDL_SCANCODE_UNKNOWN)
                        push_key(id, sc);
                    SDL_Event t;
                    memset(&t, 0, sizeof t);
                    t.type = SDL_EVENT_TEXT_INPUT;
                    t.text.windowID = id;
                    t.text.text = SDL_strdup(name);
                    SDL_PushEvent(&t);
                    if (shift)
                    {
                        SDL_Event e;
                        memset(&e, 0, sizeof e);
                        e.type = SDL_EVENT_KEY_UP, e.key.windowID = id, e.key.scancode = SDL_SCANCODE_LSHIFT, e.key.key = SDLK_LSHIFT;
                        SDL_PushEvent(&e);
                    }
                    plat_sleep_ms(60);
                }
            }
            else if (!strncmp(line, "hold ", 5))
            {
                char name[64];
                int ms = 500;
                if (sscanf(line + 5, "%63s %d", name, &ms) >= 1)
                {
                    SDL_Scancode sc = SDL_GetScancodeFromName(name);
                    SDL_Event e;
                    memset(&e, 0, sizeof e);
                    e.type = SDL_EVENT_KEY_DOWN, e.key.windowID = id, e.key.scancode = sc, e.key.down = 1;
                    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
                    SDL_PushEvent(&e);
                    plat_sleep_ms((uint32_t)ms);
                    e.type = SDL_EVENT_KEY_UP, e.key.down = 0;
                    SDL_PushEvent(&e);
                }
            }
            else if (!strncmp(line, "snap ", 5))
                g9_snap_next(line + 5);
            else if (!strncmp(line, "findf ", 6))
            {
                /* findf x y z [tol]: guest addresses holding the float triple (readable regions only) */
                float v[3], tol = 0.05f;
                if (sscanf(line + 6, "%f %f %f %f", &v[0], &v[1], &v[2], &tol) >= 3)
                    find_floats(v, tol);
            }
            else if (!strncmp(line, "dumpf ", 6))
            {
                /* dumpf <hex guest address> <count>: floats (and their bits) from guest memory */
                unsigned a = 0, n = 32;
                if (sscanf(line + 6, "%x %u", &a, &n) >= 1)
                    for (unsigned i = 0; i < n; i += 4)
                    {
                        printf("[dumpf] %08x:", a + i * 4);
                        for (unsigned k = 0; k < 4 && i + k < n; ++k)
                        {
                            uint32_t w = rd32(a + (i + k) * 4);
                            float fv;
                            memcpy(&fv, &w, 4);
                            printf(" %08x(%g)", w, fv);
                        }
                        printf("\n");
                    }
            }
            else if (!strncmp(line, "gfxdbg ", 7))
                g9_debug(atoi(line + 7)); /* 0 normal, 1 wireframe, 2 gray textures */
            else if (!strncmp(line, "snaptex ", 8))
            {
                unsigned id = 0;
                char path[900];
                if (sscanf(line + 8, "%x %899s", &id, path) == 2)
                    g9_snap_tex(id, path);
            }
            else if (!strncmp(line, "onlyvs ", 7))
                g9_only_vs((uint32_t)atoi(line + 7));
            else if (!strncmp(line, "cpuvs ", 6))
                g9_cpu_check_vs((uint32_t)atoi(line + 6));
            else if (!strncmp(line, "dumpvs ", 7))
            {
                unsigned id = 0, n = 3;
                if (sscanf(line + 7, "%u %u", &id, &n) >= 1)
                    g9_dump_vs(id, n);
            }
            else if (!strncmp(line, "skipvs ", 7))
                g9_skip_vs((uint32_t)atoi(line + 7));
            else if (!strcmp(line, "trace"))
                g9_trace_next();
            else if (!strcmp(line, "vsc"))
            {
                extern const float* d3d9_vsf(void);
                const float* c = d3d9_vsf();
                for (int r = 0; c && r < 12; ++r)
                    printf("[vsc] c%d %.9g %.9g %.9g %.9g\n", r, c[r * 4], c[r * 4 + 1], c[r * 4 + 2], c[r * 4 + 3]);
            }
            else if (!strcmp(line, "vscsrc"))
            {
                extern uint32_t d3d9_vsf8_src;
                printf("[vscsrc] c8 last from guest %08x\n", d3d9_vsf8_src);
            }
            else if (!strncmp(line, "watch ", 6))
            {
                /* watch <hex guest address> [bytes]: log the translated functions that write there */
                unsigned a = 0, n = 16;
                if (sscanf(line + 6, "%x %u", &a, &n) >= 1)
                    watch_start(a, n);
            }
            else if (!strcmp(line, "unwatch"))
                watch_stop();
            else if (!strcmp(line, "stack"))
            {
                pthread_kill(g_main_thread, SIGUSR2);
                plat_sleep_ms(200);
                plat_signal_threads(SIGUSR2);
            }
            printf("[boot] control: %s\n", line);
        }
        fclose(f);
    }
}

#include <mach/mach.h>
#include <mach/mach_vm.h>
static void find_floats(const float v[3], float tol)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)GUEST_PTR(0), end = a + 0x100000000ull;
    int hits = 0;
    while (a < end && hits < 64)
    {
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        if (mach_vm_region(mach_task_self(), &a, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            break;
        if (a >= end)
            break;
        if (info.protection & VM_PROT_READ)
        {
            const float* f = (const float*)(uintptr_t)a;
            size_t n = (size_t)((a + size > end ? end - a : size) / 4);
            for (size_t i = 0; i + 2 < n && hits < 64; ++i)
                if (fabsf(f[i] - v[0]) < tol && fabsf(f[i + 1] - v[1]) < tol && fabsf(f[i + 2] - v[2]) < tol)
                {
                    uint32_t g = (uint32_t)((uintptr_t)&f[i] - (uintptr_t)GUEST_PTR(0));
                    printf("[findf] %08x: %g %g %g | %g %g %g %g %g\n", g, f[i], f[i + 1], f[i + 2], f[i + 3], f[i + 4], f[i + 5], f[i + 6],
                        f[i + 7]);
                    hits++;
                }
        }
        a += size;
    }
    printf("[findf] %d hits\n", hits);
}

/* --- a write watchpoint: the page made read-only, each write to the watched bytes logged (the
 * writer's function names), the page writable again and re-protected a moment later --- */
#include <sys/mman.h>
static volatile uintptr_t g_watch_lo, g_watch_hi, g_watch_page;
static volatile int g_watch_open, g_watch_logs;
static size_t g_page = 16384;

static void watch_thread(void* arg)
{
    (void)arg;
    for (;;)
    {
        plat_sleep_ms(1);
        if (g_watch_page && g_watch_open)
        {
            g_watch_open = 0;
            mprotect((void*)g_watch_page, g_page, PROT_READ);
        }
    }
}

static void watch_start(uint32_t a, uint32_t n)
{
    static int started;
    if (!started)
    {
        started = 1;
        plat_thread_start(watch_thread, NULL);
    }
    if (g_watch_page)
        mprotect((void*)g_watch_page, g_page, PROT_READ | PROT_WRITE);
    uintptr_t h = (uintptr_t)GUEST_PTR(a);
    g_watch_lo = h, g_watch_hi = h + n, g_watch_logs = 0;
    g_watch_page = h & ~(uintptr_t)(g_page - 1);
    mprotect((void*)g_watch_page, g_page, PROT_READ);
    printf("[watch] %08x+%u\n", a, n);
}

static void watch_stop(void)
{
    uintptr_t p = g_watch_page;
    g_watch_page = 0;
    if (p)
        mprotect((void*)p, g_page, PROT_READ | PROT_WRITE);
}

/* a write to the watched page: 1 when handled (the faulting store retries) */
static int watch_fault(void* addr, void* ctx)
{
    uintptr_t a = (uintptr_t)addr, p = g_watch_page;
    if (!p || a < p || a >= p + g_page)
        return 0;
    if (a >= g_watch_lo && a < g_watch_hi && g_watch_logs < 300)
    {
        g_watch_logs++;
        ucontext_t* uc = (ucontext_t*)ctx;
        uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc, fp = (uintptr_t)uc->uc_mcontext->__ss.__fp;
        char line[512];
        Dl_info di;
        int n = snprintf(line, sizeof line, "[watch] write %08lx by %s", (unsigned long)(a - (uintptr_t)GUEST_PTR(0)),
            dladdr((void*)pc, &di) && di.dli_sname ? di.dli_sname : "?");
        for (int k = 0; k < 5 && fp; ++k)
        {
            uintptr_t ret = ((uintptr_t*)fp)[1];
            n += snprintf(line + n, sizeof line - (size_t)n, " < %s", dladdr((void*)ret, &di) && di.dli_sname ? di.dli_sname : "?");
            fp = ((uintptr_t*)fp)[0];
        }
        line[n++] = '\n';
        write(1, line, (size_t)n);
    }
    mprotect((void*)p, g_page, PROT_READ | PROT_WRITE);
    g_watch_open = 1;
    return 1;
}

int main(int argc, char** argv);
static void on_crash(int sig, siginfo_t* si, void* ctx)
{
    if (watch_fault(si->si_addr, ctx))
        return;
    char line[200];
    int n = snprintf(line, sizeof line, "\n[boot] signal %d at address %p (guest %08lx)\n", sig, si->si_addr,
        (unsigned long)((unsigned char*)si->si_addr - rt_guest_base));
    write(2, line, (size_t)n);
    /* the guest registers as of the last call (translated code keeps them in host registers between) */
    GThread* t = gt_holds() ? gt_self() : NULL;
    if (t)
    {
        const Guest* g = &t->g;
        n = snprintf(line, sizeof line, "  gthread %p (host thread %u)\n", (void*)t, plat_thread_id());
        write(2, line, (size_t)n);
        n = snprintf(line, sizeof line, "  last stored: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            g->eax, g->ecx, g->edx, g->ebx, g->esp, g->ebp, g->esi, g->edi);
        write(2, line, (size_t)n);
    }
    /* XIVLEGACY_DUMP=<hex guest address>: 64 bytes there, to see what overwrote something */
    if (getenv("XIVLEGACY_DUMP"))
    {
        uint32_t a = (uint32_t)strtoul(getenv("XIVLEGACY_DUMP"), NULL, 16);
        for (uint32_t r = 0; r < 64; r += 16)
        {
            n = snprintf(line, sizeof line, "  %08x:", a + r);
            for (uint32_t i = 0; i < 16; i += 4)
                n += snprintf(line + n, sizeof line - (size_t)n, " %08x", rd32(a + r + i));
            line[n++] = '\n';
            write(2, line, (size_t)n);
        }
    }
#if defined(__aarch64__) && defined(__APPLE__)
    /* the host registers: translated code keeps the guest's in them between calls */
    {
        const ucontext_t* uc = (const ucontext_t*)ctx;
        n = snprintf(line, sizeof line, "  pc %016llx  (atos -o build/boot -l <load> it; main is at %p)\n",
            (unsigned long long)uc->uc_mcontext->__ss.__pc, (void*)main);
        write(2, line, (size_t)n);
        for (int r = 0; r < 29; r += 4)
        {
            n = snprintf(line, sizeof line, "  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx  x%-2d %016llx\n", r,
                (unsigned long long)uc->uc_mcontext->__ss.__x[r], r + 1, (unsigned long long)uc->uc_mcontext->__ss.__x[r + 1],
                r + 2, (unsigned long long)uc->uc_mcontext->__ss.__x[r + 2], r + 3, (unsigned long long)uc->uc_mcontext->__ss.__x[r + 3]);
            write(2, line, (size_t)n);
        }
    }
#else
    (void)ctx;
#endif
    void* frames[40];
    int k = backtrace(frames, 40);
    backtrace_symbols_fd(frames, k, 2);
    _exit(128 + sig);
}

int main(int argc, char** argv)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_main_thread = pthread_self();
    signal(SIGUSR2, on_stack_request);
    int rc = xiv_boot_prepare(&argc, argv);
    if (rc)
        return rc;

    /* mainCRTStartup takes no arguments and ends the process through ExitProcess. */
    if (getenv("XIVLEGACY_KEYS"))
        plat_thread_start(scripted_input, getenv("XIVLEGACY_KEYS"));
    if (getenv("XIVLEGACY_CONTROL"))
        plat_thread_start(control_thread, getenv("XIVLEGACY_CONTROL"));
    if (getenv("XIVLEGACY_PROFILE"))
        plat_thread_start(profiler_thread, getenv("XIVLEGACY_PROFILE"));
    if (getenv("XIVLEGACY_HITCHPROF"))
    {
        g_hp = (HpSample*)calloc(HP_RING, sizeof *g_hp);
        signal(SIGPROF, on_prof);
        g_hp_on = 1;
        plat_thread_start(hitch_sampler, NULL);
    }
    uint32_t r = xiv_boot_run();
    printf("mainCRTStartup returned %u\n", r);
    return 0;
}
