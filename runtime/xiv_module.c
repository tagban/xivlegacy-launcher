/* The module's side of the game as a module (xiv_game.h): compiled with the translation into a
 * shared library that exports xiv_game_module and nothing else. */
#include "xiv_game.h"

#include <stdlib.h>

#if defined(_WIN32)
#define XIV_EXPORT __declspec(dllexport)
#else
#define XIV_EXPORT __attribute__((visibility("default")))
#endif

/* the runtime's variables the translation reads (guest.h), kept here */
unsigned char* rt_guest_base;
uint32_t rt_reloc_delta;
volatile uint32_t rt_lock_contended;

static XivHostFns g_host;

void rt_call_indirect(Guest* g, uint32_t target) { g_host.call_indirect(g, target); }
void rt_fatal(Guest* g, uint32_t addr, const char* what)
{
    g_host.fatal(g, addr, what);
    abort(); /* the host's does not return */
}
void rt_cpuid(Guest* g) { g_host.cpuid(g); }
void rt_rdtsc(Guest* g) { g_host.rdtsc(g); }
void rt_safepoint(void) { g_host.safepoint(); }

static void set_host(const XivHostFns* fns) { g_host = *fns; }

#include "rt_fastcall.h"

extern GuestFn rt_hook_streamlog;

XIV_EXPORT const XivGameModule xiv_game_module = {
    XIV_GAME_ABI,
    sizeof(XivGameModule),
    rt_table,
    &rt_table_count,
    rt_table_patch,
    &rt_image_base,
    &rt_image_timestamp,
    &rt_image_size,
    &rt_image_text_rva,
    &rt_image_text_size,
    &rt_image_oep,
    &rt_hook_streamlog,
    &rt_guest_base,
    &rt_reloc_delta,
    &rt_lock_contended,
    set_host,
};
