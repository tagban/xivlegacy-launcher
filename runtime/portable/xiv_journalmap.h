/* The Journal's Map button opens a map even for a quest with no marker (xiv_journalmap.c). */
#pragma once

#include <stddef.h>

/* CreateFileA is about to open a guest file for reading (host_path behind it): if it is the map
 * window's script, the patched copy's host path goes to out and the result is 1. */
int xiv_journalmap_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n);
