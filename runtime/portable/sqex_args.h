#pragma once

#include <stddef.h>
#include <stdint.h>

/* The "sqex0002...!////" launch argument for a 56-character session id and a GetTickCount value
 * (see sqex_args.c). Returns 0 if the session id is not 56 characters or `out` is too small. */
int sqex_launch_arg(const char* session_id, uint32_t tick, char* out, size_t n);
