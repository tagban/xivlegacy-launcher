/* Battle messages in a tab of the main log window instead of a window of their own.
 *
 * 1.23b already has this: each of its two log windows has tab slots, and Config > Log keeps per slot
 * a title, a message filter and an on/off switch (user config 6; slot (window - 1) * 4 + tab - 1).
 * The game's defaults, set by DesktopWidget:initConfigLogData in the client script
 * widget/desktopwidget_connector, turn on tab 1 of each window: window 1 tab 1 shows chat, window 2
 * tab 1 battle messages, and window 1 tab 2 already has the battle filter but is off. Two changes:
 *  - New characters: as the game reads that script, the test "tab == 1" in initConfigLogData becomes
 *    "window == 1" (one instruction's B register), so window 1 gets both tabs and window 2 none; the
 *    log widget hides a window with no tab on. The instruction is found by reading the player's own
 *    script (Lua 5.1 bytecode XOR 0x73 after a 13-byte header), not carried by XIVLegacy.
 *  - Characters made before: their saved settings (Documents\My Games\FINAL FANTASY XIV\user\<id>\game)
 *    are switched the same way once, if they still have the old layout, and marked so a later choice
 *    in Config > Log stands. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_logtabs.h"

#define SCRIPT_HEADER 13 /* "rle\x0c", 2 words, 1 byte: the bytecode follows, XOR 0x73 */
#define MARKER "xivlegacy-battle-tab"

/* The client's script files are named with letters and digits mirrored ("abc..z0..9" read backwards). */
static char mirror(char c)
{
    if (c >= 'a' && c <= 'z')
        return c - 'a' < 10 ? (char)('9' - (c - 'a')) : (char)('z' - (c - 'a' - 10));
    if (c >= '0' && c <= '9')
        return (char)('j' - (c - '0'));
    return c;
}

/* --- reading a Lua 5.1 chunk --------------------------------------------------------------------- */
enum { OP_LOADK = 1, OP_LOADBOOL = 2, OP_SELF = 11, OP_EQ = 23, OP_CLOSURE = 36 };
#define OP(i) ((i) & 0x3F)
#define ARG_A(i) (((i) >> 6) & 0xFF)
#define ARG_B(i) (((i) >> 23) & 0x1FF)
#define ARG_C(i) (((i) >> 14) & 0x1FF)
#define ARG_BX(i) ((i) >> 14)

typedef struct LuaK
{
    uint8_t type; /* 0 nil, 1 boolean, 3 number, 4 string */
    size_t at;    /* the number's 8 bytes, or the string's bytes (len counts its NUL) */
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

/* The method a chunk's main function defines as  Class.name = function ...  (LOADK name, CLOSURE). */
static const LuaFn* method(const uint8_t* d, const LuaFn* main, const char* name)
{
    for (uint32_t pc = 0; pc < main->ncode; ++pc)
    {
        uint32_t i = code_at(d, main, pc);
        if (OP(i) != OP_LOADK || !k_is_string(d, main, ARG_BX(i), name))
            continue;
        for (uint32_t j = pc + 1; j < main->ncode && j <= pc + 3; ++j)
        {
            uint32_t c = code_at(d, main, j);
            if (OP(c) == OP_CLOSURE)
                return ARG_BX(c) < main->nprotos ? &main->protos[ARG_BX(c)] : NULL;
        }
    }
    return NULL;
}

/* --- the script patch ----------------------------------------------------------------------------- */
static struct
{
    int state; /* 0 not looked at, 1 found, -1 not this script / not found */
    char path[1400];
    int64_t at; /* file offset of the instruction */
    uint8_t bytes[4];
} g_patch;

static void* g_open[8]; /* host files of the script open now */

/* initConfigLogData(self, window, tab):  enable = false; if tab == 1 then enable = true end;
 * self:setConfigLogEnable(window, tab, enable). The test reads register 2 (tab); point it at 1. */
static void find_patch(const char* host_path)
{
    g_patch.state = -1;
    size_t n = 0;
    uint8_t* d = plat_read_file(host_path, &n);
    if (!d || n < SCRIPT_HEADER + 12 || memcmp(d, "rle", 3))
    {
        free(d);
        rt_log("[logtabs] cannot read %s: battle messages keep their own window\n", host_path);
        return;
    }
    for (size_t i = SCRIPT_HEADER; i < n; ++i)
        d[i] ^= 0x73;
    static const uint8_t LUA51[12] = { 0x1B, 'L', 'u', 'a', 0x51, 0, 1, 4, 4, 4, 8, 0 };
    Rd r = { d + SCRIPT_HEADER, n - SCRIPT_HEADER, sizeof LUA51, 0 };
    LuaFn main;
    const uint8_t* code = d + SCRIPT_HEADER;
    int found = 0;
    if (!memcmp(r.p, LUA51, sizeof LUA51) && fn_parse(&r, &main, 0))
    {
        const LuaFn* f = method(code, &main, "initConfigLogData");
        for (uint32_t pc = 1; f && pc + 3 < f->ncode; ++pc)
        {
            uint32_t i = code_at(code, f, pc);
            if (OP(i) != OP_EQ || ARG_A(i) != 0 || ARG_B(i) != 2 || ARG_C(i) < 256 ||
                !k_is_number(code, f, ARG_C(i) - 256, 1.0) || OP(code_at(code, f, pc - 1)) != OP_LOADBOOL)
                continue;
            int sets = 0;
            for (uint32_t j = pc + 1; j < f->ncode && j <= pc + 4; ++j)
            {
                uint32_t s = code_at(code, f, j);
                sets |= OP(s) == OP_SELF && ARG_C(s) >= 256 && k_is_string(code, f, ARG_C(s) - 256, "setConfigLogEnable");
            }
            if (!sets)
                continue;
            uint32_t patched = (i & ~(0x1FFu << 23)) | 1u << 23;
            g_patch.at = SCRIPT_HEADER + (int64_t)(f->code_at + 4 * (size_t)pc);
            for (int b = 0; b < 4; ++b)
                g_patch.bytes[b] = (uint8_t)(patched >> (8 * b)) ^ 0x73;
            found++;
        }
        fn_free(&main);
    }
    free(d);
    if (found == 1)
    {
        g_patch.state = 1;
        rt_log("[logtabs] new characters: battle messages in a tab of the main log window\n");
    }
    else
        rt_log("[logtabs] initConfigLogData not as expected (%d matches): battle messages keep their own window\n", found);
}

static int ends_with_path(const char* s, const char* tail)
{
    size_t n = strlen(s), m = strlen(tail);
    if (n < m)
        return 0;
    s += n - m;
    for (size_t i = 0; i < m; ++i)
    {
        char a = s[i] == '\\' ? '/' : s[i], b = tail[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (a != b)
            return 0;
    }
    return 1;
}

void xiv_logtabs_opened(const char* guest_path, const char* host_path, void* file)
{
    if (!g_patch.state)
    {
        /* client/script/widget/desktopwidget_connector.le.lpb, by its mirrored name */
        static char tail[96];
        if (!tail[0])
        {
            const char* name = "widget/desktopwidget_connector";
            char* o = tail + sprintf(tail, "client/script/");
            for (const char* c = name; *c; ++c)
                *o++ = mirror(*c);
            strcpy(o, ".le.lpb");
        }
        if (!ends_with_path(guest_path, tail))
            return;
        snprintf(g_patch.path, sizeof g_patch.path, "%s", host_path);
        find_patch(host_path);
    }
    else if (strcmp(host_path, g_patch.path))
        return;
    if (g_patch.state > 0)
        for (int i = 0; i < 8; ++i)
            if (!g_open[i])
            {
                g_open[i] = file;
                break;
            }
}

void xiv_logtabs_read(void* file, int64_t at, uint8_t* buf, int64_t n)
{
    if (!file || g_patch.state <= 0 || n <= 0)
        return;
    int open = 0;
    for (int i = 0; i < 8; ++i)
        open |= g_open[i] == file;
    if (!open)
        return;
    if (at < 0)
        at = plat_file_seek((PlatFile*)file, 0, 1) - n;
    for (int b = 0; b < 4; ++b)
        if (g_patch.at + b >= at && g_patch.at + b < at + n)
            buf[g_patch.at + b - at] = g_patch.bytes[b];
}

void xiv_logtabs_closed(void* file)
{
    for (int i = 0; i < 8; ++i)
        if (g_open[i] == file)
            g_open[i] = NULL;
}

/* --- characters made before ---------------------------------------------------------------------- *
 * user\<id>\game: one 0xFF byte, then text XOR 0x73, a line per setting: "name,\tmin,\tmax,\tvalue\n",
 * or "name,\tvalue\n" for strings. The slots' settings are numbered slot + 1 and, in the file, named
 * one off: log_enableN the switch, log_titleN the filter ("/68/74/..."; "@" none). */

/* the value of a setting: its text after the last tab, up to the newline */
static char* setting(char* text, const char* name, size_t* len)
{
    char key[32];
    snprintf(key, sizeof key, "\n%s,\t", name);
    char* p = strstr(text, key);
    if (!p)
        return NULL;
    char* end = strchr(p + 1, '\n');
    if (!end)
        return NULL;
    char* v = p + strlen(key);
    for (char* t = v; t < end; ++t)
        if (*t == '\t')
            v = t + 1;
    *len = (size_t)(end - v);
    return v;
}

static void migrate_one(const char* dir)
{
    char path[1400], marker[1400];
    snprintf(path, sizeof path, "%s/game", dir);
    snprintf(marker, sizeof marker, "%s/" MARKER, dir);
    PlatStat st;
    if (!plat_stat(path, &st) || plat_stat(marker, &st))
        return;
    size_t n = 0;
    uint8_t* d = plat_read_file(path, &n);
    if (!d || n < 2)
    {
        free(d);
        return;
    }
    char* text = (char*)malloc(n + 1);
    for (size_t i = 0; i < n; ++i)
        text[i] = (char)(d[i] ^ 0x73);
    text[n] = 0;
    text[0] = '\n'; /* the 0xFF byte: lets the first setting be found like the rest */
    size_t l2 = 0, l5 = 0, l6 = 0, lf = 0;
    char* tab2 = setting(text, "log_enable2", &l2);        /* window 1 tab 2 */
    char* window2 = setting(text, "log_enable5", &l5);     /* window 2 tab 1 */
    char* window2b = setting(text, "log_enable6", &l6);    /* window 2 tab 2 */
    const char* filter2 = setting(text, "log_title2", &lf); /* window 1 tab 2's filter */
    int old_layout = tab2 && l2 == 1 && *tab2 == '0' && window2 && l5 == 1 && *window2 == '1' &&
                     (!window2b || (l6 == 1 && *window2b == '0')) && filter2 && lf > 1 && *filter2 == '/';
    int ok = 1;
    if (old_layout)
    {
        *tab2 = '1';
        *window2 = '0';
        for (size_t i = 1; i < n; ++i)
            d[i] = (uint8_t)text[i] ^ 0x73;
        PlatFile* f = plat_file_open(path, PLAT_WRITE | PLAT_TRUNCATE);
        ok = f && plat_file_write(f, d, (uint32_t)n) == (int64_t)n;
        if (f)
            plat_file_close(f);
        rt_log("[logtabs] %s: battle messages %s\n", path, ok ? "moved to a tab of the main log window" : "not moved (cannot write)");
    }
    if (ok)
    {
        PlatFile* m = plat_file_open(marker, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE);
        if (m)
            plat_file_close(m);
    }
    free(text);
    free(d);
}

void xiv_logtabs_migrate(const char* user_dir)
{
    char root[1200];
    snprintf(root, sizeof root, "%s/Documents/My Games/FINAL FANTASY XIV/user", user_dir);
    PlatDir* dir = plat_dir_open(root);
    if (!dir)
        return;
    for (const char* e; (e = plat_dir_next(dir));)
    {
        if (strlen(e) != 8 || strspn(e, "0123456789ABCDEFabcdef") != 8)
            continue;
        char d[1400];
        snprintf(d, sizeof d, "%s/%s", root, e);
        migrate_one(d);
    }
    plat_dir_close(dir);
}
