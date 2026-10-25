/* The encrypted command-line argument ffxivboot hands ffxivgame.exe (as Seventh Umbral and
 * Garlemald Client build it):
 *
 *   sqex0002<base64url(Blowfish(" T =<tick> /LANG =en-us /REGION =2 /SERVER_UTC =1356916742
 *            /SESSION_ID =<56 chars>" + NUL))>!////
 *
 * Blowfish is the standard cipher on little-endian 32-bit halves, keyed with the eight lowercase
 * hex digits of (tick & ~0xFFFF); only whole 8-byte blocks are encrypted. The game derives the
 * key from its own GetTickCount, so the tick must come from the same clock (rt_monotonic_ns). */
#include <stdio.h>
#include <string.h>

#include "blowfish_tables.h"
#include "sqex_args.h"

typedef struct
{
    uint32_t p[18];
    uint32_t s[4][256];
} Bf;

static uint32_t bf_f(const Bf* b, uint32_t x)
{
    return ((b->s[0][x >> 24] + b->s[1][(x >> 16) & 0xFF]) ^ b->s[2][(x >> 8) & 0xFF]) + b->s[3][x & 0xFF];
}

static void bf_encrypt(const Bf* b, uint32_t* xl, uint32_t* xr)
{
    uint32_t l = *xl, r = *xr, t;
    for (int i = 0; i < 16; ++i)
    {
        l ^= b->p[i];
        r ^= bf_f(b, l);
        t = l, l = r, r = t;
    }
    t = l, l = r, r = t;
    r ^= b->p[16];
    l ^= b->p[17];
    *xl = l, *xr = r;
}

static void bf_init(Bf* b, const uint8_t* key, size_t n)
{
    memcpy(b->p, BF_P_INIT, sizeof b->p);
    memcpy(b->s, BF_S_INIT, sizeof b->s);
    size_t j = 0;
    for (int i = 0; i < 18; ++i)
    {
        uint32_t d = 0;
        for (int k = 0; k < 4; ++k)
        {
            d = (d << 8) | key[j];
            j = (j + 1) % n;
        }
        b->p[i] ^= d;
    }
    uint32_t l = 0, r = 0;
    for (int i = 0; i < 18; i += 2)
    {
        bf_encrypt(b, &l, &r);
        b->p[i] = l, b->p[i + 1] = r;
    }
    for (int s = 0; s < 4; ++s)
        for (int i = 0; i < 256; i += 2)
        {
            bf_encrypt(b, &l, &r);
            b->s[s][i] = l, b->s[s][i + 1] = r;
        }
}

int sqex_launch_arg(const char* session_id, uint32_t tick, char* out, size_t n)
{
    if (strlen(session_id) != 56)
        return 0;
    uint8_t buf[256];
    int len = snprintf((char*)buf, sizeof buf,
        " T =%u /LANG =en-us /REGION =2 /SERVER_UTC =1356916742 /SESSION_ID =%s", tick, session_id);
    if (len < 0 || len + 1 > (int)sizeof buf)
        return 0;
    size_t plain = (size_t)len + 1; /* the NUL is encrypted too */

    char key[9];
    snprintf(key, sizeof key, "%08x", tick & ~0xFFFFu);
    Bf b;
    bf_init(&b, (const uint8_t*)key, 8);
    for (size_t o = 0; o + 8 <= plain; o += 8)
    {
        uint32_t l = buf[o] | buf[o + 1] << 8 | buf[o + 2] << 16 | (uint32_t)buf[o + 3] << 24;
        uint32_t r = buf[o + 4] | buf[o + 5] << 8 | buf[o + 6] << 16 | (uint32_t)buf[o + 7] << 24;
        bf_encrypt(&b, &l, &r);
        for (int k = 0; k < 4; ++k)
            buf[o + k] = (uint8_t)(l >> (8 * k)), buf[o + 4 + k] = (uint8_t)(r >> (8 * k));
    }

    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t need = 8 + (plain + 2) / 3 * 4 + 5 + 1;
    if (n < need)
        return 0;
    char* p = out;
    memcpy(p, "sqex0002", 8), p += 8;
    for (size_t i = 0; i < plain; i += 3)
    {
        uint32_t v = (uint32_t)buf[i] << 16 | (i + 1 < plain ? buf[i + 1] : 0) << 8 | (i + 2 < plain ? buf[i + 2] : 0);
        *p++ = b64[v >> 18];
        *p++ = b64[(v >> 12) & 63];
        *p++ = i + 1 < plain ? b64[(v >> 6) & 63] : '=';
        *p++ = i + 2 < plain ? b64[v & 63] : '=';
    }
    memcpy(p, "!////", 6);
    return 1;
}
