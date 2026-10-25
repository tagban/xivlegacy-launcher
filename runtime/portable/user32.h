/* USER32 and WINMM on SDL3 (user32.c). */
#pragma once

#include <stdint.h>

void user32_init(void);
/* An SDL_Window already open (a launcher's sign-in screen, the graphics back end already on it) for
 * the game's first top-level window to take over instead of opening one: one window, start to end. */
void user32_adopt_window(void* sdl_window);
/* A full-screen device (D3D9 Windowed = FALSE) on hwnd: its window covers the display, in the
 * desktop's own mode. */
void user32_set_fullscreen(uint32_t hwnd, int on);
/* The SDL_Window behind a guest HWND (for the graphics layer), or NULL. */
void* user32_sdl_window(uint32_t hwnd);
/* A window's client size (unchanged if hwnd is not ours); the desktop's mode. */
void user32_client_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
/* Its size as shown: the client size, unless the player made it full screen or resized it (the
 * game still draws at the client size, stretched). */
void user32_shown_size(uint32_t hwnd, uint32_t* w, uint32_t* h);
/* The player's window or full screen, over what the game set up: 0 a window, 1 full screen (the
 * desktop's mode), 2 the other of the two. Done by the next message pump; any thread. */
void user32_request_display(int mode);
void user32_desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz);
/* The window size the player last gave (points; 0 x 0: the game's): a framed window opens at it, at
 * the game's shape. */
void user32_set_window_size(int w, int h);
/* Asked when the player closes the window (its close button, Cmd+Q, the Dock's Quit): 1 when the host
 * took it (a logout begun, so the server hears the player leave), and the run goes on; 0 ends it now. */
void user32_set_close_handler(int (*fn)(void));
/* Set by the host: sees every key (SDL scancode, SDL modifiers, down) before the game does, and
 * returns 1 to keep it from the game (a host's hotkeys). */
extern int (*user32_key_hook)(int scancode, int mods, int down);
