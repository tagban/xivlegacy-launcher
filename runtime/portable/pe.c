/* The game image loader for 64-bit hosts. See pe.h. PE32 is parsed by offset, without windows.h,
 * so this file builds on macOS unchanged. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gthread.h"
#include "gwin.h"
#include "pe.h"
#include "plat.h"
#include "thunk.h"

static uint32_t rd32b(const unsigned char* p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16b(const unsigned char* p) { uint16_t v; memcpy(&v, p, 2); return v; }

/* What mapping the pinned build needs: the translation's rt_image_* constants. */
typedef struct Build
{
    const char* name;
    uint32_t base, timestamp, size;
} Build;

/* Maps the pinned build at its preferred base: headers and sections, then every import pointed at
 * a thunk and the static TLS set up. */
static int map_build(const char* path, const Build* b)
{
    const uint32_t load_base = b->base;
    size_t size = 0;
    unsigned char* f = plat_read_file(path, &size);
    if (!f)
    {
        rt_log("[recomp] cannot read %s\n", path);
        return 0;
    }
    uint32_t nt = rd32b(f + 0x3C);
    const unsigned char* fh = f + nt + 4;
    const unsigned char* oh = fh + 20;
    uint16_t nsec = rd16b(fh + 2);
    uint32_t stamp = rd32b(fh + 4), image_base = rd32b(oh + 28), image_size = rd32b(oh + 56), headers = rd32b(oh + 60);
    if (rd32b(f + nt) != 0x00004550u || image_base != b->base || stamp != b->timestamp || image_size != b->size)
    {
        rt_log("[recomp] %s is not the %s build this translation was made from\n", path, b->name);
        free(f);
        return 0;
    }
    if (gwin_reserve(load_base, image_size) != load_base || !gwin_commit(load_base, image_size))
    {
        rt_log("[recomp] cannot map %s at %08x\n", b->name, load_base);
        free(f);
        return 0;
    }
    memcpy(GUEST_PTR(load_base), f, headers < size ? headers : size);
    const unsigned char* sec = oh + rd16b(fh + 16);
    for (unsigned s = 0; s < nsec; ++s, sec += 40)
    {
        uint32_t va = rd32b(sec + 12), raw = rd32b(sec + 16), ptr = rd32b(sec + 20);
        if (raw && ptr + raw <= size)
            memcpy(GUEST_PTR(load_base + va), f + ptr, raw);
    }
    free(f);

    uint32_t opt = load_base + nt + 24; /* the optional header, in the mapped headers */
    uint32_t imp = rd32(opt + 96 + 8 * 1);
    unsigned count = 0;
    for (uint32_t d = load_base + imp; imp && rd32(d + 12); d += 20)
    {
        const char* dll = (const char*)GUEST_PTR(load_base + rd32(d + 12));
        uint32_t names = load_base + (rd32(d + 0) ? rd32(d + 0) : rd32(d + 16));
        uint32_t iat = load_base + rd32(d + 16);
        for (; rd32(names); names += 4, iat += 4, ++count)
        {
            uint32_t e = rd32(names);
            char ord[16];
            const char* name;
            if (e & 0x80000000u)
            {
                snprintf(ord, sizeof ord, "#%u", e & 0xFFFFu);
                name = ord;
            }
            else
                name = (const char*)GUEST_PTR(load_base + e + 2);
            uint32_t t = thunk_for(dll, name);
            if (!t)
            {
                rt_log("[recomp] out of thunks at %s!%s\n", dll, name);
                return 0;
            }
            wr32(iat, t);
        }
    }
    /* static TLS: index 0 for this image, and the template for every thread (gthread.c) */
    uint32_t tls = rd32(opt + 96 + 8 * 9);
    if (tls)
    {
        uint32_t d = load_base + tls;
        if (rd32(d + 8))
            wr32(rd32(d + 8), 0); /* AddressOfIndex */
        gt_set_static_tls(rd32(d + 0), rd32(d + 4), rd32(d + 16));
        rt_log("[recomp] static TLS: %u bytes + %u zero-filled\n", rd32(d + 4) - rd32(d + 0), rd32(d + 16));
    }
    rt_log("[recomp] %s mapped at %08x (host %p), %u imports bound to thunks\n", b->name, load_base,
        (void*)GUEST_PTR(load_base), count);
    return 1;
}

int pe_load(const char* retail_path)
{
    Build b = { "the game image", rt_image_base, rt_image_timestamp, rt_image_size };
    if (!map_build(retail_path, &b))
        return 0;
    rt_set_image(rt_image_base); /* at the preferred base: RD = 0 */
    return 1;
}

uint32_t pe_export_at(uint32_t base, const char* name)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t ed = base + rd32(opt + 96);
    uint32_t n = rd32(ed + 24), funcs = base + rd32(ed + 28), names = base + rd32(ed + 32), ords = base + rd32(ed + 36);
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp((const char*)GUEST_PTR(base + rd32(names + 4 * i)), name))
            return base + rd32(funcs + 4 * rd16(ords + 2 * i));
    return 0;
}

uint32_t pe_export(const char* name)
{
    return pe_export_at(rt_image_base, name);
}

/* One level of a resource directory: the entry for an integer id (0 = the first entry). */
static uint32_t res_entry(uint32_t root, uint32_t dir, uint32_t id)
{
    uint16_t named = rd16(dir + 12), ids = rd16(dir + 14);
    for (uint32_t i = 0; i < (uint32_t)named + ids; ++i)
    {
        uint32_t e = dir + 16 + 8 * i, name = rd32(e);
        if (!id || (!(name & 0x80000000u) && name == id))
            return rd32(e + 4);
    }
    return 0xFFFFFFFFu;
}

/* One level by a Win32 resource name argument: an integer (< 0x10000), "#123", or a string -
 * ANSI or UTF-16 - compared without case against the directory's counted UTF-16 names. */
static uint32_t res_entry_named(uint32_t root, uint32_t dir, uint32_t arg, int wide)
{
    if (arg < 0x10000u)
        return res_entry(root, dir, arg);
    char want[128];
    unsigned n = 0;
    for (uint32_t p = arg; n + 1 < sizeof want; p += wide ? 2 : 1)
    {
        uint32_t c = wide ? rd16(p) : rd8(p);
        if (!c)
            break;
        want[n++] = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    want[n] = 0;
    if (want[0] == '#')
        return res_entry(root, dir, (uint32_t)strtoul(want + 1, NULL, 10));
    uint16_t named = rd16(dir + 12);
    for (uint32_t i = 0; i < named; ++i)
    {
        uint32_t e = dir + 16 + 8 * i, name = rd32(e);
        if (!(name & 0x80000000u))
            continue;
        uint32_t s = root + (name & 0x7FFFFFFFu), len = rd16(s), k = 0;
        for (; k < len && k < n; ++k)
        {
            uint16_t c = rd16(s + 2 + 2 * k);
            if ((c >= 'a' && c <= 'z' ? c - 32 : c) != (uint8_t)want[k])
                break;
        }
        if (k == len && k == n)
            return rd32(e + 4);
    }
    return 0xFFFFFFFFu;
}

uint32_t pe_find_resource(uint32_t base, uint32_t type, uint32_t name, int wide)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t rva = rd32(opt + 96 + 8 * 2);
    if (!rva)
        return 0;
    uint32_t root = base + rva;
    uint32_t t = res_entry_named(root, root, type, wide);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u))
        return 0;
    uint32_t n = res_entry_named(root, root + (t & 0x7FFFFFFFu), name, wide);
    if (n == 0xFFFFFFFFu || !(n & 0x80000000u))
        return 0;
    uint32_t l = res_entry(root, root + (n & 0x7FFFFFFFu), 0);
    return l == 0xFFFFFFFFu || (l & 0x80000000u) ? 0 : root + l;
}

uint32_t pe_resource(uint32_t base, uint32_t type, uint32_t name, uint32_t* size)
{
    uint32_t opt = base + rd32(base + 0x3C) + 24;
    uint32_t rva = rd32(opt + 96 + 8 * 2);
    if (!rva)
        return 0;
    uint32_t root = base + rva;
    uint32_t t = res_entry(root, root, type);
    if (t == 0xFFFFFFFFu || !(t & 0x80000000u))
        return 0;
    uint32_t n = res_entry(root, root + (t & 0x7FFFFFFFu), name);
    if (n == 0xFFFFFFFFu || !(n & 0x80000000u))
        return 0;
    uint32_t l = res_entry(root, root + (n & 0x7FFFFFFFu), 0); /* whatever language is there */
    if (l == 0xFFFFFFFFu || (l & 0x80000000u))
        return 0;
    uint32_t data = root + l;
    if (size)
        *size = rd32(data + 4);
    return base + rd32(data);
}
