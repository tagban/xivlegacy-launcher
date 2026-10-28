/* Win32 fibers for ffxivgame.exe's job system, and OpenThread.
 *
 * Translated code runs on the host stack and keeps its Guest* for the whole call chain, so a fiber
 * here is a host thread of its own with its own GThread (registers, guest stack), parked while
 * another fiber runs. SwitchToFiber hands the guest lock over and wakes the target; what belongs to
 * the Win32 thread rather than the fiber - its TEB, thread id and TlsAlloc slots - goes with the
 * switch, and the TEB's per-fiber fields (SEH chain, stack bounds, FiberData) are swapped, as
 * Windows does. A fiber may be resumed by a different thread than the one that left it. */
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "kobj.h"
#include "plat.h"
#include "thunk.h"

#define FIBER_MAGIC 0x52424946u /* 'FIBR' */
#define MAX_FIBERS 4096
#define ERROR_ALREADY_FIBER 1280u
#define ERROR_INVALID_PARAMETER 87u

typedef struct Fiber
{
    uint32_t guest; /* what the guest holds: {FiberData, magic, index} */
    uint32_t start, param;
    uint32_t stack_lo, stack_hi; /* 0 for a converted thread: its own stack */
    GThread* gt;                 /* the registers and guest stack this fiber's frames use */
    uint32_t seh, sbase, slimit; /* the TEB's per-fiber fields while switched out */
    volatile uint32_t run;       /* set to resume the parked host thread */
    int has_thread, dead, game;
} Fiber;

static Fiber* g_fibers[MAX_FIBERS];
static int g_log = -1;
#define FLOG(...) do { if (g_log < 0) g_log = getenv("XIVLEGACY_FIBERLOG") != NULL; if (g_log) rt_log(__VA_ARGS__); } while (0)
static unsigned g_nfibers;

static Fiber* fiber_of(uint32_t p)
{
    if (!p || rd32(p + 4) != FIBER_MAGIC)
        return NULL;
    uint32_t i = rd32(p + 8);
    return i < g_nfibers ? g_fibers[i] : NULL;
}

static Fiber* fiber_new(uint32_t param)
{
    if (g_nfibers == MAX_FIBERS)
        return NULL;
    Fiber* f = (Fiber*)calloc(1, sizeof *f);
    f->guest = gheap_alloc(16, 1);
    f->param = param;
    wr32(f->guest, param);
    wr32(f->guest + 4, FIBER_MAGIC);
    wr32(f->guest + 8, g_nfibers);
    g_fibers[g_nfibers++] = f;
    return f;
}

/* ConvertThreadToFiber(param) */
static void sh_ConvertThreadToFiber(Guest* g)
{
    GThread* t = gt_self();
    if (fiber_of(rd32(t->teb + 0x10)))
    {
        gt_set_error(ERROR_ALREADY_FIBER);
        RET(0, 1);
    }
    Fiber* f = fiber_new(ARG(0));
    if (!f)
        RET(0, 1);
    f->gt = t;
    f->has_thread = 1;
    wr32(t->teb + 0x10, f->guest);
    FLOG("[fiber] host %u: ConvertThreadToFiber -> %08x (gt %p, tid %u)\n", plat_thread_id(), f->guest, (void*)t, t->tid);
    RET(f->guest, 1);
}

/* CreateFiberEx(commit, reserve, flags, start, param) */
static void sh_CreateFiberEx(Guest* g)
{
    uint32_t size = ARG(1) ? ARG(1) : ARG(0);
    if (size < (1u << 20))
        size = 1u << 20;
    size = (size + 0xFFFFu) & ~0xFFFFu;
    Fiber* f = fiber_new(ARG(4));
    uint32_t stack = f ? gwin_alloc(size) : 0;
    if (!f || !stack)
        RET(0, 5);
    f->start = ARG(3);
    f->stack_lo = stack, f->stack_hi = stack + size;
    f->seh = 0xFFFFFFFFu, f->sbase = f->stack_hi, f->slimit = f->stack_lo;
    GThread* t = (GThread*)calloc(1, sizeof *t);
    t->stack_lo = f->stack_lo, t->stack_hi = f->stack_hi;
    t->g.esp = f->stack_hi - 64;
    t->g.fcw = 0x027F;
    t->g.mxcsr = MXCSR_DEFAULT;
    f->gt = t;
    FLOG("[fiber] host %u: CreateFiberEx(start %08x) -> %08x (gt %p)\n", plat_thread_id(), f->start, f->guest, (void*)t);
    RET(f->guest, 5);
}

/* A created fiber's host thread: runs its start routine on its own guest stack. */
static void fiber_main(void* arg)
{
    Fiber* f = (Fiber*)arg;
    while (!f->run)
        plat_wait32(&f->run, 0);
    gt_fiber_enter(f->gt, f->game);
    Guest* g = &f->gt->g;
    uint32_t sp = f->stack_hi - 64;
    wr32(sp - 4, f->param);
    wr32(sp - 8, 0xFEEDF00Du);
    g->esp = sp - 8;
    g->df = 0;
    gt_guest_begin();
    rt_call_indirect(g, f->start);
    /* a fiber routine that returns ends its thread on Windows; the job system's never do */
    rt_log("[fiber] fiber %08x returned from its start routine; ending the process\n", f->guest);
    exit(1);
}

/* SwitchToFiber(fiber) */
static void sh_SwitchToFiber(Guest* g)
{
    GThread* t = gt_self();
    uint32_t teb = t->teb;
    Fiber* cur = fiber_of(rd32(teb + 0x10));
    Fiber* to = fiber_of(ARG(0));
    if (!cur || !to || to->dead)
    {
        rt_log("[fiber] SwitchToFiber(%08x) from %08x: not a fiber\n", ARG(0), rd32(teb + 0x10));
        RET(0, 1);
    }
    if (to == cur)
        RET(0, 1);
    FLOG("[fiber] host %u: switch %08x (gt %p) -> %08x (gt %p)\n", plat_thread_id(), cur->guest, (void*)cur->gt, to->guest,
        (void*)to->gt);
    if (cur->gt != t)
        rt_log("[fiber] switching from %08x whose gt %p is not this thread's %p\n", cur->guest, (void*)cur->gt, (void*)t);
    /* our own return happens now: the state saved is the state after SwitchToFiber returns */
    g->eax = 0;
    g->esp += 8;
    cur->seh = rd32(teb), cur->sbase = rd32(teb + 4), cur->slimit = rd32(teb + 8);
    cur->game = gt_is_game();

    /* the thread's things go with it to the fiber that runs next */
    GThread* d = to->gt;
    d->teb = teb;
    d->tid = t->tid;
    d->g.fs_base = teb;
    d->par = t->par;
    memcpy(d->tls, t->tls, sizeof d->tls);
    wr32(teb, to->seh);
    wr32(teb + 4, to->sbase);
    wr32(teb + 8, to->slimit);
    wr32(teb + 0x10, to->guest);
    to->game = cur->game;

    cur->run = 0;
    gt_fiber_leave();
    to->run = 1;
    if (!to->has_thread)
    {
        to->has_thread = 1;
        if (!plat_thread_start(fiber_main, to))
        {
            rt_log("[fiber] cannot start a host thread for fiber %08x\n", to->guest);
            abort();
        }
    }
    else
        plat_wake_all32(&to->run);

    while (!cur->run)
        plat_wait32(&cur->run, 0);
    gt_fiber_enter(cur->gt, cur->game);
}

/* DeleteFiber(fiber): a parked fiber's host thread stays parked (the job system deletes its
 * fibers at exit); its guest stack is kept, as frames on the host side still point into it. */
static void sh_DeleteFiber(Guest* g)
{
    Fiber* f = fiber_of(ARG(0));
    if (f && f->guest == rd32(gt_self()->teb + 0x10))
    {
        rt_log("[fiber] DeleteFiber on the running fiber: ending the thread\n");
        k_thread_exit(0);
    }
    if (f)
    {
        f->dead = 1;
        wr32(f->guest + 4, 0);
    }
    RET(0, 1);
}

/* OpenThread(access, inherit, tid): the game asks only for GetThreadPriority, which needs no
 * particular thread here. */
static void sh_OpenThread(Guest* g) { RET(k_new(K_OTHER, NULL, NULL), 3); }

static const ShimDef FIBER[] = {
    { "kernel32.dll", "ConvertThreadToFiber", sh_ConvertThreadToFiber },
    { "kernel32.dll", "CreateFiberEx", sh_CreateFiberEx },
    { "kernel32.dll", "SwitchToFiber", sh_SwitchToFiber },
    { "kernel32.dll", "DeleteFiber", sh_DeleteFiber },
    { "kernel32.dll", "OpenThread", sh_OpenThread },
    { NULL, NULL, NULL },
};

void xiv_fiber_init(void) { thunk_register(FIBER); }
