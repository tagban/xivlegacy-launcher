/* Monster Points and the monster vendors' shop pack (xiv_monster_points.c). */
#pragma once

#include <stddef.h>

/* CreateFileA is about to open a guest file for reading (host_path behind it): if it is one of the
 * sheets this changes, the changed copy's host path goes to out and the result is 1. */
int xiv_monster_points_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n);
