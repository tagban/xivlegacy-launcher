/* Guest threads, the guest lock, host->guest calls. See gthread.h. */
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "gthread.h"
#include "gwin.h"
#include "plat.h"

static RT_TLS GThread* t_self;
static RT_TLS int t_held, t_urgent;
static RT_TLS uint64_t t_acquired;
/* time other threads than the game (drawing) thread held the lock: a frame waits on it (hitch log) */
static volatile uint64_t g_other_held_ns;
uint64_t gt_other_held_ns(void) { return g_other_held_ns; }
/* the drawing thread's waits for the lock and its file reads (hitch log) */
static volatile uint64_t g_game_wait_ns, g_game_read_ns, g_game_read_bytes;
void gt_game_costs(uint64_t* wait_ns, uint64_t* read_ns, uint64_t* read_bytes)
{
    *wait_ns = g_game_wait_ns, *read_ns = g_game_read_ns, *read_bytes = g_game_read_bytes;
}

/* --- the guest lock: a FIFO ticket lock with a quantum ---------------------------------------- */
static volatile uint32_t g_next_ticket, g_now_serving;
/* How long a thread keeps the lock while others wait. The game thread (the one that called gt_init)
 * keeps it longest: each time it yields it waits out every other waiting thread's turn, which on
 * slow cores was 15% of its time with equal turns. In FFXIV 1.23b the thread that draws
 * (gt_mark_game_thread) gives others their turns whenever it blocks (its frame Sleeps), so a
 * forced yield is only for a thread starved past a frame: at 8 ms its world frame ran at 26 fps,
 * at 20 ms 36, at 50 ms 40. */
#define QUANTUM_NS 1000000ull
#define GAME_QUANTUM_NS 50000000ull
static RT_TLS uint64_t t_quantum_ns;
/* The game's main loop (the thread that pumps its window: input, the camera's controls, game
 * logic) does a tick's work in about 3 ms. With the 1 ms turn every thread gets, each tick was cut
 * into pieces with a drawing-thread turn between them, and the loop ran at 11 Hz in the world: the
 * mouse camera turned in steps, several frames apart. A 3 ms turn runs it at ~24 Hz, about once a
 * frame (fps 27 -> 24; 4 ms ran it at 29 Hz for fps 22). */
#define MAIN_QUANTUM_NS 3000000ull
static RT_TLS int t_main;
/* Behind the loading screen the drawing thread's turn is short. The game loads through worker
 * threads that each poll for work and yield (SwitchToThread) when there is none, while the drawing
 * thread keeps rendering the world behind the loading screen: every hand-off between them waited
 * out the rest of a 50 ms turn. (A 2 ms turn throughout cost a third of the frame rate in the
 * world, so it is only for the loading screen.) */
#define LOADING_QUANTUM_NS 2000000ull
static volatile uint32_t g_loading;
/* The game thread waits for its turn: other threads take no new ticket meanwhile, so it waits out
 * only those already queued, not every thread that wakes from a Sleep while it waits (a dozen
 * sound threads looping Sleep, each a turn ahead of the game's frame, every frame). */
static RT_TLS int t_game;
/* Once a thread is marked as the one that draws (gt_mark_game_thread), it alone keeps the game
 * thread's long turn and right of way: the thread that called gt_init kept them too, and a burst of
 * its work (the world streaming in as the player walks) held the lock a whole 50 ms turn while the
 * frame waited - a stutter. A thread is the drawing one while its t_drawer matches the generation. */
static volatile uint32_t g_drawer_gen;
static RT_TLS uint32_t t_drawer;
static inline int game_now(void) { return t_game && (!g_drawer_gen || t_drawer == g_drawer_gen); }
static volatile uint32_t g_game_waiting;

/* a moment's pause on the CPU without giving it up: sched_yield on macOS lowers the caller's
 * priority for a while (swtch_pri), so a thread whose turn came could be the one not running */
static inline void cpu_relax(void)
{
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(_MSC_VER) && defined(_M_ARM64)
    __yield();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause");
#else
    plat_yield();
#endif
}

/* Threads waiting for the lock, and when the holder's turn ends. rt_lock_contended, the flag the
 * translated code tests at each loop's back edge, is the request to yield: a waiter raises it once
 * the holder's quantum is over, so the holder neither calls out nor reads the clock before then
 * (with a waiter always queued, a check at every back edge was 5% of the render thread's time). */
static volatile uint32_t g_waiters, g_urgent;
static volatile uint64_t g_deadline;

void gt_note_read(uint64_t ns, uint64_t bytes)
{
    if (game_now())
        g_game_read_ns += ns, g_game_read_bytes += bytes;
}

static void g_acquire(void)
{
    uint64_t wait_t0 = game_now() ? rt_monotonic_ns() : 0;
    plat_atomic_add32(&g_waiters, 1);
    if (!game_now())
        while (g_game_waiting)
            plat_wait32(&g_game_waiting, 1);
    uint32_t t = plat_atomic_add32(&g_next_ticket, 1);
    int flagged = 0;
    for (unsigned spin = 0;; ++spin)
    {
        uint32_t cur = g_now_serving;
        if (cur == t)
            break;
        if (game_now() && !flagged)
            g_game_waiting = 1, flagged = 1;
        if (spin < 64)
        {
            cpu_relax();
            continue;
        }
        /* parked (woken by gt_unlock) until the holder's turn is up, then ask it to yield */
        uint64_t now = rt_monotonic_ns(), end = g_deadline;
        if (now >= end || t_urgent)
        {
            /* asked again each millisecond: a holder may have cleared the request before this
             * thread was counted as waiting, or a new holder clears it as it takes the lock (the
             * request was raised against the previous holder's deadline). Parked for good after
             * one request, a waiter could leave a thread running guest code with the lock for
             * seconds. */
            rt_lock_contended = 1;
            plat_wait32_ms(&g_now_serving, cur, 1);
        }
        else
            plat_wait32_ms(&g_now_serving, cur, (uint32_t)((end - now + 999999) / 1000000));
    }
    if (flagged)
    {
        g_game_waiting = 0;
        plat_wake_all32(&g_game_waiting);
    }
    plat_atomic_add32(&g_waiters, (uint32_t)-1);
    t_held = 1;
    t_acquired = rt_monotonic_ns();
    if (wait_t0)
        g_game_wait_ns += t_acquired - wait_t0;
    uint64_t quantum = QUANTUM_NS;
    if (game_now() && t_quantum_ns)
        quantum = g_loading ? LOADING_QUANTUM_NS : t_quantum_ns;
    else if (t_main && !g_loading)
        quantum = MAIN_QUANTUM_NS;
    g_deadline = t_acquired + quantum;
    rt_lock_contended = g_urgent > (t_urgent ? 1u : 0u);
}

static void g_release(void)
{
    if (!game_now() && t_acquired)
        g_other_held_ns += rt_monotonic_ns() - t_acquired; /* still held: no other writer */
    t_held = 0;
    plat_atomic_add32(&g_now_serving, 1);
    if (g_waiters)
        plat_wake_all32(&g_now_serving);
}

/* --- XIVLEGACY_PARALLEL: the main loop beside the guest lock ---------------------------------
 * On Windows the game's main loop (input, the camera's controls, game logic) and its drawing thread
 * ran at once. With XIVLEGACY_PARALLEL=1 the main loop runs translated code without the guest
 * lock, beside whichever thread holds it; every other thread keeps sharing the guest lock as before.
 * What the guest lock protected splits in two:
 *  - the runtime's own state (heap, handles, windows and messages, critical sections, sockets,
 *    d3d9): the runtime lock, held by any thread while it runs a shim (thunk_dispatch) or other
 *    runtime code, never while it runs translated code, so it is held for microseconds;
 *  - the game's own data: the game's own synchronization, as on Windows (critical sections,
 *    Interlocked*, locked instructions, which guest.h makes atomic).
 * Lock order: the guest lock, then the runtime lock. A thread that holds the guest lock and runs
 * runtime code holds both; gt_lock/gt_unlock (a shim's blocking section, a host thread) take and
 * give up both. What this does not give the guest is x86's store ordering between the two threads
 * (docs/PARALLEL.md). */
static int g_par = -1;
static RT_TLS int t_par;   /* this thread runs translated code without the guest lock */
static RT_TLS int t_rt;    /* this thread holds the runtime lock */
static RT_TLS int t_guest; /* this thread is running translated code (not runtime code) */
static volatile uint32_t g_rt; /* 0 free, 1 held, 2 held and waited for */

int gt_parallel(void)
{
    if (g_par < 0)
    {
        const char* e = getenv("XIVLEGACY_PARALLEL");
        g_par = e && *e == '1';
    }
    return g_par;
}

/* the runtime lock's waits: the drawing thread's, the others' (XIVLEGACY_FRAMESTATS) */
static volatile uint64_t g_rt_wait_game, g_rt_wait_other, g_rt_takes_game;
void gt_rt_costs(uint64_t* game_wait_ns, uint64_t* other_wait_ns, uint64_t* game_takes)
{
    *game_wait_ns = g_rt_wait_game, *other_wait_ns = g_rt_wait_other, *game_takes = g_rt_takes_game;
}

static void rt_take(void)
{
    uint64_t t0 = 0;
    if (game_now())
        g_rt_takes_game++;
    for (unsigned spin = 0;; ++spin)
    {
        uint32_t v = g_rt;
        if (v == 0 && plat_atomic_cas32(&g_rt, 0, 1) == 0)
            break;
        if (!t0)
            t0 = rt_monotonic_ns();
        if (spin < 200)
        {
            cpu_relax();
            continue;
        }
        if (v == 2 || plat_atomic_cas32(&g_rt, 1, 2) == 1)
            plat_wait32(&g_rt, 2);
        if (plat_atomic_cas32(&g_rt, 0, 2) == 0)
            break; /* taken, still marked waited for: the release wakes the others */
    }
    if (t0)
    {
        uint64_t w = rt_monotonic_ns() - t0;
        if (game_now())
            g_rt_wait_game += w;
        else
            g_rt_wait_other += w; /* approximate: racy between waiters */
    }
    t_rt = 1;
}

static void rt_drop(void)
{
    t_rt = 0;
    if (plat_atomic_add32(&g_rt, (uint32_t)-1) != 1)
    {
        g_rt = 0;
        plat_wake_all32(&g_rt);
    }
}

void gt_lock(void)
{
    if (!t_par)
        g_acquire();
    if (g_par > 0 && !t_guest && !t_rt)
        rt_take();
}

void gt_unlock(void)
{
    if (t_rt)
        rt_drop();
    if (!t_par)
        g_release();
}

static volatile int g_par_main = 1;
void gt_set_parallel_main(int on) { g_par_main = on != 0; }
static void go_parallel(void);

unsigned gt_shim_enter(void)
{
    if (t_main && t_guest && !t_rt && t_par != g_par_main)
    {
        /* switched between shims, where the main loop's thread holds neither lock (or only the guest lock) */
        if (g_par_main)
            go_parallel();
        else
        {
            t_par = 0;
            if (t_self)
                t_self->par = 0;
            g_acquire();
        }
    }
    unsigned saved = (unsigned)t_guest | (unsigned)t_rt << 1;
    t_guest = 0;
    if (!t_rt)
        rt_take();
    return saved;
}

void gt_shim_leave(unsigned saved)
{
    if (!(saved & 2) && t_rt)
        rt_drop();
    t_guest = (int)(saved & 1);
}

int gt_beside(void) { return t_par; }

/* a device call on the thread beside the guest lock: Direct3D's device is the guest lock group's */
void gt_device_enter(void)
{
    g_acquire();
    rt_take();
}

void gt_device_leave(void)
{
    rt_drop();
    g_release();
}

void gt_guest_begin(void)
{
    if (t_rt)
        rt_drop();
    t_guest = 1;
}

/* the main loop's thread, once it is known (gt_mark_main_thread), in a shim: it gives up the guest
 * lock for good */
static void go_parallel(void)
{
    if (t_par)
        return;
    t_par = 1;
    if (t_self)
        t_self->par = 1;
    if (t_held)
        g_release();
    rt_log("[gthread] main loop thread %u runs beside the guest lock (XIVLEGACY_PARALLEL)\n", plat_thread_id());
}

void gt_lock_urgent(void)
{
    plat_atomic_add32(&g_urgent, 1);
    rt_lock_contended = 1;
    t_urgent = 1;
    gt_lock();
    t_urgent = 0;
    plat_atomic_add32(&g_urgent, (uint32_t)-1);
}

void gt_mark_main_thread(void)
{
    t_main = 1;
    if (gt_parallel())
        go_parallel();
}

void gt_set_loading(int on) { g_loading = on != 0; }
int gt_loading(void) { return (int)g_loading; }

int gt_holds(void)
{
    return t_par ? t_rt : t_held;
}

/* Called at a loop's back edge once a waiter has asked (gt_lock). The clock check covers a request
 * raised against the previous holder's turn. */
static void yield_if_due(void)
{
    if (t_par)
        return; /* the request is the guest lock holder's */
    if (!t_held || !g_waiters || (!g_urgent && rt_monotonic_ns() < g_deadline))
    {
        rt_lock_contended = 0; /* a waiter asks again when this turn is up */
        return;
    }
    gt_unlock();
    gt_lock();
}

void gt_init(void)
{
    gt_parallel();
    t_quantum_ns = GAME_QUANTUM_NS;
    t_game = 1;
    rt_set_yield(yield_if_due);
}

/* --- threads ----------------------------------------------------------------------------------- */

/* Static TLS (__declspec(thread), the exe's .tls section): the template every thread copies, as
 * the Windows loader does. TEB+0x2C (ThreadLocalStoragePointer) points at an array whose slot 0
 * (the image's TLS index) is the thread's copy. */
static uint32_t g_tls_start, g_tls_end, g_tls_zero;

static void install_static_tls(GThread* t)
{
    if (!g_tls_end || rd32(t->teb + 0x2C))
        return;
    uint32_t size = g_tls_end - g_tls_start, block = gheap_alloc(size + g_tls_zero + 16, 1), array = gheap_alloc(64, 1);
    memcpy(GUEST_PTR(block), GUEST_PTR(g_tls_start), size);
    wr32(array, block);
    wr32(t->teb + 0x2C, array);
}

void gt_set_static_tls(uint32_t start, uint32_t end, uint32_t zero_fill)
{
    g_tls_start = start, g_tls_end = end, g_tls_zero = zero_fill;
    if (t_self)
        install_static_tls(t_self);
}

GThread* gt_self(void)
{
    if (t_self)
        return t_self;
    GThread* t = (GThread*)calloc(1, sizeof *t);
    uint32_t stack = gwin_alloc(GT_STACK_SIZE);
    uint32_t teb = gheap_alloc(0x1000, 1);
    if (!t || !stack || !teb)
    {
        rt_log("[recomp] out of memory creating a guest thread\n");
        abort();
    }
    t->stack_lo = stack;
    t->stack_hi = stack + GT_STACK_SIZE;
    t->teb = teb;
    t->tid = plat_thread_id();
    wr32(teb + 0x00, 0xFFFFFFFFu); /* SEH chain: empty */
    wr32(teb + 0x04, t->stack_hi); /* stack base */
    wr32(teb + 0x08, t->stack_lo); /* stack limit */
    wr32(teb + 0x18, teb);         /* self */
    wr32(teb + 0x20, 0x1000);      /* process id, as the guest sees it */
    wr32(teb + 0x24, t->tid);      /* thread id */
    t->g.esp = t->stack_hi - 64;
    t->g.fs_base = teb;
    t->g.fcw = 0x027F; /* the Win32 default: 53-bit precision, round to nearest, all masked */
    t->g.mxcsr = MXCSR_DEFAULT; /* round to nearest, all exceptions masked */
    install_static_tls(t);
    t_self = t;
    return t;
}

void gt_exit_self(void)
{
    GThread* t = t_self;
    if (!t)
        return;
    t_guest = 0; /* ExitThread may have jumped out of translated code */
    int took = !gt_holds();
    if (took)
        gt_lock();
    gwin_release(t->stack_lo);
    if (rd32(t->teb + 0x2C))
    {
        gheap_free(rd32(rd32(t->teb + 0x2C)));
        gheap_free(rd32(t->teb + 0x2C));
    }
    gheap_free(t->teb);
    t_self = NULL;
    free(t);
    if (took)
        gt_unlock();
}

void gt_mark_game_thread(void)
{
    /* the long turn and the right of way the thread that called gt_init has: for the thread that
     * draws, whose frame waits on every other thread's turn otherwise */
    t_game = 1;
    t_quantum_ns = GAME_QUANTUM_NS;
    t_drawer = ++g_drawer_gen;
}

void gt_fiber_leave(void)
{
    t_held = 0;
    t_rt = 0; /* passed on held, with the guest lock */
    t_self = NULL;
}

void gt_fiber_enter(GThread* t, int game)
{
    t_self = t;
    t_par = t->par;
    t_held = !t_par;
    t_rt = g_par > 0; /* inside SwitchToFiber's shim, or about to start the fiber (gt_guest_begin) */
    t_guest = 0;
    t_game = game;
    t_quantum_ns = game ? GAME_QUANTUM_NS : 0;
    t_drawer = game ? g_drawer_gen : 0;
}

int gt_is_game(void)
{
    return game_now();
}

void gt_set_error(uint32_t e)
{
    wr32(gt_self()->teb + 0x34, e);
}

uint32_t gt_get_error(void)
{
    return rd32(gt_self()->teb + 0x34);
}

/* --- host -> guest ----------------------------------------------------------------------------- */

static uint32_t call_with(uint32_t fn, int has_self, uint32_t self, unsigned nargs, const uint32_t* args);

uint32_t guest_call(uint32_t fn, unsigned nargs, const uint32_t* args)
{
    return call_with(fn, 0, 0, nargs, args);
}

uint32_t guest_thiscall(uint32_t fn, uint32_t self, unsigned nargs, const uint32_t* args)
{
    return call_with(fn, 1, self, nargs, args);
}

static uint32_t call_with(uint32_t fn, int has_self, uint32_t self, unsigned nargs, const uint32_t* args)
{
    int took = t_par ? (g_par > 0 && !t_guest && !t_rt) : !t_held;
    if (took)
        gt_lock();
    GThread* t = gt_self();
    Guest* g = &t->g;
    /* A call nested inside a shim (a window procedure, say) builds below the suspended call's
     * stack pointer; the whole Guest is restored afterwards except the x87 control word, which
     * is per-thread state the callee may legitimately change. */
    Guest saved = *g;
    uint32_t sp = g->esp - 4u * nargs;
    for (unsigned i = 0; i < nargs; ++i)
        wr32(sp + 4u * i, args[i]);
    sp -= 4;
    wr32(sp, 0xFEEDF00Du); /* return address: never used, the translation returns to us */
    g->esp = sp;
    g->df = 0;
    if (has_self)
        g->ecx = self;
    /* any guest function pointer: a translation, or a thunk to a shim (a method of one of our own
     * COM objects, say) - exactly as guest code's own indirect calls resolve */
    int was_guest = t_guest, had_rt = t_rt;
    if (had_rt)
        rt_drop();
    t_guest = 1;
    rt_call_indirect(g, fn);
    t_guest = was_guest;
    if (had_rt)
        rt_take();
    uint32_t eax = g->eax;
    uint16_t fcw = g->fcw;
    *g = saved;
    g->fcw = fcw;
    if (took)
        gt_unlock();
    return eax;
}
