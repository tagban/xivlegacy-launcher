/* The Journal's Map button opens a map even for a quest with no marker.
 *
 * In 1.23b a quest's Map button (widget/journaldetailwidget) opens MapNavigationWidget in its quest
 * mode (1). That mode draws nothing on its own: it asks the server for the quest's markers
 * (RequestQuestJournalCommand, a 'qtmap' request), and the answer (DesktopWidget's 'requestedData'
 * handler) sets them and shows the first. When the answer carries no marker, the window stays open
 * with no map in it, only its frame and an empty Map Menu. Retail gave every quest step a marker; the
 * server's quest scripts don't have one for every quest, so that empty window is what players saw.
 *
 * The plain map (Main Menu > Map, mode 0) is centered on the player by MapNavigationWidget:init,
 * which calls self:setDisplayLocation(-1, -1) for mode 0 only. Here quest mode does that too: in
 * init, the jump that ends mode 1's branch (to the shared end of the mode tests) is pointed at mode
 * 0's setDisplayLocation call, which is followed by its own jump to that same end. So the map opens
 * around the player at once, and a marker the server sends still moves it to the quest's place (the
 * marker answer comes after init). One instruction changes. It is found by reading the player's own
 * script (Lua 5.1 bytecode XOR 0x73 after a 13-byte header) and checked against the instructions
 * around it, so XIVLegacy carries none of the game's code; a script that doesn't match is left as it
 * is. The changed file goes to a cache file that the game opens instead.
 * XIVLEGACY_JOURNAL_MAP=0 leaves the script as it is. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_journalmap.h"

#define SCRIPT_HEADER 13 /* "rle\x0c", 2 words, 1 byte: the bytecode follows, XOR 0x73 */
#define KEY 0x73
/* widget/mapnavigationwidget, its name mirrored as the client names its scripts */
#define MAP_SCRIPT "script\\n1635q\\x9uw9o139q1vwn1635q.le.lpb"

static int enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("XIVLEGACY_JOURNAL_MAP");
        on = !(e && *e == '0');
    }
    return on;
}

/* --- reading a Lua 5.1 chunk --------------------------------------------------------------------- */
enum { OP_LOADK = 1, OP_SELF = 11, OP_JMP = 22, OP_CALL = 28 };
#define OP(i) ((i) & 0x3F)
#define ARG_A(i) (((i) >> 6) & 0xFF)
#define ARG_B(i) (((i) >> 23) & 0x1FF)
#define ARG_C(i) (((i) >> 14) & 0x1FF)
#define ARG_BX(i) ((i) >> 14)
#define ARG_SBX(i) ((int)ARG_BX(i) - 131071)

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

static int k_is_number(const uint8_t* d, const LuaFn* f, uint32_t i, double v)
{
    double x;
    if (i >= f->nk || f->k[i].type != 3)
        return 0;
    memcpy(&x, d + f->k[i].at, 8);
    return x == v;
}

/* SELF r, 0:'name' (a call on self) */
static int self_call(const uint8_t* d, const LuaFn* f, uint32_t pc, const char* name)
{
    uint32_t s = pc < f->ncode ? code_at(d, f, pc) : 0;
    return pc < f->ncode && OP(s) == OP_SELF && ARG_B(s) == 0 && ARG_C(s) >= 256 && k_is_string(d, f, ARG_C(s) - 256, name);
}

/* the jump's destination */
static uint32_t jump_to(uint32_t pc, uint32_t i) { return (uint32_t)((int)pc + 1 + ARG_SBX(i)); }

/* In init(self, mode, questIndex, journalID):
 *   mode 0:  hide Label_MessageDialog; self:setDisplayLocation(-1, -1)          ; JMP end
 *   mode 1:  self:setHelpParameter('StackPanel_AreaList', 0); hide Label_MessageDialog ; JMP end
 * The mode 1 "JMP end" goes to the setDisplayLocation call, which centers the map on the player. */
static int find(const uint8_t* d, const LuaFn* f, size_t* at, uint32_t* patched)
{
    int found = 0;
    for (uint32_t i = 0; i < f->nprotos; ++i)
        found += find(d, &f->protos[i], at, patched);
    uint32_t loc = UINT32_MAX;
    for (uint32_t pc = 0; pc + 4 < f->ncode && loc == UINT32_MAX; ++pc)
    {
        uint32_t a = code_at(d, f, pc + 1), b = code_at(d, f, pc + 2);
        if (self_call(d, f, pc, "setDisplayLocation") && OP(a) == OP_LOADK && k_is_number(d, f, ARG_BX(a), -1.0) &&
            OP(b) == OP_LOADK && k_is_number(d, f, ARG_BX(b), -1.0) && OP(code_at(d, f, pc + 3)) == OP_CALL &&
            OP(code_at(d, f, pc + 4)) == OP_JMP)
            loc = pc;
    }
    if (loc == UINT32_MAX)
        return found;
    uint32_t end = jump_to(loc + 4, code_at(d, f, loc + 4));
    for (uint32_t pc = 0; pc + 8 < f->ncode; ++pc)
    {
        uint32_t h = code_at(d, f, pc + 1), j = code_at(d, f, pc + 8);
        if (self_call(d, f, pc, "setHelpParameter") && OP(h) == OP_LOADK && k_is_string(d, f, ARG_BX(h), "StackPanel_AreaList") &&
            OP(code_at(d, f, pc + 3)) == OP_CALL && self_call(d, f, pc + 4, "setVisibility") &&
            OP(code_at(d, f, pc + 7)) == OP_CALL && OP(j) == OP_JMP && jump_to(pc + 8, j) == end)
        {
            int sbx = (int)loc - (int)(pc + 9);
            *patched = (uint32_t)OP_JMP | (uint32_t)(sbx + 131071) << 14;
            *at = f->code_at + 4 * (size_t)(pc + 8);
            found++;
        }
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

int xiv_journalmap_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n)
{
    if (!enabled() || !ends_with(guest_path, MAP_SCRIPT))
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
        uint32_t patched = 0;
        if (!memcmp(chunk, LUA51, sizeof LUA51) && fn_parse(&r, &main, 0))
        {
            found = find(chunk, &main, &at, &patched);
            fn_free(&main);
        }
        free(chunk);
        if (found == 1)
        {
            for (int b = 0; b < 4; ++b)
                file[SCRIPT_HEADER + at + (size_t)b] = (uint8_t)(patched >> (8 * b)) ^ KEY;
            const char* tmp = getenv("TMPDIR");
            char dir[1024];
            snprintf(dir, sizeof dir, "%s/xivlegacy-scripts", tmp && *tmp ? tmp : "/tmp");
            plat_mkdir(dir);
            snprintf(out, out_n, "%s/mapnavigation-%d.le.lpb", dir, (int)getpid());
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
        rt_log("[journalmap] map window: a quest's Map button opens the map around the player, then its marker\n");
    else
        rt_log("[journalmap] %s: not the map window script this expects (%d matches); left as it is\n", guest_path, found);
    return ok;
}
