/* Guest threads, the guest lock, and calls from the host into guest code (R3).
 *
 * Guest stacks and TEBs live in the guest window, and the host calls a translation directly
 * (guest_call). The guest lock keeps the policy measured on Windows: FIFO hand-off,
 * a ~2 ms quantum, released around calls that may block. On arm64 the hand-off is also what
 * gives the guest x86's memory ordering. */
#pragma once

#include "runtime.h"

#define GT_TLS_SLOTS 64u
#define GT_STACK_SIZE (4u << 20)

typedef struct GThread
{
    Guest g; /* first: a Guest* is a GThread* */
    uint32_t stack_lo, stack_hi, teb;
    uint32_t tid;
    uint32_t tls[GT_TLS_SLOTS];
    int par; /* runs beside the guest lock (XIVLEGACY_PARALLEL) */
} GThread;

/* This thread's guest state, created on first use (the caller holds the guest lock). */
GThread* gt_self(void);
/* Frees this thread's guest state; its host thread is about to end. */
void gt_exit_self(void);

/* The guest lock. gt_lock/gt_unlock bracket a shim's blocking section (Sleep, waits, I/O). */
void gt_init(void);
/* The image's static TLS template [start, end) plus zero_fill bytes (pe.c, at load). */
void gt_set_static_tls(uint32_t start, uint32_t end, uint32_t zero_fill);
void gt_lock(void);
void gt_unlock(void);
/* gt_lock for a thread on a deadline (the audio engine): the holder yields at its next safe point
 * instead of at the end of its turn. */
void gt_lock_urgent(void);
/* The calling thread runs the game's main loop: its turn with the lock is longer (gthread.c). */
void gt_mark_main_thread(void);
/* nanoseconds threads other than the drawing thread have held the lock in all (hitch log) */
uint64_t gt_other_held_ns(void);
/* the drawing thread's total lock waits and file reads (hitch log); gt_note_read records a read */
void gt_game_costs(uint64_t* wait_ns, uint64_t* read_ns, uint64_t* read_bytes);
void gt_note_read(uint64_t ns, uint64_t bytes);
int gt_holds(void);
/* XIVLEGACY_PARALLEL=1 (gthread.c): the main loop runs translated code beside the guest lock, and
 * runtime code takes the runtime lock. gt_shim_enter/leave bracket a shim; gt_guest_begin starts
 * translated code on a host thread that holds the runtime lock (a fiber's first run). */
int gt_parallel(void);
unsigned gt_shim_enter(void);
void gt_shim_leave(unsigned saved);
void gt_guest_begin(void);
/* this thread runs beside the guest lock; a Direct3D device call there takes it (thunk.c) */
int gt_beside(void);
void gt_device_enter(void);
void gt_device_leave(void);
/* with XIVLEGACY_PARALLEL: 0 puts the main loop back under the guest lock, 1 beside it again (A/B tests) */
void gt_set_parallel_main(int on);
void gt_rt_costs(uint64_t* game_wait_ns, uint64_t* other_wait_ns, uint64_t* game_takes);
/* While the game shows its loading screen (xiv_loading_poll), loading comes first: the drawing
 * thread's turn with the guest lock is short. */
void gt_set_loading(int on);
int gt_loading(void);

/* Fibers (xiv_fiber.c): the guest lock and the running guest thread pass from one host thread to
 * another with each SwitchToFiber. gt_fiber_leave gives both up without unlocking; the host
 * thread that runs next takes them with gt_fiber_enter. */
void gt_fiber_leave(void);
void gt_fiber_enter(GThread* t, int game);
int gt_is_game(void);

/* This thread gets the game thread's scheduling: a long turn with the guest lock, and others wait
 * while it waits (d3d9.c: the thread that presents). */
void gt_mark_game_thread(void);

/* Win32 last-error of the current guest thread (the TEB's LastErrorValue, fs:[0x34]). */
void gt_set_error(uint32_t e);
uint32_t gt_get_error(void);

/* Calls a guest function pointer (translated code, or a thunk to a shim) with 32-bit arguments (pushed right to left,
 * so args[0] is the first parameter), stdcall or cdecl alike; returns eax. Takes the guest lock
 * if this thread does not hold it. */
uint32_t guest_call(uint32_t fn, unsigned nargs, const uint32_t* args);
/* The same for a method (thiscall): the object in ecx. */
uint32_t guest_thiscall(uint32_t fn, uint32_t self, unsigned nargs, const uint32_t* args);
