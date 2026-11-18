/* Monster Points and the monster vendors' shop (Monster Mode).
 *
 * Monster Points are a currency the server pays only in Monster Mode, spent at three monster vendors.
 * The client knows a currency or a shop list only from its own sheets, so two of them change as the
 * game reads them:
 *
 * - Currency 1000124 (the money package's last unused id: its name is the placeholder "[en]", its icon
 *   already the guild marks') is named "Monster Points" in the money text sheets, data/0B/45/02/D9 and
 *   0B/45/03/36, /93, /F0 (one per language; each sheet is a data file, then its header of id runs
 *   (first id, count), then the start offsets of each id's row, from the first id on, and the end).
 *   A row is a run of strings (u16 length counting the 0xFF before the text and the NUL after it, 0xFF,
 *   the text XOR 0x73) and a few plain bytes between them: name, singular, plural and description
 *   (Japanese: name and description), then an empty string. The new row keeps the plain bytes.
 *
 * - Shop pack 5124 (packs end at 5123) lists MONSTER_SHOP (xiv_monster_points_table.h). The pack sheet
 *   is data/01/03/02/BF (a pair per pack: first and last goods slot), C0 its id runs from pack 0, C1
 *   the end offset of each pack's row. The goods sheet the vendor packs use is 01/03/05/19 (9-byte
 *   slots: item id, a flag byte (1), price), 1A its id runs, 1B the start offsets from its first slot.
 *   The new pack's slots are 2027111-2027131: the game reads only slots inside the sheet's id span
 *   (2001001-2027211) and a header of the same size, so they fill the hole after 2027101-2027110 and
 *   that run grows to take them (slots after the last one, or a run of their own, hang the window).
 *
 * The changed files are written to a cache folder and the game opens them in place of the install's
 * (a sheet grows, so its size changes; rewriting reads in place could not do that). Each file is checked
 * against the layout described here first; a sheet laid out otherwise (another version) is left as it
 * is. XIVLEGACY_MONSTERPOINTS=0 leaves all of them alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_monster_points.h"
#include "xiv_monster_points_table.h" /* MONSTER_SHOP */

#define CURRENCY_ID 1000124u
#define PACK 5124u
#define FIRST_SLOT 2027111u
#define KEY 0x73
#define SLOTS ((uint32_t)(sizeof MONSTER_SHOP / sizeof MONSTER_SHOP[0]))

static const char* const NAME = "Monster Points";
static const char* const SINGULAR = "Monster Point";
static const char* const PLURAL = "Monster Points";
static const char* const DESCRIPTION =
    "Earned only in Monster Mode. Monster vendors in Ul'dah, Gridania and Limsa Lominsa take them for their wares.";

enum { TEXT, PACKS, GOODS };

/* each sheet: its data file (header and offsets follow it) */
static const struct { const char* data; int kind; } SHEETS[] = {
    {"data/0b/45/02/d9.dat", TEXT}, {"data/0b/45/03/36.dat", TEXT}, {"data/0b/45/03/93.dat", TEXT},
    {"data/0b/45/03/f0.dat", TEXT}, {"data/01/03/02/bf.dat", PACKS}, {"data/01/03/05/19.dat", GOODS},
};
#define NSHEETS (sizeof SHEETS / sizeof SHEETS[0])

typedef struct
{
    uint8_t* b;
    size_t n;
} Buf;

/* per sheet: 0 not looked at, 1 patched, -1 left as it is; the cache file of data, header, offsets
 * ("" = the install's own) */
static int g_state[NSHEETS];
static char g_out[NSHEETS][3][1024];

static uint32_t g_currency = CURRENCY_ID;
#define CURRENCY g_currency

static int enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        /* 0 = off; a money item id (10001xx) = name that one instead (to try one) */
        const char* e = getenv("XIVLEGACY_MONSTERPOINTS");
        on = !(e && *e == '0');
        if (e && strtoul(e, NULL, 10) > 1000000)
            g_currency = (uint32_t)strtoul(e, NULL, 10);
    }
    return on;
}

/* the guest path ends with tail (case and slashes aside) */
static int ends_with_path(const char* s, const char* tail)
{
    size_t n = strlen(s), m = strlen(tail);
    if (n < m)
        return 0;
    s += n - m;
    for (size_t i = 0; i < m; ++i)
    {
        char a = s[i] == '\\' ? '/' : s[i];
        if (a >= 'A' && a <= 'Z')
            a += 32;
        if (a != tail[i])
            return 0;
    }
    return 1;
}

/* the tail of the file `delta` after this one in the same folder (".../36.dat" + 1 -> ".../37.dat") */
static int sibling_tail(const char* tail, int delta, char* out, size_t n)
{
    size_t m = strlen(tail);
    if (m < 6 || m >= n)
        return 0;
    unsigned v;
    if (sscanf(tail + m - 6, "%2x", &v) != 1)
        return 0;
    memcpy(out, tail, m + 1);
    char hex[3];
    snprintf(hex, sizeof hex, "%02x", (v + (unsigned)delta) & 0xFF);
    memcpy(out + m - 6, hex, 2);
    return 1;
}

/* the host file `delta` after host_path: the install's names are upper case ("36.DAT") */
static int sibling_host(const char* host_path, int delta, char* out, size_t n)
{
    size_t m = strlen(host_path);
    if (m < 6 || m >= n)
        return 0;
    unsigned v;
    if (sscanf(host_path + m - 6, "%2x", &v) != 1)
        return 0;
    int upper = host_path[m - 3] >= 'A' && host_path[m - 3] <= 'Z';
    memcpy(out, host_path, m + 1);
    char hex[3];
    snprintf(hex, sizeof hex, upper ? "%02X" : "%02x", (v + (unsigned)delta) & 0xFF);
    memcpy(out + m - 6, hex, 2);
    return 1;
}

static uint32_t u32at(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void u32put(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

static void put(Buf* o, const void* p, size_t n)
{
    o->b = (uint8_t*)realloc(o->b, o->n + n);
    memcpy(o->b + o->n, p, n);
    o->n += n;
}

static void put32(Buf* o, uint32_t v)
{
    uint8_t b[4];
    u32put(b, v);
    put(o, b, 4);
}

static void put_string(Buf* o, const char* s)
{
    size_t n = strlen(s);
    uint8_t h[3] = {(uint8_t)(n + 2), (uint8_t)((n + 2) >> 8), 0xFF};
    put(o, h, 3);
    for (size_t i = 0; i <= n; ++i)
    {
        uint8_t c = (uint8_t)s[i] ^ KEY;
        put(o, &c, 1);
    }
}

/* a string field starts at p: u16 length, 0xFF, text, the XORed NUL as its last byte */
static size_t string_at(const uint8_t* p, size_t left)
{
    if (left < 4 || p[2] != 0xFF)
        return 0;
    size_t len = (size_t)p[0] | (size_t)p[1] << 8;
    if (len < 2 || 2 + len > left || p[1 + len] != KEY)
        return 0;
    return 2 + len;
}

/* the currency's row with our strings in place of its own; NULL if it isn't laid out as expected */
static int text_row(const uint8_t* row, size_t n, Buf* out)
{
    size_t at[8], len[8];
    int k = 0;
    for (size_t i = 0; i < n;)
    {
        size_t s = string_at(row + i, n - i);
        if (s)
        {
            if (k == 8)
                return 0;
            at[k] = i, len[k] = s, ++k;
            i += s;
        }
        else
            ++i;
    }
    /* the last string is the empty one; before it four (name, singular, plural, description) or two */
    if ((k != 5 && k != 3) || len[k - 1] != 4 || at[k - 1] + len[k - 1] != n)
        return 0;
    const char* four[] = {NAME, SINGULAR, PLURAL, DESCRIPTION, ""};
    const char* two[] = {NAME, DESCRIPTION, ""};
    const char** with = k == 5 ? four : two;
    size_t from = 0;
    for (int j = 0; j < k; ++j)
    {
        put(out, row + from, at[j] - from);
        put_string(out, with[j]);
        from = at[j] + len[j];
    }
    return 1;
}

typedef struct
{
    uint32_t first, end; /* first id and one past the last */
    uint32_t count;      /* rows */
} Runs;

static int read_runs(const Buf* h, Runs* r)
{
    if (!h->n || h->n % 8 || h->n > 65536)
        return 0;
    r->first = u32at(h->b), r->count = 0, r->end = 0;
    for (size_t i = 0; i < h->n; i += 8)
    {
        uint32_t a = u32at(h->b + i), c = u32at(h->b + i + 4);
        if (a < r->end)
            return 0;
        r->end = a + c, r->count += c;
    }
    return 1;
}

static int has_id(const Buf* h, uint32_t id)
{
    for (size_t i = 0; i < h->n; i += 8)
        if (id >= u32at(h->b + i) && id - u32at(h->b + i) < u32at(h->b + i + 4))
            return 1;
    return 0;
}

/* f[0] data, f[1] header, f[2] offsets: in place, the patched files (unchanged ones freed and NULLed) */
static int patch_text(Buf f[3])
{
    Runs r;
    if (!read_runs(&f[1], &r) || !has_id(&f[1], CURRENCY) || f[2].n % 4 || f[2].n / 4 < (size_t)(r.end - r.first) + 1)
        return 0;
    size_t k = CURRENCY - r.first, nofs = f[2].n / 4;
    uint32_t a = u32at(f[2].b + 4 * k), b = u32at(f[2].b + 4 * k + 4);
    if (u32at(f[2].b + 4 * (nofs - 1)) != f[0].n || a > b || b > f[0].n)
        return 0;
    Buf row = {0}, data = {0};
    if (!text_row(f[0].b + a, b - a, &row))
    {
        free(row.b);
        return 0;
    }
    put(&data, f[0].b, a);
    put(&data, row.b, row.n);
    put(&data, f[0].b + b, f[0].n - b);
    int32_t grow = (int32_t)row.n - (int32_t)(b - a);
    for (size_t j = k + 1; j < nofs; ++j)
        u32put(f[2].b + 4 * j, u32at(f[2].b + 4 * j) + (uint32_t)grow);
    free(row.b);
    free(f[0].b);
    f[0] = data;
    free(f[1].b), f[1].b = NULL;
    return 1;
}

static int patch_packs(Buf f[3])
{
    Runs r;
    /* fixed 8-byte rows; the offsets are each pack's row END, by pack id from the first */
    if (!read_runs(&f[1], &r) || has_id(&f[1], PACK) || r.end > PACK || f[0].n != (size_t)r.count * 8 || f[2].n % 4)
        return 0;
    size_t nofs = f[2].n / 4, k = PACK - r.first;
    if (nofs < (size_t)(r.end - r.first) || u32at(f[2].b + 4 * (nofs - 1)) != f[0].n)
        return 0;
    put32(&f[0], FIRST_SLOT);
    put32(&f[0], FIRST_SLOT + SLOTS - 1);
    /* the header: the last run grows when the pack follows it, else one more run */
    size_t last = f[1].n - 8;
    if (u32at(f[1].b + last) + u32at(f[1].b + last + 4) == PACK)
        u32put(f[1].b + last + 4, u32at(f[1].b + last + 4) + 1);
    else
        put32(&f[1], PACK), put32(&f[1], 1);
    if (nofs < k + 2)
    {
        uint32_t end = (uint32_t)(f[0].n - 8);
        while (nofs < k + 2)
            put32(&f[2], end), ++nofs;
    }
    for (size_t j = k; j < nofs; ++j)
        u32put(f[2].b + 4 * j, (uint32_t)f[0].n);
    return 1;
}

static int patch_goods(Buf f[3])
{
    Runs r;
    /* 9-byte slots; the offsets are each slot's row START, by slot id from the first, then the end.
     * The slots must lie inside the ids the sheet already spans (the game never looks past them) and
     * extend a run that ends where they begin (the header keeps its size; one more run hangs the window). */
    if (!read_runs(&f[1], &r) || FIRST_SLOT <= r.first || FIRST_SLOT + SLOTS > r.end || f[0].n != (size_t)r.count * 9 ||
        f[2].n % 4)
        return 0;
    for (uint32_t i = 0; i < SLOTS; ++i)
        if (has_id(&f[1], FIRST_SLOT + i))
            return 0;
    size_t run = f[1].n;
    for (size_t i = 0; i < f[1].n; i += 8)
        if (u32at(f[1].b + i) + u32at(f[1].b + i + 4) == FIRST_SLOT)
            run = i;
    if (run == f[1].n)
        return 0;
    size_t span = r.end - r.first, nofs = f[2].n / 4, k = FIRST_SLOT - r.first;
    if (nofs < span + 1 || u32at(f[2].b + 4 * span) != f[0].n)
        return 0;
    /* the new rows go where the empty slots are, between the rows before and after them */
    uint32_t at = u32at(f[2].b + 4 * k), grow = 9 * SLOTS;
    Buf data = {0};
    put(&data, f[0].b, at);
    for (uint32_t i = 0; i < SLOTS; ++i)
    {
        uint8_t row[9];
        u32put(row, MONSTER_SHOP[i].item);
        row[4] = 1;
        u32put(row + 5, MONSTER_SHOP[i].price);
        put(&data, row, 9);
    }
    put(&data, f[0].b + at, f[0].n - at);
    free(f[0].b);
    f[0] = data;
    for (size_t j = k; j < nofs; ++j)
        u32put(f[2].b + 4 * j, j < k + SLOTS ? at + 9 * (uint32_t)(j - k) : u32at(f[2].b + 4 * j) + grow);
    /* the header: the run before the slots takes them in */
    u32put(f[1].b + run + 4, u32at(f[1].b + run + 4) + SLOTS);
    return 1;
}

static int write_cache(const char* tail, const Buf* b, char* out, size_t n)
{
    const char* tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = getenv("TEMP");
    char dir[1024], name[64], part[1100];
    snprintf(dir, sizeof dir, "%s/xivlegacy-data", tmp && *tmp ? tmp : "/tmp");
    plat_mkdir(dir);
    /* "data/0b/45/03/36.dat" -> "monsterpoints-0b4503-36.dat" */
    snprintf(name, sizeof name, "monsterpoints-%.2s%.2s%.2s-%s", tail + 5, tail + 8, tail + 11, tail + 14);
    snprintf(out, n, "%s/%s", dir, name);
    snprintf(part, sizeof part, "%s.%d", out, (int)getpid());
    PlatFile* f = plat_file_open(part, PLAT_WRITE | PLAT_CREATE | PLAT_TRUNCATE);
    if (!f)
        return 0;
    int ok = plat_file_write(f, b->b, (uint32_t)b->n) == (int64_t)b->n;
    plat_file_close(f);
    /* another client may be writing the same bytes: each writes its own file, then swaps it in */
    ok = ok && plat_rename(part, out);
    if (!ok)
        plat_unlink(part);
    return ok;
}

static void prepare(size_t s, const char* host_path, int role)
{
    g_state[s] = -1;
    char host[3][1400];
    Buf f[3] = {{0}};
    int ok = 1;
    for (int i = 0; i < 3 && ok; ++i)
        ok = sibling_host(host_path, i - role, host[i], sizeof host[i]) && (f[i].b = plat_read_file(host[i], &f[i].n)) != NULL;
    int kind = SHEETS[s].kind;
    ok = ok && (kind == TEXT ? patch_text(f) : kind == PACKS ? patch_packs(f) : patch_goods(f));
    for (int i = 0; i < 3 && ok; ++i)
    {
        char tail[64];
        g_out[s][i][0] = 0;
        if (f[i].b && !(sibling_tail(SHEETS[s].data, i, tail, sizeof tail) && write_cache(tail, &f[i], g_out[s][i], sizeof g_out[s][i])))
            ok = 0;
    }
    for (int i = 0; i < 3; ++i)
        free(f[i].b);
    if (!ok)
    {
        rt_log("[monsterpoints] %s: not the 1.23b sheet this expects, left as it is\n", SHEETS[s].data);
        return;
    }
    g_state[s] = 1;
    rt_log(kind == TEXT ? "[monsterpoints] %s: currency %u named Monster Points\n"
                        : kind == PACKS ? "[monsterpoints] %s: shop pack %u added\n" : "[monsterpoints] %s: %u goods slots added\n",
           SHEETS[s].data, kind == TEXT ? CURRENCY : kind == PACKS ? PACK : SLOTS);
}

int xiv_monster_points_redirect(const char* guest_path, const char* host_path, char* out, size_t out_n)
{
    if (!enabled())
        return 0;
    for (size_t s = 0; s < NSHEETS; ++s)
        for (int role = 0; role < 3; ++role)
        {
            char tail[64];
            if (!sibling_tail(SHEETS[s].data, role, tail, sizeof tail) || !ends_with_path(guest_path, tail))
                continue;
            if (!g_state[s])
                prepare(s, host_path, role);
            if (g_state[s] <= 0 || !g_out[s][role][0] || strlen(g_out[s][role]) >= out_n)
                return 0;
            strcpy(out, g_out[s][role]);
            return 1;
        }
    return 0;
}
