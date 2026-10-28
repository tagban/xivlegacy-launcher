/* The "A Word to Our Players" box before the lobby, in XIVLegacy's words (xiv_notice.c). */
#pragma once

#include <stdint.h>

/* CreateFileA opened a guest file for reading: if it is a sheet we patch, remember the host file. */
void xiv_notice_opened(const char* guest_path, const char* host_path, void* file);
/* n bytes of that file at offset at (< 0: ending at the file's position) were just read into buf:
 * patch them there. */
void xiv_notice_read(void* file, int64_t at, uint8_t* buf, int64_t n);
/* the host file is closing */
void xiv_notice_closed(void* file);
