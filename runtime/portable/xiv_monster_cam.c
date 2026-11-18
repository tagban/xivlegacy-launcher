/* The camera around a monster body (Monster Mode: the server swaps the local player's model for a
 * monster's with 0x00D6).
 *
 * The third-person camera aims at a point the player's skeleton names: CharaActor setup (0x664890)
 * looks up the attach points EID_CAMINT_HI and EID_CAMINT_LOW (vtable +0x54) and keeps their
 * model-space matrices at actor+0x16C0 and +0x1700; 0x664100 aims halfway between them (rotated,
 * scaled and moved with the actor). The camera distance (camera actor +0x3F0, the per-context
 * distances at +0x3F4..+0x400; default 4.5, clamped to 1.5..10 by 0x611350) is a person's whatever
 * the body, so a big monster fills the screen and a small one is a speck.
 *
 * The game sizes a monster by its own skeleton: the nameplate sits on its EID_NAME point. Player race
 * skeletons have the crafting camera point EID_CAM_CFT, monster skeletons don't (index -1). So when
 * the camera's own player (0x616380 returns it, the world's +0xD0) wears a body without that point,
 * the recompiler's hook at 0x6163A6 (`--hooks camera_follow=0x6163a6`, module ABI 6):
 * - scales the camera's distances, and their default and limits (the floats at 0xFB70B8, 0xFB70C0 and
 *   0xFB70BC, read only by the camera distance code), by the nameplate height (times the actor's
 *   scale) over a person's (NAME_PERSON), a bit more for big bodies (S^1.5 above 1), within
 *   SCALE_MIN..SCALE_MAX. The 1.5..10 clamp compared against a double 10.0 shared with other code;
 *   patches.json makes it compare against the float limit instead;
 * - if the skeleton has no EID_CAMINT points either, puts the two aim points at the heights a
 *   person's sit at, as fractions of the nameplate height (CAM_HI, CAM_LO), on the body's axis.
 * A player body puts everything back. XIVLEGACY_MONSTERCAM=0 turns it off, =2 logs each body. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "gwin.h"
#include "runtime.h"
#include "xiv_monster_cam.h"

#define OFF_WORLD 0x118u /* actor and camera: their RaptureActorContainer */
#define OFF_PLAYER 0xD0u /* world: the local player's CharaActor */
#define OFF_EID_HI 0x1690u /* CharaActor: EID_CAMINT_HI's index, -1 = none */
#define OFF_EID_LO 0x1694u
#define OFF_MAT_HI 0x16C0u /* its model-space matrix (row 3 = position) */
#define OFF_EID_CFT 0x16A8u /* EID_CAM_CFT (crafting) */
#define OFF_MAT_LO 0x1700u
#define OFF_MAT_GB 0x1900u /* the last of the ten */
#define OFF_DIST 0x3F0u /* camera: wanted distance, then one per context (5 floats) */
#define VT_FIND_EID 0x54u /* CharaActor: int (const char* name) */
#define VT_EID_MATRIX 0x60u /* CharaActor: Matrix* (Matrix* out, int index) */
#define VT_SCALE 0x9Cu /* CharaActor: Vec4* (Vec4* out) */
#define EID_NAME_STR 0x00FA71BCu /* "EID_NAME" in .rdata */
#define DIST_DEFAULT 0x00FB70B8u /* 4.5 */
#define DIST_MAX 0x00FB70BCu /* 10.0 */
#define DIST_MIN 0x00FB70C0u /* 1.5 */

/* A person, measured in the client (a Hyur): nameplate point, and the two camera points as fractions of
 * it. Monster bodies get the same proportions. */
#define NAME_PERSON 1.95f
#define CAM_HI 0.80f
#define CAM_LO 0.55f
#define SCALE_MIN 0.45f
#define SCALE_MAX 6.0f

/* 0x6163A6: pop esi; ret, the end of 0x616380 (eax = the chara the camera follows, esi = the camera) */
static const uint8_t FOLLOW_CODE[] = { 0x8B, 0x86, 0x18, 0x01, 0x00, 0x00, 0x8B, 0x80, 0xD0, 0x00, 0x00, 0x00, 0x5E, 0xC3 };
#define FOLLOW_CODE_AT 0x0061639Au

static int g_on = -1, g_verbose;
static float g_base[3]; /* the game's default, min and max distance */
static uint32_t g_actor; /* the player body last looked at */
static int32_t g_eid_hi, g_eid_cft;
static float g_hi_y, g_lo_y; /* its aim heights (a setup rewrites them) */
static int g_waits;
static float g_scale = 1.0f; /* wanted distance scale */
static uint32_t g_cam; /* the camera g_applied is for */
static float g_applied = 1.0f;

static int readable(uint32_t a, uint32_t n) { return a >= 0x10000u && gwin_is_committed(a) && gwin_is_committed(a + n - 1); }

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
    char line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    (void)!write(2, line, strlen(line));
}

static int setup(void)
{
    if (g_on >= 0)
        return g_on;
    const char* e = getenv("XIVLEGACY_MONSTERCAM");
    g_on = !(e && *e == '0');
    g_verbose = e && *e == '2';
    if (!g_on)
        return 0;
    for (uint32_t i = 0; i < sizeof FOLLOW_CODE; ++i)
        if (!readable(FOLLOW_CODE_AT + i, 1) || rd8(FOLLOW_CODE_AT + i) != FOLLOW_CODE[i])
            g_on = 0;
    g_base[0] = rdf32(DIST_DEFAULT), g_base[1] = rdf32(DIST_MIN), g_base[2] = rdf32(DIST_MAX);
    if (g_base[0] != 4.5f || g_base[1] != 1.5f || g_base[2] != 10.0f || memcmp(GUEST_PTR(EID_NAME_STR), "EID_NAME", 9))
        g_on = 0;
    say(g_on ? "[monstercam] on\n" : "[monstercam] off: not the 1.23b code\n");
    return g_on;
}

/* this->vtable[slot](args...): a thiscall into the game, on the guest stack below the hook's */
static uint32_t call_this(Guest* g, uint32_t self, uint32_t slot, int n, const uint32_t* args)
{
    Guest saved = *g;
    uint32_t sp = (g->esp - 0x800u) & ~15u;
    for (int i = n - 1; i >= 0; --i)
        sp -= 4, wr32(sp, args[i]);
    sp -= 4, wr32(sp, 0xFEEDF00Du);
    g->esp = sp, g->ecx = self, g->df = 0;
    rt_call_indirect(g, rd32(rd32(self) + slot));
    uint32_t r = g->eax;
    *g = saved;
    return r;
}

/* the body's size: its nameplate point's height (model space) times the actor's height scale */
static float body_height(Guest* g, uint32_t p, float* name_y)
{
    uint32_t buf = ((g->esp - 0x200u) & ~15u) - 0x80u; /* two scratch blocks, above call_this's stack */
    uint32_t a1[1] = { EID_NAME_STR };
    int32_t eid = (int32_t)call_this(g, p, VT_FIND_EID, 1, a1);
    float y = 0;
    if (eid >= 0)
    {
        uint32_t a2[2] = { buf, (uint32_t)eid };
        uint32_t m = call_this(g, p, VT_EID_MATRIX, 2, a2);
        if (readable(m, 64))
            y = rdf32(m + 0x34);
    }
    *name_y = y;
    if (!(y > 0.05f && y < 100.0f))
        return -1.0f; /* no nameplate point (yet) */
    uint32_t a3[1] = { buf + 0x40u };
    uint32_t v = call_this(g, p, VT_SCALE, 1, a3);
    float sy = readable(v, 8) ? rdf32(v + 4) : 1.0f;
    if (!(sy > 0.05f && sy < 50.0f))
        sy = 1.0f;
    return y * sy;
}

static void set_aim(uint32_t p, float hi, float lo)
{
    wrf32(p + OFF_MAT_HI + 0x30, 0), wrf32(p + OFF_MAT_HI + 0x34, hi), wrf32(p + OFF_MAT_HI + 0x38, 0);
    wrf32(p + OFF_MAT_LO + 0x30, 0), wrf32(p + OFF_MAT_LO + 0x34, lo), wrf32(p + OFF_MAT_LO + 0x38, 0);
}

/* the camera's distances to the current body scale */
static void apply(uint32_t cam)
{
    if (cam == g_cam && g_applied == g_scale)
        return;
    if (cam != g_cam)
        g_applied = 1.0f; /* a new camera starts at the game's own distances */
    float k = g_scale / g_applied;
    wrf32(DIST_DEFAULT, g_base[0] * g_scale);
    wrf32(DIST_MIN, g_base[1] * g_scale);
    wrf32(DIST_MAX, g_base[2] * g_scale);
    for (uint32_t i = 0; i < 5; ++i)
    {
        float d = rdf32(cam + OFF_DIST + 4 * i) * k;
        if (g_scale == 1.0f) /* back to a person: no rounding left over from the trips */
            d = roundf(d * 1000.0f) / 1000.0f;
        wrf32(cam + OFF_DIST + 4 * i, d);
    }
    if (g_verbose)
        say("[monstercam] camera %08x distance x%.2f (wanted %.2f)\n", cam, g_scale, rdf32(cam + OFF_DIST));
    g_cam = cam, g_applied = g_scale;
}

static void log_eids(uint32_t p)
{
    char line[200];
    int n = snprintf(line, sizeof line, "[monstercam]   camera points");
    for (uint32_t i = 0; i < 10 && n < (int)sizeof line - 8; ++i)
        n += snprintf(line + n, sizeof line - (size_t)n, " %d", (int32_t)rd32(p + OFF_EID_HI + 4 * i));
    say("%s; aim (%.2f %.2f %.2f) / (%.2f %.2f %.2f)\n", line, rdf32(p + OFF_MAT_HI + 0x30), rdf32(p + OFF_MAT_HI + 0x34),
        rdf32(p + OFF_MAT_HI + 0x38), rdf32(p + OFF_MAT_LO + 0x30), rdf32(p + OFF_MAT_LO + 0x34), rdf32(p + OFF_MAT_LO + 0x38));
}

void xiv_monster_cam(Guest* g)
{
    uint32_t cam = g->esi, p = g->eax;
    if (!p || !setup() || !readable(cam + OFF_WORLD, 4))
        return;
    uint32_t w = rd32(cam + OFF_WORLD);
    if (!readable(w + OFF_PLAYER, 4) || rd32(w + OFF_PLAYER) != p || !readable(p + OFF_MAT_GB + 0x40, 4))
        return;
    /* a new body: another actor, other camera points, or the aim points rewritten by a setup */
    int32_t hi = (int32_t)rd32(p + OFF_EID_HI), cft = (int32_t)rd32(p + OFF_EID_CFT);
    float hy = rdf32(p + OFF_MAT_HI + 0x34), ly = rdf32(p + OFF_MAT_LO + 0x34);
    if (p == g_actor && hi == g_eid_hi && cft == g_eid_cft && hy == g_hi_y && ly == g_lo_y)
    {
        apply(cam);
        return;
    }
    float name_y, h = body_height(g, p, &name_y);
    if (h < 0 && ++g_waits < 600)
        return; /* its model isn't loaded yet: look again */
    g_waits = 0;
    if (cft >= 0)
    {
        /* a player race: crafting has its own camera point, which no monster skeleton has */
        if (g_scale != 1.0f || g_verbose)
            say("[monstercam] player body: nameplate %.2f, height %.2f\n", name_y, h);
        g_scale = 1.0f;
    }
    else
    {
        if (h < 0)
            name_y = h = NAME_PERSON;
        if (hi < 0 && (int32_t)rd32(p + OFF_EID_LO) < 0) /* no camera points either: aim as at a person */
            set_aim(p, CAM_HI * name_y, CAM_LO * name_y), hy = rdf32(p + OFF_MAT_HI + 0x34), ly = rdf32(p + OFF_MAT_LO + 0x34);
        g_scale = h / NAME_PERSON;
        if (g_scale > 1.0f) /* big bodies are long and wide too: the camera backs off more than the height grows */
            g_scale *= sqrtf(g_scale);
        g_scale = g_scale < SCALE_MIN ? SCALE_MIN : g_scale > SCALE_MAX ? SCALE_MAX : g_scale;
        say("[monstercam] monster body: nameplate %.2f, height %.2f, camera distance x%.2f\n", name_y, h, g_scale);
    }
    if (g_verbose)
        log_eids(p);
    g_actor = p, g_eid_hi = hi, g_eid_cft = cft, g_hi_y = hy, g_lo_y = ly;
    apply(cam);
}
