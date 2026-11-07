/* Knockback slides for the local player (xiv_knockback.c). */
#pragma once
#include <stdint.h>

#define XKB_MAX_STREAMS 64

/* What the server's knockback note says. */
typedef struct
{
    uint32_t actor;       /* the pushed player */
    float to[3];          /* the landing spot (the server's: walls and cliffs already taken off) */
    float rotation;       /* facing the attacker */
    float dir[2];         /* unit push direction on the ground (x, z) */
    float distance;       /* yalms */
    uint32_t duration_ms; /* how long the slide takes */
    float start[3];       /* where the server had the player when the hit landed */
} XkbSlide;

/* Scan bytes just received on a game connection (stream = the socket's slot). A knockback warp
 * found whole in buf is offered to take(); when it says it slides the player, the warp is neutralized
 * in place (the game skips it). Returns how many were taken. Subpackets split across reads, and
 * warps take() turns down, are left alone: the plain warp still moves the player. */
int xkb_scan(int stream, uint8_t* buf, uint32_t n, int (*take)(const XkbSlide*));
/* A socket slot was opened or closed: forget its stream position. */
void xkb_stream_reset(int stream);

/* Slide the local player as *s says (the receive path, guest lock held): 1 when it will, 0 when its
 * actor wasn't found. xkb_frame moves it. */
int xkb_begin(const XkbSlide* s);

/* Where the slide is at t (0..1 of its duration) from where the player stood to the landing spot. */
void xkb_at(const float from[3], const float to[3], float t, float out[3]);
float xkb_ease(float t);
/* Each presented frame (the drawing thread, guest lock held): moves a slide in progress. */
void xkb_frame(void);

/* One subpacket (s, n bytes available): 1 and *out when it is a knockback warp with our note. */
int xkb_parse_warp(const uint8_t* s, uint32_t n, XkbSlide* out);
/* Make the game skip a knockback warp: its actor id becomes one nobody has. */
void xkb_neutralize(uint8_t* s);
