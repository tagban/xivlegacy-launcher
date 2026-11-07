/* A countdown for every timed duty: the raid timer widget, opened by the server for any content.
 *
 * 1.23b draws a duty's time left only for raids: InstanceRaidBaseClass (client script
 * director/instanceraid) opens the desktop's RaidDungeonExecutionWidget with the content's finish
 * time. Quest duties (an escort, a quest battle) and their directors have no countdown of their own,
 * so the server's limit was only text in the log. Here the player's script (chara/player,
 * PlayerBaseClass) learns one more data packet, as the game reads it:
 *
 *   'xlContentTimer', contentId, finishTime   open the timer (finishTime in server seconds)
 *   'xlContentTimer', 0, 0                    close it
 *
 * The server sends it with GenericData (0x0133, Lua player:SendDataPacket). A client without this
 * patch ignores it: the player's _onReceiveDataPacket only acts on 'requestedData', 'attention' and
 * numbers. contentId picks the title the widget shows (text 10051 of the content list).
 *
 * How: the script is Lua 5.1 bytecode (XOR 0x73 after a 13-byte header). Its main function gets
 * one more nested function, called just before it returns, which keeps the class's own
 * _onReceiveDataPacket as _xlOrigReceive and puts a wrapper in its place. The wrapper and the
 * installer are written here, instruction by instruction; the rest of the file is the player's own,
 * read from their install. The changed file goes to a cache file that the game opens instead.
 * XIVLEGACY_CONTENT_TIMER=0 leaves the script as it is.
 *
 * The desktop builds one widget at a time: while another is being built (the duty's opening
 * banner, the zone-in desktop) both openWidget and openWidgetLocal refuse, and the packet is lost.
 * The server sends it a few seconds after a duty's opening messages. openWidget also refuses when
 * the desktop mode has the widget's kind turned off; openWidgetLocal skips that check, so the
 * wrapper tries it second. A script error here would bring up the client's error dialog, so the
 * wrapper never raises one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_ctimer.h"

#define SCRIPT_HEADER 13 /* "rle\x0c", a word, the payload's size, 1 byte; then the bytecode XOR 0x73 */
#define KEY 0x73
/* chara/player/PlayerBaseClass, its name mirrored as the client names its scripts */
#define PLAYER_SCRIPT "script\\729s9\\uy9l5s\\uy9l5s89r57y9rr.le.lpb"

static int enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("XIVLEGACY_CONTENT_TIMER");
        on = !(e && *e == '0');
    }
    return on;
}

/* --- Lua 5.1 instructions ------------------------------------------------------------------------ */
enum
{
    OP_MOVE = 0, OP_LOADK = 1, OP_LOADBOOL = 2, OP_LOADNIL = 3, OP_GETGLOBAL = 5, OP_GETTABLE = 6,
    OP_SETTABLE = 9, OP_SELF = 11, OP_JMP = 22, OP_EQ = 23, OP_LT = 24, OP_TEST = 26, OP_CALL = 28,
    OP_TAILCALL = 29, OP_RETURN = 30, OP_CLOSURE = 36, OP_VARARG = 37
};
#define RK(k) (256 + (k)) /* a constant as a B or C operand */
static uint32_t abc(int op, int a, int b, int c) { return (uint32_t)op | (uint32_t)a << 6 | (uint32_t)c << 14 | (uint32_t)b << 23; }
static uint32_t abx(int op, int a, int bx) { return (uint32_t)op | (uint32_t)a << 6 | (uint32_t)bx << 14; }
static uint32_t asbx(int op, int a, int sbx) { return abx(op, a, sbx + 131071); }

/* --- writing a function -------------------------------------------------------------------------- */
typedef struct Buf
{
    uint8_t* p;
    size_t n, cap;
} Buf;

static void put(Buf* b, const void* p, size_t n)
{
    if (b->n + n > b->cap)
    {
        b->cap = (b->n + n) * 2 + 256;
        b->p = (uint8_t*)realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, p, n);
    b->n += n;
}
static void put32(Buf* b, uint32_t v) { put(b, &v, 4); } /* the chunk is little-endian, as is the host */
static void put8(Buf* b, uint8_t v) { put(b, &v, 1); }

typedef struct K
{
    int is_str;
    const char* s;
    double d;
} K;

/* A function with no source name, upvalues or debug information. */
static void put_function(Buf* b, int params, int vararg, int stack, const uint32_t* code, int ncode, const K* k, int nk,
                         void (*protos)(Buf*), int nprotos)
{
    put32(b, 0); /* source: none */
    put32(b, 0), put32(b, 0);
    put8(b, 0), put8(b, (uint8_t)params), put8(b, (uint8_t)vararg), put8(b, (uint8_t)stack);
    put32(b, (uint32_t)ncode);
    for (int i = 0; i < ncode; ++i)
        put32(b, code[i]);
    put32(b, (uint32_t)nk);
    for (int i = 0; i < nk; ++i)
        if (k[i].is_str)
        {
            put8(b, 4);
            put32(b, (uint32_t)strlen(k[i].s) + 1);
            put(b, k[i].s, strlen(k[i].s) + 1);
        }
        else
        {
            put8(b, 3);
            put(b, &k[i].d, 8);
        }
    put32(b, (uint32_t)nprotos);
    if (protos)
        protos(b);
    put32(b, 0), put32(b, 0), put32(b, 0); /* line info, locals, upvalue names */
}

/* function (self, kind, ...)
 *     if kind == 'xlContentTimer' then
 *         local contentId, finishTime = ...
 *         desktopWidget:closeRaidDungeonExecutionWidget()
 *         if 0 < finishTime then
 *             desktopWidget:openWidget(15, 'RaidDungeonExecutionWidget', nil, nil, true, contentId, finishTime)
 *         end
 *         return
 *     end
 *     return self:_xlOrigReceive(kind, ...)
 * end
 * openWidget, not openRaidDungeonExecutionWidget: that one yields, and a data packet isn't a coroutine
 * (the tutorial widgets open the same way from here). */
static void put_wrapper(Buf* b)
{
    K k[] = {
        { 1, "xlContentTimer", 0 }, { 1, "_xlOrigReceive", 0 }, { 1, "desktopWidget", 0 }, { 0, NULL, 0 },
        { 1, "openWidget", 0 }, { 0, NULL, 15 }, { 1, "RaidDungeonExecutionWidget", 0 },
        { 1, "closeRaidDungeonExecutionWidget", 0 }, { 1, "openWidgetLocal", 0 },
    };
    const uint32_t code[] = {
        abc(OP_EQ, 1, 1, RK(0)),      /* 0  kind == 'xlContentTimer': take the jump */
        asbx(OP_JMP, 0, 5),           /* 1  -> 7 */
        abc(OP_SELF, 2, 0, RK(1)),    /* 2  self:_xlOrigReceive( */
        abc(OP_MOVE, 4, 1, 0),        /* 3  kind, */
        abc(OP_VARARG, 5, 0, 0),      /* 4  ...) */
        abc(OP_TAILCALL, 2, 0, 0),    /* 5 */
        abc(OP_RETURN, 2, 0, 0),      /* 6 */
        abc(OP_VARARG, 2, 3, 0),      /* 7  contentId, finishTime = ... */
        abx(OP_GETGLOBAL, 4, 2),      /* 8 */
        abc(OP_SELF, 4, 4, RK(7)),    /* 9  desktopWidget:closeRaidDungeonExecutionWidget() */
        abc(OP_CALL, 4, 2, 1),        /* 10 */
        abc(OP_LT, 0, RK(3), 3),      /* 11 not (0 < finishTime): take the jump */
        asbx(OP_JMP, 0, 21),          /* 12 -> 34 */
        abx(OP_GETGLOBAL, 4, 2),      /* 13 */
        abc(OP_SELF, 4, 4, RK(4)),    /* 14 ok = desktopWidget:openWidget( */
        abx(OP_LOADK, 6, 5),          /* 15 15, */
        abx(OP_LOADK, 7, 6),          /* 16 'RaidDungeonExecutionWidget', */
        abc(OP_LOADNIL, 8, 9, 0),     /* 17 nil, nil, */
        abc(OP_LOADBOOL, 10, 1, 0),   /* 18 true, */
        abc(OP_MOVE, 11, 2, 0),       /* 19 contentId, */
        abc(OP_MOVE, 12, 3, 0),       /* 20 finishTime) */
        abc(OP_CALL, 4, 9, 2),        /* 21 */
        abc(OP_TEST, 4, 0, 0),        /* 22 not ok: take the jump */
        asbx(OP_JMP, 0, 1),           /* 23 -> 25 */
        abc(OP_RETURN, 0, 1, 0),      /* 24 */
        abx(OP_GETGLOBAL, 4, 2),      /* 25 openWidget refuses outside raid modes: */
        abc(OP_SELF, 4, 4, RK(8)),    /* 26 ok = desktopWidget:openWidgetLocal( */
        abx(OP_LOADK, 6, 6),          /* 27 'RaidDungeonExecutionWidget', */
        abc(OP_LOADNIL, 7, 8, 0),     /* 28 nil, nil, */
        abx(OP_LOADK, 9, 5),          /* 29 15, */
        abc(OP_LOADBOOL, 10, 1, 0),   /* 30 true, */
        abc(OP_MOVE, 11, 2, 0),       /* 31 contentId, */
        abc(OP_MOVE, 12, 3, 0),       /* 32 finishTime) */
        abc(OP_CALL, 4, 9, 1),        /* 33 */
        abc(OP_RETURN, 0, 1, 0),      /* 34 */
    };
    put_function(b, 2, 2 /* VARARG_ISVARARG */, 13, code, (int)(sizeof code / sizeof *code), k, (int)(sizeof k / sizeof *k),
                 NULL, 0);
}

/* function ()
 *     local c = PlayerBaseClass
 *     if c then c._xlOrigReceive = c._onReceiveDataPacket; c._onReceiveDataPacket = <wrapper> end
 * end */
static void put_installer(Buf* b)
{
    static const K k[] = { { 1, "PlayerBaseClass", 0 }, { 1, "_onReceiveDataPacket", 0 }, { 1, "_xlOrigReceive", 0 } };
    const uint32_t code[] = {
        abx(OP_GETGLOBAL, 0, 0),         /* 0 */
        abc(OP_TEST, 0, 0, 0),           /* 1 nil: take the jump */
        asbx(OP_JMP, 0, 4),              /* 2 -> 7 */
        abc(OP_GETTABLE, 1, 0, RK(1)),   /* 3 */
        abc(OP_SETTABLE, 0, RK(2), 1),   /* 4 */
        abx(OP_CLOSURE, 1, 0),           /* 5 */
        abc(OP_SETTABLE, 0, RK(1), 1),   /* 6 */
        abc(OP_RETURN, 0, 1, 0),         /* 7 */
    };
    put_function(b, 0, 0, 2, code, (int)(sizeof code / sizeof *code), k, (int)(sizeof k / sizeof *k), put_wrapper, 1);
}

/* --- reading the chunk --------------------------------------------------------------------------- */
typedef struct Rd
{
    const uint8_t* p;
    size_t n, at;
    int bad;
} Rd;

static uint32_t lrd32(Rd* r)
{
    if (r->at + 4 > r->n)
    {
        r->bad = 1;
        return 0;
    }
    uint32_t v;
    memcpy(&v, r->p + r->at, 4);
    r->at += 4;
    return v;
}
static void skip(Rd* r, size_t n)
{
    if (r->at + n > r->n)
        r->bad = 1, r->at = r->n;
    else
        r->at += n;
}
static void skip_string(Rd* r) { skip(r, lrd32(r)); }

/* Where the main function's parts are, as offsets into the chunk. */
typedef struct Main
{
    size_t ncode_at, code_end, nprotos_at, protos_end, nlines_at, lines_end;
    uint32_t ncode, nprotos, nlines;
} Main;

static void skip_function(Rd* r, Main* m)
{
    skip_string(r);
    skip(r, 8 + 4);
    if (m)
        m->ncode_at = r->at;
    uint32_t ncode = lrd32(r);
    skip(r, 4 * (size_t)ncode);
    if (m)
        m->ncode = ncode, m->code_end = r->at;
    uint32_t nk = lrd32(r);
    for (uint32_t i = 0; i < nk && !r->bad; ++i)
    {
        if (r->at >= r->n)
        {
            r->bad = 1;
            break;
        }
        uint8_t t = r->p[r->at++];
        if (t == 1)
            skip(r, 1);
        else if (t == 3)
            skip(r, 8);
        else if (t == 4)
            skip_string(r);
        else if (t != 0)
            r->bad = 1;
    }
    if (m)
        m->nprotos_at = r->at;
    uint32_t np = lrd32(r);
    for (uint32_t i = 0; i < np && !r->bad; ++i)
        skip_function(r, NULL);
    if (m)
        m->nprotos = np, m->protos_end = r->at, m->nlines_at = r->at;
    uint32_t nl = lrd32(r);
    skip(r, 4 * (size_t)nl);
    if (m)
        m->nlines = nl, m->lines_end = r->at;
    uint32_t nloc = lrd32(r);
    for (uint32_t i = 0; i < nloc && !r->bad; ++i)
        skip_string(r), skip(r, 8);
    uint32_t nup = lrd32(r);
    for (uint32_t i = 0; i < nup && !r->bad; ++i)
        skip_string(r);
}

/* The chunk with the installer added, or NULL if it isn't the chunk this expects. */
static uint8_t* patch_chunk(const uint8_t* c, size_t n, size_t* out_n)
{
    static const uint8_t HEADER[12] = { 0x1b, 'L', 'u', 'a', 0x51, 0, 1, 4, 4, 4, 8, 0 };
    if (n < 12 || memcmp(c, HEADER, 12))
        return NULL;
    Rd r = { c, n, 12, 0 };
    Main m;
    memset(&m, 0, sizeof m);
    skip_function(&r, &m);
    if (r.bad || r.at != n || m.ncode == 0 || (m.nlines && m.nlines != m.ncode))
        return NULL;
    uint32_t last;
    memcpy(&last, c + m.code_end - 4, 4);
    if (last != abc(OP_RETURN, 0, 1, 0))
        return NULL;
    /* the old last line, for the two new instructions' line info */
    uint32_t line = 0;
    if (m.nlines)
        memcpy(&line, c + m.lines_end - 4, 4);

    Buf b = { 0 };
    put(&b, c, m.ncode_at);
    put32(&b, m.ncode + 2);
    put(&b, c + m.ncode_at + 4, m.code_end - 4 - (m.ncode_at + 4));
    put32(&b, abx(OP_CLOSURE, 0, (int)m.nprotos)); /* the installer, made and called last */
    put32(&b, abc(OP_CALL, 0, 1, 1));
    put32(&b, abc(OP_RETURN, 0, 1, 0));
    put(&b, c + m.code_end, m.nprotos_at - m.code_end); /* constants */
    put32(&b, m.nprotos + 1);
    put(&b, c + m.nprotos_at + 4, m.protos_end - (m.nprotos_at + 4));
    put_installer(&b);
    if (m.nlines)
    {
        put32(&b, m.nlines + 2);
        put(&b, c + m.nlines_at + 4, m.lines_end - (m.nlines_at + 4));
        put32(&b, line), put32(&b, line);
    }
    else
        put32(&b, 0);
    put(&b, c + m.lines_end, n - m.lines_end); /* locals, upvalue names */
    *out_n = b.n;
    return b.p;
}

/* --- the script file ----------------------------------------------------------------------------- */
static int ends_with(const char* s, const char* tail)
{
    size_t a = strlen(s), b = strlen(tail);
    if (a < b)
        return 0;
    s += a - b;
    for (size_t i = 0; i < b; ++i)
    {
        int x = (unsigned char)s[i], y = (unsigned char)tail[i];
        if (x == '/')
            x = '\\';
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (x != y)
            return 0;
    }
    return 1;
}

static uint8_t* read_all(const char* path, size_t* n)
{
    PlatFile* f = plat_file_open(path, PLAT_READ);
    if (!f)
        return NULL;
    int64_t size = plat_file_size(f);
    uint8_t* p = size > 0 && size < (64 << 20) ? (uint8_t*)malloc((size_t)size) : NULL;
    if (p && plat_file_read(f, p, (uint32_t)size) != size)
        free(p), p = NULL;
    plat_file_close(f);
    *n = p ? (size_t)size : 0;
    return p;
}

int xiv_ctimer_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n)
{
    if (!enabled() || !ends_with(guest_path, PLAYER_SCRIPT))
        return 0;
    size_t n;
    uint8_t* file = read_all(host_path, &n);
    if (!file)
        return 0;
    int ok = 0;
    if (n > SCRIPT_HEADER && !memcmp(file, "rle\x0c", 4))
    {
        size_t cn = n - SCRIPT_HEADER;
        uint8_t* chunk = (uint8_t*)malloc(cn);
        for (size_t i = 0; i < cn; ++i)
            chunk[i] = file[SCRIPT_HEADER + i] ^ KEY;
        size_t pn;
        uint8_t* patched = patch_chunk(chunk, cn, &pn);
        if (patched)
        {
            uint8_t* o = (uint8_t*)malloc(SCRIPT_HEADER + pn);
            memcpy(o, file, SCRIPT_HEADER);
            uint32_t payload = (uint32_t)pn;
            memcpy(o + 8, &payload, 4);
            for (size_t i = 0; i < pn; ++i)
                o[SCRIPT_HEADER + i] = patched[i] ^ KEY;
            const char* tmp = getenv("TMPDIR");
            char dir[1024];
            snprintf(dir, sizeof dir, "%s/xivlegacy-scripts", tmp && *tmp ? tmp : "/tmp");
            plat_mkdir(dir);
            snprintf(out, out_n, "%s/player-%d.le.lpb", dir, (int)getpid());
            PlatFile* f = plat_file_open(out, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE);
            if (f)
            {
                ok = plat_file_write(f, o, (uint32_t)(SCRIPT_HEADER + pn)) == (int64_t)(SCRIPT_HEADER + pn);
                plat_file_close(f);
            }
            free(o);
            free(patched);
        }
        else
            rt_log("[ctimer] %s: not the player script this expects; left as it is\n", guest_path);
        free(chunk);
    }
    free(file);
    if (ok)
        rt_log("[ctimer] player script: content timer added\n");
    return ok;
}
