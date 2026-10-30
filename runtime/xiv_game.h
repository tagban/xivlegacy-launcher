/* The game as a module: the translation of the player's ffxivgame.exe, compiled on the player's
 * machine into a shared library (runtime/xiv_module.c + generated/c), and loaded by a host that
 * ships prebuilt (runtime/portable/xiv_load.c, built with XIVLEGACY_SPLIT). The host carries the
 * runtime, graphics, sound, input and networking and none of the game's code, so it can be
 * published; the module is made from each player's own install and never leaves their machine.
 *
 * The translation needs little of the runtime: five functions (given to it through XivHostFns)
 * and three variables, which the module keeps itself so its hot paths read them directly: the
 * guest window's base and the relocation delta (set once, before any guest code runs) and the
 * guest lock's contention flag (which the host's threads write through the pointer here).
 * Everything else crosses in the host's direction: the function table and the image's pinned
 * values, all in XivGameModule. */
#pragma once

#include "runtime.h"

#define XIV_GAME_ABI 4 /* 2: rt_table entries carry the fast bodies (guest.h RtRegs); 3: macOS window at 0x10000000000; 4: Lua's protected call hook */

typedef struct XivHostFns
{
    void (*call_indirect)(Guest* g, uint32_t target);
    void (*fatal)(Guest* g, uint32_t addr, const char* what);
    void (*cpuid)(Guest* g);
    void (*rdtsc)(Guest* g);
    void (*safepoint)(void);
} XivHostFns;

typedef struct XivGameModule
{
    uint32_t abi, size; /* XIV_GAME_ABI, sizeof (XivGameModule) */
    /* the translation (table.c) */
    const RtEntry* table;
    const unsigned* table_count;
    const unsigned char* table_patch;
    const uint32_t *image_base, *image_timestamp, *image_size, *image_text_rva, *image_text_size, *image_oep;
    /* the recompiler's hook point at the game's log stream (recomp --hooks streamlog=...) */
    GuestFn* hook_streamlog;
    /* Lua's protected call, run by the host (recomp --hooks replace_lua_protected=...) */
    GuestFn* hook_lua_protected;
    /* the module's own copies of the runtime's variables */
    unsigned char** guest_base;
    uint32_t* reloc_delta;
    volatile uint32_t* lock_contended;
    void (*set_host)(const XivHostFns* fns);
} XivGameModule;

/* The one symbol a module exports. */
#define XIV_GAME_SYMBOL "xiv_game_module"

#ifdef XIVLEGACY_SPLIT
/* the host: the module it loaded (xiv_load.c) */
extern const XivGameModule* xiv_game;
/* Loads the module at path (a .dylib, .so or .dll) and checks it; 0 with a message on failure. */
int xiv_game_load(const char* path, char* err, unsigned errsize);
/* Once the guest window is reserved: hands the module the host's functions and state. */
void xiv_game_bind(void);
#endif
