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
static const char* class_of(uint32_t vtbl, char* buf, size_t cap);

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

void xcl_note_free(uint32_t p, uint32_t size, uint32_t esp)
{
    if (!g_rdata_hi || !p || size < 4 || size == ~0u)
        return;
    defer_freed(p, size, esp);
    uint32_t vtbl = rd32(p);
    if (!in_rdata(vtbl) || !in_text(rd32(vtbl)))
        return;
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
 * Destroying one actor can free another that is still waiting in a local vector (an in-zone warp
 * that drops many actors at once does it). The next pass then calls through the dead object: the
 * FATAL or SIGBUS in 0x0065DD20 / 0x0065DD40 / 0x007D27A0. So on each free during a drain the
 * drain's frame is found on the guest stack (the return address of its call to 0x7D27A0, with the
 * vector it passed), and every entry in either local vector that points into the freed block is
 * set to NULL; 0x7D27A0 copes with that (dynamic_cast of NULL is NULL, the delete pass skips NULL).
 * The same entries freed while still in the global vector, or listed there twice, are logged. */
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

/* duplicates in the deferred vector, looked for when it has grown since the last look */
static void defer_dups(uint32_t esp)
{
    uint32_t b, e;
    if (!defer_bounds(&b, &e) || e == g_defer_seen_end)
        return;
    g_defer_seen_end = e;
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
    /* The actor being deleted right now loses its own slot as well (harmless, the pass has read
     * it). An entry the drain has not reached yet is the news: one in the other vector, or one in
     * this vector outside the delete pass. (A later one in this vector during the delete pass
     * can't be told from the actor's own slot here; it is dropped all the same.) */
    static int verbose = -1; /* XIVLEGACY_ACTORLOG=1: every entry dropped, the actor's own slot too */
    if (verbose < 0)
        verbose = getenv("XIVLEGACY_ACTORLOG") && *getenv("XIVLEGACY_ACTORLOG") == '1';
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

void xcl_note_cs(uint32_t esp)
{
    uint32_t b, e;
    if (!g_rdata_hi)
        return;
    if (defer_bounds(&b, &e) && e > b)
        g_drain_maybe = 1, g_drain_misses = 0; /* filled: the next drain clears it, then frees */
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

void xcl_note_alloc(uint32_t esp)
{
    uint32_t b, e;
    defer_dups(esp);
    if (defer_bounds(&b, &e) && e > b)
        g_drain_maybe = 1, g_drain_misses = 0;
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
    out(fd, "[crashlog] %u game object frees recorded, %u game subpackets seen\n", g_free_n, g_pkt_n);
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
