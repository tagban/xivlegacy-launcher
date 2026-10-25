/* The game window's icon from the player's ffxivgame.exe (xiv_icon.c). */
#pragma once

typedef struct SDL_Window SDL_Window;

/* The host path of ffxivgame.exe, once known (xiv_boot.c). */
void xiv_icon_source(const char* host_exe_path);
/* Sets the window's icon from that file's 48 px icon, if it has one. */
void xiv_icon_apply(SDL_Window* w);
