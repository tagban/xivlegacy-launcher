/* A countdown for every timed duty (xiv_ctimer.c). */
#pragma once

#include <stddef.h>

/* CreateFileA is about to open a guest file for reading (host_path behind it): if it is the player's
 * script, the patched copy's host path goes to out and the result is 1. */
int xiv_ctimer_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n);
