/* The game's "A Word to Our Players" box, shown before the lobby, in XIVLegacy's own words.
 *
 * 1.23b asks there, with Yes and No, that players not let the game crowd out their family, friends,
 * school or work. Its text is one field of an English text sheet (data/0B/45/02/BB.DAT; an older
 * copy in data/27/95/00/42.DAT): a u16 size, 0xFF, then the string XOR 0x73 with its NUL, the line
 * breaks being the control code 02 10 01 03. As the game reads such a sheet, that field's bytes are
 * swapped for NOTICE, padded with NULs to the same size, so nothing after it moves. The field is
 * known by its size and an FNV-1a hash of its text, so XIVLegacy carries none of Square Enix's
 * words, and a sheet whose field isn't one of these (another language, another version) is left as
 * it is. XIVLEGACY_NOTICE=0 keeps the game's own text. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plat.h"
#include "runtime.h"
#include "xiv_notice.h"

/* '\n' becomes the game's line break; the box keeps its Yes and No, so it ends with the question */
static const char NOTICE[] =
    "What you're about to experience was one of Square Enix's biggest\n"
    "blunders, but they learned a lot from it.\n"
    "\n"
    "After you experience this story, please go play the retail\n"
    "FINAL FANTASY XIV. Eorzea is waiting.\n"
    "\n"
    "Do you wish to continue?";

/* the original field: its text's length with the NUL, and the FNV-1a hash of those bytes */
static const struct
{
    uint32_t len;
    uint64_t hash;
} ORIGINALS[] = {
    { 402, 0xb5068bf86d71c4f6ull }, /* data/0B/45/02/BB.DAT */
    { 367, 0x85b400eaa053a0c0ull }, /* data/27/95/00/42.DAT */
};

static const char* const SHEETS[] = { "data/0b/45/02/bb.dat", "data/27/95/00/42.dat" };
#define NSHEETS (sizeof SHEETS / sizeof SHEETS[0])

static struct
{
    int state; /* 0 not looked at, 1 found, -1 not found */
    char path[1400];
    int64_t at; /* file offset of the field's text */
    uint32_t len;
    uint8_t* bytes; /* the new text, encoded and padded to len */
} g_sheet[NSHEETS];

static struct
{
    void* file;
    int sheet;
} g_open[8];

static int enabled(void)
{
    static int on = -1;
    if (on < 0)
    {
        const char* e = getenv("XIVLEGACY_NOTICE");
        on = !(e && *e == '0');
    }
    return on;
}

static uint64_t fnv1a(const uint8_t* p, size_t n, uint8_t key)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i)
        h = (h ^ (uint8_t)(p[i] ^ key)) * 0x100000001b3ull;
    return h;
}

/* NOTICE as the field's bytes: line breaks as 02 10 01 03, NUL-padded to len, XOR 0x73 */
static uint8_t* encode(uint32_t len)
{
    uint8_t* out = calloc(len, 1);
    if (!out)
        return NULL;
    uint32_t o = 0;
    for (const char* c = NOTICE; *c; ++c)
    {
        static const uint8_t BREAK[4] = { 0x02, 0x10, 0x01, 0x03 };
        uint32_t k = *c == '\n' ? 4 : 1;
        if (o + k >= len) /* the NUL must fit */
        {
            free(out);
            return NULL;
        }
        if (*c == '\n')
            memcpy(out + o, BREAK, 4);
        else
            out[o] = (uint8_t)*c;
        o += k;
    }
    for (uint32_t i = 0; i < len; ++i)
        out[i] ^= 0x73;
    return out;
}

static void find_field(int s, const char* host_path)
{
    g_sheet[s].state = -1;
    size_t n = 0;
    uint8_t* d = plat_read_file(host_path, &n);
    if (!d)
        return;
    for (size_t i = 0; i + 3 < n; ++i)
    {
        uint32_t size = (uint32_t)d[i] | (uint32_t)d[i + 1] << 8;
        if (d[i + 2] != 0xFF || size < 2 || i + 2 + size > n)
            continue;
        for (size_t k = 0; k < sizeof ORIGINALS / sizeof ORIGINALS[0]; ++k)
            if (size - 1 == ORIGINALS[k].len && fnv1a(d + i + 3, size - 1, 0x73) == ORIGINALS[k].hash)
            {
                g_sheet[s].bytes = encode(size - 1);
                if (!g_sheet[s].bytes)
                    break;
                g_sheet[s].at = (int64_t)i + 3;
                g_sheet[s].len = size - 1;
                g_sheet[s].state = 1;
                rt_log("[notice] %s: the box before the lobby has XIVLegacy's words\n", SHEETS[s]);
                free(d);
                return;
            }
    }
    free(d);
}

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

void xiv_notice_opened(const char* guest_path, const char* host_path, void* file)
{
    if (!enabled())
        return;
    for (int s = 0; s < (int)NSHEETS; ++s)
    {
        if (!ends_with_path(guest_path, SHEETS[s]))
            continue;
        if (!g_sheet[s].state)
        {
            snprintf(g_sheet[s].path, sizeof g_sheet[s].path, "%s", host_path);
            find_field(s, host_path);
        }
        if (g_sheet[s].state > 0)
            for (int i = 0; i < 8; ++i)
                if (!g_open[i].file)
                {
                    g_open[i].file = file;
                    g_open[i].sheet = s;
                    break;
                }
        return;
    }
}

void xiv_notice_read(void* file, int64_t at, uint8_t* buf, int64_t n)
{
    if (!file || n <= 0)
        return;
    for (int i = 0; i < 8; ++i)
    {
        if (g_open[i].file != file)
            continue;
        int s = g_open[i].sheet;
        if (at < 0)
            at = plat_file_seek((PlatFile*)file, 0, 1) - n;
        int64_t lo = g_sheet[s].at > at ? g_sheet[s].at : at;
        int64_t hi = g_sheet[s].at + g_sheet[s].len < at + n ? g_sheet[s].at + g_sheet[s].len : at + n;
        if (lo < hi)
            memcpy(buf + (lo - at), g_sheet[s].bytes + (lo - g_sheet[s].at), (size_t)(hi - lo));
        return;
    }
}

void xiv_notice_closed(void* file)
{
    for (int i = 0; i < 8; ++i)
        if (g_open[i].file == file)
            g_open[i].file = NULL;
}
