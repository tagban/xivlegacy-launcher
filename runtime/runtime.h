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

/* Generated with the translation (table.c). */
extern const RtEntry rt_table[];
extern const unsigned rt_table_count;
extern const unsigned char rt_table_patch[];
extern const uint32_t rt_image_base, rt_image_timestamp, rt_image_size, rt_image_text_rva, rt_image_text_size,
    rt_image_oep;

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
