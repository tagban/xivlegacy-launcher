/* Icons for monster moves on a player's hotbar (Monster Mode).
 *
 * The client has a row for every monster move (command ids 23011-23645) in each command sheet: the
 * names in xtx/command, cast and recast times, TP cost. In the sheet that holds each command's icon
 * (data/01/03/04/A6.DAT: 26-byte rows, the icon id a u16 at +0, the class at +4; the ids are the runs
 * of id,count in A7.DAT next to it) every monster row has icon 30000, the placeholder image: a red
 * cross over the word "Undefined". Monsters never needed one, so the hotbar showed that.
 *
 * As the game reads that sheet, each monster move's icon becomes one of the player action icons that
 * fits its kind: its actionType and actionProperty in XIVLegacy's server data (xiv_monster_moves_table.h,
 * made by tools/monster_moves.py): elemental moves get that element's picture, other magic a spell
 * icon, the rest a weaponskill icon. Only rows that still hold
 * the placeholder change, so XIVLegacy carries none of the sheet's bytes, and a sheet that isn't laid
 * out this way (another version) is left as it is. XIVLEGACY_MONSTERICONS=0 keeps the placeholders;
 * =<id>:<icon>,... sets icons by hand (to try one). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_monster_moves.h"
#include "xiv_monster_moves_table.h" /* MONSTER_MOVES */

#define ROWS_PATH "data/01/03/04/a6.dat"
#define HEADER_PATH "data/01/03/04/a7.dat"
#define ROW_SIZE 26u
#define PLACEHOLDER 30000u

/* player icons by kind, picked by looking at them in the client (the names in the command sheets
 * don't line up with the pictures) */
enum
{
    ICON_PHYSICAL = 30078, /* a figure striking, with the weaponskill badge */
    ICON_MAGIC = 30135,    /* a figure in purple light, with the spell badge: astral, umbral, healing */
    ICON_FIRE = 30151,     /* the element pictures, no badge */
    ICON_ICE = 30146,
    ICON_WIND = 30149,
    ICON_EARTH = 30156,
    ICON_LIGHTNING = 30153,
    ICON_WATER = 30152,
};

static uint16_t icon_for(int type, int prop)
{
    switch (prop)
    {
    case 5: return ICON_FIRE;
    case 6: return ICON_ICE;
    case 7: return ICON_WIND;
    case 8: return ICON_EARTH;
    case 9: return ICON_LIGHTNING;
    case 10: return ICON_WATER;
    }
    return type == 2 || prop > 10 ? ICON_MAGIC : ICON_PHYSICAL;
}

static int g_state; /* 0 not looked at, 1 ready, -1 off or not this layout */
static uint8_t* g_rows; /* the patched rows file */
static int64_t g_size;
static void* g_open[8];

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

static int by_hand(const char* spec, uint32_t id, uint16_t* icon)
{
    for (const char* p = spec; p && *p;)
    {
        char* end;
        unsigned long a = strtoul(p, &end, 10);
        if (*end != ':')
            return 0;
        unsigned long b = strtoul(end + 1, &end, 10);
        if (a == id)
        {
            *icon = (uint16_t)b;
            return 1;
        }
        p = *end == ',' ? end + 1 : NULL;
    }
    return 0;
}

static void prepare(const char* host_rows)
{
    g_state = -1;
    const char* e = getenv("XIVLEGACY_MONSTERICONS");
    if (e && !strcmp(e, "0"))
        return;
    const char* spec = e && strchr(e, ':') ? e : NULL;
    /* the header next to it: runs of (first id, count) */
    char host_hdr[1400];
    size_t n = strlen(host_rows);
    if (n < 6 || n >= sizeof host_hdr)
        return;
    memcpy(host_hdr, host_rows, n + 1);
    if (host_hdr[n - 5] != '6') /* ...A6.DAT -> ...A7.DAT */
        return;
    host_hdr[n - 5] = '7';
    size_t hn = 0, rn = 0;
    uint8_t* h = plat_read_file(host_hdr, &hn);
    uint8_t* r = plat_read_file(host_rows, &rn);
    uint32_t count = 0;
    int ok = h && r && hn && hn % 8 == 0 && hn <= 4096;
    for (size_t i = 0; ok && i < hn; i += 8)
        count += (uint32_t)h[i + 4] | (uint32_t)h[i + 5] << 8 | (uint32_t)h[i + 6] << 16 | (uint32_t)h[i + 7] << 24;
    ok = ok && (uint64_t)count * ROW_SIZE == rn;
    uint32_t changed = 0, row = 0;
    for (size_t i = 0; ok && i < hn; i += 8)
    {
        uint32_t first = (uint32_t)h[i] | (uint32_t)h[i + 1] << 8 | (uint32_t)h[i + 2] << 16 | (uint32_t)h[i + 3] << 24;
        uint32_t cnt = (uint32_t)h[i + 4] | (uint32_t)h[i + 5] << 8 | (uint32_t)h[i + 6] << 16 | (uint32_t)h[i + 7] << 24;
        for (uint32_t k = 0; k < cnt; ++k, ++row)
        {
            uint32_t id = first + k;
            uint8_t* p = r + (size_t)row * ROW_SIZE;
            if (id < 23000 || id > 23999 || (p[0] | p[1] << 8) != PLACEHOLDER)
                continue;
            uint16_t icon = 0;
            if (spec)
                by_hand(spec, id, &icon);
            for (size_t m = 0; !icon && m < sizeof MONSTER_MOVES / sizeof MONSTER_MOVES[0]; ++m)
                if (MONSTER_MOVES[m].id == id)
                    icon = icon_for(MONSTER_MOVES[m].type, MONSTER_MOVES[m].prop);
            if (!icon)
                icon = ICON_PHYSICAL;
            p[0] = (uint8_t)icon, p[1] = (uint8_t)(icon >> 8);
            ++changed;
        }
    }
    free(h);
    if (!ok || !changed)
    {
        free(r);
        rt_log("[monstericons] %s: not the 1.23b command sheet, left as it is\n", ROWS_PATH);
        return;
    }
    g_rows = r, g_size = (int64_t)rn, g_state = 1;
    rt_log("[monstericons] %u monster moves get an icon\n", changed);
}

void xiv_monster_moves_opened(const char* guest_path, const char* host_path, void* file)
{
    if (!ends_with_path(guest_path, ROWS_PATH))
        return;
    if (!g_state)
        prepare(host_path);
    if (g_state > 0)
        for (int i = 0; i < 8; ++i)
            if (!g_open[i])
            {
                g_open[i] = file;
                break;
            }
}

void xiv_monster_moves_read(void* file, int64_t at, uint8_t* buf, int64_t n)
{
    if (!file || n <= 0 || g_state <= 0)
        return;
    for (int i = 0; i < 8; ++i)
    {
        if (g_open[i] != file)
            continue;
        if (at < 0)
            at = plat_file_seek((PlatFile*)file, 0, 1) - n;
        int64_t hi = at + n < g_size ? at + n : g_size;
        if (at >= 0 && at < hi)
            memcpy(buf, g_rows + at, (size_t)(hi - at));
        return;
    }
}

void xiv_monster_moves_closed(void* file)
{
    for (int i = 0; i < 8; ++i)
        if (g_open[i] == file)
            g_open[i] = NULL;
}
