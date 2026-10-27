/* Battle messages as a tab of the main log window (xiv_logtabs.c). */
#pragma once

#include <stdint.h>

/* CreateFileA opened a guest file: if it is one we patch, remember the host file. */
void xiv_logtabs_opened(const char* guest_path, const char* host_path, void* file);
/* n bytes of that file at offset at (< 0: ending at the file's position) were just read into buf:
 * patch them there. */
void xiv_logtabs_read(void* file, int64_t at, uint8_t* buf, int64_t n);
/* the host file is closing */
void xiv_logtabs_closed(void* file);
/* Once per character: the saved log settings under the game's Documents folder (user_dir is the
 * host folder behind C:\users\Player) moved from a separate battle window to a Battle tab. */
void xiv_logtabs_migrate(const char* user_dir);
