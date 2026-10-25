/* The host's side of the game as a module (xiv_game.h): load it, check it, and bind it. */
#include "xiv_game.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#endif

const XivGameModule* xiv_game;

int xiv_game_load(const char* path, char* err, unsigned errsize)
{
#ifdef _WIN32
    HMODULE h = LoadLibraryA(path);
    if (!h)
        return snprintf(err, errsize, "%s: cannot load it (error %lu)", path, (unsigned long)GetLastError()), 0;
    const XivGameModule* m = (const XivGameModule*)(void*)GetProcAddress(h, XIV_GAME_SYMBOL);
#else
    /* a hardened macOS program (the signed app) refuses relative library paths */
    char abs[PATH_MAX];
    void* h = dlopen(realpath(path, abs) ? abs : path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        return snprintf(err, errsize, "%s", dlerror()), 0;
    const XivGameModule* m = (const XivGameModule*)dlsym(h, XIV_GAME_SYMBOL);
#endif
    if (!m)
        return snprintf(err, errsize, "%s: not a game module (no %s)", path, XIV_GAME_SYMBOL), 0;
    if (m->abi != XIV_GAME_ABI || m->size != sizeof(XivGameModule))
        return snprintf(err, errsize,
                   "%s: made for another version of XIVLegacy (module ABI %u, host %u); make the game again", path,
                   m->abi, XIV_GAME_ABI),
               0;
    xiv_game = m;
    return 1;
}

void xiv_game_bind(void)
{
    *xiv_game->guest_base = rt_guest_base;
    *xiv_game->reloc_delta = rt_reloc_delta;
    rt_lock_contended_ptr = xiv_game->lock_contended;
    XivHostFns fns = { rt_call_indirect, rt_fatal, rt_cpuid, rt_rdtsc, rt_safepoint };
    xiv_game->set_host(&fns);
}
