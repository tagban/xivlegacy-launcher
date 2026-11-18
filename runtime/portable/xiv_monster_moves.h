/* Icons for monster moves on a player's hotbar (xiv_monster_moves.c). */
#pragma once

#include <stdint.h>

/* CreateFileA opened a guest file for reading: if it is the command icon sheet, remember the host file. */
void xiv_monster_moves_opened(const char* guest_path, const char* host_path, void* file);
/* n bytes of that file at offset at (< 0: ending at the file's position) were just read into buf. */
void xiv_monster_moves_read(void* file, int64_t at, uint8_t* buf, int64_t n);
/* the host file is closing */
void xiv_monster_moves_closed(void* file);
