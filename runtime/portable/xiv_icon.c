/* The game window's icon: the 48 px 32-bit icon in the player's own ffxivgame.exe (its RT_ICON
 * resources), handed to SDL so the title bar, taskbar and app switcher show FINAL FANTASY XIV. It is
 * Square Enix's artwork, read from the install at run time and never carried by XIVLegacy. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "plat.h"
#include "xiv_icon.h"

static char g_exe[1024];

void xiv_icon_source(const char* host_exe_path)
{
    snprintf(g_exe, sizeof g_exe, "%s", host_exe_path);
}

static uint32_t u16(const unsigned char* d, size_t n, size_t o) { return o + 2 <= n ? (uint32_t)(d[o] | d[o + 1] << 8) : 0; }
static uint32_t u32(const unsigned char* d, size_t n, size_t o)
{
    return o + 4 <= n ? (uint32_t)d[o] | (uint32_t)d[o + 1] << 8 | (uint32_t)d[o + 2] << 16 | (uint32_t)d[o + 3] << 24 : 0;
}

/* file offset of an RVA, or 0 */
static size_t rva_off(const unsigned char* d, size_t n, size_t pe, uint32_t rva)
{
    unsigned sections = u16(d, n, pe + 6), opt = u16(d, n, pe + 20);
    for (unsigned i = 0; i < sections; ++i)
    {
        size_t s = pe + 24 + opt + i * 40;
        uint32_t vsize = u32(d, n, s + 8), va = u32(d, n, s + 12), rsize = u32(d, n, s + 16), raw = u32(d, n, s + 20);
        if (va <= rva && rva < va + (vsize > rsize ? vsize : rsize))
            return rva - va + raw;
    }
    return 0;
}

/* The 48x48 32-bit icon bitmap (BITMAPINFOHEADER, height doubled for the mask, bottom-up BGRA). */
static const unsigned char* find_icon48(const unsigned char* d, size_t n)
{
    size_t pe = u32(d, n, 0x3c);
    unsigned magic = u16(d, n, pe + 24);
    size_t dirs = pe + 24 + (magic == 0x10b ? 96 : 112);
    size_t base = rva_off(d, n, pe, u32(d, n, dirs + 16));
    if (!base)
        return NULL;
    unsigned types = u16(d, n, base + 12) + u16(d, n, base + 14);
    for (unsigned t = 0; t < types; ++t)
    {
        if (u32(d, n, base + 16 + 8 * t) != 3) /* RT_ICON */
            continue;
        size_t names = base + (u32(d, n, base + 20 + 8 * t) & 0x7fffffffu);
        unsigned nn = u16(d, n, names + 12) + u16(d, n, names + 14);
        for (unsigned i = 0; i < nn; ++i)
        {
            size_t langs = base + (u32(d, n, names + 20 + 8 * i) & 0x7fffffffu);
            size_t leaf = base + u32(d, n, langs + 20);
            size_t o = rva_off(d, n, pe, u32(d, n, leaf));
            uint32_t size = u32(d, n, leaf + 4);
            if (!o || o + size > n || size < 40 + 48 * 48 * 4)
                continue;
            const unsigned char* bmp = d + o;
            if (u32(bmp, size, 0) == 40 && u32(bmp, size, 4) == 48 && u32(bmp, size, 8) == 96 && u16(bmp, size, 14) == 32)
                return bmp;
        }
    }
    return NULL;
}

void xiv_icon_apply(SDL_Window* w)
{
    if (!w || !g_exe[0])
        return;
    size_t n = 0;
    unsigned char* d = plat_read_file(g_exe, &n);
    if (!d)
        return;
    const unsigned char* bmp = find_icon48(d, n);
    if (bmp)
    {
        SDL_Surface* s = SDL_CreateSurface(48, 48, SDL_PIXELFORMAT_ARGB8888);
        if (s)
        {
            /* BGRA bytes are ARGB8888 on a little-endian machine; the rows are bottom-up */
            for (int y = 0; y < 48; ++y)
                memcpy((unsigned char*)s->pixels + y * s->pitch, bmp + 40 + (47 - y) * 48 * 4, 48 * 4);
            SDL_SetWindowIcon(w, s);
            SDL_DestroySurface(s);
        }
    }
    free(d);
}
