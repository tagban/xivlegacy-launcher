/* The bug-report key (bug_report.c): a screenshot and the logs to the server, then "!bug " in chat. */
#pragma once

#include <SDL3/SDL.h>

/* the game session id, which signs the uploads in */
void bug_report_init(const char* session_id);
/* every SDL event, before the game's input sees it: 1 when it was the bug-report key (eaten) */
int bug_report_event(const SDL_Event* e);
/* once a pump: a gamepad's held L3+R3 */
void bug_report_tick(void);
