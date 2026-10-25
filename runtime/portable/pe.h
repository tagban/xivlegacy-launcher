/* The retail image in the guest window (R3): ffxivgame.exe, mapped by our own loader at its
 * preferred base 0x400000 (RD = 0). */
#pragma once

#include <stdint.h>

#include "runtime.h"

/* Maps the game image, checks the build and points every import at a thunk. Returns 1 on
 * success; the reason is logged otherwise. */
int pe_load(const char* retail_path);
/* An export of the mapped image at base (pe_export: the game image's), or 0. */
uint32_t pe_export_at(uint32_t base, const char* name);
uint32_t pe_export(const char* name);
/* A resource of the image at base, by integer type and name (any language): its guest address
 * and size, or 0. */
uint32_t pe_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t* size);
/* FindResource's lookup: type and name as Win32 passes them (integer ids, "#n", or ANSI/UTF-16
 * strings). Returns the guest address of the IMAGE_RESOURCE_DATA_ENTRY (the HRSRC), or 0. */
uint32_t pe_find_resource(uint32_t base, uint32_t type, uint32_t name, int wide);
