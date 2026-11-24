/* Scripted walking for test clients (walk.c): the XIVLEGACY_CONTROL commands
 *   walkto X Z [stop] [near SX SZ [SY]] run to (X, Z), stop within `stop` yalms (default 1)
 *   walkpath FILE [stop] [near ...]      follow the points in FILE ("x y z" or "x z" a line)
 *   walkstop                            stop a walk
 *   walkwhere [near SX SZ]              log the player's position and facing
 * `near` is a hint for finding the player in memory the first time (the server's last position). */
#pragma once
#include <SDL3/SDL.h>

/* 1 when the line was a walk command (handled), 0 otherwise */
int walk_command(const char* line, SDL_WindowID win);
