/* Starting the game (xiv_boot.c), shared by the hosts. */
#pragma once

#include <stdint.h>

/* argv: [--module <file>] <1.23b install folder> [session id]; 0 when ready, else an exit code. */
int xiv_boot_prepare(int* argcp, char** argv);
/* The game's entry point; returns when the game ends (it usually ends the process itself). */
uint32_t xiv_boot_run(void);
