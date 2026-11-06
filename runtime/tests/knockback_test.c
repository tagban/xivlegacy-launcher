/* xiv_knockback.c stream scanner:
 *   clang -O1 -Iruntime/portable runtime/tests/knockback_test.c runtime/portable/xiv_knockback.c -o /tmp/kbt && /tmp/kbt */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "xiv_knockback.h"

static void w16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void w32(uint8_t* p, uint32_t v) { w16(p, (uint16_t)v), w16(p + 2, (uint16_t)(v >> 16)); }
static void wf(uint8_t* p, float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    w32(p, v);
}

/* a game-message subpacket: opcode, source actor, body of n bytes */
static uint32_t sub(uint8_t* p, uint16_t opcode, uint32_t actor, uint32_t n)
{
    memset(p, 0, 0x20 + n);
    w16(p, (uint16_t)(0x20 + n)), w16(p + 2, 3), w32(p + 4, actor), w32(p + 8, actor);
    w16(p + 0x10, 0x14), w16(p + 0x12, opcode);
    return 0x20 + n;
}

static uint32_t warp(uint8_t* p, int note)
{
    uint32_t n = sub(p, 0xCE, 9, note ? 0x48 : 0x28);
    uint8_t* b = p + 0x20;
    w32(b + 4, 0xC0000000u), wf(b + 8, 1.5f), wf(b + 12, 2.0f), wf(b + 16, -3.0f), wf(b + 20, 0.75f), w16(b + 0x24, 0x11);
    if (note)
    {
        memcpy(b + 0x28, "XKB1", 4), wf(b + 0x2C, 0.6f), wf(b + 0x30, -0.8f), wf(b + 0x34, 4.0f), w16(b + 0x38, 410);
        wf(b + 0x3C, -0.9f), wf(b + 0x40, 2.0f), wf(b + 0x44, 0.2f);
    }
    return n;
}

/* a base packet around the given subpackets */
static uint32_t base(uint8_t* p, const uint8_t* subs, uint32_t n, uint16_t count)
{
    memset(p, 0, 0x10);
    p[0] = 1, w16(p + 2, 1), w16(p + 4, (uint16_t)(0x10 + n)), w16(p + 6, count);
    memcpy(p + 0x10, subs, n);
    return 0x10 + n;
}

static XkbSlide g_taken;
static int g_takes;
static int take(const XkbSlide* s)
{
    g_taken = *s, g_takes++;
    return 1;
}
static int refuse(const XkbSlide* s)
{
    (void)s;
    g_takes++;
    return 0;
}

int main(void)
{
    uint8_t subs[512], stream[2048];
    XkbSlide s;

    /* one base packet: a chat line, the knockback warp, another subpacket */
    uint32_t n = sub(subs, 0x03, 9, 0x30);
    uint32_t at = n;
    n += warp(subs + n, 1);
    n += sub(subs + n, 0x139, 0x44C200B5u, 0x20);
    uint32_t len = base(stream, subs, n, 3);
    len += base(stream + len, subs, n, 3); /* and the same again */

    assert(xkb_scan(0, stream, len, take) == 2 && g_takes == 2);
    s = g_taken;
    assert(s.actor == 9 && s.to[0] == 1.5f && s.to[2] == -3.0f && s.rotation == 0.75f);
    assert(s.dir[0] == 0.6f && s.dir[1] == -0.8f && s.distance == 4.0f && s.duration_ms == 410 && s.start[0] == -0.9f);
    /* both warps neutralized, nothing else touched */
    uint8_t* w1 = stream + 0x10 + at;
    uint8_t* w2 = stream + (len / 2) + 0x10 + at;
    assert(w1[4] == 0xF0 && w1[7] == 0x7F && w2[4] == 0xF0);
    assert(stream[0x10 + 4] == 9 && stream[0x10 + at + 0x68 + 4] == 0xB5);

    /* the same stream fed a few bytes at a time: the warp is never whole in one read, so it is left for the game */
    xkb_stream_reset(1);
    len = base(stream, subs, n, 3);
    int hits = 0;
    for (uint32_t i = 0; i < len; i += 7)
        hits += xkb_scan(1, stream + i, len - i < 7 ? len - i : 7, take);
    assert(hits == 0 && stream[0x10 + at + 4] == 9);
    /* and the stream position survives that: a whole packet next is still found */
    len = base(stream, subs, n, 3);
    assert(xkb_scan(1, stream, len, take) == 1);

    /* a warp the slide turns down (its actor not found) is left for the game */
    xkb_stream_reset(4);
    len = base(stream, subs, n, 3);
    g_takes = 0;
    assert(xkb_scan(4, stream, len, refuse) == 0 && g_takes == 1 && stream[0x10 + at + 4] == 9);

    /* a warp without the note (a GM warp, an older server) is the game's */
    xkb_stream_reset(2);
    n = warp(subs, 0);
    len = base(stream, subs, n, 1);
    assert(xkb_scan(2, stream, len, take) == 0 && stream[0x10 + 4] == 9);

    /* garbage never crashes and never matches */
    xkb_stream_reset(3);
    for (uint32_t i = 0; i < sizeof stream; ++i)
        stream[i] = (uint8_t)(i * 37 + 11);
    assert(xkb_scan(3, stream, sizeof stream, take) == 0);

    /* the slide: from where the player stood to the landing spot, quick at first, there at the end */
    float from[3] = { 0, 10, 0 }, to[3] = { 4, 10, -2 }, p[3];
    xkb_at(from, to, 0.0f, p);
    assert(p[0] == 0 && p[2] == 0);
    xkb_at(from, to, 0.25f, p);
    assert(p[0] > 1.5f && p[0] < 4.0f); /* more than half the way in the first quarter */
    xkb_at(from, to, 1.0f, p);
    assert(p[0] == 4 && p[1] == 10 && p[2] == -2);
    xkb_at(from, to, 3.0f, p);
    assert(p[0] == 4);
    float prev = 0;
    for (int k = 1; k <= 20; ++k)
    {
        float e = xkb_ease(k / 20.0f), step = e - prev;
        assert(step >= 0 && (k == 1 || step <= 0.3f));
        prev = e;
    }

    printf("knockback_test: ok\n");
    return 0;
}
