/* The party list shows NPC party members' HP and MP numbers (xiv_partynpc.c). */
#pragma once

#include <stddef.h>

/* CreateFileA is about to open a guest file for reading (host_path behind it): if it is the party
 * list's script, the patched copy's host path goes to out and the result is 1. */
int xiv_partynpc_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n);
