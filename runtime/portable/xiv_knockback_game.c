/* Knockback slides, the game side (xiv_knockback.c finds the server's note): move the local
 * player from where it stands to the landing spot, a little each presented frame, facing the
 * attacker.
 *
 * Where the position lives: the game copies it every frame from the player model's scene node
 * into the actor and the character controller (writing either of those is undone a frame later).
 * The node keeps its scale (x, y, z, 1) at -0x10, the position (x, y, z, 1), the facing (radians)
 * at +0x14 and the world matrix (rows right, up, forward scaled by the scale, then the position
 * again) from +0x20; its class (vtable 0x010653A4 in the 1.23b exe) is at -0x30. Moving the
 * node's x and z moves the player: the controller keeps the feet on the ground, and the client
 * reports the positions to the server as it goes.
 *
 * The node's address changes from zone to zone and every model has one. The player's is found
 * through the actor: the actor nearest the spot the server last had the player (x, y, z, 1 with the
 * facing after it), then the node holding exactly the actor's position. One search of committed
 * guest memory near that spot, then both are kept while they still look the same. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gwin.h"
#include "plat.h"
#include "runtime.h"
#include "xiv_knockback.h"

#define PAGE 0x1000u
#define SCAN_LO 0x00400000u
#define SCAN_HI 0xF0000000u
#define NEAR 3.0f /* yalms between the server's start spot and the client's own */
#define MAX_HITS 512

/* the actor: x, y, z, 1.0, the facing at +0x10, another 1.0 at +0x20, the facing turned toward at +0x44 */
#define ACTOR_ROT 0x10u
#define ACTOR_ROT2 0x44u
/* the model's scene node */
#define NODE_VTABLE 0x010653A4u
#define NODE_VT 0x30u    /* before the position */
#define NODE_SCALE 0x10u /* before the position */
#define NODE_ROT 0x14u
#define NODE_MATRIX 0x20u
#define NODE_POS2 0x50u

static int g_active;
static uint64_t g_t0;
static XkbSlide g_cur;
static float g_from[3];
static uint32_t g_actor, g_node;
static int g_log = -1;
static int g_trace; /* frames left to trace after a landing (XIVLEGACY_KBLOG=2) */

static float rdf(uint32_t a)
{
    float f;
    memcpy(&f, GUEST_PTR(a), 4);
    return f;
}
static void wrf(uint32_t a, float f) { memcpy(GUEST_PTR(a), &f, 4); }
static int committed(uint32_t a, uint32_t n) { return a >= SCAN_LO && gwin_is_committed(a) && gwin_is_committed(a + n - 1); }

static int looks_like_actor(uint32_t a)
{
    if (!committed(a, 0x48))
        return 0;
    float r = rdf(a + ACTOR_ROT);
    return rdf(a + 0x0C) == 1.0f && rdf(a + 0x20) == 1.0f && fabsf(r) <= 3.2f && fabsf(rdf(a + ACTOR_ROT2)) <= 3.2f &&
           isfinite(rdf(a)) && isfinite(rdf(a + 4)) && isfinite(rdf(a + 8));
}

static int looks_like_node(uint32_t a)
{
    if (a < SCAN_LO + NODE_VT || !committed(a - NODE_VT, NODE_VT + NODE_POS2 + 0x10) || rd32(a - NODE_VT) != NODE_VTABLE)
        return 0;
    float sc = rdf(a - NODE_SCALE);
    return rdf(a + 0x0C) == 1.0f && sc > 0.01f && sc < 100.0f && fabsf(rdf(a + NODE_ROT)) <= 6.3f;
}

static float dist2(uint32_t a, const float p[3])
{
    float dx = rdf(a) - p[0], dz = rdf(a + 8) - p[2];
    return dx * dx + dz * dz;
}

/* the actor nearest the start spot, and the node holding exactly its position */
static void find_player(const float start[3])
{
    uint64_t t = rt_monotonic_ns();
    static uint32_t hits[MAX_HITS];
    uint32_t actor = 0, node = 0;
    int nhits = 0;
    float bestd = NEAR * NEAR;
    for (uint32_t pg = SCAN_LO; pg < SCAN_HI; pg += PAGE)
    {
        if (!gwin_is_committed(pg))
            continue;
        const float* f = (const float*)GUEST_PTR(pg);
        for (uint32_t i = 0; i + 3 < PAGE / 4; ++i)
        {
            if (f[i + 3] != 1.0f || fabsf(f[i] - start[0]) > NEAR || fabsf(f[i + 2] - start[2]) > NEAR)
                continue;
            uint32_t a = pg + i * 4;
            if (nhits < MAX_HITS)
                hits[nhits++] = a;
            float d = dist2(a, start);
            if (d < bestd && looks_like_actor(a))
                bestd = d, actor = a;
        }
    }
    if (actor)
        for (int k = 0; k < nhits && !node; ++k)
            if (hits[k] != actor && !memcmp(GUEST_PTR(hits[k]), GUEST_PTR(actor), 12) && looks_like_node(hits[k]))
                node = hits[k];
    g_actor = actor, g_node = node;
    if (g_log)
        fprintf(stderr, "[knockback] actor %08x, node %08x (%d near) found in %.1f ms\n", actor, node, nhits,
            (double)(rt_monotonic_ns() - t) * 1e-6);
}

static int still_the_player(void)
{
    return g_actor && g_node && looks_like_actor(g_actor) && looks_like_node(g_node) &&
           !memcmp(GUEST_PTR(g_node), GUEST_PTR(g_actor), 12);
}

static void trace(const char* what)
{
    fprintf(stderr, "[knockback] %s node %.2f %.2f r %.2f | actor %.2f %.2f %.2f\n", what, rdf(g_node), rdf(g_node + 8),
        rdf(g_node + NODE_ROT), rdf(g_actor), rdf(g_actor + 4), rdf(g_actor + 8));
}

/* the receive path, guest lock held (as for xkb_frame): 1 when the player will slide, 0 to leave
 * the warp to the game */
int xkb_begin(const XkbSlide* s)
{
    if (g_log < 0)
    {
        const char* e = getenv("XIVLEGACY_KBLOG"); /* =2: each frame of a slide too */
        g_log = e ? (*e == '2' ? 2 : 1) : 0;
    }
    g_cur = *s;
    if (!(still_the_player() && dist2(g_actor, g_cur.start) < NEAR * NEAR))
        find_player(g_cur.start);
    if (!g_actor || !g_node)
    {
        if (g_log)
            fprintf(stderr, "[knockback] player not found near %.2f %.2f: the warp moves it\n", g_cur.start[0], g_cur.start[2]);
        g_active = 0;
        return 0;
    }
    for (int i = 0; i < 3; ++i)
        g_from[i] = rdf(g_node + (uint32_t)i * 4);
    g_t0 = rt_monotonic_ns();
    g_active = 1;
    return 1;
}

/* the node's x and z, its facing and the world matrix built from them */
static void put(const float p[3], float rot)
{
    float sc = rdf(g_node - NODE_SCALE), c = cosf(rot) * sc, sn = sinf(rot) * sc;
    wrf(g_node, p[0]);
    wrf(g_node + 8, p[2]);
    wrf(g_node + NODE_POS2, p[0]);
    wrf(g_node + NODE_POS2 + 8, p[2]);
    wrf(g_node + NODE_ROT, rot);
    wrf(g_node + NODE_MATRIX + 0x00, c);
    wrf(g_node + NODE_MATRIX + 0x08, -sn);
    wrf(g_node + NODE_MATRIX + 0x20, sn);
    wrf(g_node + NODE_MATRIX + 0x28, c);
}

void xkb_frame(void)
{
    if (g_trace > 0 && !g_active)
    {
        g_trace--;
        if (still_the_player())
            trace("after");
    }
    if (!g_active)
        return;
    if (!looks_like_node(g_node))
    {
        g_active = 0; /* the zone changed under the slide */
        return;
    }
    float t = (float)((double)(rt_monotonic_ns() - g_t0) / ((double)g_cur.duration_ms * 1e6));
    float p[3];
    xkb_at(g_from, g_cur.to, t, p);
    put(p, g_cur.rotation);
    if (g_log > 1)
    {
        fprintf(stderr, "[knockback] t %.3f at %.2f %.2f\n", (double)t, p[0], p[2]);
        trace("wrote");
    }
    if (t >= 1.0f)
    {
        g_active = 0;
        g_trace = g_log > 1 ? 30 : 0;
        if (g_log)
            fprintf(stderr, "[knockback] landed at %.2f %.2f %.2f\n", p[0], p[1], p[2]);
    }
}
