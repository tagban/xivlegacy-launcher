/* The party list shows NPC party members' HP and MP numbers, not "???".
 *
 * In 1.23b the party list (widget/partyparameterwidget) draws each member's HP and MP from what the
 * server sends, but its setParameter, on an HP update, calls
 *     self:changeUnknownParameter(index, actor:isPlayer())
 * and changeUnknownParameter(index, false) hides the HP/MP number labels and shows the "???" labels
 * instead. So a party NPC (the opening battle's helpers, other allies the server adds to a party)
 * got moving bars but "???" for its numbers. The server sends real HP and MP for them, so the
 * numbers can be shown like a player's (tagban/xivlegacy-server#346).
 *
 * Here that call passes true instead of actor:isPlayer(). Three instructions change:
 *     SELF  A+3, actor, 'isPlayer'   ->  LOADBOOL A+3, true
 *     CALL  A+3, 1 arg, all results  ->  LOADBOOL A+3, true   (the same again, in place of the call)
 *     CALL  A, args up to top, 0     ->  CALL A, 3 args, 0   (self, index, true)
 * The rows with no actor, or with no HP/MP sent yet, still show "???" (setParameter's own earlier
 * branches), and an empty row is still reset to "???". The instructions are found by reading the
 * player's own script (Lua 5.1 bytecode XOR 0x73 after a 13-byte header) and checked against the
 * instructions around them, so XIVLegacy carries none of the game's code; a script that doesn't
 * match is left as it is. The changed file goes to a cache file that the game opens instead.
 * XIVLEGACY_PARTY_NUMBERS=0 leaves the script as it is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_partynpc.h"

#define SCRIPT_HEADER 13 /* "rle\x0c", 2 words, 1 byte: the bytecode follows, XOR 0x73 */
#define KEY 0x73
/* widget/partyparameterwidget, its name mirrored as the client names its scripts */
#define PARTY_SCRIPT "script\\n1635q\\u9sqlu9s9x5q5sn1635q.le.lpb"

static int enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("XIVLEGACY_PARTY_NUMBERS");
        on = !(e && *e == '0');
    }
    return on;
}

/* --- reading a Lua 5.1 chunk --------------------------------------------------------------------- */
enum { OP_MOVE = 0, OP_LOADBOOL = 2, OP_SELF = 11, OP_CALL = 28 };
#define OP(i) ((i) & 0x3F)
#define ARG_A(i) (((i) >> 6) & 0xFF)
#define ARG_B(i) (((i) >> 23) & 0x1FF)
#define ARG_C(i) (((i) >> 14) & 0x1FF)

typedef struct LuaK
{
    uint8_t type; /* 0 nil, 1 boolean, 3 number, 4 string */
    size_t at;    /* the boolean's byte, the number's 8 bytes, or the string's bytes (len counts its NUL) */
    uint32_t len;
} LuaK;

typedef struct LuaFn
{
    size_t code_at;
    uint32_t ncode, nk, nprotos;
    LuaK* k;
    struct LuaFn* protos;
} LuaFn;

typedef struct Rd
{
    const uint8_t* p;
    size_t n, at;
    int bad;
} Rd;

static uint32_t rd_u32(Rd* r)
{
    if (r->at + 4 > r->n)
    {
        r->bad = 1;
        return 0;
    }
    const uint8_t* p = r->p + r->at;
    r->at += 4;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint8_t rd_u8(Rd* r)
{
    if (r->at + 1 > r->n)
    {
        r->bad = 1;
        return 0;
    }
    return r->p[r->at++];
}

static void rd_skip(Rd* r, size_t k)
{
    if (k > r->n - r->at)
        r->bad = 1;
    else
        r->at += k;
}

static void rd_str(Rd* r) { rd_skip(r, rd_u32(r)); }

static void fn_free(LuaFn* f)
{
    for (uint32_t i = 0; f->protos && i < f->nprotos; ++i)
        fn_free(&f->protos[i]);
    free(f->protos);
    free(f->k);
}

static int fn_parse(Rd* r, LuaFn* f, int depth)
{
    memset(f, 0, sizeof *f);
    if (depth > 64)
        return 0;
    rd_str(r);         /* source */
    rd_skip(r, 4 + 4); /* line defined, last line defined */
    rd_skip(r, 4);     /* upvalues, parameters, vararg, max stack */
    f->ncode = rd_u32(r);
    f->code_at = r->at;
    rd_skip(r, (size_t)f->ncode * 4);
    f->nk = rd_u32(r);
    if (r->bad || f->nk > r->n)
        return 0;
    f->k = (LuaK*)calloc(f->nk ? f->nk : 1, sizeof *f->k);
    for (uint32_t i = 0; i < f->nk && !r->bad; ++i)
    {
        LuaK* k = &f->k[i];
        k->type = rd_u8(r);
        k->at = r->at;
        switch (k->type)
        {
        case 0: break;
        case 1: rd_skip(r, 1); break;
        case 3: rd_skip(r, 8); break;
        case 4:
            k->len = rd_u32(r);
            k->at = r->at;
            rd_skip(r, k->len);
            break;
        default: r->bad = 1;
        }
    }
    f->nprotos = rd_u32(r);
    if (r->bad || f->nprotos > r->n)
        return 0;
    f->protos = (LuaFn*)calloc(f->nprotos ? f->nprotos : 1, sizeof *f->protos);
    for (uint32_t i = 0; i < f->nprotos; ++i)
        if (!fn_parse(r, &f->protos[i], depth + 1))
            return 0;
    rd_skip(r, (size_t)rd_u32(r) * 4); /* line info */
    for (uint32_t i = rd_u32(r); i && !r->bad; --i)
    {
        rd_str(r); /* local name, start pc, end pc */
        rd_skip(r, 8);
    }
    for (uint32_t i = rd_u32(r); i && !r->bad; --i)
        rd_str(r); /* upvalue names */
    return !r->bad;
}

static uint32_t code_at(const uint8_t* d, const LuaFn* f, uint32_t pc)
{
    const uint8_t* p = d + f->code_at + 4 * (size_t)pc;
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int k_is_string(const uint8_t* d, const LuaFn* f, uint32_t i, const char* s)
{
    return i < f->nk && f->k[i].type == 4 && f->k[i].len == strlen(s) + 1 && !memcmp(d + f->k[i].at, s, f->k[i].len);
}

/* SELF r, 0:'name' (a call on self) */
static int self_call(const uint8_t* d, const LuaFn* f, uint32_t pc, const char* name)
{
    uint32_t s = pc < f->ncode ? code_at(d, f, pc) : 0;
    return pc < f->ncode && OP(s) == OP_SELF && ARG_B(s) == 0 && ARG_C(s) >= 256 && k_is_string(d, f, ARG_C(s) - 256, name);
}

static uint32_t enc_abc(uint32_t op, uint32_t a, uint32_t b, uint32_t c)
{
    return op | a << 6 | c << 14 | b << 23;
}

/* In setParameter(self, index, kind), kind 1 (HP):
 *   SELF A, r0, 'changeUnknownParameter'; MOVE A+2, r1 (index);
 *   SELF A+3, rX (the member's actor), 'isPlayer'; CALL A+3, 2, 0 (all results); CALL A, 0, 1 */
static int find(const uint8_t* d, const LuaFn* f, size_t* at, uint32_t patched[3])
{
    int found = 0;
    for (uint32_t i = 0; i < f->nprotos; ++i)
        found += find(d, &f->protos[i], at, patched);
    for (uint32_t pc = 0; pc + 4 < f->ncode; ++pc)
    {
        uint32_t s = code_at(d, f, pc), m = code_at(d, f, pc + 1), p = code_at(d, f, pc + 2);
        uint32_t c1 = code_at(d, f, pc + 3), c2 = code_at(d, f, pc + 4);
        uint32_t a = ARG_A(s);
        if (!self_call(d, f, pc, "changeUnknownParameter") || a + 3 > 255)
            continue;
        if (OP(m) != OP_MOVE || ARG_A(m) != a + 2 || ARG_B(m) != 1)
            continue;
        if (OP(p) != OP_SELF || ARG_A(p) != a + 3 || ARG_C(p) < 256 || !k_is_string(d, f, ARG_C(p) - 256, "isPlayer"))
            continue;
        if (OP(c1) != OP_CALL || ARG_A(c1) != a + 3 || ARG_B(c1) != 2 || ARG_C(c1) != 0)
            continue;
        if (OP(c2) != OP_CALL || ARG_A(c2) != a || ARG_B(c2) != 0 || ARG_C(c2) != 1)
            continue;
        patched[0] = enc_abc(OP_LOADBOOL, a + 3, 1, 0);
        patched[1] = enc_abc(OP_LOADBOOL, a + 3, 1, 0);
        patched[2] = enc_abc(OP_CALL, a, 4, 1);
        *at = f->code_at + 4 * (size_t)(pc + 2);
        found++;
    }
    return found;
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

int xiv_partynpc_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n)
{
    if (!enabled() || !ends_with(guest_path, PARTY_SCRIPT))
        return 0;
    size_t n = 0;
    uint8_t* file = plat_read_file(host_path, &n);
    if (!file)
        return 0;
    static const uint8_t LUA51[12] = { 0x1B, 'L', 'u', 'a', 0x51, 0, 1, 4, 4, 4, 8, 0 };
    int ok = 0, found = 0;
    if (n > SCRIPT_HEADER + sizeof LUA51 && !memcmp(file, "rle\x0c", 4))
    {
        size_t cn = n - SCRIPT_HEADER;
        uint8_t* chunk = (uint8_t*)malloc(cn);
        for (size_t i = 0; i < cn; ++i)
            chunk[i] = file[SCRIPT_HEADER + i] ^ KEY;
        Rd r = { chunk, cn, sizeof LUA51, 0 };
        LuaFn main;
        size_t at = 0;
        uint32_t patched[3] = { 0, 0, 0 };
        if (!memcmp(chunk, LUA51, sizeof LUA51) && fn_parse(&r, &main, 0))
        {
            found = find(chunk, &main, &at, patched);
            fn_free(&main);
        }
        free(chunk);
        if (found == 1)
        {
            for (int w = 0; w < 3; ++w)
                for (int b = 0; b < 4; ++b)
                    file[SCRIPT_HEADER + at + 4 * (size_t)w + (size_t)b] = (uint8_t)(patched[w] >> (8 * b)) ^ KEY;
            const char* tmp = getenv("TMPDIR");
            char dir[1024];
            snprintf(dir, sizeof dir, "%s/xivlegacy-scripts", tmp && *tmp ? tmp : "/tmp");
            plat_mkdir(dir);
            snprintf(out, out_n, "%s/partyparameter-%d.le.lpb", dir, (int)getpid());
            PlatFile* f = plat_file_open(out, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE);
            if (f)
            {
                ok = plat_file_write(f, file, (uint32_t)n) == (int64_t)n;
                plat_file_close(f);
            }
        }
    }
    free(file);
    if (ok)
        rt_log("[partynpc] party list: NPC members show their HP and MP numbers\n");
    else
        rt_log("[partynpc] %s: not the party list script this expects (%d matches); left as it is\n", guest_path, found);
    return ok;
}
