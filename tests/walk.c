/* Scripted walking for test clients (XIVLEGACY_CONTROL commands walkto, walkpath, walkstop, walkwhere).
 *
 * A test character walks like a player at a keyboard: W runs toward where the camera faces, and the
 * camera-turn keys (J left, L right) steer. No mouse: mouse-look drags used to flip the camera under
 * the world. A walker thread reads the player's own position and facing from guest memory every 20 ms
 * and presses or lets go of W, J and L, so it corrects course many times a second instead of once a
 * second from a server log.
 *
 * Where the player is: the game keeps each actor's position as x, y, z, 1.0 with the facing (radians,
 * atan2(dx, dz) of where it runs) at +0x10, another 1.0 at +0x20 and the facing it turns toward at
 * +0x44 (xiv_knockback_game.c reads the same). The local player's copy is found once by a scan of
 * committed guest memory for that shape near a hint spot (the server's last position for the player,
 * which the driver passes; or the last spot the walker saw), then kept while it still looks the same.
 * Stale copies exist (after a zone seam the old one keeps its x, z current but not its facing, and
 * an NPC's block can sit close by): the one a drawn model's scene node also holds is preferred, and a
 * block whose facing doesn't follow the way it runs, or that stops changing while W is held, is
 * dropped for the rest of the walk and the scan runs again.
 *
 * Steering: W runs where the camera looks and J/L turn the camera at a steady 1.88 rad/s (J toward
 * larger angles). While running the body faces where it runs, so the walker steers by the body's
 * facing, in timed pulses (the body lags the camera about LAG_MS); a turn sharper than TURN_IN_PLACE
 * is made standing, timed by that rate. (The game's camera
 * matrices aren't used: other passes rewrite them while a frame is drawn.)
 *
 * Stuck: no progress toward the current waypoint for 2 s -> step back (S), side-step (A or D, in turn)
 * and go on; the third time at one waypoint it stops and says where, so the driver can plan again.
 *
 * Every report is a "[walk] ..." line on stdout (the client log): "arrived", "stuck", "lost", "stopped".
 * Don't walk right after a GM warp: the camera can come loose from the player (W then runs one fixed
 * way whatever J and L do); log in again there first.
 * XIVLEGACY_WALKLOG=1 adds a trace line every 100 ms (position, facing, heading error, keys). */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "gthread.h"
#include "gwin.h"
#include "plat.h"
#include "runtime.h"
#include "walk.h"

#define PAGE 0x1000u
#define SCAN_LO 0x00400000u
#define SCAN_HI 0xF0000000u
#define ACTOR_ROT 0x10u
#define ACTOR_ROT2 0x44u
#define MAX_POINTS 1024
#define MAX_HITS 512
#define NODE_VTABLE 0x010653A4u /* a model's scene node (1.23b exe): its class, 0x30 before the position */
#define NODE_VT 0x30u
#define TICK_MS 20
#define TURN_DEADBAND 0.10f /* radians */
#define LAG_MS 250          /* the body follows a camera turn within about this long */
#define TURN_IN_PLACE 0.9f  /* radians: stop running and turn first above this */
#define TURN_RATE 1.88f     /* radians a second: the camera turn keys (measured) */
#define SETTLE_MS 300       /* run straight this long after a standing turn before trusting the facing */
#define ADVANCE 1.0f        /* yalms: close enough to an inner waypoint */
#define STUCK_MS 2000
#define STUCK_GAIN 0.3f     /* yalms of progress that count */
#define STUCK_TRIES 3

typedef struct
{
    float x, y, z;
} Pt;

static struct
{
    SDL_Mutex* mu;
    int busy;          /* a walk is running */
    volatile int stop; /* walkstop asked */
    Pt pts[MAX_POINTS];
    int n;
    float stop_dist;
    float hint[3]; /* x, z, y (NAN: unknown) */
    int have_hint;
    SDL_WindowID win;
} W;

static uint32_t g_actor;       /* the player's position block, 0 = not found yet */
static uint32_t g_bad[16];     /* blocks found stale during this walk (a zone seam leaves the old copy) */
static int g_nbad;
static float g_last[3];        /* where the walker last saw the player */
static int g_have_last;
static int g_trace;
static int g_keys[3];          /* W, J, L held by us */
static const SDL_Scancode KEYS[3] = { SDL_SCANCODE_W, SDL_SCANCODE_J, SDL_SCANCODE_L };

static void say(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("[walk] ");
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static float rdf(uint32_t a)
{
    float f;
    memcpy(&f, GUEST_PTR(a), 4);
    return f;
}

static int committed(uint32_t a, uint32_t n) { return a >= SCAN_LO && gwin_is_committed(a) && gwin_is_committed(a + n - 1); }

static int looks_like_actor(uint32_t a)
{
    if (!committed(a, 0x48))
        return 0;
    float r = rdf(a + ACTOR_ROT);
    return rdf(a + 0x0C) == 1.0f && rdf(a + 0x20) == 1.0f && fabsf(r) <= 3.2f && fabsf(rdf(a + ACTOR_ROT2)) <= 3.2f &&
           isfinite(rdf(a)) && isfinite(rdf(a + 4)) && isfinite(rdf(a + 8));
}

static float wrap(float a)
{
    while (a > (float)M_PI)
        a -= 2.0f * (float)M_PI;
    while (a < -(float)M_PI)
        a += 2.0f * (float)M_PI;
    return a;
}

static void key(int k, int down)
{
    if (g_keys[k] == down)
        return;
    g_keys[k] = down;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, e.key.windowID = W.win, e.key.scancode = KEYS[k], e.key.down = down;
    e.key.key = SDL_GetKeyFromScancode(KEYS[k], SDL_KMOD_NONE, false);
    SDL_PushEvent(&e);
}

static void all_up(void)
{
    for (int k = 0; k < 3; ++k)
        key(k, 0);
}

static void tap(SDL_Scancode sc, int ms)
{
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_EVENT_KEY_DOWN, e.key.windowID = W.win, e.key.scancode = sc, e.key.down = 1;
    e.key.key = SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false);
    SDL_PushEvent(&e);
    plat_sleep_ms((uint32_t)ms);
    e.type = SDL_EVENT_KEY_UP, e.key.down = 0;
    SDL_PushEvent(&e);
    plat_sleep_ms(150);
}

static int looks_like_node(uint32_t a)
{
    if (a < SCAN_LO + NODE_VT || !committed(a - NODE_VT, NODE_VT + 0x60) || rd32(a - NODE_VT) != NODE_VTABLE)
        return 0;
    float sc = rdf(a - 0x10);
    return rdf(a + 0x0C) == 1.0f && sc > 0.01f && sc < 100.0f;
}

/* The player's position block near (hx, hz) (and height hy, if known), within r yalms; guest lock
 * held. Several blocks can hold the player's spot (stale copies that don't move); the live one is
 * an actor whose exact position a drawn model's scene node also holds (xiv_knockback_game.c). */
static uint32_t scan(float hx, float hy, float hz, float r)
{
    static uint32_t hits[MAX_HITS];
    int n = 0;
    for (uint32_t pg = SCAN_LO; pg < SCAN_HI && n < MAX_HITS; pg += PAGE)
    {
        if (!gwin_is_committed(pg))
            continue;
        const float* f = (const float*)GUEST_PTR(pg);
        for (uint32_t i = 0; i + 3 < PAGE / 4 && n < MAX_HITS; ++i)
            if (f[i + 3] == 1.0f && fabsf(f[i] - hx) <= r && fabsf(f[i + 2] - hz) <= r)
                hits[n++] = pg + i * 4;
    }
    uint32_t best = 0;
    float bestd = 1e30f;
    for (int k = 0; k < n; ++k)
    {
        uint32_t a = hits[k];
        if (!looks_like_actor(a))
            continue;
        int bad = 0;
        for (int b = 0; b < g_nbad && !bad; ++b)
            bad = g_bad[b] == a;
        if (bad)
            continue;
        int drawn = 0;
        for (int j = 0; j < n && !drawn; ++j)
            drawn = hits[j] != a && !memcmp(GUEST_PTR(hits[j]), GUEST_PTR(a), 12) && looks_like_node(hits[j]);
        float dx = rdf(a) - hx, dz = rdf(a + 8) - hz, dy = isfinite(hy) ? rdf(a + 4) - hy : 0.0f;
        float d = dx * dx + dz * dz + dy * dy + (drawn ? 0.0f : 1e6f);
        if (d < bestd)
            bestd = d, best = a;
    }
    return best;
}

/* the player's x, y, z and facing; 0 when it can't be found */
static int where(float p[4])
{
    gt_lock();
    if (!(g_actor && looks_like_actor(g_actor)))
    {
        g_actor = 0;
        uint64_t t = rt_monotonic_ns();
        if (W.have_hint)
            g_actor = scan(W.hint[0], W.hint[2], W.hint[1], 4.0f);
        if (!g_actor && g_have_last)
            g_actor = scan(g_last[0], g_last[1], g_last[2], 6.0f);
        if (!g_actor && g_nbad && g_have_last)
        {
            g_nbad = 0; /* only stale copies left: maybe one was live after all */
            g_actor = scan(g_last[0], g_last[1], g_last[2], 6.0f);
        }
        say("player %s (%08x) in %.0f ms", g_actor ? "found" : "not found", g_actor, (double)(rt_monotonic_ns() - t) * 1e-6);
        W.have_hint = 0;
    }
    int ok = g_actor != 0;
    if (ok)
    {
        p[0] = rdf(g_actor), p[1] = rdf(g_actor + 4), p[2] = rdf(g_actor + 8), p[3] = rdf(g_actor + ACTOR_ROT);
        g_last[0] = p[0], g_last[1] = p[1], g_last[2] = p[2], g_have_last = 1;
    }
    gt_unlock();
    return ok;
}

static uint64_t now_ms(void) { return rt_monotonic_ns() / 1000000u; }

/* the block being read is a stale copy of the player: never read it again this walk, look again */
static void drop_actor(const char* why)
{
    gt_lock();
    if (g_actor && g_nbad < (int)(sizeof g_bad / sizeof g_bad[0]))
        g_bad[g_nbad++] = g_actor;
    say("player block %08x looks stale (%s): looking again", g_actor, why);
    g_actor = 0;
    gt_unlock();
}

static void walk_thread(void* arg)
{
    (void)arg;
    float p[4];
    uint64_t t0 = now_ms();
    int i = 0, tries = 0;
    float best = 1e9f;
    uint64_t best_t = now_ms();
    int lost_ticks = 0, trace_n = 0, aligned = 0;
    uint64_t settle_until = 0, turn_until = 0, judge_at = 0;
    g_trace = getenv("XIVLEGACY_WALKLOG") != NULL; /* a line every 100 ms */
    g_nbad = 0;
    float same[4] = { NAN, NAN, NAN, NAN }; /* position and facing this block last showed, and since when */
    uint64_t same_t = now_ms(), mo_t = 0;
    float mo_x = 0.0f, mo_z = 0.0f; /* where the player was mo_t ms ago, for the facing check */
    int mo_bad = 0;
    const char* why = "stopped";
    if (where(p))
        say("start at %.2f %.2f %.2f facing %.2f, %d points, stop %.2f", p[0], p[1], p[2], p[3], W.n, W.stop_dist);
    while (!W.stop)
    {
        plat_sleep_ms(TICK_MS);
        if (!where(p))
        {
            all_up();
            if (++lost_ticks * TICK_MS > 30000)
            {
                why = "lost";
                break;
            }
            plat_sleep_ms(500); /* a zone change: look again */
            continue;
        }
        lost_ticks = 0;
        /* Stale-copy checks. Running, a live block never shows the very same position for 1.5 s (even
         * against a wall the client nudges it). */
        if (memcmp(same, p, sizeof same))
            memcpy(same, p, sizeof same), same_t = now_ms();
        else if (g_keys[0] && now_ms() - same_t > 1500)
        {
            drop_actor("frozen"), same_t = now_ms();
            continue;
        }
        /* Running near full speed, the live block faces the way it moves (a wall slide that fast is a
         * shallow one). A copy whose facing doesn't follow is stale. */
        uint64_t tn = now_ms();
        if (!g_keys[0] || tn < settle_until)
            mo_t = 0;
        else if (!mo_t)
            mo_t = tn, mo_x = p[0], mo_z = p[2];
        else if (tn - mo_t >= 300)
        {
            float mx = p[0] - mo_x, mz = p[2] - mo_z;
            int off = hypotf(mx, mz) > 1.2f && fabsf(wrap(atan2f(mx, mz) - p[3])) > 1.2f;
            mo_bad = off ? mo_bad + 1 : 0;
            mo_t = tn, mo_x = p[0], mo_z = p[2];
            if (mo_bad >= 2)
            {
                mo_bad = 0, mo_t = 0;
                drop_actor("its facing doesn't follow the way it runs");
                continue;
            }
        }
        Pt* t = &W.pts[i];
        int last = i == W.n - 1;
        float dx = t->x - p[0], dz = t->z - p[2], d = sqrtf(dx * dx + dz * dz);
        /* advance: close enough, or past an inner point along its segment */
        if (!last && (d < ADVANCE || (i + 1 < W.n && (W.pts[i + 1].x - t->x) * (p[0] - t->x) + (W.pts[i + 1].z - t->z) * (p[2] - t->z) > 0 &&
                                         d < 3.0f)))
        {
            i++, tries = 0, best = 1e9f, best_t = now_ms();
            continue;
        }
        if (last && d < W.stop_dist)
        {
            why = "arrived";
            break;
        }
        /* stuck? */
        if (d < best - STUCK_GAIN)
            best = d, best_t = now_ms();
        else if (now_ms() - best_t > STUCK_MS)
        {
            all_up();
            if (++tries >= STUCK_TRIES)
            {
                why = "stuck";
                break;
            }
            say("no progress at %.2f %.2f %.2f toward point %d/%d (%.1f away): back off, %s", p[0], p[1], p[2], i + 1, W.n, d,
                tries & 1 ? "right" : "left");
            tap(SDL_SCANCODE_S, 400);
            tap(tries & 1 ? SDL_SCANCODE_D : SDL_SCANCODE_A, 500);
            best = 1e9f, best_t = now_ms();
            aligned = 0; /* backing and side-stepping turn the body away from the camera */
        }
        /* Steering. W runs where the camera looks; J and L turn the camera at TURN_RATE. Running, the
         * body faces the way it runs within about 0.1 s, so its facing tells where the camera looks.
         * Standing, the body doesn't follow the camera: a sharp turn is done standing, by time, then
         * the walker runs straight a moment before it trusts the facing again. */
        float want = atan2f(dx, dz), err = wrap(want - p[3]);
        uint64_t now = now_ms();
        if (g_trace && ++trace_n % 5 == 0)
            say("trace at %.2f %.2f %.2f facing %.2f want %.2f err %.2f point %d/%d %.1f away keys %d%d%d", p[0], p[1], p[2], p[3], want,
                err, i + 1, W.n, d, g_keys[0], g_keys[1], g_keys[2]);
        if (now < settle_until)
        {
            key(1, 0), key(2, 0), key(0, 1);
            continue;
        }
        if (!aligned)
        {
            /* the body and the camera may face different ways at the start: run a moment so they agree */
            key(0, 1);
            settle_until = now + SETTLE_MS;
            aligned = 1;
            continue;
        }
        if (fabsf(err) > TURN_IN_PLACE)
        {
            all_up();
            plat_sleep_ms(120); /* stop first */
            if (!where(p))
                continue;
            err = wrap(atan2f(t->x - p[0], t->z - p[2]) - p[3]);
            int ms = (int)(fabsf(err) / TURN_RATE * 1000.0f);
            key(err > 0 ? 1 : 2, 1);
            for (int k = 0; k < ms && !W.stop; k += 10)
                plat_sleep_ms(10);
            all_up();
            key(0, 1);
            settle_until = now_ms() + SETTLE_MS;
            continue;
        }
        /* Running: turn in timed pulses (|err| / TURN_RATE), then wait LAG_MS for the body to catch
         * up with the camera before judging again; holding J/L until the facing agrees overshoots. */
        key(0, 1);
        if (now < turn_until)
            continue;
        key(1, 0), key(2, 0);
        if (now < judge_at || fabsf(err) <= TURN_DEADBAND)
            continue;
        int ms = (int)(fabsf(err) / TURN_RATE * 1000.0f);
        key(err > 0 ? 1 : 2, 1);
        turn_until = now + (uint64_t)(ms < TICK_MS ? TICK_MS : ms);
        judge_at = turn_until + LAG_MS;
    }
    all_up();
    float q[4] = { 0 };
    int ok = where(q);
    if (ok)
        say("%s at %.2f %.2f %.2f point %d/%d, %.1f s%s", W.stop && strcmp(why, "arrived") ? "stopped" : why, q[0], q[1], q[2], i + 1, W.n,
            (double)(now_ms() - t0) / 1000.0, "");
    else
        say("%s: player not found, point %d/%d, %.1f s", why, i + 1, W.n, (double)(now_ms() - t0) / 1000.0);
    SDL_LockMutex(W.mu);
    W.busy = 0;
    SDL_UnlockMutex(W.mu);
}

static int add_point(float x, float y, float z)
{
    if (W.n >= MAX_POINTS)
        return 0;
    W.pts[W.n].x = x, W.pts[W.n].y = y, W.pts[W.n].z = z;
    W.n++;
    return 1;
}

/* "near SX SZ [SY]" at the end of a command: a hint for finding the player */
static void parse_hint(const char* s)
{
    const char* h = strstr(s, "near ");
    int k = h ? sscanf(h + 5, "%f %f %f", &W.hint[0], &W.hint[1], &W.hint[2]) : 0;
    if (k >= 2)
    {
        if (k == 2)
            W.hint[2] = NAN;
        W.have_hint = 1, g_actor = 0;
    }
}

static void start(void)
{
    W.stop = 0, W.busy = 1;
    plat_thread_start(walk_thread, NULL);
}

int walk_command(const char* line, SDL_WindowID win)
{
    if (!W.mu)
        W.mu = SDL_CreateMutex();
    if (!strcmp(line, "walkstop"))
    {
        W.stop = 1;
        return 1;
    }
    if (!strncmp(line, "walkwhere", 9))
    {
        float p[4];
        parse_hint(line);
        if (where(p))
            say("at %.2f %.2f %.2f facing %.3f", p[0], p[1], p[2], p[3]);
        else
            say("at: player not found");
        return 1;
    }
    int to = !strncmp(line, "walkto ", 7), path = !strncmp(line, "walkpath ", 9);
    if (!to && !path)
        return 0;
    SDL_LockMutex(W.mu);
    int busy = W.busy;
    SDL_UnlockMutex(W.mu);
    if (busy)
    {
        say("busy: walkstop first");
        return 1;
    }
    W.win = win, W.n = 0, W.stop_dist = 1.0f;
    parse_hint(line);
    if (to)
    {
        /* walkto X Z [stop] [near SX SZ] */
        float x, z, s;
        int k = sscanf(line + 7, "%f %f %f", &x, &z, &s);
        if (k < 2)
        {
            say("usage: walkto X Z [stop yalms] [near SX SZ]");
            return 1;
        }
        if (k == 3 && s > 0)
            W.stop_dist = s;
        add_point(x, 0, z);
    }
    else
    {
        /* walkpath FILE [stop] [near SX SZ]: one point a line, "x y z" or "x z" ('#' comments) */
        char file[600];
        float s;
        int k = sscanf(line + 9, "%599s %f", file, &s);
        FILE* f = k >= 1 ? fopen(file, "r") : NULL;
        if (!f)
        {
            say("walkpath: can't open %s", k >= 1 ? file : "(none)");
            return 1;
        }
        if (k == 2 && s > 0)
            W.stop_dist = s;
        char l[256];
        while (fgets(l, sizeof l, f))
        {
            float a, b, c;
            if (l[0] == '#')
                continue;
            int m = sscanf(l, "%f %f %f", &a, &b, &c);
            if (m == 3)
                add_point(a, b, c);
            else if (m == 2)
                add_point(a, 0, b);
        }
        fclose(f);
        if (!W.n)
        {
            say("walkpath: no points in %s", file);
            return 1;
        }
    }
    start();
    return 1;
}
