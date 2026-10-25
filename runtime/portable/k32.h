/* KERNEL32 shims (k32.c). game_dir: the game's install folder, as the guest sees it; exe_path:
 * what the host process is called, as the guest sees it (<game_dir>\ffxivgame.exe). */
#pragma once

void k32_init(const char* game_dir, const char* exe_path);
/* The arguments after the exe in GetCommandLine (call before the game starts). */
void k32_set_args(const char* args);
/* Files, directories, synchronisation, threads, time (k32_io.c). */
void k32_io_init(void);
void k32_misc_init(void); /* k32_misc.c */

/* The registry (reg.c): .reg files in `reg export` format, loaded in order, then the overlay the
 * game's own changes are saved to (may be NULL: changes stay in memory). */
void reg_init(const char* const* files, unsigned nfiles, const char* overlay);
/* A .reg loaded after the overlay, so its values win over what the game saved (a launcher's settings). */
void reg_load_final(const char* path);
/* ole32 / OLEAUT32 (ole.c): COM plumbing, CoCreateInstance of the host's own classes. */
void ole_init(void);
/* A COM class the host implements: create makes an instance (a guest COM object). */
void ole_register_host_class(const uint8_t clsid[16], uint32_t (*create)(void));
void xaudio2_init(void);

/* A DWORD value by full path ("HKEY_LOCAL_MACHINE\SOFTWARE\..."), for the host; 1 if found. */
int reg_get_dword(const char* path, const char* name, uint32_t* out);
/* A string value, the same way (NUL-terminated, truncated to n). */
int reg_get_string(const char* path, const char* name, char* out, size_t n);
/* Sets a string value (REG_SZ), creating the key: what the host knows better than the imported file. */
void reg_set_string(const char* path, const char* name, const char* value);

/* xiv_k32.c: the wide-character and newer KERNEL32 functions ffxivgame.exe needs. */
void xiv_k32_init(void);
/* xiv_misc.c: CRYPT32, WINTRUST and the other small DLLs ffxivgame.exe imports. */
void xiv_misc_init(void);
void xiv_patches_apply(void); /* xiv_misc.c: after pe_load */
void xiv_game_log_poll(void);  /* xiv_misc.c: each frame (XIVLEGACY_GAMELOG) */
/* d3d9.c: Direct3D 9 (in progress). */
void d3d9_init(void);
void d3dx9_init(void);
void xiv_fiber_init(void);
void xiv_net_init(void);
