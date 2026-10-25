/* The d3dx9_41.dll functions ffxivgame.exe imports, over d3d9.c.
 *
 * The game ships its shaders compiled, so what it needs from D3DX is small: the constant table of a
 * compiled shader (it reads the CTAB block itself through ID3DXBuffer::GetBufferPointer), texture
 * creation with D3DX's defaults, and loading pixels into surfaces and volumes. The HLSL compilers
 * fail with a log line; if the game ever relies on them, that shows here first. */
#include <stdio.h>
#include <string.h>

#include "d3d9.h"
#include "gwin.h"
#include "plat.h"
#include "thunk.h"

#define D3D_OK 0u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define D3DX_DEFAULT 0xFFFFFFFFu
#define D3DX_DEFAULT_NONPOW2 0xFFFFFFFEu
#define FMT_A8R8G8B8 21u
#define FMT_X8R8G8B8 22u
#define FMT_A8B8G8R8 32u
#define FMT_X8B8G8R8 33u

/* --- ID3DXBuffer and ID3DXConstantTable: {vtable, data, size, refs} in guest memory --------------- */

static const char* const BUFFER_METHODS[] = { "QueryInterface", "AddRef", "Release", "GetBufferPointer",
                                              "GetBufferSize" };
static const char* const TABLE_METHODS[] = {
    "QueryInterface", "AddRef", "Release", "GetBufferPointer", "GetBufferSize", "GetDesc", "GetConstantDesc",
    "GetSamplerIndex", "GetConstant", "GetConstantByName", "GetConstantElement", "SetDefaults", "SetValue", "SetBool",
    "SetBoolArray", "SetInt", "SetIntArray", "SetFloat", "SetFloatArray", "SetVector", "SetVectorArray", "SetMatrix",
    "SetMatrixArray", "SetMatrixPointerArray", "SetMatrixTranspose", "SetMatrixTransposeArray",
    "SetMatrixTransposePointerArray",
};

static uint32_t make_vtable(const char* iface, const char* const* methods, unsigned n)
{
    uint32_t v = gheap_alloc(4u * n, 1);
    char full[96];
    for (unsigned i = 0; i < n; ++i)
    {
        snprintf(full, sizeof full, "%s::%s", iface, methods[i]);
        wr32(v + 4u * i, thunk_for("d3dx9_41.dll", full));
    }
    return v;
}

/* A buffer holding a copy of `n` bytes at guest `src` (or zeroed when src is 0). */
static uint32_t buffer_new(int table, uint32_t src, uint32_t n)
{
    static uint32_t vbuf, vtab;
    if (!vbuf)
    {
        vbuf = make_vtable("ID3DXBuffer", BUFFER_METHODS, sizeof BUFFER_METHODS / sizeof BUFFER_METHODS[0]);
        vtab = make_vtable("ID3DXConstantTable", TABLE_METHODS, sizeof TABLE_METHODS / sizeof TABLE_METHODS[0]);
    }
    uint32_t o = gheap_alloc(16, 1), data = gheap_alloc(n ? n : 4, 1);
    if (src && n)
        memcpy(GUEST_PTR(data), GUEST_PTR(src), n);
    wr32(o, table ? vtab : vbuf);
    wr32(o + 4, data);
    wr32(o + 8, n);
    wr32(o + 12, 1);
    return o;
}

static void sh_Buf_QueryInterface(Guest* g) { wr32(ARG(2), 0); RET(E_NOINTERFACE, 3); }
static void sh_Buf_AddRef(Guest* g) { uint32_t o = ARG(0), r = rd32(o + 12) + 1; wr32(o + 12, r); RET(r, 1); }

static void sh_Buf_Release(Guest* g)
{
    uint32_t o = ARG(0), r = rd32(o + 12);
    if (r)
        wr32(o + 12, --r);
    if (!r)
    {
        gheap_free(rd32(o + 4));
        gheap_free(o);
    }
    RET(r, 1);
}

static void sh_Buf_GetBufferPointer(Guest* g) { RET(rd32(ARG(0) + 4), 1); }
static void sh_Buf_GetBufferSize(Guest* g) { RET(rd32(ARG(0) + 8), 1); }

/* D3DXGetShaderConstantTable(const DWORD* function, ID3DXConstantTable**): the table's data is the
 * CTAB comment's contents after its FOURCC, which is what GetBufferPointer returns in D3DX. */
static void sh_D3DXGetShaderConstantTable(Guest* g)
{
    uint32_t code = ARG(0), out = ARG(1);
    if (!code || !out)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(out, 0);
    for (uint32_t i = 1; i < 65536;)
    {
        uint32_t t = rd32(code + 4u * i);
        if (t == 0x0000FFFFu)
            break;
        if ((t & 0xFFFF) == 0xFFFE)
        {
            uint32_t len = (t >> 16) & 0x7FFF;
            if (len >= 1 && rd32(code + 4u * (i + 1)) == 0x42415443u) /* 'CTAB' */
            {
                wr32(out, buffer_new(1, code + 4u * (i + 2), 4u * (len - 1)));
                RET(D3D_OK, 2);
            }
            i += len + 1;
        }
        else
            i++; /* instruction tokens are never 0xFFFE-tagged: stepping one at a time is enough */
    }
    RET(E_FAIL, 2); /* D3DXERR_INVALIDDATA in D3DX; the game only checks for failure */
}

/* --- profiles and compilers -------------------------------------------------------------------- */

static uint32_t guest_string(const char* s)
{
    uint32_t p = gheap_alloc((uint32_t)strlen(s) + 1, 1);
    memcpy(GUEST_PTR(p), s, strlen(s) + 1);
    return p;
}

static void sh_D3DXGetPixelShaderProfile(Guest* g)
{
    static uint32_t s;
    if (!s)
        s = guest_string("ps_3_0");
    RET(s, 1);
}

static void sh_D3DXGetVertexShaderProfile(Guest* g)
{
    static uint32_t s;
    if (!s)
        s = guest_string("vs_3_0");
    RET(s, 1);
}

static void compile_failed(Guest* g, const char* what, uint32_t entry, uint32_t profile, uint32_t shader, uint32_t errors,
    uint32_t table)
{
    char e[64] = "", p[16] = "";
    if (entry)
        snprintf(e, sizeof e, "%s", (const char*)GUEST_PTR(entry));
    if (profile)
        snprintf(p, sizeof p, "%s", (const char*)GUEST_PTR(profile));
    rt_log("[d3dx9] %s(%s, %s): no HLSL compiler here\n", what, e, p);
    if (shader)
        wr32(shader, 0);
    if (table)
        wr32(table, 0);
    if (errors)
    {
        static const char msg[] = "XIVLegacy: HLSL compilation is not available";
        uint32_t s = guest_string(msg);
        wr32(errors, buffer_new(0, s, sizeof msg));
        gheap_free(s);
    }
}

/* D3DXCompileShader(src, len, defines, include, function, profile, flags, shader**, errors**, table**) */
static void sh_D3DXCompileShader(Guest* g)
{
    compile_failed(g, "D3DXCompileShader", ARG(4), ARG(5), ARG(7), ARG(8), ARG(9));
    RET(E_FAIL, 10);
}

/* D3DXCompileShaderFromFileA(file, defines, include, function, profile, flags, shader**, errors**, table**) */
static void sh_D3DXCompileShaderFromFileA(Guest* g)
{
    compile_failed(g, "D3DXCompileShaderFromFileA", ARG(3), ARG(4), ARG(6), ARG(7), ARG(8));
    RET(E_FAIL, 9);
}

/* --- textures ------------------------------------------------------------------------------------ */

static uint32_t dflt(uint32_t v, uint32_t d) { return v == 0 || v == D3DX_DEFAULT || v == D3DX_DEFAULT_NONPOW2 ? d : v; }

static uint32_t levels_arg(uint32_t v) { return v == D3DX_DEFAULT ? 0 : v; } /* 0 = the full chain */

static uint32_t fmt_arg(uint32_t v) { return v == 0 || v == D3DX_DEFAULT ? FMT_A8R8G8B8 : v; }

/* D3DXCreateTexture(device, w, h, levels, usage, format, pool, IDirect3DTexture9**) */
static void sh_D3DXCreateTexture(Guest* g)
{
    uint32_t t = d3d9_create_texture(D3D9_TEXTURE, dflt(ARG(1), 256), dflt(ARG(2), 256), 1, levels_arg(ARG(3)), ARG(4),
        fmt_arg(ARG(5)), ARG(6));
    wr32(ARG(7), t);
    RET(t ? D3D_OK : D3DERR_INVALIDCALL, 8);
}

/* D3DXCreateVolumeTexture(device, w, h, d, levels, usage, format, pool, IDirect3DVolumeTexture9**) */
static void sh_D3DXCreateVolumeTexture(Guest* g)
{
    uint32_t t = d3d9_create_texture(D3D9_VOLUME_TEXTURE, dflt(ARG(1), 256), dflt(ARG(2), 256), dflt(ARG(3), 1),
        levels_arg(ARG(4)), ARG(5), fmt_arg(ARG(6)), ARG(7));
    wr32(ARG(8), t);
    RET(t ? D3D_OK : D3DERR_INVALIDCALL, 9);
}

/* D3DXCreateCubeTexture(device, size, levels, usage, format, pool, IDirect3DCubeTexture9**) */
static void sh_D3DXCreateCubeTexture(Guest* g)
{
    uint32_t e = dflt(ARG(1), 256);
    uint32_t t = d3d9_create_texture(D3D9_CUBE, e, e, 1, levels_arg(ARG(2)), ARG(3), fmt_arg(ARG(4)), ARG(5));
    wr32(ARG(6), t);
    RET(t ? D3D_OK : D3DERR_INVALIDCALL, 7);
}

/* --- loading pixels ------------------------------------------------------------------------------ */

typedef struct
{
    uint32_t l, t, r, b, front, back;
} Box;

static Box box_of(uint32_t rect, int is_box, uint32_t w, uint32_t h, uint32_t d)
{
    Box x = { 0, 0, w, h, 0, d };
    if (rect)
    {
        x.l = rd32(rect), x.t = rd32(rect + 4), x.r = rd32(rect + 8), x.b = rd32(rect + 12);
        if (is_box)
            x.front = rd32(rect + 16), x.back = rd32(rect + 20);
    }
    return x;
}

/* --- pixel conversion: through RGBA floats, for the formats the game's textures use ---------------- */

enum
{
    F_R5G6B5 = 23, F_X1R5G5B5 = 24, F_A1R5G5B5 = 25, F_A4R4G4B4 = 26, F_A8 = 28, F_A2B10G10R10 = 31,
    F_G16R16 = 34, F_A2R10G10B10 = 35, F_A16B16G16R16 = 36, F_L8 = 50, F_A8L8 = 51, F_L16 = 81, F_R16F = 111,
    F_G16R16F = 112, F_A16B16G16R16F = 113, F_R32F = 114, F_G32R32F = 115, F_A32B32G32R32F = 116,
};

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF, u;
    if (e == 0)
    {
        if (!m)
            u = s;
        else
        {
            e = 127 - 15 + 1;
            while (!(m & 0x400))
                m <<= 1, e--;
            u = s | (e << 23) | ((m & 0x3FF) << 13);
        }
    }
    else if (e == 31)
        u = s | 0x7F800000u | (m << 13);
    else
        u = s | ((e + 127 - 15) << 23) | (m << 13);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static uint16_t float_to_half(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000, e = (u >> 23) & 0xFF, m = u & 0x7FFFFF;
    if (e == 0xFF)
        return (uint16_t)(s | 0x7C00 | (m ? 0x200 : 0));
    int ne = (int)e - 127 + 15;
    if (ne >= 31)
        return (uint16_t)(s | 0x7C00);
    if (ne <= 0)
    {
        if (ne < -10)
            return (uint16_t)s;
        m |= 0x800000;
        uint32_t shift = (uint32_t)(14 - ne);
        uint32_t hm = m >> shift;
        if ((m >> (shift - 1)) & 1)
            hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13;
    uint16_t h = (uint16_t)(s | ((uint32_t)ne << 10) | hm);
    if (m & 0x1000)
        h++; /* round; a carry into the exponent is still right */
    return h;
}

static float unorm(uint32_t v, unsigned bits) { return (float)v / (float)((1u << bits) - 1); }

static uint32_t to_unorm(float f, unsigned bits)
{
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (uint32_t)(f * (float)((1u << bits) - 1) + 0.5f);
}

static int convertible(uint32_t f)
{
    switch (f)
    {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_A8B8G8R8: case FMT_X8B8G8R8: case F_R5G6B5: case F_X1R5G5B5:
    case F_A1R5G5B5: case F_A4R4G4B4: case F_A8: case F_A2B10G10R10: case F_G16R16: case F_A2R10G10B10:
    case F_A16B16G16R16: case F_L8: case F_A8L8: case F_L16: case F_R16F: case F_G16R16F: case F_A16B16G16R16F:
    case F_R32F: case F_G32R32F: case F_A32B32G32R32F:
        return 1;
    default:
        return 0;
    }
}

/* one pixel to RGBA */
static void decode(uint32_t f, const uint8_t* p, float* c)
{
    uint32_t v = 0;
    uint16_t h[4];
    c[0] = c[1] = c[2] = 0, c[3] = 1;
    switch (f)
    {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8:
        memcpy(&v, p, 4);
        c[0] = unorm((v >> 16) & 0xFF, 8), c[1] = unorm((v >> 8) & 0xFF, 8), c[2] = unorm(v & 0xFF, 8);
        if (f == FMT_A8R8G8B8)
            c[3] = unorm(v >> 24, 8);
        break;
    case FMT_A8B8G8R8: case FMT_X8B8G8R8:
        memcpy(&v, p, 4);
        c[0] = unorm(v & 0xFF, 8), c[1] = unorm((v >> 8) & 0xFF, 8), c[2] = unorm((v >> 16) & 0xFF, 8);
        if (f == FMT_A8B8G8R8)
            c[3] = unorm(v >> 24, 8);
        break;
    case F_R5G6B5:
        memcpy(&v, p, 2);
        c[0] = unorm((v >> 11) & 31, 5), c[1] = unorm((v >> 5) & 63, 6), c[2] = unorm(v & 31, 5);
        break;
    case F_X1R5G5B5: case F_A1R5G5B5:
        memcpy(&v, p, 2);
        c[0] = unorm((v >> 10) & 31, 5), c[1] = unorm((v >> 5) & 31, 5), c[2] = unorm(v & 31, 5);
        if (f == F_A1R5G5B5)
            c[3] = (float)(v >> 15);
        break;
    case F_A4R4G4B4:
        memcpy(&v, p, 2);
        c[0] = unorm((v >> 8) & 15, 4), c[1] = unorm((v >> 4) & 15, 4), c[2] = unorm(v & 15, 4), c[3] = unorm(v >> 12, 4);
        break;
    case F_A8: c[3] = unorm(p[0], 8); break;
    case F_L8: c[0] = c[1] = c[2] = unorm(p[0], 8); break;
    case F_A8L8: c[0] = c[1] = c[2] = unorm(p[0], 8), c[3] = unorm(p[1], 8); break;
    case F_L16: memcpy(&v, p, 2); c[0] = c[1] = c[2] = unorm(v, 16); break;
    case F_A2R10G10B10:
        memcpy(&v, p, 4);
        c[0] = unorm((v >> 20) & 1023, 10), c[1] = unorm((v >> 10) & 1023, 10), c[2] = unorm(v & 1023, 10), c[3] = unorm(v >> 30, 2);
        break;
    case F_A2B10G10R10:
        memcpy(&v, p, 4);
        c[0] = unorm(v & 1023, 10), c[1] = unorm((v >> 10) & 1023, 10), c[2] = unorm((v >> 20) & 1023, 10), c[3] = unorm(v >> 30, 2);
        break;
    case F_G16R16:
        memcpy(h, p, 4);
        c[0] = unorm(h[0], 16), c[1] = unorm(h[1], 16), c[2] = 1;
        break;
    case F_A16B16G16R16:
        memcpy(h, p, 8);
        c[0] = unorm(h[0], 16), c[1] = unorm(h[1], 16), c[2] = unorm(h[2], 16), c[3] = unorm(h[3], 16);
        break;
    case F_R16F: memcpy(h, p, 2); c[0] = half_to_float(h[0]), c[1] = c[2] = 1; break;
    case F_G16R16F: memcpy(h, p, 4); c[0] = half_to_float(h[0]), c[1] = half_to_float(h[1]), c[2] = 1; break;
    case F_A16B16G16R16F:
        memcpy(h, p, 8);
        for (int i = 0; i < 4; ++i)
            c[i] = half_to_float(h[i]);
        break;
    case F_R32F: memcpy(c, p, 4); c[1] = c[2] = 1; break;
    case F_G32R32F: memcpy(c, p, 8); c[2] = 1; break;
    case F_A32B32G32R32F: memcpy(c, p, 16); break;
    }
}

/* RGBA to one pixel */
static void encode(uint32_t f, uint8_t* p, const float* c)
{
    uint32_t v;
    uint16_t h[4];
    switch (f)
    {
    case FMT_A8R8G8B8: case FMT_X8R8G8B8:
        v = to_unorm(c[2], 8) | to_unorm(c[1], 8) << 8 | to_unorm(c[0], 8) << 16 | (f == FMT_A8R8G8B8 ? to_unorm(c[3], 8) : 255u) << 24;
        memcpy(p, &v, 4);
        break;
    case FMT_A8B8G8R8: case FMT_X8B8G8R8:
        v = to_unorm(c[0], 8) | to_unorm(c[1], 8) << 8 | to_unorm(c[2], 8) << 16 | (f == FMT_A8B8G8R8 ? to_unorm(c[3], 8) : 255u) << 24;
        memcpy(p, &v, 4);
        break;
    case F_R5G6B5: v = to_unorm(c[0], 5) << 11 | to_unorm(c[1], 6) << 5 | to_unorm(c[2], 5); memcpy(p, &v, 2); break;
    case F_X1R5G5B5: case F_A1R5G5B5:
        v = to_unorm(c[0], 5) << 10 | to_unorm(c[1], 5) << 5 | to_unorm(c[2], 5) | (f == F_A1R5G5B5 ? (c[3] >= 0.5f) : 1u) << 15;
        memcpy(p, &v, 2);
        break;
    case F_A4R4G4B4:
        v = to_unorm(c[0], 4) << 8 | to_unorm(c[1], 4) << 4 | to_unorm(c[2], 4) | to_unorm(c[3], 4) << 12;
        memcpy(p, &v, 2);
        break;
    case F_A8: p[0] = (uint8_t)to_unorm(c[3], 8); break;
    case F_L8: p[0] = (uint8_t)to_unorm(c[0], 8); break;
    case F_A8L8: p[0] = (uint8_t)to_unorm(c[0], 8), p[1] = (uint8_t)to_unorm(c[3], 8); break;
    case F_L16: v = to_unorm(c[0], 16); memcpy(p, &v, 2); break;
    case F_A2R10G10B10:
        v = to_unorm(c[2], 10) | to_unorm(c[1], 10) << 10 | to_unorm(c[0], 10) << 20 | to_unorm(c[3], 2) << 30;
        memcpy(p, &v, 4);
        break;
    case F_A2B10G10R10:
        v = to_unorm(c[0], 10) | to_unorm(c[1], 10) << 10 | to_unorm(c[2], 10) << 20 | to_unorm(c[3], 2) << 30;
        memcpy(p, &v, 4);
        break;
    case F_G16R16: h[0] = (uint16_t)to_unorm(c[0], 16), h[1] = (uint16_t)to_unorm(c[1], 16); memcpy(p, h, 4); break;
    case F_A16B16G16R16:
        for (int i = 0; i < 4; ++i)
            h[i] = (uint16_t)to_unorm(c[i], 16);
        memcpy(p, h, 8);
        break;
    case F_R16F: h[0] = float_to_half(c[0]); memcpy(p, h, 2); break;
    case F_G16R16F: h[0] = float_to_half(c[0]), h[1] = float_to_half(c[1]); memcpy(p, h, 4); break;
    case F_A16B16G16R16F:
        for (int i = 0; i < 4; ++i)
            h[i] = float_to_half(c[i]);
        memcpy(p, h, 8);
        break;
    case F_R32F: memcpy(p, c, 4); break;
    case F_G32R32F: memcpy(p, c, 8); break;
    case F_A32B32G32R32F: memcpy(p, c, 16); break;
    }
}

/* Copy the source box at `src` (row pitch sp, slice pitch ss, format sf) into the destination box of
 * `dst`, converting between the formats above; sizes that differ are point-sampled (uncompressed)
 * or clipped (compressed, which must match). */
static uint32_t load(const D3d9Mem* dst, Box db, uint32_t src, uint32_t sf, uint32_t sp, uint32_t ss, Box sb)
{
    static int warned;
    uint32_t block = d3d9_fmt_block(dst->fmt);
    int same = sf == dst->fmt, convert = !same && convertible(sf) && convertible(dst->fmt);
    if (!same && !convert)
    {
        if (!warned++)
            rt_log("[d3dx9] loading format %#x into %#x is not converted yet (copied as is)\n", sf, dst->fmt);
        if (block != d3d9_fmt_block(sf))
            return D3D_OK; /* compressed into uncompressed or the reverse: nothing sensible to copy */
    }
    uint32_t dw = db.r - db.l, dh = db.b - db.t, dd = db.back - db.front;
    uint32_t sw = sb.r - sb.l, sh = sb.b - sb.t, sd = sb.back - sb.front;
    if (!dw || !dh || !sw || !sh)
        return D3D_OK;
    if (!dd)
        dd = 1;
    if (!sd)
        sd = 1;
    uint32_t dbpp = d3d9_fmt_bytes(dst->fmt), sbpp = convert ? d3d9_fmt_bytes(sf) : dbpp;
    for (uint32_t z = 0; z < dd; ++z)
    {
        uint32_t szi = sb.front + (z * sd) / dd;
        uint32_t dslice = dst->mem + (db.front + z) * dst->slice, sslice = src + szi * ss;
        if (block)
        {
            /* whole blocks, clipped to the smaller size */
            uint32_t bw = ((dw < sw ? dw : sw) + 3) / 4, bh = ((dh < sh ? dh : sh) + 3) / 4;
            for (uint32_t y = 0; y < bh; ++y)
                memmove(GUEST_PTR(dslice + (db.t / 4 + y) * dst->pitch + (db.l / 4) * block),
                    GUEST_PTR(sslice + (sb.t / 4 + y) * sp + (sb.l / 4) * block), (size_t)bw * block);
            continue;
        }
        for (uint32_t y = 0; y < dh; ++y)
        {
            uint32_t sy = sb.t + (y * sh) / dh;
            uint8_t* drow = GUEST_PTR(dslice + (db.t + y) * dst->pitch + db.l * dbpp);
            const uint8_t* srow = GUEST_PTR(sslice + sy * sp);
            if (dw == sw && !convert)
                memmove(drow, srow + sb.l * dbpp, (size_t)dw * dbpp);
            else
                for (uint32_t x = 0; x < dw; ++x)
                {
                    const uint8_t* sp_ = srow + (sb.l + (x * sw) / dw) * sbpp;
                    if (convert)
                    {
                        float c[4];
                        decode(sf, sp_, c);
                        encode(dst->fmt, drow + x * dbpp, c);
                    }
                    else
                        memcpy(drow + x * dbpp, sp_, dbpp);
                }
        }
    }
    return D3D_OK;
}

/* D3DXLoadSurfaceFromMemory(dst, dst_palette, dst_rect, src, src_format, src_pitch, src_palette,
 *                           src_rect, filter, color_key) */
static void sh_D3DXLoadSurfaceFromMemory(Guest* g)
{
    D3d9Mem m;
    if (!d3d9_resource_mem(ARG(0), &m) || !ARG(3) || !ARG(7))
        RET(D3DERR_INVALIDCALL, 10);
    Box db = box_of(ARG(2), 0, m.w, m.h, 1);
    Box sb = box_of(ARG(7), 0, 0, 0, 1);
    RET(load(&m, db, ARG(3), ARG(4), ARG(5), 0, sb), 10);
}

/* D3DXLoadVolumeFromMemory(dst, dst_palette, dst_box, src, src_format, row_pitch, slice_pitch,
 *                          src_palette, src_box, filter, color_key) */
static void sh_D3DXLoadVolumeFromMemory(Guest* g)
{
    D3d9Mem m;
    if (!d3d9_resource_mem(ARG(0), &m) || !ARG(3) || !ARG(8))
        RET(D3DERR_INVALIDCALL, 11);
    Box db = box_of(ARG(2), 1, m.w, m.h, m.d);
    Box sb = box_of(ARG(8), 1, 0, 0, 1);
    RET(load(&m, db, ARG(3), ARG(4), ARG(5), ARG(6), sb), 11);
}

/* D3DXSaveSurfaceToFileW(file, format, surface, palette, rect): the screenshot key; not written yet */
static void sh_D3DXSaveSurfaceToFileW(Guest* g)
{
    rt_log("[d3dx9] D3DXSaveSurfaceToFileW: screenshots are not written yet\n");
    RET(E_NOTIMPL, 5);
}

#define X(name, fn) { "d3dx9_41.dll", name, fn }
static const ShimDef D3DX9[] = {
    X("D3DXGetShaderConstantTable", sh_D3DXGetShaderConstantTable),
    X("D3DXGetPixelShaderProfile", sh_D3DXGetPixelShaderProfile),
    X("D3DXGetVertexShaderProfile", sh_D3DXGetVertexShaderProfile),
    X("D3DXCompileShader", sh_D3DXCompileShader),
    X("D3DXCompileShaderFromFileA", sh_D3DXCompileShaderFromFileA),
    X("D3DXCreateTexture", sh_D3DXCreateTexture),
    X("D3DXCreateVolumeTexture", sh_D3DXCreateVolumeTexture),
    X("D3DXCreateCubeTexture", sh_D3DXCreateCubeTexture),
    X("D3DXLoadSurfaceFromMemory", sh_D3DXLoadSurfaceFromMemory),
    X("D3DXLoadVolumeFromMemory", sh_D3DXLoadVolumeFromMemory),
    X("D3DXSaveSurfaceToFileW", sh_D3DXSaveSurfaceToFileW),
    X("ID3DXBuffer::QueryInterface", sh_Buf_QueryInterface),
    X("ID3DXBuffer::AddRef", sh_Buf_AddRef),
    X("ID3DXBuffer::Release", sh_Buf_Release),
    X("ID3DXBuffer::GetBufferPointer", sh_Buf_GetBufferPointer),
    X("ID3DXBuffer::GetBufferSize", sh_Buf_GetBufferSize),
    X("ID3DXConstantTable::QueryInterface", sh_Buf_QueryInterface),
    X("ID3DXConstantTable::AddRef", sh_Buf_AddRef),
    X("ID3DXConstantTable::Release", sh_Buf_Release),
    X("ID3DXConstantTable::GetBufferPointer", sh_Buf_GetBufferPointer),
    X("ID3DXConstantTable::GetBufferSize", sh_Buf_GetBufferSize),
    { NULL, NULL, NULL },
};

void d3dx9_init(void) { thunk_register(D3DX9); }
