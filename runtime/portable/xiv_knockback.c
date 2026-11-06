/* Knockback slides for the local player.
 *
 * The server pushes a player hit by a knockback move (Drop Kick, Iron Chariot, Vulcan Burst...) and
 * tells that player's own client with the in-zone warp (SetActorPosition 0x00CE, spawn type 0x11):
 * the game draws it as a fade to black and a camera reset. Our server appends a note to that warp
 * ("XKB1": push direction, distance, duration, start spot). This file finds the note in the game
 * connection's incoming stream, keeps the game from acting on the warp, and slides the player there
 * itself: from where it stands to the server's landing spot over the note's duration, facing the
 * attacker. A server without the note, or a client without this file, still gets the plain warp.
 *
 * Stream format (plain TCP, little endian): a 0x10-byte base header (u8 authenticated, u8 compressed,
 * u16 connection type, u16 total size, u16 subpacket count, u64 time), then subpackets: u16 size,
 * u16 type (3 = game message), u32 source actor, u32 target, u32 0, and for game messages a 0x10
 * header (u16 0x14, u16 opcode, ...) before the body. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xiv_knockback.h"

#define OP_SET_ACTOR_POSITION 0x00CEu
#define NOTE_MAGIC 0x31424B58u /* "XKB1" */
#define WARP_SIZE 0x68u        /* 0x10 subpacket + 0x10 game message + 0x28 body + 0x20 note */
#define BODY 0x20u
#define NOTE (BODY + 0x28u)

/* The game ignores a game message about an actor it doesn't know: the neutralized warp. */
#define NOBODY 0x7FFFFFF0u

typedef struct
{
    uint8_t hdr[0x10];
    uint32_t hdr_have;  /* bytes of the base header so far */
    uint32_t left;      /* bytes of the current base packet's subpackets left */
    int compressed;     /* the current base packet's body is zlib: not looked into */
    uint8_t sub[4];     /* the current subpacket's first bytes (its size) */
    uint32_t sub_have;  /* bytes of the current subpacket so far */
    uint32_t sub_size;
} Stream;

static Stream g_streams[XKB_MAX_STREAMS];
static int g_log = -1;

static uint16_t u16le(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t u32le(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static float f32le(const uint8_t* p)
{
    uint32_t w = u32le(p);
    float f;
    memcpy(&f, &w, 4);
    return f;
}

int xkb_parse_warp(const uint8_t* s, uint32_t n, XkbSlide* out)
{
    if (n < WARP_SIZE || u16le(s) != WARP_SIZE || u16le(s + 2) != 3 || u16le(s + 0x12) != OP_SET_ACTOR_POSITION ||
        u32le(s + NOTE) != NOTE_MAGIC)
        return 0;
    const uint8_t* b = s + BODY;
    out->actor = u32le(s + 4);
    out->to[0] = f32le(b + 0x08), out->to[1] = f32le(b + 0x0C), out->to[2] = f32le(b + 0x10);
    out->rotation = f32le(b + 0x14);
    out->dir[0] = f32le(s + NOTE + 0x04), out->dir[1] = f32le(s + NOTE + 0x08);
    out->distance = f32le(s + NOTE + 0x0C);
    out->duration_ms = u16le(s + NOTE + 0x10);
    out->start[0] = f32le(s + NOTE + 0x14), out->start[1] = f32le(s + NOTE + 0x18), out->start[2] = f32le(s + NOTE + 0x1C);
    if (!isfinite(out->to[0]) || !isfinite(out->to[1]) || !isfinite(out->to[2]) || !isfinite(out->rotation))
        return 0;
    if (out->duration_ms < 50)
        out->duration_ms = 50;
    if (out->duration_ms > 2000)
        out->duration_ms = 2000;
    return 1;
}

void xkb_neutralize(uint8_t* s)
{
    s[4] = (uint8_t)NOBODY, s[5] = (uint8_t)(NOBODY >> 8), s[6] = (uint8_t)(NOBODY >> 16), s[7] = (uint8_t)(NOBODY >> 24);
}

void xkb_stream_reset(int stream)
{
    if (stream >= 0 && stream < XKB_MAX_STREAMS)
        memset(&g_streams[stream], 0, sizeof g_streams[stream]);
}

int xkb_scan(int stream, uint8_t* buf, uint32_t n, int (*take)(const XkbSlide*))
{
    if (stream < 0 || stream >= XKB_MAX_STREAMS || !buf)
        return 0;
    if (g_log < 0)
        g_log = getenv("XIVLEGACY_KBLOG") != NULL;
    Stream* st = &g_streams[stream];
    int found = 0;
    uint32_t i = 0;
    while (i < n)
    {
        if (st->hdr_have < sizeof st->hdr)
        {
            st->hdr[st->hdr_have++] = buf[i++];
            if (st->hdr_have == sizeof st->hdr)
            {
                uint16_t size = u16le(st->hdr + 4);
                if (size < sizeof st->hdr)
                {
                    /* not a stream we understand (or out of step): look no further on it */
                    st->hdr_have = sizeof st->hdr, st->left = 0xFFFFFFFFu, st->compressed = 1;
                    continue;
                }
                st->left = size - (uint32_t)sizeof st->hdr;
                st->compressed = st->hdr[1] != 0;
                st->sub_have = 0;
                if (!st->left)
                    st->hdr_have = 0;
            }
            continue;
        }
        if (st->compressed)
        {
            uint32_t k = n - i < st->left ? n - i : st->left;
            if (st->left != 0xFFFFFFFFu)
                st->left -= k;
            i += k;
            if (!st->left)
                st->hdr_have = 0;
            continue;
        }
        /* inside a subpacket */
        if (st->sub_have == 0 && n - i >= WARP_SIZE && st->left >= WARP_SIZE)
        {
            /* a whole subpacket header is here: check it in place (the only case we change) */
            uint32_t size = u16le(buf + i);
            XkbSlide s;
            if (size == WARP_SIZE && xkb_parse_warp(buf + i, n - i, &s))
            {
                int took = take && take(&s);
                if (took)
                    xkb_neutralize(buf + i), found++;
                if (g_log)
                    fprintf(stderr, "[knockback] note for %08x: to %.2f %.2f %.2f rot %.2f, %.2f y over %u ms%s\n", s.actor, s.to[0],
                        s.to[1], s.to[2], s.rotation, s.distance, s.duration_ms, took ? "" : " (left to the warp)");
            }
        }
        if (st->sub_have < 2)
            st->sub[st->sub_have] = buf[i];
        st->sub_have++, st->left--, i++;
        if (st->sub_have == 2)
        {
            st->sub_size = u16le(st->sub);
            if (st->sub_size < 2 || st->sub_size > st->left + 2)
            {
                /* out of step: skip the rest of this base packet */
                st->compressed = 1;
                if (!st->left)
                    st->hdr_have = 0;
                continue;
            }
        }
        if (st->sub_have >= 2 && st->sub_have == st->sub_size)
            st->sub_have = 0;
        if (!st->left)
            st->hdr_have = 0, st->sub_have = 0;
    }
    return found;
}

float xkb_ease(float t)
{
    if (!(t > 0.0f))
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    /* a shove: fastest at the hit, settling into the landing spot (ease-out cubic) */
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}

void xkb_at(const float from[3], const float to[3], float t, float out[3])
{
    float e = xkb_ease(t);
    for (int i = 0; i < 3; ++i)
        out[i] = from[i] + (to[i] - from[i]) * e;
}
