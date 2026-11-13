/* Crash diagnostics: recent frees of game objects and the last game packets (xiv_crashlog.h). */
#include "xiv_crashlog.h"

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#define write _write
#else
#include <unistd.h>
#endif

#include "gwin.h"
#include "plat.h"

static uint32_t g_text_lo, g_text_hi, g_rdata_lo, g_rdata_hi;

void xcl_set_image(uint32_t text_lo, uint32_t text_hi, uint32_t rdata_lo, uint32_t rdata_hi)
{
    g_text_lo = text_lo, g_text_hi = text_hi, g_rdata_lo = rdata_lo, g_rdata_hi = rdata_hi;
}

static int in_text(uint32_t a) { return a >= g_text_lo && a < g_text_hi; }
static int in_rdata(uint32_t a) { return a >= g_rdata_lo && a < g_rdata_hi && g_rdata_hi - a >= 4; }

/* Guest memory that can be read without faulting (the guest window's committed pages). */
static int readable(uint32_t a) { return a >= 0x10000u && gwin_is_committed(a) && gwin_is_committed(a + 3); }

/* A return address: in the image's code, right after a call (e8 rel32, ff /2 forms). */
static int is_return(uint32_t r)
{
    if (r < 6 || !in_text(r) || !in_text(r - 6))
        return 0;
    if (rd8(r - 5) == 0xE8)
        return 1;
    if (rd8(r - 2) == 0xFF && (rd8(r - 1) & 0x38) == 0x10) /* call reg / call [reg] */
        return 1;
    if (rd8(r - 3) == 0xFF && (rd8(r - 2) & 0x38) == 0x10) /* call [reg+d8] */
        return 1;
    if (rd8(r - 6) == 0xFF && (rd8(r - 5) & 0x38) == 0x10) /* call [reg+d32] / call [abs] */
        return 1;
    return 0;
}

static void defer_freed(uint32_t p, uint32_t size, uint32_t esp);
static void defer_filter(uint32_t esp);
static const char* class_of(uint32_t vtbl, char* buf, size_t cap);
static void id_map_check(uint32_t p, uint32_t size, uint32_t esp);

/* --- frees ------------------------------------------------------------------------------------- */

#define FREES 32768u
#define FREE_RETS 7
typedef struct
{
    uint32_t p, size, vtbl, ms;
    uint32_t ret[FREE_RETS];
} FreeRec;
static FreeRec g_frees[FREES];
static uint32_t g_free_n; /* total recorded; the ring holds the last FREES */

/* Blocks freed and not handed out again since, by address (a lossy cache: a colliding free
 * replaces the older one). A pointer found here is dangling. */
#define DEAD_BITS 18
typedef struct
{
    uint32_t p, ms, ret[2];
} Dead;
static Dead g_dead[1u << DEAD_BITS];
static Dead* dead_slot(uint32_t p) { return &g_dead[(p >> 3 ^ p >> 21) & ((1u << DEAD_BITS) - 1)]; }

static void dead_note(uint32_t p, uint32_t esp)
{
    Dead* d = dead_slot(p);
    d->p = p, d->ms = (uint32_t)plat_wall_ms(), d->ret[0] = d->ret[1] = 0;
    for (uint32_t i = 0, k = 0; i < 64 && k < 2 && readable(esp + 4 * i); ++i)
        if (is_return(rd32(esp + 4 * i)))
            d->ret[k++] = rd32(esp + 4 * i);
}

static const Dead* dead_find(uint32_t p)
{
    const Dead* d = dead_slot(p);
    return p && d->p == p ? d : NULL;
}

static uint32_t g_got_n; /* allocations seen (both allocators), for the report */

void xcl_note_got(uint32_t p)
{
    Dead* d = dead_slot(p);
    ++g_got_n;
    if (p && d->p == p)
        d->p = 0;
}

void xcl_note_free(uint32_t p, uint32_t size, uint32_t esp)
{
    if (!g_rdata_hi || !p || size < 4 || size == ~0u)
        return;
    dead_note(p, esp);
    defer_freed(p, size, esp);
    uint32_t vtbl = rd32(p);
    if (!in_rdata(vtbl) || !in_text(rd32(vtbl)))
        return;
    id_map_check(p, size, esp);
    FreeRec* r = &g_frees[g_free_n++ % FREES];
    r->p = p, r->size = size, r->vtbl = vtbl, r->ms = (uint32_t)plat_wall_ms();
    int k = 0;
    for (uint32_t i = 0; i < 64 && k < FREE_RETS && readable(esp + 4 * i); ++i)
    {
        uint32_t v = rd32(esp + 4 * i);
        if (is_return(v))
            r->ret[k++] = v;
    }
    for (; k < FREE_RETS; ++k)
        r->ret[k] = 0;
}

/* --- the game's deferred actor deletes (1.23b) --------------------------------------------------
 * The world's remove-actor (0x7C9740) takes an actor out of its list and pushes it onto a global
 * std::vector<Actor*> at 0x134DFA0 (begin +4, end +8, capacity +12). Once a frame 0x7D2C40 copies
 * it into two local vectors (CommonResourceActors at esp+0x18, the rest at esp+0x28), clears it and
 * hands each local vector to 0x7D27A0, which runs three passes over it: dynamic_cast to CharaActor
 * and 0x65DD20, the same and 0x65DD40, then the deleting destructor.
 *
 * The common crash there (FATAL or SIGBUS in 0x0065DD20, under 0x007D27A0) was the RTTI type names
 * overwritten by a mistranslated call (fixed in crates/recomp; see types_check). What is left: a
 * CharaActor queued again after its destructor ran, its effect controller long gone (see
 * effect_check below). Around it, on each free during a drain the
 * drain's frame is found on the guest stack (the return address of its call to 0x7D27A0, with the
 * vector it passed), and every entry in either local vector that points into the freed block, or
 * is no longer an object, is set to NULL; 0x7D27A0 copes with that (dynamic_cast of NULL is NULL,
 * the delete pass skips NULL). As the drain starts, the global vector is checked as well
 * (defer_filter). An entry freed while still in the global vector, or listed there twice, is logged. */
#define DEFER_VEC 0x0134DFA0u
#define DEFER_MAX 4096u

static int defer_bounds(uint32_t* b, uint32_t* e)
{
    if (!g_rdata_hi || !readable(DEFER_VEC + 4) || !readable(DEFER_VEC + 12))
        return 0;
    uint32_t begin = rd32(DEFER_VEC + 4), end = rd32(DEFER_VEC + 8), cap = rd32(DEFER_VEC + 12);
    if (!begin || begin > end || end > cap || (end - begin) % 4 || (end - begin) / 4 > DEFER_MAX || !readable(begin) ||
        (end > begin && !readable(end - 4)))
        return 0;
    *b = begin, *e = end;
    return 1;
}

static void log_rets(uint32_t esp)
{
    char line[160];
    int n = snprintf(line, sizeof line, " from");
    for (uint32_t i = 0, k = 0; i < 64 && k < FREE_RETS && readable(esp + 4 * i); ++i)
    {
        uint32_t v = rd32(esp + 4 * i);
        if (is_return(v) && n < (int)sizeof line - 12)
            n += snprintf(line + n, sizeof line - (size_t)n, " %08x", v), ++k;
    }
    snprintf(line + n, sizeof line - (size_t)n, "\n");
    (void)!write(2, line, strlen(line));
}

static uint32_t g_defer_seen_end; /* the end the last duplicate scan saw */

/* An entry queued for delete that no longer looks like an object (no vtable in .rdata): it was
 * freed before it was queued, so the drain will call through garbage. */
static void defer_check_object(uint32_t a, uint32_t esp)
{
    if (!a || (readable(a) && in_rdata(rd32(a)) && in_text(rd32(rd32(a)))))
        return;
    static uint32_t logged;
    if (++logged > 32)
        return;
    char line[256];
    int n = snprintf(line, sizeof line, "[crashlog] actor %08x queued for deferred delete is not an object (word %08x)", a,
        readable(a) ? rd32(a) : 0);
    for (uint32_t k = 0; k < FREES && k < g_free_n; ++k)
    {
        const FreeRec* r = &g_frees[(g_free_n - 1 - k) % FREES];
        if (a >= r->p && a - r->p < r->size)
        {
            char cls[96];
            n += snprintf(line + n, sizeof line - (size_t)n, ", a %s freed %.1f s before from %08x %08x %08x",
                class_of(r->vtbl, cls, sizeof cls), ((uint32_t)plat_wall_ms() - r->ms) / 1000.0, r->ret[0], r->ret[1], r->ret[2]);
            break;
        }
    }
    snprintf(line + n, sizeof line - (size_t)n, ";");
    (void)!write(2, line, strlen(line));
    log_rets(esp);
}

static int effect_gone(uint32_t a);

/* A CharaActor queued after it was destroyed (its effect controller is gone): who queued it. The
 * hook that sees it runs just after the push, often inside it (the vector growing), so the guest
 * stack still holds the remover's callers. */
static void defer_check_destroyed(uint32_t a, uint32_t esp)
{
    static uint32_t logged;
    if (!a || logged >= 32 || !effect_gone(a))
        return;
    ++logged;
    char line[400];
    int n = snprintf(line, sizeof line, "[actors] %08x queued for delete after it was destroyed (effect controller %08x gone), from", a,
        rd32(a + 0x12F0u));
    for (uint32_t i = 0, k = 0; i < 1024 && k < 16 && readable(esp + 4 * i) && n < (int)sizeof line - 12; ++i)
        if (is_return(rd32(esp + 4 * i)))
            n += snprintf(line + n, sizeof line - (size_t)n, " %08x", rd32(esp + 4 * i)), ++k;
    snprintf(line + n, sizeof line - (size_t)n, "\n");
    (void)!write(2, line, strlen(line));
}

/* duplicates in the deferred vector, looked for when it has grown since the last look */
static void defer_dups(uint32_t esp)
{
    uint32_t b, e;
    if (!defer_bounds(&b, &e) || e == g_defer_seen_end)
        return;
    uint32_t from = g_defer_seen_end >= b && g_defer_seen_end < e ? g_defer_seen_end : b;
    g_defer_seen_end = e;
    for (uint32_t i = from; i < e; i += 4)
        defer_check_object(rd32(i), esp), defer_check_destroyed(rd32(i), esp);
    for (uint32_t i = b + 4; i < e; i += 4)
        for (uint32_t j = b; j < i; j += 4)
            if (rd32(i) == rd32(j))
            {
                char cls[96], line[256];
                uint32_t a = rd32(i);
                snprintf(line, sizeof line, "[crashlog] actor %08x (%s) is queued twice for deferred delete (%u queued),", a,
                    readable(a) ? class_of(rd32(a), cls, sizeof cls) : "?", (e - b) / 4);
                (void)!write(2, line, strlen(line));
                log_rets(esp);
                break;
            }
}


/* the drain's calls to 0x7D27A0: return addresses, and where the vector each passes sits in its frame */
#define DRAIN_RET1 0x007D2DE1u /* vector at esp+0x28 */
#define DRAIN_RET2 0x007D2DEDu /* vector at esp+0x18 */
static const uint8_t DRAIN_CODE[] = { 0x8D, 0x4C, 0x24, 0x28, 0x51, 0x8B, 0xCE, 0xE8, 0xBF, 0xF9, 0xFF, 0xFF,
    0x8D, 0x54, 0x24, 0x18, 0x52, 0x8B, 0xCE, 0xE8, 0xB3, 0xF9, 0xFF, 0xFF };
static int g_drain_code = -1; /* the image is the one these addresses are for */
static int g_drain_maybe;     /* the global vector was seen holding actors: a drain may be under way */
static uint32_t g_drain_misses; /* frees since then with no drain on the stack (other threads, or it ended) */
static uint32_t g_drain_nulled;

static int drain_code_ok(void)
{
    if (g_drain_code < 0)
    {
        g_drain_code = in_text(DRAIN_RET1) && in_text(DRAIN_RET2);
        for (uint32_t i = 0; g_drain_code && i < sizeof DRAIN_CODE; ++i)
            g_drain_code = rd8(0x007D2DD5u + i) == DRAIN_CODE[i];
    }
    return g_drain_code;
}

#define DELETE_RET 0x007D28D5u /* 0x7D27A0's third pass, after its call of the deleting destructor */

/* The drain's esp (its frame's base for the local vectors), or 0 when no drain is on this stack;
 * *cur is the vector being processed, *deleting whether that is in its delete pass. */
static uint32_t drain_frame(uint32_t esp, uint32_t* cur, int* deleting)
{
    *deleting = 0;
    for (uint32_t a = esp, n = 0; n < 2048 && readable(a + 4); a += 4, ++n)
    {
        uint32_t r = rd32(a);
        if (r == DELETE_RET)
            *deleting = 1;
        if (r != DRAIN_RET1 && r != DRAIN_RET2)
            continue;
        uint32_t e = a + 8; /* esp in the drain before it pushed the vector and called */
        if (rd32(a + 4) == e + (r == DRAIN_RET1 ? 0x28u : 0x18u))
            return *cur = rd32(a + 4), e;
    }
    return 0;
}


/* Is the actor still in its world's list? The world is at actor+0x118 (0x7CEF20 removes through it);
 * its actor manager at world+0xA4 keeps a std::list (head node at +0x14; node: next, prev, actor). */
static int in_world_list(uint32_t actor)
{
    if (!readable(actor + 0x118))
        return 0;
    uint32_t w = rd32(actor + 0x118);
    if (!readable(w + 0xA4 + 0x14))
        return 0;
    uint32_t head = rd32(w + 0xA4 + 0x14);
    uint32_t n = 0;
    for (uint32_t node = readable(head) ? rd32(head) : head; node != head && n < 20000; node = rd32(node), ++n)
        if (!readable(node + 8) || rd32(node + 8) == actor)
            return readable(node + 8) ? 1 : 0;
    return 0;
}

/* A C++ object of the image: its first word a vtable in .rdata whose first slot is code. */
static int is_object(uint32_t a) { return readable(a) && in_rdata(rd32(a)) && in_text(rd32(rd32(a))); }

static int actor_log(void) /* XIVLEGACY_ACTORLOG=1 */
{
    static int v = -1;
    if (v < 0)
        v = getenv("XIVLEGACY_ACTORLOG") && *getenv("XIVLEGACY_ACTORLOG") == '1';
    return v;
}

/* Entries in a batch that are no longer objects (their memory was freed and reused): NULL. */
static uint32_t null_garbage(uint32_t vec)
{
    uint32_t b = rd32(vec + 4), e = rd32(vec + 8), n = 0;
    if (!b || b > e || (e - b) % 4 || (e - b) / 4 > DEFER_MAX || !readable(b) || (e > b && !readable(e - 4)))
        return 0;
    for (uint32_t i = b; i < e; i += 4)
    {
        uint32_t a = rd32(i);
        if (a && !is_object(a))
        {
            wr32(i, 0), ++n;
            if (++g_drain_nulled <= 64)
            {
                char line[160];
                snprintf(line, sizeof line, "[actors] %08x in the delete batch is no longer an object (word %08x): dropped from it\n",
                    a, readable(a) ? rd32(a) : 0);
                (void)!write(2, line, strlen(line));
            }
        }
    }
    return n;
}

/* --- a CharaActor's effect controller ---------------------------------------------------------
 * 0x7D27A0's first pass calls 0x65DD20 on each CharaActor in the batch, which calls through the
 * RaptureSchEffectController at actor+0x12F0 (NULL is skipped). Only the actor's destructor
 * (0x666130) deletes it, without clearing the field. A batch actor whose controller is already
 * gone (freed, or no longer an object) crashes there: the SIGBUS in 0x65DD20 at a small address,
 * the controller's first word being heap bookkeeping, or a stale CharaActor that a cast still
 * takes for one while its block is reused. */
#define TD_CHARA 0x012B7810u /* .?AVCharaActor@Chara@Actor@Scene@Application@@ */
#define CHARA_EFFECT 0x12F0u

static int is_chara(uint32_t a)
{
    if (!is_object(a) || !in_rdata(rd32(a) - 4))
        return 0;
    uint32_t col = rd32(rd32(a) - 4);
    if (!in_rdata(col) || !in_rdata(col + 0x10))
        return 0;
    if (rd32(col + 0xC) == TD_CHARA)
        return 1;
    uint32_t chd = rd32(col + 0x10);
    if (!in_rdata(chd) || !in_rdata(chd + 0xC))
        return 0;
    uint32_t n = rd32(chd + 8), arr = rd32(chd + 0xC);
    for (uint32_t i = 0; i < n && i < 64 && in_rdata(arr + 4 * i); ++i)
    {
        uint32_t bcd = rd32(arr + 4 * i);
        if (!in_rdata(bcd))
            break;
        if (rd32(bcd) == TD_CHARA)
            return 1;
    }
    return 0;
}

/* The world's actor manager (world+0xA4) also keeps a std::map<id, Actor*> at +4 (0x7D2A50 finds
 * in it: map +4 head; node: left, parent, right, key +0xC, actor +0x10, isnil +0x15). Lookups by id
 * (0x610D30, for the updater 0x7F1BD0) hand out whatever it holds, so an actor freed while still in
 * it is a use after free waiting to happen (SIGBUS in 0x663CE0 under 0x7F1BD0). Returns the node. */
static uint32_t g_last_world;

static uint32_t id_map_node(uint32_t w, uint32_t actor)
{
    uint32_t map = w + 0xA4 + 4;
    if (!readable(map + 8))
        return 0;
    uint32_t head = rd32(map + 4), size = rd32(map + 8);
    if (!readable(head + 0x14) || !(rd32(head + 0x14) >> 8 & 0xFF) || size > 100000)
        return 0;
    uint32_t stack[96], n = 0, seen = 0, root = rd32(head + 4);
    if (root != head)
        stack[n++] = root;
    while (n && seen++ <= size)
    {
        uint32_t x = stack[--n];
        if (!readable(x + 0x14) || (rd32(x + 0x14) >> 8 & 0xFF))
            continue;
        if (rd32(x + 0x10) == actor)
            return x;
        uint32_t l = rd32(x), r = rd32(x + 8);
        if (l != head && n < 96)
            stack[n++] = l;
        if (r != head && n < 96)
            stack[n++] = r;
    }
    return 0;
}

static uint32_t g_map_logged;

/* A RaptureActorContainer with a sane id map? */
static int world_ok(uint32_t w)
{
    if (!is_object(w) || !readable(w + 0xA4 + 12) || rd32(w + 0xA4 + 12) > 20000)
        return 0;
    uint32_t head = rd32(w + 0xA4 + 8);
    if (!readable(head + 0x14) || (rd32(head + 0x14) >> 8 & 0xFF) != 1)
        return 0;
    uint32_t root = rd32(head + 4);
    if (root == head ? rd32(w + 0xA4 + 12) != 0 : !readable(root + 0x14) || rd32(root + 4) != head)
        return 0;
    static uint32_t good_vt; /* the container's class, by name the first time */
    if (good_vt)
        return rd32(w) == good_vt;
    char cls[96];
    if (!strstr(class_of(rd32(w), cls, sizeof cls), "RaptureActorContainer@"))
        return 0;
    good_vt = rd32(w);
    return 1;
}

/* An actor being freed: is it still in its world's id map? Log it with the stack that frees it. */
static void id_map_check(uint32_t p, uint32_t size, uint32_t esp)
{
    /* The game allocator's frees come with no size (4): go by the world pointer at +0x118, which a
     * destroyed actor still has. A known size is a big enough block: try the last world seen. */
    uint32_t w = readable(p + 0x118) ? rd32(p + 0x118) : 0;
    if (world_ok(w))
    {
        if (w != g_last_world)
        {
            char cls[96], line[192];
            snprintf(line, sizeof line, "[actors] world %08x (%s): %u actors in its id map\n", w, class_of(rd32(w), cls, sizeof cls),
                rd32(w + 0xA4 + 12));
            (void)!write(2, line, strlen(line));
        }
        g_last_world = w;
    }
    else if (size < 0x200 || !world_ok(w = g_last_world))
        return;
    uint32_t node = id_map_node(w, p);
    if (!node)
        return;
    if (++g_map_logged <= 64)
    {
        char cls[96], line[256];
        snprintf(line, sizeof line, "[actors] %08x (%s) freed while still in its world's id map (id %08x, world %08x),", p,
            class_of(rd32(p), cls, sizeof cls), rd32(node + 0xC), w);
        (void)!write(2, line, strlen(line));
        log_rets(esp);
    }
}

/* 0x610D30 finds an actor by id in its world's id map: the world's +0x70 object, the id at its
 * +0x134. Its only callers are the camera contexts (0x7F1BD0 ... 0x7F7BA0, e.g. the third-person lock
 * camera CameraActorContextTPS_Lock), which call it each frame, cope with NULL, and treat what it finds
 * as a CharaActor: 0x663CE0 reads +0x1350 of it. In Ul'dah the id once found a BgObjActor (a live
 * object, far smaller): SIGBUS at +0x1350, after a same-zone !warp in the Platinum Mirage and after a
 * !goto to Gegeissa, whose frames before the crash were black (the camera aimed from that object).
 * The id was 0xC0000000, the game's "no actor" id (what a warp leaves as the lock target), under which
 * the map also holds local actors without a server id (WeaponActor, BgObjActor). The recompiler's hook at 0x610D55 (test eax, eax, right after the find) checks what it found:
 * anything but a whole, live CharaActor is not found. */
static const uint8_t LOOKUP_CODE[] = { 0x8D, 0x88, 0xA4, 0x00, 0x00, 0x00, 0xE8 }; /* 0x610D4A: lea ecx, [eax + 0xa4]; call */

void xcl_actor_lookup(Guest* g)
{
    static int code = -1;
    static uint32_t logged;
    if (code < 0)
    {
        code = g_rdata_hi && in_text(0x610D4Au);
        for (uint32_t i = 0; code && i < sizeof LOOKUP_CODE; ++i)
            code = rd8(0x610D4Au + i) == LOOKUP_CODE[i];
        const char* m = code ? "[actors] id lookup check on\n" : "[actors] id lookup check off: not the 1.23b code\n";
        (void)!write(2, m, strlen(m));
    }
    uint32_t a = g->eax;
    if (!code || !a)
        return;
    int obj = is_object(a), chara = obj && is_chara(a);
    if (chara && gwin_is_committed(a + 0x1000) && gwin_is_committed(a + CHARA_EFFECT + 0x100))
        return;
    if (++logged <= 32)
    {
        /* 0x7D2A50 pops its one argument (ret 4), the id, which is left just below esp. */
        char cls[96], line[256];
        snprintf(line, sizeof line, "[actors] id lookup for %08x found %08x (%s), which is not a live CharaActor: not found,",
            rd32(g->esp - 4), a, obj ? class_of(rd32(a), cls, sizeof cls) : readable(a) ? "not an object" : "unmapped");
        (void)!write(2, line, strlen(line));
        log_rets(g->esp);
    }
    g->eax = 0;
}

static const FreeRec* free_rec(uint32_t a)
{
    for (uint32_t k = 0; k < FREES && k < g_free_n; ++k)
    {
        const FreeRec* r = &g_frees[(g_free_n - 1 - k) % FREES];
        if (a >= r->p && a - r->p < r->size)
            return r;
    }
    return NULL;
}

static uint32_t g_effect_logged;

static int effect_gone(uint32_t a)
{
    if (!readable(a + CHARA_EFFECT))
        return 0;
    uint32_t c = rd32(a + CHARA_EFFECT);
    return c && !is_object(c) && is_chara(a);
}

/* What to do with a queued or batched actor a, given its effect controller (and (p, size), the
 * block being freed right now, if any):
 *   0 nothing;
 *   1 its controller is being freed: the field was cleared, the actor stays;
 *   2 its controller is no longer an object: the actor was destroyed already (its destructor is
 *     the only code that deletes the controller) and must not be deleted again; take it out.
 * (Not the freed-blocks table: the game's allocators aren't all seen, so a block found there may be
 * live again; a controller was, at login.) */
static int effect_check(uint32_t a, uint32_t p, uint32_t size, const char* when)
{
    if (!readable(a + CHARA_EFFECT))
        return 0;
    uint32_t c = rd32(a + CHARA_EFFECT);
    if (!c)
        return 0;
    int freeing = size && c >= p && c - p < size;
    if (!freeing && is_object(c))
        return 0;
    if (!is_chara(a))
        return 0;
    if (freeing)
        wr32(a + CHARA_EFFECT, 0);
    if (++g_effect_logged > 64 || (freeing && !actor_log()))
        return freeing ? 1 : 2;
    char cls[96], line[360];
    const FreeRec* r = free_rec(c);
    const Dead *da = dead_find(a), *dc = dead_find(c);
    int n = snprintf(line, sizeof line, "[actors] %08x (vtable %08x %s) %s: its effect controller %08x %s", a, rd32(a),
        class_of(rd32(a), cls, sizeof cls), when, c,
        freeing ? "is being freed: cleared" : "is no longer an object: destroyed already, not deleted again");
    if (!freeing)
        n += snprintf(line + n, sizeof line - (size_t)n, " (word %08x)", readable(c) ? rd32(c) : 0);
    if (r)
        n += snprintf(line + n, sizeof line - (size_t)n, "; controller freed %.1f s before from %08x %08x %08x",
            ((uint32_t)plat_wall_ms() - r->ms) / 1000.0, r->ret[0], r->ret[1], r->ret[2]);
    else if (dc && !freeing)
        n += snprintf(line + n, sizeof line - (size_t)n, "; controller freed %.1f s before from %08x %08x",
            ((uint32_t)plat_wall_ms() - dc->ms) / 1000.0, dc->ret[0], dc->ret[1]);
    if (da)
        n += snprintf(line + n, sizeof line - (size_t)n, "; actor block freed %.1f s before from %08x %08x",
            ((uint32_t)plat_wall_ms() - da->ms) / 1000.0, da->ret[0], da->ret[1]);
    snprintf(line + n, sizeof line - (size_t)n, "\n");
    (void)!write(2, line, strlen(line));
    return freeing ? 1 : 2;
}

/* The batch's CharaActors: a controller being freed is cleared; an actor destroyed already is
 * dropped (its own slot as well, when it is the one being deleted now: the pass has read it). */
static void effects_in(uint32_t vec, uint32_t p, uint32_t size)
{
    uint32_t b = rd32(vec + 4), e = rd32(vec + 8);
    if (!b || b > e || (e - b) % 4 || (e - b) / 4 > DEFER_MAX || !readable(b) || (e > b && !readable(e - 4)))
        return;
    for (uint32_t i = b; i < e; i += 4)
        if (rd32(i) && effect_check(rd32(i), p, size, "in the delete batch") == 2)
            wr32(i, 0);
}

static uint32_t null_in(uint32_t vec, uint32_t p, uint32_t size)
{
    uint32_t b = rd32(vec + 4), e = rd32(vec + 8), n = 0;
    if (!b || b > e || (e - b) % 4 || (e - b) / 4 > DEFER_MAX || !readable(b) || (e > b && !readable(e - 4)))
        return 0;
    for (uint32_t i = b; i < e; i += 4)
    {
        uint32_t a = rd32(i);
        if (a && a >= p && a - p < size)
            wr32(i, 0), ++n;
    }
    return n;
}

static void drain_guard(uint32_t p, uint32_t size, uint32_t esp)
{
    uint32_t b, e;
    if (defer_bounds(&b, &e) && e > b)
    {
        g_drain_maybe = 1, g_drain_misses = 0; /* filled: the next drain clears it, then frees */
        return;
    }
    if (!g_drain_maybe || !drain_code_ok())
        return;
    uint32_t cur;
    int deleting;
    uint32_t f = drain_frame(esp, &cur, &deleting);
    if (!f)
    {
        if (++g_drain_misses > 512)
            g_drain_maybe = 0; /* long over */
        return;
    }
    g_drain_misses = 0;
    uint32_t other = cur == f + 0x28 ? f + 0x18 : f + 0x28;
    uint32_t n_cur = null_in(cur, p, size), n_other = null_in(other, p, size);
    null_garbage(cur), null_garbage(other);
    effects_in(cur, p, size), effects_in(other, p, size);
    /* The actor being deleted right now loses its own slot as well (harmless, the pass has read
     * it). An entry the drain has not reached yet is the news: one in the other vector, or one in
     * this vector outside the delete pass. (A later one in this vector during the delete pass
     * can't be told from the actor's own slot here; it is dropped all the same.) */
    int verbose = actor_log(); /* every entry dropped, the actor's own slot too */
    if (deleting && n_cur && in_world_list(p))
    {
        char line[200];
        snprintf(line, sizeof line, "[actors] %08x deleted while still in its world's actor list (world %08x)", p, rd32(p + 0x118));
        (void)!write(2, line, strlen(line));
        log_rets(esp);
    }
    if (verbose && (n_cur || n_other))
    {
        char line[160];
        snprintf(line, sizeof line, "[actors] drain %08x: %08x+%x freed, %u in this batch%s, %u in the other\n", f, p, size, n_cur,
            deleting ? " (deleting)" : "", n_other);
        (void)!write(2, line, strlen(line));
    }
    if (!n_other && !(n_cur && !deleting))
        return;
    char cls[96];
    const char* c = readable(p) ? class_of(rd32(p), cls, sizeof cls) : "?";
    if (++g_drain_nulled <= 64)
    {
        char line[256];
        snprintf(line, sizeof line, "[actors] %08x (%s) freed while waiting in the delete batch: dropped from it,", p, c);
        (void)!write(2, line, strlen(line));
        log_rets(esp);
    }
}

/* The game's own allocator frees through 0x40DF70(ptr) (this = the allocator kept at ptr - 4), which
 * takes the allocator's critical section first: an EnterCriticalSection returning to 0x40DF87 is a
 * free of the object at [esp + 0x18]. Actors are freed this way, not by HeapFree. */
#define GAME_FREE_RET 0x0040DF87u
static const uint8_t GAME_FREE_CODE[] = { 0x55, 0xFF, 0x15, 0x6C, 0xE1, 0xF3, 0x00 }; /* push ebp; call [EnterCriticalSection] */
static int g_game_free_code = -1;

static void types_check(uint32_t esp);

void xcl_note_cs(uint32_t esp)
{
    uint32_t b, e;
    if (!g_rdata_hi)
        return;
    types_check(esp);
    if (defer_bounds(&b, &e) && e > b)
        g_drain_maybe = 1, g_drain_misses = 0, defer_dups(esp), defer_filter(esp); /* filled: the next drain clears it, then frees */
    if (rd32(esp) != GAME_FREE_RET)
        return;
    if (g_game_free_code < 0)
    {
        g_game_free_code = in_text(GAME_FREE_RET);
        for (uint32_t i = 0; g_game_free_code && i < sizeof GAME_FREE_CODE; ++i)
            g_game_free_code = rd8(GAME_FREE_RET - 7 + i) == GAME_FREE_CODE[i];
    }
    if (!g_game_free_code || !readable(esp + 0x18))
        return;
    uint32_t p = rd32(esp + 0x18);
    if (p < 0x10000u || !readable(p))
        return;
    xcl_note_free(p, 4, esp); /* the size isn't known here: the object's start is what the queues hold */
}

/* --- the queue, checked as the drain starts ----------------------------------------------------
 * The drain's first allocations (it reserves its two local vectors, sized from the global one,
 * before it copies anything) are seen here with its return addresses on the stack. At that point
 * the global vector still holds this frame's whole batch, so entries the drain must not delete
 * are taken out of it:
 * - a pointer to a block freed since (the actor was queued after it had already been deleted:
 *   deleting it again calls through whatever reuses the memory, the crash in 0x7D27A0's delete
 *   pass, with a vtable of floats or string bytes);
 * - an entry that isn't an object (no vtable of the image);
 * - the same actor queued twice (it would be deleted twice);
 * - an actor back in its world's actor list (added again after it was removed: deleting it would
 *   leave the list pointing at freed memory, and its next removal queues that). */
#define RESERVE_RET1 0x007D2CB0u
#define RESERVE_RET2 0x007D2CD2u
static const uint8_t RESERVE_CODE1[] = { 0x8D, 0x4C, 0x24, 0x2C, 0xE8, 0x50, 0xBB, 0xD0, 0xFF };
static const uint8_t RESERVE_CODE2[] = { 0x8D, 0x4C, 0x24, 0x1C, 0xE8, 0x2E, 0xBB, 0xD0, 0xFF };
static int g_reserve_code = -1;
static int g_cast_check; /* a drain is starting: compare the game's dynamic_cast with ours */
static uint32_t g_filter_logged;

static int reserve_code_ok(void)
{
    if (g_reserve_code < 0)
    {
        g_reserve_code = in_text(RESERVE_RET1) && in_text(RESERVE_RET2);
        for (uint32_t i = 0; g_reserve_code && i < sizeof RESERVE_CODE1; ++i)
            g_reserve_code = rd8(RESERVE_RET1 - 9 + i) == RESERVE_CODE1[i] && rd8(RESERVE_RET2 - 9 + i) == RESERVE_CODE2[i];
    }
    return g_reserve_code;
}

static void filter_log(const char* what, uint32_t a, const Dead* d)
{
    if (++g_filter_logged > 200)
        return;
    char cls[96], line[256];
    int ok = readable(a) && in_rdata(rd32(a)) && in_text(rd32(rd32(a)));
    int n = snprintf(line, sizeof line, "[actors] %08x (%s) %s%s", a, ok ? class_of(rd32(a), cls, sizeof cls) : "?", what,
        strstr(what, "(kept)") ? "" : ": taken out of the delete queue");
    if (d)
        n += snprintf(line + n, sizeof line - (size_t)n, " (freed %.1f s before, from %08x %08x)",
            ((uint32_t)plat_wall_ms() - d->ms) / 1000.0, d->ret[0], d->ret[1]);
    snprintf(line + n, sizeof line - (size_t)n, "\n");
    (void)!write(2, line, strlen(line));
}

static void defer_filter(uint32_t esp)
{
    uint32_t b, e;
    if (!defer_bounds(&b, &e) || e == b || !reserve_code_ok())
        return;
    int found = 0;
    for (uint32_t a = esp, n = 0; n < 512 && readable(a); a += 4, ++n)
        if (rd32(a) == RESERVE_RET1 || rd32(a) == RESERVE_RET2)
        {
            found = 1;
            break;
        }
    if (!found)
        return;
    g_cast_check = 1;
    uint32_t out = b;
    for (uint32_t i = b; i < e; i += 4)
    {
        uint32_t a = rd32(i);
        const Dead* d = dead_find(a);
        int dup = 0;
        for (uint32_t j = b; j < out && !dup; j += 4)
            dup = rd32(j) == a;
        if (!a)
            continue;
        if (!is_object(a))
            filter_log("queued but not an object", a, d);
        else if (dup)
            filter_log("queued twice", a, NULL);
        else if (in_world_list(a))
            filter_log("back in its world's actor list", a, NULL);
        else if (effect_check(a, 0, 0, "queued for delete") == 2)
            ;
        else
        {
            if (d && actor_log()) /* a hint only: the allocators aren't all seen, so the block may be live again */
                filter_log("was freed once (kept)", a, d), --g_filter_logged;
            wr32(out, a), out += 4;
            continue;
        }
    }
    if (out != e)
        wr32(DEFER_VEC + 8, out), g_defer_seen_end = out;
}

/* The type descriptors of RaptureActor (0x12B77E0) and CharaActor (0x12B7810) in .data: in long
 * sessions their names have been found blanked, and with both empty __RTDynamicCast's name compare
 * matches any RaptureActor (a BgPlateActor, a WindowActor) as a CharaActor; the deferred delete's
 * first two passes then read and write a CharaActor's fields past the end of it (the crashes in
 * 0x65DD20, 0x65DD40 and 0x7D27A0). The names are kept as first seen and checked on each critical
 * section; a change is logged with the guest stack and undone. The spare word at +4 (type_info's
 * cached readable name) is left alone. */
#define TD_RAPTURE_ACTOR 0x012B77E0u
static const struct { uint32_t td; const char* name; } TYPE_NAMES[] = {
    { TD_RAPTURE_ACTOR, ".?AVRaptureActor@Scene@Application@@" },
    { 0x012B7810u, ".?AVCharaActor@Chara@Actor@Scene@Application@@" },
};
static int g_types_ok = -1;
static uint32_t g_types_fixed;

static void types_check(uint32_t esp)
{
    if (g_types_ok < 0)
    {
        g_types_ok = 1;
        for (size_t i = 0; i < sizeof TYPE_NAMES / sizeof *TYPE_NAMES; ++i)
            g_types_ok &= readable(TYPE_NAMES[i].td + 8) &&
                !memcmp(GUEST_PTR(TYPE_NAMES[i].td + 8), TYPE_NAMES[i].name, strlen(TYPE_NAMES[i].name) + 1);
    }
    if (!g_types_ok)
        return; /* another exe, or already changed before the first look: leave it */
    for (size_t i = 0; i < sizeof TYPE_NAMES / sizeof *TYPE_NAMES; ++i)
    {
        uint32_t td = TYPE_NAMES[i].td;
        size_t n = strlen(TYPE_NAMES[i].name) + 1;
        if (!memcmp(GUEST_PTR(td + 8), TYPE_NAMES[i].name, n))
            continue;
        if (++g_types_fixed <= 8)
        {
            char line[256];
            int k = snprintf(line, sizeof line, "[actors] type name at %08x overwritten (spare %08x, now", td, rd32(td + 4));
            for (uint32_t j = 0; j < 16 && k < (int)sizeof line - 12; j += 4)
                k += snprintf(line + k, sizeof line - (size_t)k, " %08x", rd32(td + 8 + j));
            snprintf(line + k, sizeof line - (size_t)k, "): restored\n");
            (void)!write(2, line, strlen(line));
            log_rets(esp);
        }
        memcpy(GUEST_PTR(td + 8), TYPE_NAMES[i].name, n);
    }
}

/* XIVLEGACY_CASTCHECK=1: as a drain starts, the game's own __RTDynamicCast (0x9DA6CC) to CharaActor
 * is run on every queued entry and compared with this file's RTTI walk (the drain's pass 1 has been
 * seen taking a BgPlateActor or a WindowActor for a CharaActor). */
#define RT_DYNAMIC_CAST 0x009DA6CCu
static uint32_t g_cast_bad;

static uint32_t cast_once(Guest* g, uint32_t a, uint32_t src, uint32_t dst)
{
    static Guest saved;
    saved = *g;
    uint32_t sp = (g->esp - 0x400u) & ~15u;
    wr32(sp - 4, 0), wr32(sp - 8, dst), wr32(sp - 12, src), wr32(sp - 16, 0), wr32(sp - 20, a);
    wr32(sp - 24, 0xFEEDF00Du);
    g->esp = sp - 24, g->df = 0;
    rt_call_indirect(g, RT_DYNAMIC_CAST);
    uint32_t r = g->eax;
    *g = saved;
    return r;
}

static void td_line(const char* what, uint32_t td)
{
    char line[200];
    int n = snprintf(line, sizeof line, "  %s td %08x: spare %08x name \"", what, td, readable(td + 4) ? rd32(td + 4) : 0);
    for (uint32_t k = 0; k < 64 && readable(td + 8 + k) && rd8(td + 8 + k) && n < (int)sizeof line - 4; ++k)
        line[n++] = (char)rd8(td + 8 + k);
    snprintf(line + n, sizeof line - (size_t)n, "\"\n");
    (void)!write(2, line, strlen(line));
}

/* a wrong cast: again, on its own; then the type names it compares */
static void cast_dump(Guest* g, uint32_t a)
{
    char line[200];
    uint32_t again = cast_once(g, a, TD_RAPTURE_ACTOR, TD_CHARA), self = cast_once(g, a, TD_RAPTURE_ACTOR, TD_RAPTURE_ACTOR);
    snprintf(line, sizeof line, "  again: %08x; to RaptureActor: %08x\n", again, self);
    (void)!write(2, line, strlen(line));
    td_line("target", TD_CHARA);
    td_line("source", TD_RAPTURE_ACTOR);
    uint32_t col = rd32(rd32(a) - 4), chd = rd32(col + 0x10), n = rd32(chd + 8), arr = rd32(chd + 0xC);
    snprintf(line, sizeof line, "  col %08x (sig %08x off %08x cd %08x td %08x) chd %08x attr %08x n %u\n", col, rd32(col), rd32(col + 4),
        rd32(col + 8), rd32(col + 12), chd, rd32(chd + 4), n);
    (void)!write(2, line, strlen(line));
    for (uint32_t i = 0; i < n && i < 16; ++i)
    {
        uint32_t bcd = rd32(arr + 4 * i);
        char what[64];
        snprintf(what, sizeof what, "base %u (bcd %08x sub %u pmd %d,%d,%d attr %x)", i, bcd, rd32(bcd + 4), (int)rd32(bcd + 8),
            (int)rd32(bcd + 12), (int)rd32(bcd + 16), rd32(bcd + 20));
        td_line(what, rd32(bcd));
    }
}

void xcl_cast_check(Guest* g)
{
    static int on = -1;
    if (on < 0)
        on = getenv("XIVLEGACY_CASTCHECK") && *getenv("XIVLEGACY_CASTCHECK") == '1';
    if (!g_cast_check || !on)
        return;
    g_cast_check = 0;
    uint32_t b, e;
    if (!defer_bounds(&b, &e) || rd8(RT_DYNAMIC_CAST) != 0x6A || rd8(RT_DYNAMIC_CAST + 1) != 0x18)
        return;
    static uint32_t checks;
    for (uint32_t i = b; i < e; i += 4)
    {
        uint32_t a = rd32(i);
        if (!is_object(a))
            continue;
        uint32_t r = cast_once(g, a, TD_RAPTURE_ACTOR, TD_CHARA);
        ++checks;
        uint32_t want = is_chara(a) ? a : 0;
        if (r != want && ++g_cast_bad <= 64)
        {
            char cls[96], line[256];
            snprintf(line, sizeof line, "[actors] dynamic_cast<CharaActor>(%08x, vtable %08x %s) = %08x, its RTTI says %08x (%u checked)\n",
                a, rd32(a), class_of(rd32(a), cls, sizeof cls), r, want, checks);
            (void)!write(2, line, strlen(line));
            if (g_cast_bad <= 2)
                cast_dump(g, a);
        }
    }
    if (actor_log())
    {
        char line[96];
        snprintf(line, sizeof line, "[actors] cast check: %u queued, %u casts so far, %u wrong\n", (e - b) / 4, checks, g_cast_bad);
        (void)!write(2, line, strlen(line));
    }
}

/* The game allocator's two allocs (0x40DFD0 tracked, 0x40E040 not) leave its critical section
 * with the block in esi: "push ebp/edi; mov esi, eax; call [LeaveCriticalSection]". */
#define GAME_ALLOC_RET1 0x0040E029u
#define GAME_ALLOC_RET2 0x0040E072u
static const uint8_t GAME_ALLOC_CODE1[] = { 0x55, 0x8B, 0xF0, 0xFF, 0x15, 0x68, 0xE1, 0xF3, 0x00 };
static const uint8_t GAME_ALLOC_CODE2[] = { 0x57, 0x8B, 0xF0, 0xFF, 0x15, 0x68, 0xE1, 0xF3, 0x00 };
static int g_game_alloc_code = -1;

void xcl_note_leave_cs(uint32_t esp, uint32_t esi)
{
    uint32_t r = g_rdata_hi ? rd32(esp) : 0;
    if (r != GAME_ALLOC_RET1 && r != GAME_ALLOC_RET2)
        return;
    if (g_game_alloc_code < 0)
    {
        g_game_alloc_code = in_text(GAME_ALLOC_RET1) && in_text(GAME_ALLOC_RET2);
        for (uint32_t i = 0; g_game_alloc_code && i < sizeof GAME_ALLOC_CODE1; ++i)
            g_game_alloc_code = rd8(GAME_ALLOC_RET1 - 9 + i) == GAME_ALLOC_CODE1[i] &&
                                rd8(GAME_ALLOC_RET2 - 9 + i) == GAME_ALLOC_CODE2[i];
    }
    if (g_game_alloc_code)
        xcl_note_got(esi);
}

void xcl_note_alloc(uint32_t esp)
{
    uint32_t b, e;
    defer_dups(esp);
    if (defer_bounds(&b, &e) && e > b)
        g_drain_maybe = 1, g_drain_misses = 0, defer_filter(esp);
}

static void defer_freed(uint32_t p, uint32_t size, uint32_t esp)
{
    uint32_t b, e;
    defer_dups(esp);
    drain_guard(p, size, esp);
    if (!defer_bounds(&b, &e))
        return;
    for (uint32_t i = b; i < e; i += 4)
    {
        uint32_t a = rd32(i);
        if (a >= p && a - p < size)
        {
            char cls[96], line[256];
            snprintf(line, sizeof line, "[crashlog] actor %08x (%s) freed while queued for deferred delete (%u queued),", a,
                readable(p) ? class_of(rd32(p), cls, sizeof cls) : "?", (e - b) / 4);
            (void)!write(2, line, strlen(line));
            log_rets(esp);
            return;
        }
    }
}

/* --- packets ----------------------------------------------------------------------------------- */

#define STREAMS 64
#define PKTS 96u
typedef struct
{
    uint32_t ms;
    uint8_t stream, dir;
    uint16_t size, type, opcode;
    uint32_t source, target;
    uint32_t words[4]; /* the first payload words after the 0x20-byte headers */
} PktRec;
static PktRec g_pkts[PKTS];
static uint32_t g_pkt_n;

typedef struct
{
    uint8_t hdr[16];
    uint32_t hdr_have; /* base packet header bytes so far */
    uint32_t left;     /* bytes of this base packet's body still to come */
    int skip;          /* compressed or out of step: pass the body over */
    uint8_t sub[0x30]; /* the current subpacket's first bytes */
    uint32_t sub_have, sub_size;
} Stream;
static Stream g_streams[STREAMS][2];

void xcl_stream_reset(int stream)
{
    if (stream >= 0 && stream < STREAMS)
        memset(g_streams[stream], 0, sizeof g_streams[stream]);
}

static uint16_t u16(const uint8_t* b) { return (uint16_t)(b[0] | b[1] << 8); }
static uint32_t u32(const uint8_t* b) { return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24; }

static void keep_sub(int stream, int dir, const uint8_t* s, uint32_t have)
{
    PktRec* r = &g_pkts[g_pkt_n++ % PKTS];
    memset(r, 0, sizeof *r);
    r->ms = (uint32_t)plat_wall_ms(), r->stream = (uint8_t)stream, r->dir = (uint8_t)dir;
    r->size = u16(s), r->type = u16(s + 2), r->source = u32(s + 4), r->target = u32(s + 8);
    if (have >= 0x14)
        r->opcode = u16(s + 0x12);
    for (uint32_t i = 0; i < 4 && 0x20 + 4 * i + 4 <= have; ++i)
        r->words[i] = u32(s + 0x20 + 4 * i);
}

void xcl_packets(int stream, int dir, const uint8_t* buf, uint32_t n)
{
    if (stream < 0 || stream >= STREAMS || !buf)
        return;
    Stream* st = &g_streams[stream][dir & 1];
    uint32_t i = 0;
    while (i < n)
    {
        if (st->hdr_have < sizeof st->hdr)
        {
            st->hdr[st->hdr_have++] = buf[i++];
            if (st->hdr_have == sizeof st->hdr)
            {
                uint16_t size = u16(st->hdr + 4);
                if (size < sizeof st->hdr)
                {
                    st->hdr_have = sizeof st->hdr, st->left = 0xFFFFFFFFu, st->skip = 1; /* not 1.0 framing */
                    continue;
                }
                st->left = size - (uint32_t)sizeof st->hdr;
                st->skip = st->hdr[1] != 0; /* compressed */
                st->sub_have = 0;
                if (!st->left)
                    st->hdr_have = 0;
            }
            continue;
        }
        if (st->skip)
        {
            uint32_t k = n - i < st->left ? n - i : st->left;
            if (st->left != 0xFFFFFFFFu)
                st->left -= k;
            i += k;
            if (!st->left)
                st->hdr_have = 0;
            continue;
        }
        if (st->sub_have < sizeof st->sub)
            st->sub[st->sub_have] = buf[i];
        st->sub_have++, st->left--, i++;
        if (st->sub_have == 2)
        {
            st->sub_size = u16(st->sub);
            if (st->sub_size < 0x10 || st->sub_size > st->left + 2)
            {
                st->skip = 1; /* out of step: pass the rest of this base packet over */
                if (!st->left)
                    st->hdr_have = 0;
                continue;
            }
        }
        if (st->sub_have >= 2 && st->sub_have == st->sub_size)
        {
            keep_sub(stream, dir, st->sub, st->sub_size < sizeof st->sub ? st->sub_size : (uint32_t)sizeof st->sub);
            st->sub_have = 0;
        }
        if (!st->left)
            st->hdr_have = 0, st->sub_have = 0;
    }
}

/* --- the report -------------------------------------------------------------------------------- */

static void out(int fd, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void out(int fd, const char* fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof line - 1)
        n = (int)sizeof line - 1;
    if (n > 0)
        (void)!write(fd, line, (size_t)n);
}

/* The MSVC RTTI class name of a vtable (".?AVName@Ns@@" -> "Name@Ns"), or "?" */
static const char* class_of(uint32_t vtbl, char* buf, size_t cap)
{
    snprintf(buf, cap, "?");
    if (!in_rdata(vtbl) || !in_rdata(vtbl - 4))
        return buf;
    uint32_t col = rd32(vtbl - 4);
    if (!in_rdata(col) || !in_rdata(col + 12))
        return buf;
    uint32_t td = rd32(col + 12);
    if (td < g_rdata_lo || td >= 0x7FFFFFFFu || !readable(td + 8))
        return buf;
    size_t k = 0;
    for (uint32_t a = td + 8 + 4; k + 1 < cap && readable(a) && rd8(a) && !(rd8(a) == '@' && rd8(a + 1) == '@'); ++a)
        buf[k++] = (char)rd8(a);
    buf[k] = 0;
    return k ? buf : (snprintf(buf, cap, "?"), buf);
}

/* What is known about guest address a: a recorded free of the block holding it. */
static void explain(int fd, const char* what, uint32_t a, uint32_t now)
{
    if (a < 0x10000u)
        return;
    int shown = 0;
    for (uint32_t k = 0; k < FREES && k < g_free_n && shown < 2; ++k)
    {
        const FreeRec* r = &g_frees[(g_free_n - 1 - k) % FREES];
        if (a < r->p || a - r->p >= r->size)
            continue;
        char cls[96];
        out(fd, "  %s %08x is %u bytes into a %u-byte %s (vtable %08x) freed %.1f s before, from", what, a, a - r->p, r->size,
            class_of(r->vtbl, cls, sizeof cls), r->vtbl, (double)(uint32_t)(now - r->ms) / 1000.0);
        for (int i = 0; i < FREE_RETS && r->ret[i]; ++i)
            out(fd, " %08x", r->ret[i]);
        out(fd, "%s\n", readable(a) && readable(r->p) && rd32(r->p) != r->vtbl ? " (since reused)" : "");
        ++shown;
    }
}

void xcl_report(int fd, const Guest* g)
{
    uint32_t now = (uint32_t)plat_wall_ms();
    out(fd, "[crashlog] %u game object frees recorded, %u allocations seen, %u game subpackets seen\n", g_free_n, g_got_n,
        g_pkt_n);
    if (g)
    {
        const struct
        {
            const char* name;
            uint32_t v;
        } regs[] = { { "eax", g->eax }, { "ecx", g->ecx }, { "edx", g->edx }, { "ebx", g->ebx }, { "esi", g->esi }, { "edi", g->edi },
            { "ebp", g->ebp } };
        for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; ++i)
        {
            uint32_t v = regs[i].v;
            char cls[96], what[24];
            explain(fd, regs[i].name, v, now);
            if (readable(v))
            {
                uint32_t w = rd32(v);
                if (in_rdata(w))
                    out(fd, "  %s %08x: vtable %08x %s\n", regs[i].name, v, w, class_of(w, cls, sizeof cls));
                snprintf(what, sizeof what, "[%s]", regs[i].name);
                explain(fd, what, w, now);
            }
        }
        /* the stack's words that point into freed objects (a caller's `this`) */
        for (uint32_t i = 0; i < 32 && readable(g->esp + 4 * i); ++i)
        {
            char what[24];
            snprintf(what, sizeof what, "[esp+%02x]", 4 * i);
            uint32_t v = rd32(g->esp + 4 * i);
            if (!in_text(v))
                explain(fd, what, v, now);
        }
    }
    if (g && g_rdata_hi && drain_code_ok())
    {
        uint32_t cur;
        int deleting;
        uint32_t f = drain_frame(g->esp, &cur, &deleting);
        for (int v = 0; f && v < 2; ++v)
        {
            uint32_t vec = f + (v ? 0x18u : 0x28u), b = rd32(vec + 4), e = rd32(vec + 8);
            if (!b || b > e || (e - b) / 4 > DEFER_MAX || !readable(b))
                continue;
            out(fd, "[crashlog] delete batch %s (%u)%s:\n", v ? "CommonResourceActors" : "actors", (e - b) / 4,
                vec == cur ? (deleting ? ", in its delete pass" : ", being processed") : "");
            for (uint32_t i = b; i < e && i < b + 4 * 64; i += 4)
            {
                uint32_t a = rd32(i), c = readable(a + CHARA_EFFECT) ? rd32(a + CHARA_EFFECT) : 0;
                char cls[96], ccls[96];
                out(fd, "  %08x %s", a, is_object(a) ? class_of(rd32(a), cls, sizeof cls) : "(not an object)");
                if (is_chara(a))
                    out(fd, ", effect controller %08x %s", c, !c ? "" : is_object(c) ? class_of(rd32(c), ccls, sizeof ccls) : "(not an object)");
                out(fd, "\n");
                explain(fd, "   actor", a, now);
                if (c && is_chara(a))
                    explain(fd, "   controller", c, now);
            }
        }
    }
    uint32_t n = g_pkt_n < PKTS ? g_pkt_n : PKTS;
    out(fd, "[crashlog] the last %u game subpackets (s before, stream, in/out, opcode, source -> target, size, payload):\n", n);
    for (uint32_t k = n; k > 0; --k)
    {
        const PktRec* r = &g_pkts[(g_pkt_n - k) % PKTS];
        out(fd, "  %6.2f  s%-2u %s op %04x  %08x -> %08x  %4u  %08x %08x %08x %08x\n", (double)(uint32_t)(now - r->ms) / 1000.0,
            r->stream, r->dir ? "out" : "in ", r->opcode, r->source, r->target, r->size, r->words[0], r->words[1], r->words[2],
            r->words[3]);
    }
}
