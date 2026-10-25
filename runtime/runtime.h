#pragma once

#include "guest.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

typedef struct RtEntry
{
    uint32_t addr;
    GuestFn fn;
} RtEntry;

/* Translated function at a guest address, or NULL. */
GuestFn rt_lookup(uint32_t addr);
/* Index of a translated function in rt_table, or -1. */
int rt_index(uint32_t addr);

/* Generated with the translation (table.c); a host built without it (XIVLEGACY_SPLIT) reads them
 * from the game module it loaded (xiv_game.h), under the same names. */
#ifndef XIVLEGACY_SPLIT
extern const RtEntry rt_table[];
extern const unsigned rt_table_count;
extern const unsigned char rt_table_patch[];
extern const uint32_t rt_image_base, rt_image_timestamp, rt_image_size, rt_image_text_rva, rt_image_text_size,
    rt_image_oep;
#else
#include "xiv_game.h"
#define rt_table (xiv_game->table)
#define rt_table_count (*xiv_game->table_count)
#define rt_table_patch (xiv_game->table_patch)
#define rt_image_base (*xiv_game->image_base)
#define rt_image_timestamp (*xiv_game->image_timestamp)
#define rt_image_size (*xiv_game->image_size)
#define rt_image_text_rva (*xiv_game->image_text_rva)
#define rt_image_text_size (*xiv_game->image_text_size)
#define rt_image_oep (*xiv_game->image_oep)
#endif

/* The image is at runtime_base: sets the relocation delta and the image range. */
void rt_set_image(uint32_t runtime_base);

/* The translation at a runtime guest address, or NULL. */
GuestFn rt_lookup_any(uint32_t runtime_addr);

#include <stdio.h>

/* Diagnostics: everything goes to stderr and, once set, to a log file. The fatal hook runs just
 * before abort (the Windows host shows a message box there). */
void rt_log(const char* fmt, ...);
void rt_set_log(FILE* f);
typedef void (*RtFatalHook)(uint32_t addr, const char* what);
void rt_set_fatal_hook(RtFatalHook h);

/* Monotonic nanoseconds, from the platform layer (64-bit hosts: rdtsc is built on it). */
uint64_t rt_monotonic_ns(void);

/* The guest lock's yield (release, let a waiter in, reacquire), run at safepoints. */
void rt_set_yield(void (*yield)(void));

/* Called for indirect targets with no translation (imports, mostly). Returns nonzero if handled. */
typedef int (*RtNativeHandler)(Guest* g, uint32_t target);
void rt_set_native_handler(RtNativeHandler h);
