/* Direct3D 9 for ffxivgame.exe, over the portable graphics back end (gfx9.h).
 *
 * The COM objects live in guest memory (a vtable
 * pointer and our object index), every method is a thunk named "d3d9.dll!IDirect3DDevice9::Clear"
 * and so on, so a method the game reaches that is not written yet traps with its name. The vtables
 * come from d3d9_vtables.h (ABI order, from Wine's d3d9.h); structure offsets from d3d9_structs.h.
 *
 * What is here: IDirect3D9 (adapter, modes, formats, caps), the device with full D3D9 state
 * semantics (defaults, get/set, state blocks that record what is set while recording), and the
 * resources (textures, cube and volume textures, surfaces, vertex/index buffers, declarations,
 * shaders, queries) with their memory in the guest window, allocated on first lock. Clear, the
 * Draw* calls and Present validate, count and hand the work to gfx9.h. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "d3d9.h"
#include "gfx9.h"
#include "d3d9_structs.h"
#include "d3d9_vtables.h"
#include "gthread.h"
#include "gwin.h"
#include "k32.h"
#include "plat.h"
#include "thunk.h"
#include "user32.h"

#define D3D_OK 0u
#define S_FALSE 1u
#define D3DERR_INVALIDCALL 0x8876086Cu
#define D3DERR_NOTAVAILABLE 0x8876086Au
#define D3DERR_NOTFOUND 0x88760866u
#define E_NOINTERFACE 0x80004002u
#define E_OUTOFMEMORY 0x8007000Eu

#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* D3DFORMAT values */
enum
{
    FMT_UNKNOWN = 0, FMT_R8G8B8 = 20, FMT_A8R8G8B8 = 21, FMT_X8R8G8B8 = 22, FMT_R5G6B5 = 23, FMT_X1R5G5B5 = 24,
    FMT_A1R5G5B5 = 25, FMT_A4R4G4B4 = 26, FMT_R3G3B2 = 27, FMT_A8 = 28, FMT_A2B10G10R10 = 31, FMT_A8B8G8R8 = 32,
    FMT_X8B8G8R8 = 33, FMT_G16R16 = 34, FMT_A2R10G10B10 = 35, FMT_A16B16G16R16 = 36, FMT_L8 = 50, FMT_A8L8 = 51,
    FMT_A4L4 = 52, FMT_V8U8 = 60, FMT_L6V5U5 = 61, FMT_X8L8V8U8 = 62, FMT_Q8W8V8U8 = 63, FMT_V16U16 = 64,
    FMT_D16_LOCKABLE = 70, FMT_D32 = 71, FMT_D15S1 = 73, FMT_D24S8 = 75, FMT_D24X8 = 77, FMT_D24X4S4 = 79,
    FMT_D16 = 80, FMT_L16 = 81, FMT_D32F_LOCKABLE = 82, FMT_D24FS8 = 83, FMT_INDEX16 = 101, FMT_INDEX32 = 102,
    FMT_Q16W16V16U16 = 110, FMT_R16F = 111, FMT_G16R16F = 112, FMT_A16B16G16R16F = 113, FMT_R32F = 114,
    FMT_G32R32F = 115, FMT_A32B32G32R32F = 116,
};
#define FMT_DXT1 FOURCC('D', 'X', 'T', '1')
#define FMT_DXT2 FOURCC('D', 'X', 'T', '2')
#define FMT_DXT3 FOURCC('D', 'X', 'T', '3')
#define FMT_DXT4 FOURCC('D', 'X', 'T', '4')
#define FMT_DXT5 FOURCC('D', 'X', 'T', '5')
#define FMT_INTZ FOURCC('I', 'N', 'T', 'Z')
#define FMT_NULL FOURCC('N', 'U', 'L', 'L')

/* D3DPOOL, D3DUSAGE */
#define POOL_DEFAULT 0u
#define POOL_MANAGED 1u
#define POOL_SYSTEMMEM 2u
#define USAGE_RENDERTARGET 0x1u
#define USAGE_DEPTHSTENCIL 0x2u
#define USAGE_DYNAMIC 0x200u
#define USAGE_AUTOGENMIPMAP 0x400u

/* D3DRESOURCETYPE */
#define RTYPE_SURFACE 1u
#define RTYPE_VOLUME 2u
#define RTYPE_TEXTURE 3u
#define RTYPE_VOLUMETEXTURE 4u
#define RTYPE_CUBETEXTURE 5u
#define RTYPE_VERTEXBUFFER 6u
#define RTYPE_INDEXBUFFER 7u

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* --- formats ------------------------------------------------------------------------------------ */

static uint32_t fmt_block(uint32_t f) /* bytes per 4x4 block for the compressed formats, else 0 */
{
    return f == FMT_DXT1 ? 8 : (f == FMT_DXT2 || f == FMT_DXT3 || f == FMT_DXT4 || f == FMT_DXT5) ? 16 : 0;
}

static uint32_t fmt_bytes(uint32_t f) /* bytes per pixel, uncompressed */
{
    switch (f)
    {
    case FMT_A8: case FMT_L8: case FMT_A4L4: case FMT_R3G3B2: return 1;
    case FMT_R5G6B5: case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_A8L8: case FMT_V8U8:
    case FMT_L6V5U5: case FMT_D16: case FMT_D16_LOCKABLE: case FMT_D15S1: case FMT_L16: case FMT_R16F:
    case FMT_INDEX16: return 2;
    case FMT_R8G8B8: return 3;
    case FMT_A16B16G16R16: case FMT_A16B16G16R16F: case FMT_G32R32F: case FMT_Q16W16V16U16: return 8;
    case FMT_A32B32G32R32F: return 16;
    case FMT_NULL: return 0;
    default: return 4;
    }
}

static uint32_t fmt_pitch(uint32_t f, uint32_t w)
{
    uint32_t b = fmt_block(f);
    return b ? ((w + 3) / 4 ? (w + 3) / 4 : 1) * b : w * fmt_bytes(f);
}

static uint32_t fmt_rows(uint32_t f, uint32_t h) { return fmt_block(f) ? ((h + 3) / 4 ? (h + 3) / 4 : 1) : h; }

static uint32_t fmt_size(uint32_t f, uint32_t w, uint32_t h) { return fmt_pitch(f, w) * fmt_rows(f, h); }

static uint32_t mip_levels(uint32_t w, uint32_t h, uint32_t d)
{
    uint32_t n = 1;
    while (w > 1 || h > 1 || d > 1)
        w = w > 1 ? w / 2 : 1, h = h > 1 ? h / 2 : 1, d = d > 1 ? d / 2 : 1, n++;
    return n;
}

static uint32_t level_dim(uint32_t v, uint32_t level)
{
    v >>= level;
    return v ? v : 1;
}

/* --- objects ------------------------------------------------------------------------------------ */

enum Kind
{
    K_D3D9, K_DEVICE, K_SWAPCHAIN, K_SURFACE, K_TEXTURE, K_CUBE, K_VOLUME_TEXTURE, K_VOLUME, K_VB, K_IB, K_DECL,
    K_VS, K_PS, K_STATEBLOCK, K_QUERY, K_KINDS
};

static const struct
{
    const char* iface;
    const D3d9Method* methods;
    unsigned count;
} IFACES[K_KINDS] = {
    [K_D3D9] = { "IDirect3D9", IDIRECT3D9_METHODS, IDIRECT3D9_COUNT },
    [K_DEVICE] = { "IDirect3DDevice9", IDIRECT3DDEVICE9_METHODS, IDIRECT3DDEVICE9_COUNT },
    [K_SWAPCHAIN] = { "IDirect3DSwapChain9", IDIRECT3DSWAPCHAIN9_METHODS, IDIRECT3DSWAPCHAIN9_COUNT },
    [K_SURFACE] = { "IDirect3DSurface9", IDIRECT3DSURFACE9_METHODS, IDIRECT3DSURFACE9_COUNT },
    [K_TEXTURE] = { "IDirect3DTexture9", IDIRECT3DTEXTURE9_METHODS, IDIRECT3DTEXTURE9_COUNT },
    [K_CUBE] = { "IDirect3DCubeTexture9", IDIRECT3DCUBETEXTURE9_METHODS, IDIRECT3DCUBETEXTURE9_COUNT },
    [K_VOLUME_TEXTURE] = { "IDirect3DVolumeTexture9", IDIRECT3DVOLUMETEXTURE9_METHODS, IDIRECT3DVOLUMETEXTURE9_COUNT },
    [K_VOLUME] = { "IDirect3DVolume9", IDIRECT3DVOLUME9_METHODS, IDIRECT3DVOLUME9_COUNT },
    [K_VB] = { "IDirect3DVertexBuffer9", IDIRECT3DVERTEXBUFFER9_METHODS, IDIRECT3DVERTEXBUFFER9_COUNT },
    [K_IB] = { "IDirect3DIndexBuffer9", IDIRECT3DINDEXBUFFER9_METHODS, IDIRECT3DINDEXBUFFER9_COUNT },
    [K_DECL] = { "IDirect3DVertexDeclaration9", IDIRECT3DVERTEXDECLARATION9_METHODS, IDIRECT3DVERTEXDECLARATION9_COUNT },
    [K_VS] = { "IDirect3DVertexShader9", IDIRECT3DVERTEXSHADER9_METHODS, IDIRECT3DVERTEXSHADER9_COUNT },
    [K_PS] = { "IDirect3DPixelShader9", IDIRECT3DPIXELSHADER9_METHODS, IDIRECT3DPIXELSHADER9_COUNT },
    [K_STATEBLOCK] = { "IDirect3DStateBlock9", IDIRECT3DSTATEBLOCK9_METHODS, IDIRECT3DSTATEBLOCK9_COUNT },
    [K_QUERY] = { "IDirect3DQuery9", IDIRECT3DQUERY9_METHODS, IDIRECT3DQUERY9_COUNT },
};

#define MAX_OBJS 262144
#define MAX_SUBS 16 /* mip levels of a texture (or 6 faces x levels for a cube, see below) */

struct Block;

typedef struct Obj
{
    int kind; /* -1: free */
    uint32_t refs, guest;
    uint32_t container; /* a level surface's texture (guest), else 0 */
    /* resources */
    uint32_t fmt, usage, pool, w, h, d, levels;
    uint32_t mem, size;  /* guest memory (surfaces, volumes, buffers), allocated on first lock */
    uint32_t level, face; /* a level surface's place in its container */
    uint32_t subs[6 * MAX_SUBS]; /* a texture's level surfaces (cube: face * MAX_SUBS + level) */
    uint32_t lod, autogen;
    uint32_t fvf; /* vertex buffer */
    /* declarations, shaders */
    uint32_t* data;
    uint32_t ndata;
    /* queries */
    uint32_t qtype, issued;
    /* shaders: an FNV-1a hash of the bytecode (the name of its dump) */
    uint32_t hash;
    /* the back end's: a texture (textures, render targets, depth buffers), a shader */
    G9Tex* gt;
    G9Shader* gs;
    uint32_t dirty; /* a level surface or volume written by the CPU since its last upload */
    /* state blocks */
    struct Block* block;
} Obj;

static Obj* g_objs;
static unsigned g_nobjs, g_free_hint;
static uint32_t g_vtbls[K_KINDS];

static uint32_t vtable(int kind)
{
    if (!g_vtbls[kind])
    {
        uint32_t v = gheap_alloc(4u * IFACES[kind].count, 1);
        char full[128];
        for (unsigned i = 0; i < IFACES[kind].count; ++i)
        {
            snprintf(full, sizeof full, "%s::%s", IFACES[kind].iface, IFACES[kind].methods[i].name);
            wr32(v + 4u * i, thunk_for("d3d9.dll", full));
        }
        g_vtbls[kind] = v;
    }
    return g_vtbls[kind];
}

static Obj* obj(uint32_t p)
{
    if (!p || !g_objs)
        return NULL;
    uint32_t i = rd32(p + 4);
    return i < g_nobjs && g_objs[i].guest == p ? &g_objs[i] : NULL;
}

/* A new guest object of `kind` with one reference. */
static Obj* obj_new(int kind)
{
    if (!g_objs)
        g_objs = (Obj*)calloc(MAX_OBJS, sizeof *g_objs);
    unsigned i = g_free_hint;
    while (i < g_nobjs && g_objs[i].kind >= 0)
        i++;
    if (i == g_nobjs)
    {
        if (g_nobjs == MAX_OBJS)
        {
            rt_log("[d3d9] out of objects\n");
            return NULL;
        }
        g_nobjs++;
    }
    g_free_hint = i + 1;
    Obj* o = &g_objs[i];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    o->refs = 1;
    o->guest = gheap_alloc(8, 1);
    wr32(o->guest, vtable(kind));
    wr32(o->guest + 4, i);
    return o;
}

static void obj_free(Obj* o);

static uint32_t addref(uint32_t p)
{
    Obj* o = obj(p);
    if (o && o->container)
        return addref(o->container); /* a level surface's references are its texture's */
    return o ? ++o->refs : 0;
}

static uint32_t release(uint32_t p)
{
    Obj* o = obj(p);
    if (!o || !o->refs)
        return 0;
    /* a level surface's references are its texture's */
    if (o->container)
        return release(o->container);
    if (--o->refs)
        return o->refs;
    obj_free(o);
    return 0;
}

/* A slot that holds a reference (bound textures, streams, targets). */
static void bind(uint32_t* slot, uint32_t p)
{
    if (*slot == p)
        return;
    if (p)
        addref(p);
    uint32_t old = *slot;
    *slot = p;
    if (old)
        release(old);
}

static void obj_free(Obj* o)
{
    for (unsigned i = 0; i < 6 * MAX_SUBS; ++i)
        if (o->subs[i])
        {
            Obj* s = obj(o->subs[i]);
            if (s)
            {
                s->container = 0;
                s->refs = 1;
                release(s->guest);
            }
        }
    if (o->mem)
        gheap_free(o->mem);
    g9_tex_destroy(o->gt);
    g9_shader_destroy(o->gs);
    free(o->data);
    free(o->block);
    gheap_free(o->guest);
    unsigned i = (unsigned)(o - g_objs);
    o->kind = -1;
    o->guest = 0;
    if (i < g_free_hint)
        g_free_hint = i;
}

/* The guest memory of a surface, volume or buffer, made on first use. */
static uint32_t obj_mem(Obj* o)
{
    if (!o->mem && o->size)
        o->mem = gheap_alloc(o->size, 1);
    return o->mem;
}

/* --- IUnknown and IDirect3DResource9, for every interface ------------------------------------------ */

static void sh_QueryInterface(Guest* g) { wr32(ARG(2), 0); RET(E_NOINTERFACE, 3); }
static void sh_AddRef(Guest* g) { RET(addref(ARG(0)), 1); }
static void sh_Release(Guest* g) { RET(release(ARG(0)), 1); }

typedef struct Device Device;
static Device* g_dev;
static uint32_t device_guest(void);

static void sh_GetDevice(Guest* g)
{
    uint32_t d = device_guest();
    if (d)
        addref(d);
    wr32(ARG(1), d);
    RET(d ? D3D_OK : D3DERR_INVALIDCALL, 2);
}

static void sh_SetPrivateData(Guest* g) { RET(D3D_OK, 5); }
static void sh_GetPrivateData(Guest* g) { RET(D3DERR_NOTFOUND, 4); }
static void sh_FreePrivateData(Guest* g) { RET(D3D_OK, 2); }
static void sh_SetPriority(Guest* g) { RET(0, 2); }
static void sh_GetPriority(Guest* g) { RET(0, 1); }
static void sh_PreLoad(Guest* g) { RET(0, 1); }

static void sh_GetType(Guest* g)
{
    Obj* o = obj(ARG(0));
    static const uint32_t types[K_KINDS] = { [K_SURFACE] = RTYPE_SURFACE, [K_VOLUME] = RTYPE_VOLUME,
                                             [K_TEXTURE] = RTYPE_TEXTURE, [K_VOLUME_TEXTURE] = RTYPE_VOLUMETEXTURE,
                                             [K_CUBE] = RTYPE_CUBETEXTURE, [K_VB] = RTYPE_VERTEXBUFFER,
                                             [K_IB] = RTYPE_INDEXBUFFER };
    RET(o ? types[o->kind] : 0, 1);
}

/* --- IDirect3D9 ----------------------------------------------------------------------------------- */

static void desktop_mode(uint32_t* w, uint32_t* h, uint32_t* hz)
{
    user32_desktop_mode(w, h, hz);
    if (!*w || !*h)
        *w = 1920, *h = 1080;
    if (!*hz)
        *hz = 60;
}

/* The display modes offered: common sizes up to the desktop's, then the desktop's own. */
static unsigned modes(uint32_t (*out)[3], unsigned max)
{
    static const uint32_t sizes[][2] = { { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 800 }, { 1280, 1024 },
                                         { 1366, 768 }, { 1440, 900 }, { 1600, 900 }, { 1680, 1050 }, { 1920, 1080 },
                                         { 1920, 1200 }, { 2560, 1440 } };
    uint32_t dw, dh, hz;
    desktop_mode(&dw, &dh, &hz);
    unsigned n = 0;
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0] && n < max; ++i)
        if (sizes[i][0] <= dw && sizes[i][1] <= dh && !(sizes[i][0] == dw && sizes[i][1] == dh))
            out[n][0] = sizes[i][0], out[n][1] = sizes[i][1], out[n][2] = 60, n++;
    if (n < max)
        out[n][0] = dw, out[n][1] = dh, out[n][2] = hz, n++;
    return n;
}

static void write_mode(uint32_t p, uint32_t w, uint32_t h, uint32_t hz)
{
    wr32(p + D3DDISPLAYMODE_Width, w);
    wr32(p + D3DDISPLAYMODE_Height, h);
    wr32(p + D3DDISPLAYMODE_RefreshRate, hz);
    wr32(p + D3DDISPLAYMODE_Format, FMT_X8R8G8B8);
}

static void sh_D3D9_RegisterSoftwareDevice(Guest* g) { RET(D3D_OK, 2); }
static void sh_D3D9_GetAdapterCount(Guest* g) { RET(1, 1); }

/* GetAdapterIdentifier(adapter, flags, D3DADAPTER_IDENTIFIER9*): a GeForce of the game's era. */
static void sh_D3D9_GetAdapterIdentifier(Guest* g)
{
    uint32_t p = ARG(3);
    if (ARG(1) != 0 || !p)
        RET(D3DERR_INVALIDCALL, 4);
    memset(GUEST_PTR(p), 0, D3DADAPTER_IDENTIFIER9_SIZE);
    strcpy((char*)GUEST_PTR(p + D3DADAPTER_IDENTIFIER9_Driver), "nvd3dum.dll");
    strcpy((char*)GUEST_PTR(p + D3DADAPTER_IDENTIFIER9_Description), "NVIDIA GeForce GTX 260 (XIVLegacy)");
    strcpy((char*)GUEST_PTR(p + D3DADAPTER_IDENTIFIER9_DeviceName), "\\\\.\\DISPLAY1");
    wr32(p + D3DADAPTER_IDENTIFIER9_DriverVersion, 0x000F1012u);
    wr32(p + D3DADAPTER_IDENTIFIER9_DriverVersion + 4, 0x00080011u);
    wr32(p + D3DADAPTER_IDENTIFIER9_VendorId, 0x10DE);
    wr32(p + D3DADAPTER_IDENTIFIER9_DeviceId, 0x05E2);
    wr32(p + D3DADAPTER_IDENTIFIER9_Revision, 0xA1);
    static const uint8_t guid[16] = { 0xD7, 0xB7, 0x1E, 0x3E, 0x41, 0xE2, 0x11, 0xCF, 0x96, 0x24, 0x90, 0x1F, 0x5A, 0xC2, 0xC5, 0x35 };
    memcpy(GUEST_PTR(p + D3DADAPTER_IDENTIFIER9_DeviceIdentifier), guid, 16);
    wr32(p + D3DADAPTER_IDENTIFIER9_WHQLLevel, 1);
    RET(D3D_OK, 4);
}

static void sh_D3D9_GetAdapterModeCount(Guest* g)
{
    uint32_t m[16][3];
    RET(ARG(1) == 0 && (ARG(2) == FMT_X8R8G8B8 || ARG(2) == FMT_A8R8G8B8) ? modes(m, 16) : 0, 3);
}

static void sh_D3D9_EnumAdapterModes(Guest* g)
{
    uint32_t m[16][3];
    unsigned n = modes(m, 16);
    if (ARG(1) != 0 || ARG(3) >= n)
        RET(D3DERR_INVALIDCALL, 5);
    write_mode(ARG(4), m[ARG(3)][0], m[ARG(3)][1], m[ARG(3)][2]);
    RET(D3D_OK, 5);
}

static void sh_D3D9_GetAdapterDisplayMode(Guest* g)
{
    uint32_t w, h, hz;
    desktop_mode(&w, &h, &hz);
    write_mode(ARG(2), w, h, hz);
    RET(D3D_OK, 3);
}

static void sh_D3D9_CheckDeviceType(Guest* g) { RET(D3D_OK, 6); }

static int format_supported(uint32_t usage, uint32_t f)
{
    switch (f)
    {
    case FMT_D16: case FMT_D24S8: case FMT_D24X8: case FMT_D32: case FMT_INTZ:
        return !(usage & USAGE_RENDERTARGET);
    case FMT_NULL:
        return (usage & USAGE_RENDERTARGET) != 0;
    case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_R5G6B5: case FMT_A1R5G5B5: case FMT_X1R5G5B5: case FMT_A4R4G4B4:
    case FMT_A2R10G10B10: case FMT_A2B10G10R10: case FMT_A8B8G8R8: case FMT_X8B8G8R8: case FMT_G16R16:
    case FMT_A16B16G16R16: case FMT_R16F: case FMT_G16R16F: case FMT_A16B16G16R16F: case FMT_R32F: case FMT_G32R32F:
    case FMT_A32B32G32R32F:
        return !(usage & USAGE_DEPTHSTENCIL);
    case FMT_A8: case FMT_L8: case FMT_A8L8: case FMT_L16: case FMT_V8U8: case FMT_Q8W8V8U8:
    case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
        return !(usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL));
    default:
        return 0;
    }
}

static void sh_D3D9_CheckDeviceFormat(Guest* g)
{
    uint32_t usage = ARG(4), rtype = ARG(5), f = ARG(6);
    int ok = format_supported(usage, f);
    char fc[5] = { 0 };
    if (f > 0xFF)
        memcpy(fc, &f, 4);
    rt_log("[d3d9] CheckDeviceFormat(usage %#x, type %u, format %s%u) -> %s\n", usage, rtype, fc, fc[0] ? 0 : f,
        ok ? "ok" : "not available");
    RET(ok ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void sh_D3D9_CheckDeviceMultiSampleType(Guest* g)
{
    if (ARG(6))
        wr32(ARG(6), 1);
    RET(ARG(5) <= 1 ? D3D_OK : D3DERR_NOTAVAILABLE, 7);
}

static void sh_D3D9_CheckDepthStencilMatch(Guest* g) { RET(D3D_OK, 6); }
static void sh_D3D9_CheckDeviceFormatConversion(Guest* g) { RET(D3D_OK, 5); }

/* D3DCAPS9 of a Shader Model 3 part. */
static void caps(uint32_t p)
{
    memset(GUEST_PTR(p), 0, D3DCAPS9_SIZE);
    wr32(p + D3DCAPS9_DeviceType, 1);
    wr32(p + D3DCAPS9_Caps2, 0x00020000u | 0x80000000u | 0x20000000u);
    wr32(p + D3DCAPS9_Caps3, 0x00000020u | 0x00000080u);
    wr32(p + D3DCAPS9_PresentationIntervals, 0x80000000u | 0x1u | 0x2u);
    wr32(p + D3DCAPS9_DevCaps, 0x001BBEF0u);
    wr32(p + D3DCAPS9_PrimitiveMiscCaps, 0x0002FCF2u);
    wr32(p + D3DCAPS9_RasterCaps, 0x07732191u);
    wr32(p + D3DCAPS9_ZCmpCaps, 0xFF);
    wr32(p + D3DCAPS9_SrcBlendCaps, 0x3FFF);
    wr32(p + D3DCAPS9_DestBlendCaps, 0x3FFF);
    wr32(p + D3DCAPS9_AlphaCmpCaps, 0xFF);
    wr32(p + D3DCAPS9_ShadeCaps, 0x00084208u);
    wr32(p + D3DCAPS9_TextureCaps, 0x0001ECC5u);
    wr32(p + D3DCAPS9_TextureFilterCaps, 0x03030700u);
    wr32(p + D3DCAPS9_CubeTextureFilterCaps, 0x03030300u);
    wr32(p + D3DCAPS9_VolumeTextureFilterCaps, 0x03030300u);
    wr32(p + D3DCAPS9_TextureAddressCaps, 0x3F);
    wr32(p + D3DCAPS9_VolumeTextureAddressCaps, 0x3F);
    wr32(p + D3DCAPS9_LineCaps, 0x1F);
    wr32(p + D3DCAPS9_MaxTextureWidth, 8192);
    wr32(p + D3DCAPS9_MaxTextureHeight, 8192);
    wr32(p + D3DCAPS9_MaxVolumeExtent, 2048);
    wr32(p + D3DCAPS9_MaxTextureRepeat, 8192);
    wr32(p + D3DCAPS9_MaxTextureAspectRatio, 8192);
    wr32(p + D3DCAPS9_MaxAnisotropy, 16);
    wr32(p + D3DCAPS9_MaxVertexW, f2u(1e10f));
    wr32(p + D3DCAPS9_GuardBandLeft, f2u(-1e8f));
    wr32(p + D3DCAPS9_GuardBandTop, f2u(-1e8f));
    wr32(p + D3DCAPS9_GuardBandRight, f2u(1e8f));
    wr32(p + D3DCAPS9_GuardBandBottom, f2u(1e8f));
    wr32(p + D3DCAPS9_StencilCaps, 0x1FF);
    wr32(p + D3DCAPS9_FVFCaps, 0x00180008u);
    wr32(p + D3DCAPS9_TextureOpCaps, 0x03FEFFFFu);
    wr32(p + D3DCAPS9_MaxTextureBlendStages, 8);
    wr32(p + D3DCAPS9_MaxSimultaneousTextures, 8);
    wr32(p + D3DCAPS9_VertexProcessingCaps, 0x17B);
    wr32(p + D3DCAPS9_MaxActiveLights, 8);
    wr32(p + D3DCAPS9_MaxUserClipPlanes, 8);
    wr32(p + D3DCAPS9_MaxVertexBlendMatrices, 4);
    wr32(p + D3DCAPS9_MaxVertexBlendMatrixIndex, 8);
    wr32(p + D3DCAPS9_MaxPointSize, f2u(8192.0f));
    wr32(p + D3DCAPS9_MaxPrimitiveCount, 0x00FFFFFFu);
    wr32(p + D3DCAPS9_MaxVertexIndex, 0x00FFFFFFu);
    wr32(p + D3DCAPS9_MaxStreams, 16);
    wr32(p + D3DCAPS9_MaxStreamStride, 255);
    wr32(p + D3DCAPS9_VertexShaderVersion, 0xFFFE0300u);
    wr32(p + D3DCAPS9_MaxVertexShaderConst, 256);
    wr32(p + D3DCAPS9_PixelShaderVersion, 0xFFFF0300u);
    wr32(p + D3DCAPS9_PixelShader1xMaxValue, f2u(65504.0f));
    wr32(p + D3DCAPS9_DevCaps2, 0x51);
    wr32(p + D3DCAPS9_NumberOfAdaptersInGroup, 1);
    wr32(p + D3DCAPS9_DeclTypes, 0x30F);
    wr32(p + D3DCAPS9_NumSimultaneousRTs, 4);
    wr32(p + D3DCAPS9_StretchRectFilterCaps, 0x03000300u);
    wr32(p + D3DCAPS9_VS20Caps, 1);
    wr32(p + D3DCAPS9_VS20Caps + 4, 24);
    wr32(p + D3DCAPS9_VS20Caps + 8, 32);
    wr32(p + D3DCAPS9_VS20Caps + 12, 4);
    wr32(p + D3DCAPS9_PS20Caps, 0x1F);
    wr32(p + D3DCAPS9_PS20Caps + 4, 24);
    wr32(p + D3DCAPS9_PS20Caps + 8, 32);
    wr32(p + D3DCAPS9_PS20Caps + 12, 4);
    wr32(p + D3DCAPS9_PS20Caps + 16, 512);
    wr32(p + D3DCAPS9_VertexTextureFilterCaps, 0x03000300u);
    wr32(p + D3DCAPS9_MaxVShaderInstructionsExecuted, 65535);
    wr32(p + D3DCAPS9_MaxPShaderInstructionsExecuted, 65535);
    wr32(p + D3DCAPS9_MaxVertexShader30InstructionSlots, 4096);
    wr32(p + D3DCAPS9_MaxPixelShader30InstructionSlots, 4096);
}

static void sh_D3D9_GetDeviceCaps(Guest* g) { caps(ARG(3)); RET(D3D_OK, 4); }
static void sh_D3D9_GetAdapterMonitor(Guest* g) { RET(0x00010001u, 2); }

/* --- device state ---------------------------------------------------------------------------------- */

#define NRS 256
#define NTSS 33
#define NSTAGES 8
#define NSS 14
#define NSAMP 20 /* 16 pixel samplers, then the 4 vertex samplers (D3DVERTEXTEXTURESAMPLER0 = 257) */
#define NXF 512  /* D3DTS_VIEW 2, PROJECTION 3, TEXTURE0..7 16..23, WORLD 256..511 */
#define NLIGHTS 32
#define VS_F 256
#define PS_F 224

typedef struct State
{
    uint32_t rs[NRS];
    uint32_t tss[NSTAGES][NTSS];
    uint32_t ss[NSAMP][NSS];
    float xf[NXF][16];
    uint32_t viewport[6]; /* X, Y, Width, Height, MinZ, MaxZ (floats in the last two) */
    float material[17];
    uint32_t light[NLIGHTS][D3DLIGHT9_SIZE / 4];
    uint8_t light_set[NLIGHTS], light_on[NLIGHTS];
    float clip[8][4];
    int32_t scissor[4];
    uint32_t stream[16], stream_off[16], stream_stride[16], stream_freq[16];
    uint32_t indices, decl, fvf, vs, ps;
    float vsf[VS_F][4], psf[PS_F][4];
    int32_t vsi[16][4], psi[16][4];
    uint32_t vsb[16], psb[16];
    uint32_t tex[NSAMP];
} State;

/* What a recorded state block holds: one bit per state it captured. */
typedef struct Mask
{
    uint8_t rs[NRS], tss[NSTAGES][NTSS], ss[NSAMP][NSS], xf[NXF], light[NLIGHTS], clip[8];
    uint8_t viewport, material, scissor, stream[16], indices, decl, fvf, vs, ps, vsf[VS_F], psf[PS_F], vsi[16], psi[16],
        vsb[16], psb[16], tex[NSAMP];
} Mask;

typedef struct Block
{
    State s;
    Mask m;
} Block;

struct Device
{
    Obj* o;
    uint32_t window;
    uint32_t pp[D3DPRESENT_PARAMETERS_SIZE / 4];
    uint32_t backbuffer, depth, swapchain;
    uint32_t rt[4], ds;
    State s;
    Block* recording;
    uint32_t in_scene, cursor_shown;
    uint64_t frames, draws, clears;
};

static uint32_t device_guest(void) { return g_dev ? g_dev->o->guest : 0; }

/* index into State.tex / ss for a sampler number (0..15, D3DVERTEXTEXTURESAMPLER0..3 = 257..260) */
static int sampler_index(uint32_t s)
{
    if (s < 16)
        return (int)s;
    if (s >= 257 && s <= 260)
        return (int)(s - 257 + 16);
    return -1;
}

static void identity(float* m)
{
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void state_defaults(State* s, int zbuffer, uint32_t w, uint32_t h)
{
    memset(s, 0, sizeof *s);
    static const struct { uint16_t k; uint32_t v; } rs[] = {
        { 8, 3 }, { 9, 2 }, { 14, 1 }, { 16, 1 }, { 19, 2 }, { 20, 1 }, { 22, 3 }, { 23, 4 }, { 25, 8 }, { 53, 1 },
        { 54, 1 }, { 55, 1 }, { 56, 8 }, { 58, 0xFFFFFFFFu }, { 59, 0xFFFFFFFFu }, { 60, 0xFFFFFFFFu }, { 136, 1 },
        { 137, 1 }, { 141, 1 }, { 142, 1 }, { 145, 1 }, { 146, 2 }, { 161, 1 }, { 162, 0xFFFFFFFFu },
        { 165, 0xBAADCAFEu }, { 168, 0xF }, { 171, 1 }, { 172, 3 }, { 173, 1 }, { 186, 1 }, { 187, 1 }, { 188, 1 },
        { 189, 8 }, { 190, 0xF }, { 191, 0xF }, { 192, 0xF }, { 193, 0xFFFFFFFFu }, { 207, 2 }, { 208, 1 }, { 209, 1 },
    };
    for (unsigned i = 0; i < sizeof rs / sizeof rs[0]; ++i)
        s->rs[rs[i].k] = rs[i].v;
    s->rs[7] = zbuffer ? 1 : 0; /* ZENABLE */
    s->rs[37] = f2u(1.0f);      /* FOGEND */
    s->rs[38] = f2u(1.0f);      /* FOGDENSITY */
    s->rs[154] = f2u(1.0f);     /* POINTSIZE */
    s->rs[155] = f2u(1.0f);     /* POINTSIZE_MIN */
    s->rs[158] = f2u(1.0f);     /* POINTSCALE_A */
    s->rs[166] = f2u(64.0f);    /* POINTSIZE_MAX */
    s->rs[178] = f2u(1.0f);     /* MINTESSELLATIONLEVEL */
    s->rs[179] = f2u(1.0f);     /* MAXTESSELLATIONLEVEL */
    s->rs[182] = f2u(1.0f);     /* ADAPTIVETESS_Z */
    for (int t = 0; t < NSTAGES; ++t)
    {
        s->tss[t][1] = t ? 1 : 4; /* COLOROP: MODULATE on stage 0, DISABLE after */
        s->tss[t][2] = 2;         /* COLORARG1: TEXTURE */
        s->tss[t][3] = 1;         /* COLORARG2: CURRENT */
        s->tss[t][4] = t ? 1 : 2; /* ALPHAOP: SELECTARG1 on stage 0 */
        s->tss[t][5] = 2;
        s->tss[t][6] = 1;
        s->tss[t][11] = (uint32_t)t; /* TEXCOORDINDEX */
        s->tss[t][26] = 1;
        s->tss[t][27] = 1;
        s->tss[t][28] = 1; /* RESULTARG: CURRENT */
    }
    for (int i = 0; i < NSAMP; ++i)
    {
        s->ss[i][1] = s->ss[i][2] = s->ss[i][3] = 1; /* ADDRESSU/V/W: WRAP */
        s->ss[i][5] = s->ss[i][6] = 1;               /* MAG/MINFILTER: POINT */
        s->ss[i][10] = 1;                            /* MAXANISOTROPY */
    }
    for (int i = 0; i < NXF; ++i)
        identity(s->xf[i]);
    s->viewport[2] = w, s->viewport[3] = h, s->viewport[5] = f2u(1.0f);
    s->scissor[2] = (int32_t)w, s->scissor[3] = (int32_t)h;
    for (int i = 0; i < 16; ++i)
        s->stream_freq[i] = 1;
}

/* A setter's effect: on the device, or into the state block being recorded. */
#define TARGET (g_dev->recording ? &g_dev->recording->s : &g_dev->s)
#define MARK(field) do { if (g_dev->recording) g_dev->recording->m.field = 1; } while (0)

static void state_copy(State* to, const State* from, const Mask* m)
{
    for (int i = 0; i < NRS; ++i)
        if (m->rs[i])
            to->rs[i] = from->rs[i];
    for (int t = 0; t < NSTAGES; ++t)
        for (int i = 0; i < NTSS; ++i)
            if (m->tss[t][i])
                to->tss[t][i] = from->tss[t][i];
    for (int t = 0; t < NSAMP; ++t)
        for (int i = 0; i < NSS; ++i)
            if (m->ss[t][i])
                to->ss[t][i] = from->ss[t][i];
    for (int i = 0; i < NXF; ++i)
        if (m->xf[i])
            memcpy(to->xf[i], from->xf[i], 64);
    for (int i = 0; i < NLIGHTS; ++i)
        if (m->light[i])
        {
            memcpy(to->light[i], from->light[i], sizeof to->light[i]);
            to->light_set[i] = from->light_set[i], to->light_on[i] = from->light_on[i];
        }
    for (int i = 0; i < 8; ++i)
        if (m->clip[i])
            memcpy(to->clip[i], from->clip[i], 16);
    if (m->viewport)
        memcpy(to->viewport, from->viewport, sizeof to->viewport);
    if (m->material)
        memcpy(to->material, from->material, sizeof to->material);
    if (m->scissor)
        memcpy(to->scissor, from->scissor, sizeof to->scissor);
    for (int i = 0; i < 16; ++i)
        if (m->stream[i])
        {
            bind(&to->stream[i], from->stream[i]);
            to->stream_off[i] = from->stream_off[i], to->stream_stride[i] = from->stream_stride[i];
            to->stream_freq[i] = from->stream_freq[i];
        }
    if (m->indices)
        bind(&to->indices, from->indices);
    if (m->decl)
        bind(&to->decl, from->decl);
    if (m->fvf)
        to->fvf = from->fvf;
    if (m->vs)
        bind(&to->vs, from->vs);
    if (m->ps)
        bind(&to->ps, from->ps);
    for (int i = 0; i < VS_F; ++i)
        if (m->vsf[i])
            memcpy(to->vsf[i], from->vsf[i], 16);
    for (int i = 0; i < PS_F; ++i)
        if (m->psf[i])
            memcpy(to->psf[i], from->psf[i], 16);
    for (int i = 0; i < 16; ++i)
    {
        if (m->vsi[i])
            memcpy(to->vsi[i], from->vsi[i], 16);
        if (m->psi[i])
            memcpy(to->psi[i], from->psi[i], 16);
        if (m->vsb[i])
            to->vsb[i] = from->vsb[i];
        if (m->psb[i])
            to->psb[i] = from->psb[i];
    }
    for (int i = 0; i < NSAMP; ++i)
        if (m->tex[i])
            bind(&to->tex[i], from->tex[i]);
}

/* --- resources ------------------------------------------------------------------------------------- */

static Obj* new_surface(uint32_t fmt, uint32_t w, uint32_t h, uint32_t usage, uint32_t pool)
{
    Obj* s = obj_new(K_SURFACE);
    if (!s)
        return NULL;
    s->fmt = fmt, s->w = w, s->h = h, s->usage = usage, s->pool = pool;
    s->size = fmt_size(fmt, w, h);
    if (usage & (USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL))
        s->gt = g9_tex_create(G9_TEX_2D, fmt, w, h, 1, 1, usage & USAGE_DEPTHSTENCIL ? G9_USE_DEPTH : G9_USE_RT);
    return s;
}

/* A texture of `kind` (K_TEXTURE, K_CUBE, K_VOLUME_TEXTURE) with its level surfaces / volumes. */
static Obj* new_texture(int kind, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt,
    uint32_t pool)
{
    uint32_t full = mip_levels(w, h, d);
    if (usage & USAGE_AUTOGENMIPMAP)
        levels = 1; /* the level count the game sees; the back end makes the rest */
    if (!levels || levels > full)
        levels = full;
    if (levels > MAX_SUBS)
        levels = MAX_SUBS;
    Obj* t = obj_new(kind);
    if (!t)
        return NULL;
    t->fmt = fmt, t->w = w, t->h = h, t->d = d, t->levels = levels, t->usage = usage, t->pool = pool;
    t->autogen = (usage & USAGE_AUTOGENMIPMAP) ? 2 /* D3DTEXF_LINEAR */ : 0;
    t->gt = g9_tex_create(kind == K_CUBE ? G9_TEX_CUBE : kind == K_VOLUME_TEXTURE ? G9_TEX_3D : G9_TEX_2D, fmt, w, h, d, levels,
        usage & USAGE_DEPTHSTENCIL ? G9_USE_DEPTH : usage & USAGE_RENDERTARGET ? G9_USE_RT : G9_USE_SAMPLE);
    unsigned faces = kind == K_CUBE ? 6 : 1;
    for (unsigned f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < levels; ++l)
        {
            Obj* s;
            if (kind == K_VOLUME_TEXTURE)
            {
                s = obj_new(K_VOLUME);
                if (!s)
                    return t;
                s->fmt = fmt, s->w = level_dim(w, l), s->h = level_dim(h, l), s->d = level_dim(d, l);
                s->usage = usage, s->pool = pool;
                s->size = fmt_size(fmt, s->w, s->h) * s->d;
            }
            else
            {
                /* a level of the texture's own GPU texture: no render target of its own */
                s = new_surface(fmt, level_dim(w, l), level_dim(h, l), usage & ~(USAGE_RENDERTARGET | USAGE_DEPTHSTENCIL), pool);
                if (!s)
                    return t;
                s->usage = usage;
            }
            s->container = t->guest;
            s->level = l, s->face = f;
            s->refs = 0; /* counted on the container */
            t->subs[f * MAX_SUBS + l] = s->guest;
        }
    return t;
}

static void write_surface_desc(uint32_t p, const Obj* s, uint32_t type)
{
    memset(GUEST_PTR(p), 0, D3DSURFACE_DESC_SIZE);
    wr32(p + D3DSURFACE_DESC_Format, s->fmt);
    wr32(p + D3DSURFACE_DESC_Type, type);
    wr32(p + D3DSURFACE_DESC_Usage, s->usage);
    wr32(p + D3DSURFACE_DESC_Pool, s->pool);
    wr32(p + D3DSURFACE_DESC_Width, s->w);
    wr32(p + D3DSURFACE_DESC_Height, s->h);
}

/* Lock a surface (or a rectangle of it): D3DLOCKED_RECT {Pitch, pBits}. */
static uint32_t lock_rect(Obj* s, uint32_t locked, uint32_t rect)
{
    uint32_t mem = obj_mem(s);
    if (!mem && s->size)
        return E_OUTOFMEMORY;
    uint32_t pitch = fmt_pitch(s->fmt, s->w), at = mem;
    if (rect)
    {
        int32_t l = (int32_t)rd32(rect), t = (int32_t)rd32(rect + 4);
        if (fmt_block(s->fmt))
            at += (uint32_t)(t / 4) * pitch + (uint32_t)(l / 4) * fmt_block(s->fmt);
        else
            at += (uint32_t)t * pitch + (uint32_t)l * fmt_bytes(s->fmt);
    }
    wr32(locked + D3DLOCKED_RECT_Pitch, pitch);
    wr32(locked + D3DLOCKED_RECT_pBits, at);
    return D3D_OK;
}

static uint32_t lock_box(Obj* v, uint32_t locked, uint32_t box)
{
    uint32_t mem = obj_mem(v);
    if (!mem && v->size)
        return E_OUTOFMEMORY;
    uint32_t row = fmt_pitch(v->fmt, v->w), slice = row * fmt_rows(v->fmt, v->h), at = mem;
    if (box)
        at += rd32(box + D3DBOX_Front) * slice + rd32(box + D3DBOX_Top) * row + rd32(box + D3DBOX_Left) * fmt_bytes(v->fmt);
    wr32(locked + D3DLOCKED_BOX_RowPitch, row);
    wr32(locked + D3DLOCKED_BOX_SlicePitch, slice);
    wr32(locked + D3DLOCKED_BOX_pBits, at);
    return D3D_OK;
}

/* --- IDirect3DSurface9 ---------------------------------------------------------------------------- */

static void sh_Surface_GetContainer(Guest* g)
{
    Obj* s = obj(ARG(0));
    uint32_t c = s && s->container ? s->container : device_guest();
    if (c)
        addref(c);
    wr32(ARG(2), c);
    RET(c ? D3D_OK : E_NOINTERFACE, 3);
}

static void sh_Surface_GetDesc(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 2);
    write_surface_desc(ARG(1), s, RTYPE_SURFACE);
    RET(D3D_OK, 2);
}

static void sh_Surface_LockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    RET(s ? lock_rect(s, ARG(1), ARG(2)) : D3DERR_INVALIDCALL, 4);
}

static void sh_Surface_UnlockRect(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (s)
        s->dirty = 1;
    RET(D3D_OK, 1);
}
static void sh_Surface_GetDC(Guest* g) { wr32(ARG(1), 0x0D0C0002u); RET(D3D_OK, 2); }
static void sh_Surface_ReleaseDC(Guest* g) { RET(D3D_OK, 2); }

/* --- textures ------------------------------------------------------------------------------------- */

static void sh_Tex_SetLOD(Guest* g)
{
    Obj* t = obj(ARG(0));
    uint32_t old = t ? t->lod : 0;
    if (t && t->pool == POOL_MANAGED)
        t->lod = ARG(1) < t->levels ? ARG(1) : t->levels - 1;
    RET(old, 2);
}

static void sh_Tex_GetLOD(Guest* g) { Obj* t = obj(ARG(0)); RET(t ? t->lod : 0, 1); }
static void sh_Tex_GetLevelCount(Guest* g) { Obj* t = obj(ARG(0)); RET(t ? t->levels : 0, 1); }
static void sh_Tex_SetAutoGenFilterType(Guest* g) { Obj* t = obj(ARG(0)); if (t) t->autogen = ARG(1); RET(D3D_OK, 2); }
static void sh_Tex_GetAutoGenFilterType(Guest* g) { Obj* t = obj(ARG(0)); RET(t ? t->autogen : 0, 1); }
static void sh_Tex_GenerateMipSubLevels(Guest* g) { RET(0, 1); }

static Obj* sub(uint32_t tex, uint32_t face, uint32_t level)
{
    Obj* t = obj(tex);
    if (!t || level >= t->levels || face >= 6)
        return NULL;
    return obj(t->subs[face * MAX_SUBS + level]);
}

/* IDirect3DTexture9 */
static void sh_Tex_GetLevelDesc(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s, RTYPE_SURFACE);
    RET(D3D_OK, 3);
}

static void sh_Tex_GetSurfaceLevel(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
    {
        wr32(ARG(2), 0);
        RET(D3DERR_INVALIDCALL, 3);
    }
    addref(s->guest);
    wr32(ARG(2), s->guest);
    RET(D3D_OK, 3);
}

static void sh_Tex_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    RET(s ? lock_rect(s, ARG(2), ARG(3)) : D3DERR_INVALIDCALL, 5);
}

static void sh_Tex_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (s)
        s->dirty = 1;
    RET(D3D_OK, 2);
}
static void sh_Tex_AddDirtyRect(Guest* g) { RET(D3D_OK, 2); }

/* IDirect3DCubeTexture9 */
static void sh_Cube_GetLevelDesc(Guest* g)
{
    Obj* s = sub(ARG(0), 0, ARG(1));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    write_surface_desc(ARG(2), s, RTYPE_SURFACE);
    RET(D3D_OK, 3);
}

static void sh_Cube_GetCubeMapSurface(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (!s)
    {
        wr32(ARG(3), 0);
        RET(D3DERR_INVALIDCALL, 4);
    }
    addref(s->guest);
    wr32(ARG(3), s->guest);
    RET(D3D_OK, 4);
}

static void sh_Cube_LockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    RET(s ? lock_rect(s, ARG(3), ARG(4)) : D3DERR_INVALIDCALL, 6);
}

static void sh_Cube_UnlockRect(Guest* g)
{
    Obj* s = sub(ARG(0), ARG(1), ARG(2));
    if (s)
        s->dirty = 1;
    RET(D3D_OK, 3);
}
static void sh_Cube_AddDirtyRect(Guest* g) { RET(D3D_OK, 3); }

/* IDirect3DVolumeTexture9 / IDirect3DVolume9 */
static void write_volume_desc(uint32_t p, const Obj* v)
{
    memset(GUEST_PTR(p), 0, D3DVOLUME_DESC_SIZE);
    wr32(p + D3DVOLUME_DESC_Format, v->fmt);
    wr32(p + D3DVOLUME_DESC_Type, RTYPE_VOLUME);
    wr32(p + D3DVOLUME_DESC_Usage, v->usage);
    wr32(p + D3DVOLUME_DESC_Pool, v->pool);
    wr32(p + D3DVOLUME_DESC_Width, v->w);
    wr32(p + D3DVOLUME_DESC_Height, v->h);
    wr32(p + D3DVOLUME_DESC_Depth, v->d);
}

static void sh_VolTex_GetLevelDesc(Guest* g)
{
    Obj* v = sub(ARG(0), 0, ARG(1));
    if (!v)
        RET(D3DERR_INVALIDCALL, 3);
    write_volume_desc(ARG(2), v);
    RET(D3D_OK, 3);
}

static void sh_VolTex_GetVolumeLevel(Guest* g)
{
    Obj* v = sub(ARG(0), 0, ARG(1));
    if (!v)
    {
        wr32(ARG(2), 0);
        RET(D3DERR_INVALIDCALL, 3);
    }
    addref(v->guest);
    wr32(ARG(2), v->guest);
    RET(D3D_OK, 3);
}

static void sh_VolTex_LockBox(Guest* g)
{
    Obj* v = sub(ARG(0), 0, ARG(1));
    RET(v ? lock_box(v, ARG(2), ARG(3)) : D3DERR_INVALIDCALL, 5);
}

static void sh_VolTex_UnlockBox(Guest* g)
{
    Obj* v = sub(ARG(0), 0, ARG(1));
    if (v)
        v->dirty = 1;
    RET(D3D_OK, 2);
}
static void sh_VolTex_AddDirtyBox(Guest* g) { RET(D3D_OK, 2); }

static void sh_Volume_GetContainer(Guest* g)
{
    Obj* v = obj(ARG(0));
    uint32_t c = v ? v->container : 0;
    if (c)
        addref(c);
    wr32(ARG(2), c);
    RET(c ? D3D_OK : E_NOINTERFACE, 3);
}

static void sh_Volume_GetDesc(Guest* g)
{
    Obj* v = obj(ARG(0));
    if (!v)
        RET(D3DERR_INVALIDCALL, 2);
    write_volume_desc(ARG(1), v);
    RET(D3D_OK, 2);
}

static void sh_Volume_LockBox(Guest* g)
{
    Obj* v = obj(ARG(0));
    RET(v ? lock_box(v, ARG(1), ARG(2)) : D3DERR_INVALIDCALL, 4);
}

static void sh_Volume_UnlockBox(Guest* g)
{
    Obj* v = obj(ARG(0));
    if (v)
        v->dirty = 1;
    RET(D3D_OK, 1);
}

/* --- vertex and index buffers ------------------------------------------------------------------------- */

/* Lock(offset, size, void** data, flags) */
static void sh_Buffer_Lock(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b)
        RET(D3DERR_INVALIDCALL, 5);
    uint32_t mem = obj_mem(b);
    if (!mem)
        RET(E_OUTOFMEMORY, 5);
    wr32(ARG(3), mem + (ARG(1) < b->size ? ARG(1) : 0));
    RET(D3D_OK, 5);
}

static void sh_Buffer_Unlock(Guest* g) { RET(D3D_OK, 1); }

static void sh_VB_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    uint32_t p = ARG(1);
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(p + D3DVERTEXBUFFER_DESC_Format, 100); /* D3DFMT_VERTEXDATA */
    wr32(p + D3DVERTEXBUFFER_DESC_Type, RTYPE_VERTEXBUFFER);
    wr32(p + D3DVERTEXBUFFER_DESC_Usage, b->usage);
    wr32(p + D3DVERTEXBUFFER_DESC_Pool, b->pool);
    wr32(p + D3DVERTEXBUFFER_DESC_Size, b->size);
    wr32(p + D3DVERTEXBUFFER_DESC_FVF, b->fvf);
    RET(D3D_OK, 2);
}

static void sh_IB_GetDesc(Guest* g)
{
    Obj* b = obj(ARG(0));
    uint32_t p = ARG(1);
    if (!b)
        RET(D3DERR_INVALIDCALL, 2);
    wr32(p + D3DINDEXBUFFER_DESC_Format, b->fmt);
    wr32(p + D3DINDEXBUFFER_DESC_Type, RTYPE_INDEXBUFFER);
    wr32(p + D3DINDEXBUFFER_DESC_Usage, b->usage);
    wr32(p + D3DINDEXBUFFER_DESC_Pool, b->pool);
    wr32(p + D3DINDEXBUFFER_DESC_Size, b->size);
    RET(D3D_OK, 2);
}

/* --- declarations, shaders, queries, state blocks ----------------------------------------------------- */

/* GetDeclaration(D3DVERTEXELEMENT9* elements, UINT* count): count includes D3DDECL_END */
static void sh_Decl_GetDeclaration(Guest* g)
{
    Obj* d = obj(ARG(0));
    if (!d)
        RET(D3DERR_INVALIDCALL, 3);
    if (ARG(1))
        memcpy(GUEST_PTR(ARG(1)), d->data, d->ndata * 4u);
    wr32(ARG(2), d->ndata / 2);
    RET(D3D_OK, 3);
}

/* GetFunction(void* data, UINT* size) */
static void sh_Shader_GetFunction(Guest* g)
{
    Obj* s = obj(ARG(0));
    if (!s)
        RET(D3DERR_INVALIDCALL, 3);
    if (ARG(1))
        memcpy(GUEST_PTR(ARG(1)), s->data, s->ndata * 4u);
    wr32(ARG(2), s->ndata * 4u);
    RET(D3D_OK, 3);
}

/* D3DQUERYTYPE: EVENT 8, OCCLUSION 9, TIMESTAMP 10, TIMESTAMPDISJOINT 11, TIMESTAMPFREQ 12 */
static uint32_t query_size(uint32_t type)
{
    switch (type)
    {
    case 8: case 9: return 4;
    case 10: case 12: return 8;
    case 11: return 4;
    default: return 0;
    }
}

static void sh_Query_GetType(Guest* g) { Obj* q = obj(ARG(0)); RET(q ? q->qtype : 0, 1); }
static void sh_Query_GetDataSize(Guest* g) { Obj* q = obj(ARG(0)); RET(q ? query_size(q->qtype) : 0, 1); }
static void sh_Query_Issue(Guest* g) { Obj* q = obj(ARG(0)); if (q) q->issued = 1; RET(D3D_OK, 2); }

/* GetData(void* data, DWORD size, DWORD flags): every query is done at once (no GPU behind yet) */
static void sh_Query_GetData(Guest* g)
{
    Obj* q = obj(ARG(0));
    uint32_t p = ARG(1), n = ARG(2);
    if (!q)
        RET(D3DERR_INVALIDCALL, 4);
    if (p && n)
    {
        uint64_t v = 0;
        switch (q->qtype)
        {
        case 8: v = 1; break;          /* EVENT: done */
        case 9: v = 1000; break;       /* OCCLUSION: some pixels drawn */
        case 10: v = rt_monotonic_ns(); break;
        case 12: v = 1000000000ull; break;
        default: break;
        }
        memcpy(GUEST_PTR(p), &v, n < 8 ? n : 8);
    }
    RET(D3D_OK, 4);
}

static void sh_StateBlock_Capture(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b || !b->block || !g_dev)
        RET(D3DERR_INVALIDCALL, 1);
    State s = b->block->s; /* bindings are copied with references: start from the block's own */
    state_copy(&b->block->s, &g_dev->s, &b->block->m);
    (void)s;
    RET(D3D_OK, 1);
}

static void sh_StateBlock_Apply(Guest* g)
{
    Obj* b = obj(ARG(0));
    if (!b || !b->block || !g_dev)
        RET(D3DERR_INVALIDCALL, 1);
    state_copy(&g_dev->s, &b->block->s, &b->block->m);
    RET(D3D_OK, 1);
}

/* --- the device ----------------------------------------------------------------------------------- */

static void make_targets(void)
{
    Device* d = g_dev;
    uint32_t w = d->pp[0], h = d->pp[1], fmt = d->pp[2];
    Obj* bb = new_surface(fmt ? fmt : FMT_X8R8G8B8, w, h, USAGE_RENDERTARGET, POOL_DEFAULT);
    d->backbuffer = bb ? bb->guest : 0;
    d->depth = 0;
    if (d->pp[D3DPRESENT_PARAMETERS_EnableAutoDepthStencil / 4])
    {
        Obj* ds = new_surface(d->pp[D3DPRESENT_PARAMETERS_AutoDepthStencilFormat / 4], w, h, USAGE_DEPTHSTENCIL, POOL_DEFAULT);
        d->depth = ds ? ds->guest : 0;
    }
    memset(d->rt, 0, sizeof d->rt);
    d->ds = 0;
    bind(&d->rt[0], d->backbuffer);
    bind(&d->ds, d->depth);
}

/* --- to the back end ---------------------------------------------------------------------------- */

/* a surface as the back end sees it: its own texture, or its container's at its face and level */
static G9Target target_of(uint32_t p)
{
    G9Target t;
    memset(&t, 0, sizeof t);
    Obj* s = obj(p);
    if (!s)
        return t;
    if (s->container)
    {
        Obj* c = obj(s->container);
        if (c)
            t.tex = c->gt, t.face = s->face, t.level = s->level;
    }
    else
        t.tex = s->gt;
    return t;
}

static void sync_targets(void)
{
    G9Target rt[4];
    for (int i = 0; i < 4; ++i)
        rt[i] = target_of(g_dev->rt[i]);
    g9_set_targets(rt, target_of(g_dev->ds).tex);
}

/* what the CPU wrote into a texture's levels, to its GPU texture */
static void upload(Obj* t)
{
    if (!t || !t->gt)
        return;
    unsigned faces = t->kind == K_CUBE ? 6 : 1;
    for (unsigned f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < t->levels; ++l)
        {
            Obj* s = obj(t->subs[f * MAX_SUBS + l]);
            if (!s || !s->dirty || !s->mem)
                continue;
            uint32_t pitch = fmt_pitch(s->fmt, s->w);
            g9_tex_upload(t->gt, f, l, GUEST_PTR(s->mem), pitch, pitch * fmt_rows(s->fmt, s->h));
            s->dirty = 0;
        }
}

/* a flexible vertex format as declaration elements (stream 0) */
static uint32_t fvf_elems(uint32_t fvf, G9Elem* e)
{
    uint32_t n = 0, off = 0, pos = fvf & 0x400E;
    if (pos == 0x002)
        e[n++] = (G9Elem){ 0, 2, 0, 0, (uint16_t)off }, off += 12;
    else if (pos == 0x004)
        e[n++] = (G9Elem){ 0, 3, 9, 0, (uint16_t)off }, off += 16; /* POSITIONT */
    else if (pos == 0x4002)
        e[n++] = (G9Elem){ 0, 3, 0, 0, (uint16_t)off }, off += 16;
    else if (pos >= 0x006 && pos <= 0x00E)
    {
        uint32_t weights = (pos - 4) / 2;
        e[n++] = (G9Elem){ 0, 2, 0, 0, (uint16_t)off }, off += 12;
        if (weights)
            e[n++] = (G9Elem){ 0, (uint8_t)(weights - 1), 1, 0, (uint16_t)off }, off += 4 * weights;
    }
    if (fvf & 0x010)
        e[n++] = (G9Elem){ 0, 2, 3, 0, (uint16_t)off }, off += 12;
    if (fvf & 0x020)
        e[n++] = (G9Elem){ 0, 0, 4, 0, (uint16_t)off }, off += 4;
    if (fvf & 0x040)
        e[n++] = (G9Elem){ 0, 4, 10, 0, (uint16_t)off }, off += 4;
    if (fvf & 0x080)
        e[n++] = (G9Elem){ 0, 4, 10, 1, (uint16_t)off }, off += 4;
    uint32_t ntex = (fvf >> 8) & 15;
    for (uint32_t t = 0; t < ntex && n < 16; ++t)
    {
        uint32_t code = (fvf >> (16 + 2 * t)) & 3;
        static const uint8_t type[4] = { 1, 2, 3, 0 }, size[4] = { 8, 12, 16, 4 };
        e[n++] = (G9Elem){ 0, type[code], 5, (uint8_t)t, (uint16_t)off };
        off += size[code];
    }
    return n;
}

/* the draw's state, without its vertex and index data; 0 when nothing can be drawn */
static int draw_state(G9Draw* d, uint32_t prim, uint32_t prims)
{
    memset(d, 0, sizeof *d);
    State* st = &g_dev->s;
    Obj* vs = obj(st->vs);
    Obj* ps = obj(st->ps);
    if (!g9_active() || !vs || !ps || !vs->gs || !ps->gs)
        return 0;
    d->vs = vs->gs, d->ps = ps->gs;
    d->prim = prim, d->prims = prims;
    Obj* decl = obj(st->decl);
    if (decl)
    {
        for (uint32_t i = 0; i < decl->ndata / 2 && d->nelems < 16; ++i)
        {
            const uint8_t* el = (const uint8_t*)&decl->data[2 * i];
            uint16_t stream = (uint16_t)(el[0] | el[1] << 8);
            if (stream == 0xFF)
                break;
            d->elems[d->nelems++] = (G9Elem){ (uint8_t)stream, el[4], el[6], el[7], (uint16_t)(el[2] | el[3] << 8) };
        }
    }
    else if (st->fvf)
        d->nelems = fvf_elems(st->fvf, d->elems);
    for (int n = 0; n < NSAMP; ++n)
    {
        Obj* t = obj(st->tex[n]);
        if (t)
        {
            upload(t);
            d->tex[n] = t->gt;
        }
    }
    d->ss = (const uint32_t(*)[14])st->ss;
    d->rs = st->rs;
    d->vsc = &st->vsf[0][0], d->vsi = &st->vsi[0][0], d->vsb = st->vsb;
    d->psc = &st->psf[0][0], d->psi = &st->psi[0][0], d->psb = st->psb;
    memcpy(d->viewport, st->viewport, sizeof d->viewport);
    memcpy(d->scissor, st->scissor, sizeof d->scissor);
    d->instances = (st->stream_freq[0] & 0x40000000u) ? (st->stream_freq[0] & 0x3FFFFFFFu) : 1;
    if (!d->instances)
        d->instances = 1;
    sync_targets();
    return 1;
}

/* the bound streams from vertex `first`, `count` vertices (instanced streams: every instance) */
static void stream_range(G9Draw* d, uint32_t first, uint32_t count)
{
    State* st = &g_dev->s;
    for (int i = 0; i < 16; ++i)
    {
        Obj* vb = obj(st->stream[i]);
        if (!vb)
            continue;
        uint32_t mem = obj_mem(vb), stride = st->stream_stride[i];
        if (!mem)
            continue;
        int inst = (st->stream_freq[i] & 0x80000000u) != 0;
        uint32_t start = st->stream_off[i] + (inst ? 0 : first * stride);
        uint32_t size = (inst ? d->instances : count) * stride;
        if (start >= vb->size)
            continue;
        if (!size || start + size > vb->size)
            size = vb->size - start;
        d->streams[i] = (G9Stream){ GUEST_PTR(mem + start), size, stride, inst ? (st->stream_freq[i] & 0x7FFFFFFFu) : 0 };
    }
}

static uint32_t prim_vertices(uint32_t prim, uint32_t n)
{
    switch (prim)
    {
    case 1: return n;
    case 2: return n * 2;
    case 3: return n + 1;
    case 4: return n * 3;
    case 5: case 6: return n + 2;
    default: return 0;
    }
}

/* CreateDevice(adapter, type, focus_hwnd, behavior, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**) */
static void sh_D3D9_CreateDevice(Guest* g)
{
    uint32_t hwnd = ARG(3), pp = ARG(5), out = ARG(6);
    if (g_dev)
    {
        rt_log("[d3d9] CreateDevice: a device exists already\n");
        RET(D3DERR_NOTAVAILABLE, 7);
    }
    uint32_t window = rd32(pp + D3DPRESENT_PARAMETERS_hDeviceWindow) ? rd32(pp + D3DPRESENT_PARAMETERS_hDeviceWindow) : hwnd;
    uint32_t w = rd32(pp + D3DPRESENT_PARAMETERS_BackBufferWidth), h = rd32(pp + D3DPRESENT_PARAMETERS_BackBufferHeight);
    if (!w || !h)
    {
        user32_client_size(window, &w, &h);
        wr32(pp + D3DPRESENT_PARAMETERS_BackBufferWidth, w);
        wr32(pp + D3DPRESENT_PARAMETERS_BackBufferHeight, h);
    }
    if (rd32(pp + D3DPRESENT_PARAMETERS_BackBufferFormat) == 0)
        wr32(pp + D3DPRESENT_PARAMETERS_BackBufferFormat, FMT_X8R8G8B8);
    if (!rd32(pp + D3DPRESENT_PARAMETERS_BackBufferCount))
        wr32(pp + D3DPRESENT_PARAMETERS_BackBufferCount, 1);
    g_dev = (Device*)calloc(1, sizeof *g_dev);
    memcpy(g_dev->pp, GUEST_PTR(pp), sizeof g_dev->pp);
    g_dev->window = window;
    g_dev->o = obj_new(K_DEVICE);
    g_dev->cursor_shown = 1;
    g9_init(user32_sdl_window(window), rd32(pp + D3DPRESENT_PARAMETERS_PresentationInterval) != 0x80000000u);
    Obj* sc = obj_new(K_SWAPCHAIN);
    g_dev->swapchain = sc ? sc->guest : 0;
    make_targets();
    state_defaults(&g_dev->s, rd32(pp + D3DPRESENT_PARAMETERS_EnableAutoDepthStencil) != 0, w, h);
    rt_log("[d3d9] CreateDevice: %ux%u format %u, %s, depth %s (format %u), behavior %#x, window %08x\n", w, h,
        rd32(pp + D3DPRESENT_PARAMETERS_BackBufferFormat), rd32(pp + D3DPRESENT_PARAMETERS_Windowed) ? "windowed" : "full screen",
        rd32(pp + D3DPRESENT_PARAMETERS_EnableAutoDepthStencil) ? "auto" : "none",
        rd32(pp + D3DPRESENT_PARAMETERS_AutoDepthStencilFormat), ARG(4), window);
    wr32(out, g_dev->o->guest);
    RET(D3D_OK, 7);
}

static void sh_Dev_TestCooperativeLevel(Guest* g) { RET(D3D_OK, 1); }
static void sh_Dev_GetAvailableTextureMem(Guest* g) { RET(0x40000000u, 1); } /* 1 GB */
static void sh_Dev_EvictManagedResources(Guest* g) { RET(D3D_OK, 1); }

static uint32_t g_d3d9_obj;

static void sh_Dev_GetDirect3D(Guest* g)
{
    if (g_d3d9_obj)
        addref(g_d3d9_obj);
    wr32(ARG(1), g_d3d9_obj);
    RET(g_d3d9_obj ? D3D_OK : D3DERR_INVALIDCALL, 2);
}

static void sh_Dev_GetDeviceCaps(Guest* g) { caps(ARG(1)); RET(D3D_OK, 2); }

static void sh_Dev_GetDisplayMode(Guest* g)
{
    uint32_t w, h, hz;
    desktop_mode(&w, &h, &hz);
    if (g_dev->pp[D3DPRESENT_PARAMETERS_Windowed / 4] == 0)
        w = g_dev->pp[0], h = g_dev->pp[1];
    write_mode(ARG(2), w, h, hz);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetCreationParameters(Guest* g)
{
    uint32_t p = ARG(1);
    wr32(p + D3DDEVICE_CREATION_PARAMETERS_AdapterOrdinal, 0);
    wr32(p + D3DDEVICE_CREATION_PARAMETERS_DeviceType, 1);
    wr32(p + D3DDEVICE_CREATION_PARAMETERS_hFocusWindow, g_dev->window);
    wr32(p + D3DDEVICE_CREATION_PARAMETERS_BehaviorFlags, 0x54);
    RET(D3D_OK, 2);
}

static void sh_Dev_SetCursorProperties(Guest* g) { RET(D3D_OK, 4); }
static void sh_Dev_SetCursorPosition(Guest* g) { RET(0, 4); }
static void sh_Dev_ShowCursor(Guest* g) { uint32_t o = g_dev->cursor_shown; g_dev->cursor_shown = ARG(1); RET(o, 2); }
static void sh_Dev_CreateAdditionalSwapChain(Guest* g) { wr32(ARG(2), 0); RET(D3DERR_NOTAVAILABLE, 3); }

static void sh_Dev_GetSwapChain(Guest* g)
{
    if (ARG(1) != 0)
        RET(D3DERR_INVALIDCALL, 3);
    addref(g_dev->swapchain);
    wr32(ARG(2), g_dev->swapchain);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetNumberOfSwapChains(Guest* g) { RET(1, 1); }

/* Reset(D3DPRESENT_PARAMETERS*): new back buffer and depth, state to defaults. */
static void sh_Dev_Reset(Guest* g)
{
    uint32_t pp = ARG(1);
    uint32_t w = rd32(pp + D3DPRESENT_PARAMETERS_BackBufferWidth), h = rd32(pp + D3DPRESENT_PARAMETERS_BackBufferHeight);
    if (!w || !h)
    {
        user32_client_size(g_dev->window, &w, &h);
        wr32(pp + D3DPRESENT_PARAMETERS_BackBufferWidth, w);
        wr32(pp + D3DPRESENT_PARAMETERS_BackBufferHeight, h);
    }
    memcpy(g_dev->pp, GUEST_PTR(pp), sizeof g_dev->pp);
    for (int i = 0; i < 4; ++i)
        bind(&g_dev->rt[i], 0);
    bind(&g_dev->ds, 0);
    if (g_dev->backbuffer)
        release(g_dev->backbuffer);
    if (g_dev->depth)
        release(g_dev->depth);
    make_targets();
    /* bindings the state held go with the old state */
    State fresh;
    state_defaults(&fresh, rd32(pp + D3DPRESENT_PARAMETERS_EnableAutoDepthStencil) != 0, w, h);
    Mask all;
    memset(&all, 1, sizeof all);
    state_copy(&g_dev->s, &fresh, &all);
    rt_log("[d3d9] Reset: %ux%u %s\n", w, h, rd32(pp + D3DPRESENT_PARAMETERS_Windowed) ? "windowed" : "full screen");
    RET(D3D_OK, 2);
}

/* the test host's hitch profiler, when there is one (tests/boot.c): what the drawing thread did over a
 * slow frame's span */
extern void xiv_on_hitch(uint64_t from_ns, uint64_t to_ns) __attribute__((weak));
uint32_t d3d9_drawer_tid;
pthread_t d3d9_drawer_thread;

/* XIVLEGACY_WAITPROF=1: every 2 s, where the drawing thread (the one presenting) spent its time,
 * per import (Sleep, the waits, PeekMessage, Present itself...), on any host. */
static void waitprof_none(uint64_t ns) { (void)ns; }
static void waitprof_frame(void)
{
    static int on = -1;
    static uint64_t last, frames;
    if (on < 0)
        on = getenv("XIVLEGACY_WAITPROF") != NULL;
    if (!on)
        return;
    thunk_prof_thread = plat_thread_id();
    thunk_timer = waitprof_none;
    uint64_t now = rt_monotonic_ns();
    frames++;
    if (!last)
        last = now;
    else if (now - last >= 2000000000ull)
    {
        rt_log("[waitprof] %llu frames in %.1f s\n", (unsigned long long)frames, (double)(now - last) * 1e-9);
        thunk_prof_report();
        last = now, frames = 0;
    }
}

static void present(void)
{
    waitprof_frame();
    if (!d3d9_drawer_tid)
        d3d9_drawer_thread = pthread_self();
    d3d9_drawer_tid = plat_thread_id();
    g_dev->frames++;
    /* the thread that presents gets the game thread's turn with the guest lock */
    static _Thread_local int marked;
    if (!marked)
    {
        marked = 1;
        gt_mark_game_thread();
    }
    xiv_game_log_poll();
    xiv_loading_poll();
    /* XIVLEGACY_FRAMESTATS: draws and clears per frame, every 60 frames */
    {
        static int on = -1;
        static uint64_t last_draws, last_clears, last_ns;
        if (on < 0)
            on = getenv("XIVLEGACY_FRAMESTATS") != NULL;
        if (on && g_dev->frames % 60 == 0)
        {
            uint64_t now = rt_monotonic_ns();
            rt_log("[d3d9] frames %llu-%llu: %.1f fps, %.1f draws, %.1f clears a frame\n", (unsigned long long)g_dev->frames - 59,
                (unsigned long long)g_dev->frames, last_ns ? 60e9 / (double)(now - last_ns) : 0.0, (g_dev->draws - last_draws) / 60.0,
                (g_dev->clears - last_clears) / 60.0);
            last_draws = g_dev->draws, last_clears = g_dev->clears, last_ns = now;
        }
    }
    /* XIVLEGACY_FRAMESTATS: each slow frame (over 45 ms) with what happened during it */
    {
        static int on = -1;
        static uint64_t last_ns, last_other, last_draws, last_wait, last_read, last_rbytes;
        if (on < 0)
            on = getenv("XIVLEGACY_FRAMESTATS") != NULL;
        uint64_t now = rt_monotonic_ns(), other = gt_other_held_ns();
        uint32_t nc, np, nu;
        uint64_t cns, pns, ub, uns;
        g9_frame_costs(&nc, &cns, &np, &pns, &nu, &ub, &uns);
        uint64_t wait, rd, rbytes;
        gt_game_costs(&wait, &rd, &rbytes);
        if (on && last_ns && now - last_ns > 45000000ull && xiv_on_hitch)
            xiv_on_hitch(last_ns, now);
        if (on && last_ns && now - last_ns > 45000000ull)
            rt_log("[hitch] frame %llu: %.0f ms; other threads held the lock %.0f ms; %u shader compiles %.0f ms, %u pipelines %.0f ms, "
                   "%u uploads %.1f MB %.0f ms; %llu draws; this thread waited for the lock %.0f ms, read files %.0f ms (%.1f MB)\n",
                (unsigned long long)g_dev->frames, (now - last_ns) / 1e6, (other - last_other) / 1e6, nc, cns / 1e6, np, pns / 1e6, nu,
                ub / 1048576.0, uns / 1e6, (unsigned long long)(g_dev->draws - last_draws), (wait - last_wait) / 1e6, (rd - last_read) / 1e6,
                (rbytes - last_rbytes) / 1048576.0);
        last_ns = now, last_other = other, last_draws = g_dev->draws, last_wait = wait, last_read = rd, last_rbytes = rbytes;
    }
    Obj* bb = obj(g_dev->backbuffer);
    g9_present(bb ? bb->gt : NULL);
    if (g_dev->frames == 1 || (g_dev->frames & 1023) == 0)
        rt_log("[d3d9] frame %llu: %llu draws, %llu clears so far\n", (unsigned long long)g_dev->frames,
            (unsigned long long)g_dev->draws, (unsigned long long)g_dev->clears);
}

static void sh_Dev_Present(Guest* g) { present(); RET(D3D_OK, 5); }

static void sh_Dev_GetBackBuffer(Guest* g)
{
    if (ARG(1) != 0 || ARG(2) != 0)
    {
        wr32(ARG(4), 0);
        RET(D3DERR_INVALIDCALL, 5);
    }
    addref(g_dev->backbuffer);
    wr32(ARG(4), g_dev->backbuffer);
    RET(D3D_OK, 5);
}

static void sh_Dev_GetRasterStatus(Guest* g)
{
    uint32_t p = ARG(2);
    wr32(p + D3DRASTER_STATUS_InVBlank, 0);
    wr32(p + D3DRASTER_STATUS_ScanLine, (uint32_t)(rt_monotonic_ns() / 15000u) % (g_dev->pp[1] ? g_dev->pp[1] : 720));
    RET(D3D_OK, 3);
}

static void sh_Dev_SetDialogBoxMode(Guest* g) { RET(D3D_OK, 2); }
static void sh_Dev_SetGammaRamp(Guest* g) { RET(0, 4); }

static void sh_Dev_GetGammaRamp(Guest* g)
{
    uint32_t p = ARG(2);
    for (uint32_t c = 0; c < 3; ++c)
        for (uint32_t i = 0; i < 256; ++i)
            wr16(p + c * 512 + 2 * i, (uint16_t)(i * 257));
    RET(0, 3);
}

/* CreateTexture(w, h, levels, usage, format, pool, IDirect3DTexture9**, HANDLE*) */
static void sh_Dev_CreateTexture(Guest* g)
{
    Obj* t = new_texture(K_TEXTURE, ARG(1), ARG(2), 1, ARG(3), ARG(4), ARG(5), ARG(6));
    wr32(ARG(7), t ? t->guest : 0);
    RET(t ? D3D_OK : E_OUTOFMEMORY, 9);
}

/* CreateVolumeTexture(w, h, d, levels, usage, format, pool, IDirect3DVolumeTexture9**, HANDLE*) */
static void sh_Dev_CreateVolumeTexture(Guest* g)
{
    Obj* t = new_texture(K_VOLUME_TEXTURE, ARG(1), ARG(2), ARG(3), ARG(4), ARG(5), ARG(6), ARG(7));
    wr32(ARG(8), t ? t->guest : 0);
    RET(t ? D3D_OK : E_OUTOFMEMORY, 10);
}

/* CreateCubeTexture(edge, levels, usage, format, pool, IDirect3DCubeTexture9**, HANDLE*) */
static void sh_Dev_CreateCubeTexture(Guest* g)
{
    Obj* t = new_texture(K_CUBE, ARG(1), ARG(1), 1, ARG(2), ARG(3), ARG(4), ARG(5));
    wr32(ARG(6), t ? t->guest : 0);
    RET(t ? D3D_OK : E_OUTOFMEMORY, 8);
}

/* CreateVertexBuffer(length, usage, fvf, pool, IDirect3DVertexBuffer9**, HANDLE*) */
static void sh_Dev_CreateVertexBuffer(Guest* g)
{
    Obj* b = obj_new(K_VB);
    if (b)
        b->size = ARG(1), b->usage = ARG(2), b->fvf = ARG(3), b->pool = ARG(4);
    wr32(ARG(5), b ? b->guest : 0);
    RET(b ? D3D_OK : E_OUTOFMEMORY, 7);
}

/* CreateIndexBuffer(length, usage, format, pool, IDirect3DIndexBuffer9**, HANDLE*) */
static void sh_Dev_CreateIndexBuffer(Guest* g)
{
    Obj* b = obj_new(K_IB);
    if (b)
        b->size = ARG(1), b->usage = ARG(2), b->fmt = ARG(3), b->pool = ARG(4);
    wr32(ARG(5), b ? b->guest : 0);
    RET(b ? D3D_OK : E_OUTOFMEMORY, 7);
}

/* CreateRenderTarget(w, h, format, ms, msq, lockable, IDirect3DSurface9**, HANDLE*) */
static void sh_Dev_CreateRenderTarget(Guest* g)
{
    Obj* s = new_surface(ARG(3), ARG(1), ARG(2), USAGE_RENDERTARGET, POOL_DEFAULT);
    wr32(ARG(7), s ? s->guest : 0);
    RET(s ? D3D_OK : E_OUTOFMEMORY, 9);
}

/* CreateDepthStencilSurface(w, h, format, ms, msq, discard, IDirect3DSurface9**, HANDLE*) */
static void sh_Dev_CreateDepthStencilSurface(Guest* g)
{
    Obj* s = new_surface(ARG(3), ARG(1), ARG(2), USAGE_DEPTHSTENCIL, POOL_DEFAULT);
    wr32(ARG(7), s ? s->guest : 0);
    RET(s ? D3D_OK : E_OUTOFMEMORY, 9);
}

/* CreateOffscreenPlainSurface(w, h, format, pool, IDirect3DSurface9**, HANDLE*) */
static void sh_Dev_CreateOffscreenPlainSurface(Guest* g)
{
    Obj* s = new_surface(ARG(3), ARG(1), ARG(2), 0, ARG(4));
    wr32(ARG(5), s ? s->guest : 0);
    RET(s ? D3D_OK : E_OUTOFMEMORY, 7);
}

/* Copies between surfaces (CPU side for now: the GPU copy comes with the back end). */
static void copy_surface(Obj* src, uint32_t srect, Obj* dst, uint32_t dx, uint32_t dy)
{
    if (!src || !dst || src->fmt != dst->fmt || fmt_block(src->fmt))
        return;
    uint32_t sx = 0, sy = 0, w = src->w, h = src->h;
    if (srect)
    {
        sx = rd32(srect), sy = rd32(srect + 4);
        w = rd32(srect + 8) - sx, h = rd32(srect + 12) - sy;
    }
    if (dx + w > dst->w || dy + h > dst->h)
        return;
    uint32_t bpp = fmt_bytes(src->fmt), sp = fmt_pitch(src->fmt, src->w), dp = fmt_pitch(dst->fmt, dst->w);
    uint32_t sm = obj_mem(src), dm = obj_mem(dst);
    if (!sm || !dm)
        return;
    for (uint32_t y = 0; y < h; ++y)
        memmove(GUEST_PTR(dm + (dy + y) * dp + dx * bpp), GUEST_PTR(sm + (sy + y) * sp + sx * bpp), (size_t)w * bpp);
}

/* UpdateSurface(src, src_rect, dst, dst_point) */
static void sh_Dev_UpdateSurface(Guest* g)
{
    uint32_t pt = ARG(4);
    Obj* dst = obj(ARG(3));
    copy_surface(obj(ARG(1)), ARG(2), dst, pt ? rd32(pt) : 0, pt ? rd32(pt + 4) : 0);
    if (dst)
        dst->dirty = 1;
    RET(D3D_OK, 5);
}

/* UpdateTexture(src, dst): level by level */
static void sh_Dev_UpdateTexture(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(2));
    if (!s || !d || s->kind != d->kind)
        RET(D3DERR_INVALIDCALL, 3);
    unsigned faces = s->kind == K_CUBE ? 6 : 1;
    for (unsigned f = 0; f < faces; ++f)
        for (uint32_t l = 0; l < s->levels && l < d->levels; ++l)
        {
            Obj* a = obj(s->subs[f * MAX_SUBS + l]);
            Obj* b = obj(d->subs[f * MAX_SUBS + l]);
            if (a && b && a->size == b->size && obj_mem(a) && obj_mem(b))
            {
                memcpy(GUEST_PTR(b->mem), GUEST_PTR(a->mem), a->size);
                b->dirty = 1;
            }
        }
    RET(D3D_OK, 3);
}

/* GetRenderTargetData(render_target, system_memory_surface) */
static void sh_Dev_GetRenderTargetData(Guest* g)
{
    G9Target t = target_of(ARG(1));
    Obj* d = obj(ARG(2));
    if (t.tex && d && obj_mem(d))
        g9_tex_read(t.tex, t.face, t.level, GUEST_PTR(d->mem), fmt_pitch(d->fmt, d->w));
    else
        copy_surface(obj(ARG(1)), 0, d, 0, 0);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetFrontBufferData(Guest* g) { RET(D3D_OK, 3); }

/* StretchRect(src, src_rect, dst, dst_rect, filter): the GPU's job; same-size copies here */
static void sh_Dev_StretchRect(Guest* g)
{
    Obj* s = obj(ARG(1));
    Obj* d = obj(ARG(3));
    uint32_t dr = ARG(4);
    G9Target st = target_of(ARG(1)), dt = target_of(ARG(3));
    if (st.tex && dt.tex)
        g9_stretch(st.tex, st.face, st.level, ARG(2) ? (const int32_t*)GUEST_PTR(ARG(2)) : NULL, dt.tex, dt.face, dt.level,
            dr ? (const int32_t*)GUEST_PTR(dr) : NULL, ARG(5) == 2);
    else if (s && d && !dr && s->w == d->w && s->h == d->h)
    {
        copy_surface(s, ARG(2), d, 0, 0);
        d->dirty = 1;
    }
    RET(D3D_OK, 6);
}

/* ColorFill(surface, rect, color) */
static void sh_Dev_ColorFill(Guest* g)
{
    Obj* s = obj(ARG(1));
    G9Target t = target_of(ARG(1));
    if (s && t.tex && (s->usage & USAGE_RENDERTARGET))
    {
        G9Target rt[4];
        memset(rt, 0, sizeof rt);
        rt[0] = t;
        g9_set_targets(rt, NULL);
        uint32_t vp[6] = { 0, 0, s->w, s->h, 0, 0x3F800000u };
        g9_clear(ARG(2) ? 1 : 0, ARG(2) ? (const int32_t*)GUEST_PTR(ARG(2)) : NULL, 1, ARG(3), 0, 0, vp, NULL);
        RET(D3D_OK, 4);
    }
    if (s)
        s->dirty = 1;
    if (s && fmt_bytes(s->fmt) == 4 && !fmt_block(s->fmt) && obj_mem(s))
    {
        uint32_t x0 = 0, y0 = 0, x1 = s->w, y1 = s->h, r = ARG(2), c = ARG(3), p = fmt_pitch(s->fmt, s->w);
        if (r)
            x0 = rd32(r), y0 = rd32(r + 4), x1 = rd32(r + 8), y1 = rd32(r + 12);
        for (uint32_t y = y0; y < y1 && y < s->h; ++y)
            for (uint32_t x = x0; x < x1 && x < s->w; ++x)
                wr32(s->mem + y * p + 4 * x, c);
    }
    RET(D3D_OK, 4);
}

static void sh_Dev_SetRenderTarget(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= 4 || (i == 0 && !ARG(2)))
        RET(D3DERR_INVALIDCALL, 3);
    bind(&g_dev->rt[i], ARG(2));
    if (i == 0)
    {
        /* the viewport and scissor follow render target 0 */
        Obj* s = obj(ARG(2));
        if (s)
        {
            uint32_t* v = g_dev->s.viewport;
            v[0] = v[1] = 0, v[2] = s->w, v[3] = s->h, v[4] = 0, v[5] = f2u(1.0f);
            g_dev->s.scissor[0] = g_dev->s.scissor[1] = 0;
            g_dev->s.scissor[2] = (int32_t)s->w, g_dev->s.scissor[3] = (int32_t)s->h;
        }
    }
    RET(D3D_OK, 3);
}

static void sh_Dev_GetRenderTarget(Guest* g)
{
    uint32_t i = ARG(1), s = i < 4 ? g_dev->rt[i] : 0;
    if (s)
        addref(s);
    wr32(ARG(2), s);
    RET(s ? D3D_OK : D3DERR_NOTFOUND, 3);
}

static void sh_Dev_SetDepthStencilSurface(Guest* g) { bind(&g_dev->ds, ARG(1)); RET(D3D_OK, 2); }

static void sh_Dev_GetDepthStencilSurface(Guest* g)
{
    uint32_t s = g_dev->ds;
    if (s)
        addref(s);
    wr32(ARG(1), s);
    RET(s ? D3D_OK : D3DERR_NOTFOUND, 2);
}

static void sh_Dev_BeginScene(Guest* g) { g_dev->in_scene = 1; RET(D3D_OK, 1); }
static void sh_Dev_EndScene(Guest* g) { g_dev->in_scene = 0; RET(D3D_OK, 1); }
/* Clear(count, const D3DRECT*, flags, color, z, stencil) */
static void sh_Dev_Clear(Guest* g)
{
    g_dev->clears++;
    if (g9_active())
    {
        float z;
        uint32_t zu = ARG(5);
        memcpy(&z, &zu, 4);
        sync_targets();
        g9_clear(ARG(2) ? ARG(1) : 0, ARG(2) ? (const int32_t*)GUEST_PTR(ARG(2)) : NULL, ARG(3), ARG(4), z, ARG(6),
            g_dev->s.viewport, g_dev->s.rs[174] ? g_dev->s.scissor : NULL);
    }
    RET(D3D_OK, 7);
}

static int xf_index(uint32_t t) { return t < NXF ? (int)t : -1; }

static void sh_Dev_SetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(TARGET->xf[i], GUEST_PTR(ARG(2)), 64);
    MARK(xf[i]);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(GUEST_PTR(ARG(2)), g_dev->s.xf[i], 64);
    RET(D3D_OK, 3);
}

static void sh_Dev_MultiplyTransform(Guest* g)
{
    int i = xf_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    float m[16], r[16], *a = g_dev->s.xf[i];
    memcpy(m, GUEST_PTR(ARG(2)), 64);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x)
            r[y * 4 + x] = a[y * 4] * m[x] + a[y * 4 + 1] * m[4 + x] + a[y * 4 + 2] * m[8 + x] + a[y * 4 + 3] * m[12 + x];
    memcpy(TARGET->xf[i], r, 64);
    MARK(xf[i]);
    RET(D3D_OK, 3);
}

static void sh_Dev_SetViewport(Guest* g) { memcpy(TARGET->viewport, GUEST_PTR(ARG(1)), 24); MARK(viewport); RET(D3D_OK, 2); }
static void sh_Dev_GetViewport(Guest* g) { memcpy(GUEST_PTR(ARG(1)), g_dev->s.viewport, 24); RET(D3D_OK, 2); }
static void sh_Dev_SetMaterial(Guest* g) { memcpy(TARGET->material, GUEST_PTR(ARG(1)), 68); MARK(material); RET(D3D_OK, 2); }
static void sh_Dev_GetMaterial(Guest* g) { memcpy(GUEST_PTR(ARG(1)), g_dev->s.material, 68); RET(D3D_OK, 2); }

static void sh_Dev_SetLight(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= NLIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(TARGET->light[i], GUEST_PTR(ARG(2)), D3DLIGHT9_SIZE);
    TARGET->light_set[i] = 1;
    MARK(light[i]);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetLight(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= NLIGHTS || !g_dev->s.light_set[i])
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(GUEST_PTR(ARG(2)), g_dev->s.light[i], D3DLIGHT9_SIZE);
    RET(D3D_OK, 3);
}

static void sh_Dev_LightEnable(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= NLIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    TARGET->light_on[i] = ARG(2) != 0;
    if (!TARGET->light_set[i])
    {
        /* enabling a light never set gives the default directional light */
        uint32_t* l = TARGET->light[i];
        memset(l, 0, D3DLIGHT9_SIZE);
        l[0] = 3; /* D3DLIGHT_DIRECTIONAL */
        l[1] = l[2] = l[3] = f2u(1.0f); /* Diffuse */
        l[D3DLIGHT9_Direction / 4 + 2] = f2u(1.0f);
        TARGET->light_set[i] = 1;
    }
    MARK(light[i]);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetLightEnable(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= NLIGHTS)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev->s.light_on[i] ? 128u : 0u); /* TRUE as D3D9 reports it */
    RET(D3D_OK, 3);
}

static void sh_Dev_SetClipPlane(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(TARGET->clip[ARG(1)], GUEST_PTR(ARG(2)), 16);
    MARK(clip[ARG(1)]);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetClipPlane(Guest* g)
{
    if (ARG(1) >= 8)
        RET(D3DERR_INVALIDCALL, 3);
    memcpy(GUEST_PTR(ARG(2)), g_dev->s.clip[ARG(1)], 16);
    RET(D3D_OK, 3);
}

static void sh_Dev_SetRenderState(Guest* g)
{
    uint32_t k = ARG(1);
    if (k < NRS)
    {
        TARGET->rs[k] = ARG(2);
        MARK(rs[k]);
    }
    RET(D3D_OK, 3);
}

static void sh_Dev_GetRenderState(Guest* g)
{
    uint32_t k = ARG(1);
    wr32(ARG(2), k < NRS ? g_dev->s.rs[k] : 0);
    RET(D3D_OK, 3);
}

/* CreateStateBlock(type, IDirect3DStateBlock9**): ALL 1, PIXELSTATE 2, VERTEXSTATE 3, captured now */
static void sh_Dev_CreateStateBlock(Guest* g)
{
    Obj* b = obj_new(K_STATEBLOCK);
    if (!b)
        RET(E_OUTOFMEMORY, 3);
    b->block = (Block*)calloc(1, sizeof *b->block);
    /* the captured kinds: all of it for D3DSBT_ALL; the vertex/pixel split is approximated by all */
    memset(&b->block->m, 1, sizeof b->block->m);
    state_copy(&b->block->s, &g_dev->s, &b->block->m);
    wr32(ARG(2), b->guest);
    RET(D3D_OK, 3);
}

static void sh_Dev_BeginStateBlock(Guest* g)
{
    if (g_dev->recording)
        RET(D3DERR_INVALIDCALL, 1);
    g_dev->recording = (Block*)calloc(1, sizeof *g_dev->recording);
    RET(D3D_OK, 1);
}

static void sh_Dev_EndStateBlock(Guest* g)
{
    if (!g_dev->recording)
        RET(D3DERR_INVALIDCALL, 2);
    Obj* b = obj_new(K_STATEBLOCK);
    if (!b)
        RET(E_OUTOFMEMORY, 2);
    b->block = g_dev->recording;
    g_dev->recording = NULL;
    wr32(ARG(1), b->guest);
    RET(D3D_OK, 2);
}

static void sh_Dev_SetClipStatus(Guest* g) { RET(D3D_OK, 2); }
static void sh_Dev_GetClipStatus(Guest* g) { memset(GUEST_PTR(ARG(1)), 0, D3DCLIPSTATUS9_SIZE); RET(D3D_OK, 2); }

static void sh_Dev_GetTexture(Guest* g)
{
    int i = sampler_index(ARG(1));
    uint32_t t = i >= 0 ? g_dev->s.tex[i] : 0;
    if (t)
        addref(t);
    wr32(ARG(2), t);
    RET(i >= 0 ? D3D_OK : D3DERR_INVALIDCALL, 3);
}

static void sh_Dev_SetTexture(Guest* g)
{
    int i = sampler_index(ARG(1));
    if (i < 0)
        RET(D3DERR_INVALIDCALL, 3);
    bind(&TARGET->tex[i], ARG(2));
    MARK(tex[i]);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetTextureStageState(Guest* g)
{
    uint32_t t = ARG(1), k = ARG(2);
    wr32(ARG(3), t < NSTAGES && k < NTSS ? g_dev->s.tss[t][k] : 0);
    RET(D3D_OK, 4);
}

static void sh_Dev_SetTextureStageState(Guest* g)
{
    uint32_t t = ARG(1), k = ARG(2);
    if (t < NSTAGES && k < NTSS)
    {
        TARGET->tss[t][k] = ARG(3);
        MARK(tss[t][k]);
    }
    RET(D3D_OK, 4);
}

static void sh_Dev_GetSamplerState(Guest* g)
{
    int i = sampler_index(ARG(1));
    uint32_t k = ARG(2);
    wr32(ARG(3), i >= 0 && k < NSS ? g_dev->s.ss[i][k] : 0);
    RET(D3D_OK, 4);
}

static void sh_Dev_SetSamplerState(Guest* g)
{
    int i = sampler_index(ARG(1));
    uint32_t k = ARG(2);
    if (i >= 0 && k < NSS)
    {
        TARGET->ss[i][k] = ARG(3);
        MARK(ss[i][k]);
    }
    RET(D3D_OK, 4);
}

static void sh_Dev_ValidateDevice(Guest* g) { if (ARG(1)) wr32(ARG(1), 1); RET(D3D_OK, 2); }
static void sh_Dev_SetPaletteEntries(Guest* g) { RET(D3D_OK, 3); }
static void sh_Dev_GetPaletteEntries(Guest* g) { RET(D3DERR_INVALIDCALL, 3); }
static void sh_Dev_SetCurrentTexturePalette(Guest* g) { RET(D3D_OK, 2); }
static void sh_Dev_GetCurrentTexturePalette(Guest* g) { wr32(ARG(1), 0); RET(D3D_OK, 2); }
static void sh_Dev_SetScissorRect(Guest* g) { memcpy(TARGET->scissor, GUEST_PTR(ARG(1)), 16); MARK(scissor); RET(D3D_OK, 2); }
static void sh_Dev_GetScissorRect(Guest* g) { memcpy(GUEST_PTR(ARG(1)), g_dev->s.scissor, 16); RET(D3D_OK, 2); }
static void sh_Dev_SetSoftwareVertexProcessing(Guest* g) { RET(D3D_OK, 2); }
static void sh_Dev_GetSoftwareVertexProcessing(Guest* g) { RET(0, 1); }
static void sh_Dev_SetNPatchMode(Guest* g) { RET(D3D_OK, 2); }
static void sh_Dev_GetNPatchMode(Guest* g) { RET(0, 1); }

/* XIVLEGACY_DUMP_SHADERS: the vertex/pixel shader pairs drawn with, once each (pairs.txt) */
static void note_pair(void)
{
    static int on = -1;
    static uint64_t seen[4096];
    static unsigned nseen;
    if (on < 0)
        on = getenv("XIVLEGACY_DUMP_SHADERS") != NULL;
    Obj* vs = obj(g_dev->s.vs);
    Obj* ps = obj(g_dev->s.ps);
    if (!on || !vs || !ps)
        return;
    uint64_t key = (uint64_t)vs->hash << 32 | ps->hash;
    for (unsigned i = 0; i < nseen; ++i)
        if (seen[i] == key)
            return;
    if (nseen < 4096)
        seen[nseen++] = key;
    char path[1024];
    snprintf(path, sizeof path, "%s/pairs.txt", getenv("XIVLEGACY_DUMP_SHADERS"));
    FILE* f = fopen(path, "a");
    if (f)
    {
        fprintf(f, "vs_%08x ps_%08x\n", vs->hash, ps->hash);
        fclose(f);
    }
}

/* DrawPrimitive(type, start_vertex, primitive_count) */
static void sh_Dev_DrawPrimitive(Guest* g)
{
    g_dev->draws++;
    note_pair();
    G9Draw d;
    if (draw_state(&d, ARG(1), ARG(3)))
    {
        stream_range(&d, ARG(2), prim_vertices(ARG(1), ARG(3)));
        g9_draw(&d);
    }
    RET(D3D_OK, 4);
}

/* DrawIndexedPrimitive(type, base_vertex, min_index, num_vertices, start_index, primitive_count) */
static void sh_Dev_DrawIndexedPrimitive(Guest* g)
{
    g_dev->draws++;
    note_pair();
    G9Draw d;
    Obj* ib = obj(g_dev->s.indices);
    if (ib && obj_mem(ib) && draw_state(&d, ARG(1), ARG(6)))
    {
        int32_t base = (int32_t)ARG(2);
        uint32_t start = ARG(5), isize = ib->fmt == FMT_INDEX32 ? 4 : 2;
        uint32_t count = prim_vertices(ARG(1), ARG(6));
        if ((uint64_t)(start + count) * isize <= ib->size)
        {
            /* the vertices the indices actually reach: MinVertexIndex/NumVertices are hints drivers
             * ignore, and the game's are not always right (Gridania's cutscene: a copy cut to
             * NumVertices left indices past its end, triangles across the screen) */
            const uint8_t* ip = GUEST_PTR(ib->mem + start * isize);
            uint32_t lo = 0xFFFFFFFFu, hi = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t v = isize == 4 ? ((const uint32_t*)ip)[i] : ((const uint16_t*)ip)[i];
                lo = v < lo ? v : lo;
                hi = v > hi ? v : hi;
            }
            if (lo > hi)
                lo = hi = 0;
            int32_t first = base + (int32_t)lo;
            if (first < 0)
                first = 0;
            uint32_t num = (uint32_t)(base + (int32_t)hi + 1 - first);
            stream_range(&d, (uint32_t)first, num);
            {
                /* debugging: a stream shorter than the vertices the indices reach (XIVLEGACY_VBCHECK) */
                static int chk = -1;
                static uint32_t logged;
                if (chk < 0)
                    chk = getenv("XIVLEGACY_VBCHECK") != NULL;
                /* float positions that are garbage (huge or NaN): a stream read with the wrong offset or stride */
                for (uint32_t ei = 0; chk && ei < d.nelems && logged < 200; ++ei)
                {
                    const G9Elem* e = &d.elems[ei];
                    if (e->usage != 0 || e->index != 0 || (e->type != 2 && e->type != 3) || !d.streams[e->stream].data)
                        continue;
                    const G9Stream* st = &d.streams[e->stream];
                    uint32_t bad = 0, first_bad = 0;
                    for (uint32_t v = 0; v < num && (uint64_t)v * st->stride + e->offset + 12 <= st->size; ++v)
                    {
                        float f[3];
                        memcpy(f, st->data + (size_t)v * st->stride + e->offset, 12);
                        int b = !(fabsf(f[0]) < 1e6f && fabsf(f[1]) < 1e6f && fabsf(f[2]) < 1e6f);
                        if (b && !bad++)
                            first_bad = v;
                    }
                    if (bad)
                    {
                        Obj* decl = obj(g_dev->s.decl);
                        logged++;
                        rt_log("[vbcheck] f%llu %u of %u positions garbage (first %u) stride %u offset %u type %u decl %08x fvf %08x vs %08x prims %u\n",
                            (unsigned long long)g_dev->frames, bad, num, first_bad, st->stride, e->offset, e->type, decl ? decl->guest : 0,
                            g_dev->s.fvf, g_dev->s.vs, ARG(6));
                    }
                }
                for (int si = 0; chk && si < 16 && logged < 200; ++si)
                    if (d.streams[si].data && d.streams[si].stride && !d.streams[si].instanced &&
                        (uint64_t)num * d.streams[si].stride > d.streams[si].size)
                    {
                        Obj* vb = obj(g_dev->s.stream[si]);
                        logged++;
                        rt_log("[vbcheck] f%llu stream %d: %u vertices x %u > %u bytes (vb %u bytes, off %u, first %d, base %d, lo %u hi %u, fvf/decl elems %u)\n",
                            (unsigned long long)g_dev->frames, si, num, d.streams[si].stride, d.streams[si].size, vb ? vb->size : 0,
                            g_dev->s.stream_off[si], first, base, lo, hi, d.nelems);
                    }
            }
            d.base_vertex = base - first;
            d.indices = GUEST_PTR(ib->mem + start * isize);
            d.index_size = isize;
            g9_draw(&d);
        }
    }
    RET(D3D_OK, 7);
}

/* The ...UP draws leave stream 0 and the indices unset, as D3D9 does. */
/* DrawPrimitiveUP(type, primitive_count, data, stride) */
static void sh_Dev_DrawPrimitiveUP(Guest* g)
{
    g_dev->draws++;
    note_pair();
    G9Draw d;
    if (draw_state(&d, ARG(1), ARG(2)))
    {
        d.streams[0] = (G9Stream){ GUEST_PTR(ARG(3)), prim_vertices(ARG(1), ARG(2)) * ARG(4), ARG(4), 0 };
        d.instances = 1;
        g9_draw(&d);
    }
    bind(&g_dev->s.stream[0], 0);
    g_dev->s.stream_off[0] = g_dev->s.stream_stride[0] = 0;
    RET(D3D_OK, 5);
}

/* DrawIndexedPrimitiveUP(type, min_index, num_vertices, primitive_count, indices, index_format, data, stride) */
static void sh_Dev_DrawIndexedPrimitiveUP(Guest* g)
{
    g_dev->draws++;
    note_pair();
    G9Draw d;
    if (draw_state(&d, ARG(1), ARG(4)))
    {
        /* the range from the indices, as in DrawIndexedPrimitive */
        uint32_t stride = ARG(8), isize = ARG(6) == FMT_INDEX32 ? 4 : 2, count = prim_vertices(ARG(1), ARG(4));
        const uint8_t* ip = GUEST_PTR(ARG(5));
        uint32_t lo = 0xFFFFFFFFu, hi = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t v = isize == 4 ? ((const uint32_t*)ip)[i] : ((const uint16_t*)ip)[i];
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
        }
        if (lo > hi)
            lo = hi = 0;
        d.streams[0] = (G9Stream){ GUEST_PTR(ARG(7) + lo * stride), (hi - lo + 1) * stride, stride, 0 };
        d.base_vertex = -(int32_t)lo;
        d.indices = ip;
        d.index_size = isize;
        d.instances = 1;
        g9_draw(&d);
    }
    bind(&g_dev->s.stream[0], 0);
    g_dev->s.stream_off[0] = g_dev->s.stream_stride[0] = 0;
    bind(&g_dev->s.indices, 0);
    RET(D3D_OK, 9);
}

static void sh_Dev_ProcessVertices(Guest* g) { RET(D3D_OK, 7); }

/* CreateVertexDeclaration(const D3DVERTEXELEMENT9*, IDirect3DVertexDeclaration9**) */
static void sh_Dev_CreateVertexDeclaration(Guest* g)
{
    uint32_t e = ARG(1), n = 0;
    while (n < 65 && rd16(e + 8u * n) != 0xFF) /* D3DDECL_END: Stream 0xFF */
        n++;
    n++; /* the end marker too */
    Obj* d = obj_new(K_DECL);
    if (!d)
        RET(E_OUTOFMEMORY, 3);
    d->ndata = 2 * n;
    d->data = (uint32_t*)malloc(8u * n);
    memcpy(d->data, GUEST_PTR(e), 8u * n);
    wr32(ARG(2), d->guest);
    RET(D3D_OK, 3);
}

static void sh_Dev_SetVertexDeclaration(Guest* g) { bind(&TARGET->decl, ARG(1)); MARK(decl); RET(D3D_OK, 2); }

static void sh_Dev_GetVertexDeclaration(Guest* g)
{
    uint32_t d = g_dev->s.decl;
    if (d)
        addref(d);
    wr32(ARG(1), d);
    RET(D3D_OK, 2);
}

static void sh_Dev_SetFVF(Guest* g)
{
    TARGET->fvf = ARG(1);
    MARK(fvf);
    if (ARG(1))
    {
        bind(&TARGET->decl, 0); /* an FVF replaces the declaration */
        MARK(decl);
    }
    RET(D3D_OK, 2);
}

static void sh_Dev_GetFVF(Guest* g) { wr32(ARG(1), g_dev->s.fvf); RET(D3D_OK, 2); }

/* A shader's bytecode: tokens up to and including D3DSIO_END (0x0000FFFF). */
static Obj* new_shader(int kind, uint32_t code)
{
    uint32_t n = 0;
    while (n < 65536 && rd32(code + 4u * n) != 0x0000FFFFu)
    {
        uint32_t t = rd32(code + 4u * n);
        if ((t & 0xFFFF) == 0xFFFE) /* a comment: its length in the high word */
            n += (t >> 16) & 0x7FFF;
        n++;
    }
    n++;
    Obj* s = obj_new(kind);
    if (!s)
        return NULL;
    s->ndata = n;
    s->data = (uint32_t*)malloc(4u * n);
    memcpy(s->data, GUEST_PTR(code), 4u * n);
    s->gs = g9_shader_create(s->data, n);
    /* XIVLEGACY_DUMP_SHADERS=<folder>: each distinct shader's bytecode, for the shader translator's
     * tests (game data: keep it out of the repository) */
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; ++i)
        h = (h ^ s->data[i]) * 16777619u;
    s->hash = h;
    const char* dir = getenv("XIVLEGACY_DUMP_SHADERS");
    if (dir)
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s_%08x.dxbc9", dir, kind == K_VS ? "vs" : "ps", h);
        FILE* f = fopen(path, "rb");
        if (f)
            fclose(f);
        else if ((f = fopen(path, "wb")))
        {
            fwrite(s->data, 4, n, f);
            fclose(f);
        }
    }
    return s;
}

static void sh_Dev_CreateVertexShader(Guest* g)
{
    Obj* s = new_shader(K_VS, ARG(1));
    wr32(ARG(2), s ? s->guest : 0);
    RET(s ? D3D_OK : E_OUTOFMEMORY, 3);
}

static void sh_Dev_SetVertexShader(Guest* g) { bind(&TARGET->vs, ARG(1)); MARK(vs); RET(D3D_OK, 2); }

static void sh_Dev_GetVertexShader(Guest* g)
{
    uint32_t s = g_dev->s.vs;
    if (s)
        addref(s);
    wr32(ARG(1), s);
    RET(D3D_OK, 2);
}

/* debugging (tests/boot.c `vscsrc`): where in guest memory vertex constant c8 last came from */
uint32_t d3d9_vsf8_src;
/* debugging: the current vertex float constants (tests/boot.c `vsc`) */
const float* d3d9_vsf(void) { return g_dev ? g_dev->s.vsf : NULL; }
static void const_note(int vsf, uint32_t start, uint32_t n, uint32_t src)
{
    if (vsf && start <= 8 && 8 < start + n)
        d3d9_vsf8_src = src + (8 - start) * 16;
}

#define CONSTANTS(name, field, maskf, max, width, type)                                                        \
    static void sh_Dev_Set##name(Guest* g)                                                                      \
    {                                                                                                           \
        uint32_t start = ARG(1), n = ARG(3);                                                                    \
        if (start + n > (max))                                                                                  \
            RET(D3DERR_INVALIDCALL, 4);                                                                         \
        memcpy(&TARGET->field[start], GUEST_PTR(ARG(2)), (size_t)n * (width));                                  \
        const_note((void*)TARGET->field == (void*)g_dev->s.vsf, start, n, ARG(2));                              \
        if (g_dev->recording)                                                                                   \
            for (uint32_t i = 0; i < n; ++i)                                                                    \
                g_dev->recording->m.maskf[start + i] = 1;                                                       \
        RET(D3D_OK, 4);                                                                                         \
    }                                                                                                           \
    static void sh_Dev_Get##name(Guest* g)                                                                      \
    {                                                                                                           \
        uint32_t start = ARG(1), n = ARG(3);                                                                    \
        if (start + n > (max))                                                                                  \
            RET(D3DERR_INVALIDCALL, 4);                                                                         \
        memcpy(GUEST_PTR(ARG(2)), &g_dev->s.field[start], (size_t)n * (width));                                 \
        RET(D3D_OK, 4);                                                                                         \
    }

CONSTANTS(VertexShaderConstantF, vsf, vsf, VS_F, 16, float)
CONSTANTS(VertexShaderConstantI, vsi, vsi, 16, 16, int)
CONSTANTS(VertexShaderConstantB, vsb, vsb, 16, 4, uint32_t)
CONSTANTS(PixelShaderConstantF, psf, psf, PS_F, 16, float)
CONSTANTS(PixelShaderConstantI, psi, psi, 16, 16, int)
CONSTANTS(PixelShaderConstantB, psb, psb, 16, 4, uint32_t)

/* SetStreamSource(stream, vb, offset, stride) */
static void sh_Dev_SetStreamSource(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= 16)
        RET(D3DERR_INVALIDCALL, 5);
    bind(&TARGET->stream[i], ARG(2));
    TARGET->stream_off[i] = ARG(3), TARGET->stream_stride[i] = ARG(4);
    MARK(stream[i]);
    RET(D3D_OK, 5);
}

static void sh_Dev_GetStreamSource(Guest* g)
{
    uint32_t i = ARG(1);
    if (i >= 16)
        RET(D3DERR_INVALIDCALL, 5);
    uint32_t b = g_dev->s.stream[i];
    if (b)
        addref(b);
    wr32(ARG(2), b);
    wr32(ARG(3), g_dev->s.stream_off[i]);
    wr32(ARG(4), g_dev->s.stream_stride[i]);
    RET(D3D_OK, 5);
}

static void sh_Dev_SetStreamSourceFreq(Guest* g)
{
    if (ARG(1) >= 16)
        RET(D3DERR_INVALIDCALL, 3);
    TARGET->stream_freq[ARG(1)] = ARG(2);
    MARK(stream[ARG(1)]);
    RET(D3D_OK, 3);
}

static void sh_Dev_GetStreamSourceFreq(Guest* g)
{
    if (ARG(1) >= 16)
        RET(D3DERR_INVALIDCALL, 3);
    wr32(ARG(2), g_dev->s.stream_freq[ARG(1)]);
    RET(D3D_OK, 3);
}

static void sh_Dev_SetIndices(Guest* g) { bind(&TARGET->indices, ARG(1)); MARK(indices); RET(D3D_OK, 2); }

static void sh_Dev_GetIndices(Guest* g)
{
    uint32_t b = g_dev->s.indices;
    if (b)
        addref(b);
    wr32(ARG(1), b);
    RET(D3D_OK, 2);
}

static void sh_Dev_CreatePixelShader(Guest* g)
{
    Obj* s = new_shader(K_PS, ARG(1));
    wr32(ARG(2), s ? s->guest : 0);
    RET(s ? D3D_OK : E_OUTOFMEMORY, 3);
}

static void sh_Dev_SetPixelShader(Guest* g) { bind(&TARGET->ps, ARG(1)); MARK(ps); RET(D3D_OK, 2); }

static void sh_Dev_GetPixelShader(Guest* g)
{
    uint32_t s = g_dev->s.ps;
    if (s)
        addref(s);
    wr32(ARG(1), s);
    RET(D3D_OK, 2);
}

static void sh_Dev_DrawRectPatch(Guest* g) { RET(D3D_OK, 4); }
static void sh_Dev_DrawTriPatch(Guest* g) { RET(D3D_OK, 4); }
static void sh_Dev_DeletePatch(Guest* g) { RET(D3D_OK, 2); }

/* CreateQuery(type, IDirect3DQuery9**): NULL out asks whether the type is supported */
static void sh_Dev_CreateQuery(Guest* g)
{
    uint32_t type = ARG(1), out = ARG(2);
    if (!query_size(type))
    {
        if (out)
            wr32(out, 0);
        RET(D3DERR_NOTAVAILABLE, 3);
    }
    if (!out)
        RET(D3D_OK, 3);
    Obj* q = obj_new(K_QUERY);
    if (!q)
        RET(E_OUTOFMEMORY, 3);
    q->qtype = type;
    wr32(out, q->guest);
    RET(D3D_OK, 3);
}

/* --- IDirect3DSwapChain9 ---------------------------------------------------------------------------- */

static void sh_Swap_Present(Guest* g) { present(); RET(D3D_OK, 6); }
static void sh_Swap_GetFrontBufferData(Guest* g) { RET(D3D_OK, 2); }

static void sh_Swap_GetBackBuffer(Guest* g)
{
    if (ARG(1) != 0)
    {
        wr32(ARG(3), 0);
        RET(D3DERR_INVALIDCALL, 4);
    }
    addref(g_dev->backbuffer);
    wr32(ARG(3), g_dev->backbuffer);
    RET(D3D_OK, 4);
}

static void sh_Swap_GetRasterStatus(Guest* g)
{
    wr32(ARG(1) + D3DRASTER_STATUS_InVBlank, 0);
    wr32(ARG(1) + D3DRASTER_STATUS_ScanLine, 0);
    RET(D3D_OK, 2);
}

static void sh_Swap_GetDisplayMode(Guest* g)
{
    uint32_t w, h, hz;
    desktop_mode(&w, &h, &hz);
    write_mode(ARG(1), w, h, hz);
    RET(D3D_OK, 2);
}

static void sh_Swap_GetPresentParameters(Guest* g)
{
    memcpy(GUEST_PTR(ARG(1)), g_dev->pp, sizeof g_dev->pp);
    RET(D3D_OK, 2);
}

/* --- for d3dx9.c --------------------------------------------------------------------------------- */

uint32_t d3d9_create_texture(int type, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt,
    uint32_t pool)
{
    static const int kinds[] = { [D3D9_TEXTURE] = K_TEXTURE, [D3D9_CUBE] = K_CUBE, [D3D9_VOLUME_TEXTURE] = K_VOLUME_TEXTURE };
    Obj* t = new_texture(kinds[type], w, h, type == D3D9_VOLUME_TEXTURE ? d : 1, levels, usage, fmt, pool);
    return t ? t->guest : 0;
}

int d3d9_resource_mem(uint32_t p, D3d9Mem* out)
{
    Obj* o = obj(p);
    if (!o || (o->kind != K_SURFACE && o->kind != K_VOLUME))
        return 0;
    out->fmt = o->fmt, out->w = o->w, out->h = o->h, out->d = o->kind == K_VOLUME ? o->d : 1;
    out->pitch = fmt_pitch(o->fmt, o->w);
    out->slice = out->pitch * fmt_rows(o->fmt, o->h);
    out->mem = obj_mem(o);
    o->dirty = 1; /* the caller writes it */
    return out->mem != 0;
}

uint32_t d3d9_fmt_block(uint32_t fmt) { return fmt_block(fmt); }
uint32_t d3d9_fmt_bytes(uint32_t fmt) { return fmt_bytes(fmt); }

/* --- entry points ------------------------------------------------------------------------------------ */

static void sh_D3DPERF_SetOptions(Guest* g) { RET(0, 1); }
static void sh_D3DPERF_BeginEvent(Guest* g) { RET(0, 2); }
static void sh_D3DPERF_EndEvent(Guest* g) { RET(0, 0); }

static void sh_Direct3DCreate9(Guest* g)
{
    rt_log("[d3d9] Direct3DCreate9(sdk %u)\n", ARG(0));
    Obj* o = obj_new(K_D3D9);
    if (o && !g_d3d9_obj)
        g_d3d9_obj = o->guest;
    RET(o ? o->guest : 0, 1);
}

#define D(name, fn) { "d3d9.dll", name, fn }
static const ShimDef D3D9[] = {
    D("D3DPERF_SetOptions", sh_D3DPERF_SetOptions),
    D("D3DPERF_BeginEvent", sh_D3DPERF_BeginEvent),
    D("D3DPERF_EndEvent", sh_D3DPERF_EndEvent),
    D("Direct3DCreate9", sh_Direct3DCreate9),
    D("IDirect3D9::RegisterSoftwareDevice", sh_D3D9_RegisterSoftwareDevice),
    D("IDirect3D9::GetAdapterCount", sh_D3D9_GetAdapterCount),
    D("IDirect3D9::GetAdapterIdentifier", sh_D3D9_GetAdapterIdentifier),
    D("IDirect3D9::GetAdapterModeCount", sh_D3D9_GetAdapterModeCount),
    D("IDirect3D9::EnumAdapterModes", sh_D3D9_EnumAdapterModes),
    D("IDirect3D9::GetAdapterDisplayMode", sh_D3D9_GetAdapterDisplayMode),
    D("IDirect3D9::CheckDeviceType", sh_D3D9_CheckDeviceType),
    D("IDirect3D9::CheckDeviceFormat", sh_D3D9_CheckDeviceFormat),
    D("IDirect3D9::CheckDeviceMultiSampleType", sh_D3D9_CheckDeviceMultiSampleType),
    D("IDirect3D9::CheckDepthStencilMatch", sh_D3D9_CheckDepthStencilMatch),
    D("IDirect3D9::CheckDeviceFormatConversion", sh_D3D9_CheckDeviceFormatConversion),
    D("IDirect3D9::GetDeviceCaps", sh_D3D9_GetDeviceCaps),
    D("IDirect3D9::GetAdapterMonitor", sh_D3D9_GetAdapterMonitor),
    D("IDirect3D9::CreateDevice", sh_D3D9_CreateDevice),
    D("IDirect3DDevice9::TestCooperativeLevel", sh_Dev_TestCooperativeLevel),
    D("IDirect3DDevice9::GetAvailableTextureMem", sh_Dev_GetAvailableTextureMem),
    D("IDirect3DDevice9::EvictManagedResources", sh_Dev_EvictManagedResources),
    D("IDirect3DDevice9::GetDirect3D", sh_Dev_GetDirect3D),
    D("IDirect3DDevice9::GetDeviceCaps", sh_Dev_GetDeviceCaps),
    D("IDirect3DDevice9::GetDisplayMode", sh_Dev_GetDisplayMode),
    D("IDirect3DDevice9::GetCreationParameters", sh_Dev_GetCreationParameters),
    D("IDirect3DDevice9::SetCursorProperties", sh_Dev_SetCursorProperties),
    D("IDirect3DDevice9::SetCursorPosition", sh_Dev_SetCursorPosition),
    D("IDirect3DDevice9::ShowCursor", sh_Dev_ShowCursor),
    D("IDirect3DDevice9::CreateAdditionalSwapChain", sh_Dev_CreateAdditionalSwapChain),
    D("IDirect3DDevice9::GetSwapChain", sh_Dev_GetSwapChain),
    D("IDirect3DDevice9::GetNumberOfSwapChains", sh_Dev_GetNumberOfSwapChains),
    D("IDirect3DDevice9::Reset", sh_Dev_Reset),
    D("IDirect3DDevice9::Present", sh_Dev_Present),
    D("IDirect3DDevice9::GetBackBuffer", sh_Dev_GetBackBuffer),
    D("IDirect3DDevice9::GetRasterStatus", sh_Dev_GetRasterStatus),
    D("IDirect3DDevice9::SetDialogBoxMode", sh_Dev_SetDialogBoxMode),
    D("IDirect3DDevice9::SetGammaRamp", sh_Dev_SetGammaRamp),
    D("IDirect3DDevice9::GetGammaRamp", sh_Dev_GetGammaRamp),
    D("IDirect3DDevice9::CreateTexture", sh_Dev_CreateTexture),
    D("IDirect3DDevice9::CreateVolumeTexture", sh_Dev_CreateVolumeTexture),
    D("IDirect3DDevice9::CreateCubeTexture", sh_Dev_CreateCubeTexture),
    D("IDirect3DDevice9::CreateVertexBuffer", sh_Dev_CreateVertexBuffer),
    D("IDirect3DDevice9::CreateIndexBuffer", sh_Dev_CreateIndexBuffer),
    D("IDirect3DDevice9::CreateRenderTarget", sh_Dev_CreateRenderTarget),
    D("IDirect3DDevice9::CreateDepthStencilSurface", sh_Dev_CreateDepthStencilSurface),
    D("IDirect3DDevice9::UpdateSurface", sh_Dev_UpdateSurface),
    D("IDirect3DDevice9::UpdateTexture", sh_Dev_UpdateTexture),
    D("IDirect3DDevice9::GetRenderTargetData", sh_Dev_GetRenderTargetData),
    D("IDirect3DDevice9::GetFrontBufferData", sh_Dev_GetFrontBufferData),
    D("IDirect3DDevice9::StretchRect", sh_Dev_StretchRect),
    D("IDirect3DDevice9::ColorFill", sh_Dev_ColorFill),
    D("IDirect3DDevice9::CreateOffscreenPlainSurface", sh_Dev_CreateOffscreenPlainSurface),
    D("IDirect3DDevice9::SetRenderTarget", sh_Dev_SetRenderTarget),
    D("IDirect3DDevice9::GetRenderTarget", sh_Dev_GetRenderTarget),
    D("IDirect3DDevice9::SetDepthStencilSurface", sh_Dev_SetDepthStencilSurface),
    D("IDirect3DDevice9::GetDepthStencilSurface", sh_Dev_GetDepthStencilSurface),
    D("IDirect3DDevice9::BeginScene", sh_Dev_BeginScene),
    D("IDirect3DDevice9::EndScene", sh_Dev_EndScene),
    D("IDirect3DDevice9::Clear", sh_Dev_Clear),
    D("IDirect3DDevice9::SetTransform", sh_Dev_SetTransform),
    D("IDirect3DDevice9::GetTransform", sh_Dev_GetTransform),
    D("IDirect3DDevice9::MultiplyTransform", sh_Dev_MultiplyTransform),
    D("IDirect3DDevice9::SetViewport", sh_Dev_SetViewport),
    D("IDirect3DDevice9::GetViewport", sh_Dev_GetViewport),
    D("IDirect3DDevice9::SetMaterial", sh_Dev_SetMaterial),
    D("IDirect3DDevice9::GetMaterial", sh_Dev_GetMaterial),
    D("IDirect3DDevice9::SetLight", sh_Dev_SetLight),
    D("IDirect3DDevice9::GetLight", sh_Dev_GetLight),
    D("IDirect3DDevice9::LightEnable", sh_Dev_LightEnable),
    D("IDirect3DDevice9::GetLightEnable", sh_Dev_GetLightEnable),
    D("IDirect3DDevice9::SetClipPlane", sh_Dev_SetClipPlane),
    D("IDirect3DDevice9::GetClipPlane", sh_Dev_GetClipPlane),
    D("IDirect3DDevice9::SetRenderState", sh_Dev_SetRenderState),
    D("IDirect3DDevice9::GetRenderState", sh_Dev_GetRenderState),
    D("IDirect3DDevice9::CreateStateBlock", sh_Dev_CreateStateBlock),
    D("IDirect3DDevice9::BeginStateBlock", sh_Dev_BeginStateBlock),
    D("IDirect3DDevice9::EndStateBlock", sh_Dev_EndStateBlock),
    D("IDirect3DDevice9::SetClipStatus", sh_Dev_SetClipStatus),
    D("IDirect3DDevice9::GetClipStatus", sh_Dev_GetClipStatus),
    D("IDirect3DDevice9::GetTexture", sh_Dev_GetTexture),
    D("IDirect3DDevice9::SetTexture", sh_Dev_SetTexture),
    D("IDirect3DDevice9::GetTextureStageState", sh_Dev_GetTextureStageState),
    D("IDirect3DDevice9::SetTextureStageState", sh_Dev_SetTextureStageState),
    D("IDirect3DDevice9::GetSamplerState", sh_Dev_GetSamplerState),
    D("IDirect3DDevice9::SetSamplerState", sh_Dev_SetSamplerState),
    D("IDirect3DDevice9::ValidateDevice", sh_Dev_ValidateDevice),
    D("IDirect3DDevice9::SetPaletteEntries", sh_Dev_SetPaletteEntries),
    D("IDirect3DDevice9::GetPaletteEntries", sh_Dev_GetPaletteEntries),
    D("IDirect3DDevice9::SetCurrentTexturePalette", sh_Dev_SetCurrentTexturePalette),
    D("IDirect3DDevice9::GetCurrentTexturePalette", sh_Dev_GetCurrentTexturePalette),
    D("IDirect3DDevice9::SetScissorRect", sh_Dev_SetScissorRect),
    D("IDirect3DDevice9::GetScissorRect", sh_Dev_GetScissorRect),
    D("IDirect3DDevice9::SetSoftwareVertexProcessing", sh_Dev_SetSoftwareVertexProcessing),
    D("IDirect3DDevice9::GetSoftwareVertexProcessing", sh_Dev_GetSoftwareVertexProcessing),
    D("IDirect3DDevice9::SetNPatchMode", sh_Dev_SetNPatchMode),
    D("IDirect3DDevice9::GetNPatchMode", sh_Dev_GetNPatchMode),
    D("IDirect3DDevice9::DrawPrimitive", sh_Dev_DrawPrimitive),
    D("IDirect3DDevice9::DrawIndexedPrimitive", sh_Dev_DrawIndexedPrimitive),
    D("IDirect3DDevice9::DrawPrimitiveUP", sh_Dev_DrawPrimitiveUP),
    D("IDirect3DDevice9::DrawIndexedPrimitiveUP", sh_Dev_DrawIndexedPrimitiveUP),
    D("IDirect3DDevice9::ProcessVertices", sh_Dev_ProcessVertices),
    D("IDirect3DDevice9::CreateVertexDeclaration", sh_Dev_CreateVertexDeclaration),
    D("IDirect3DDevice9::SetVertexDeclaration", sh_Dev_SetVertexDeclaration),
    D("IDirect3DDevice9::GetVertexDeclaration", sh_Dev_GetVertexDeclaration),
    D("IDirect3DDevice9::SetFVF", sh_Dev_SetFVF),
    D("IDirect3DDevice9::GetFVF", sh_Dev_GetFVF),
    D("IDirect3DDevice9::CreateVertexShader", sh_Dev_CreateVertexShader),
    D("IDirect3DDevice9::SetVertexShader", sh_Dev_SetVertexShader),
    D("IDirect3DDevice9::GetVertexShader", sh_Dev_GetVertexShader),
    D("IDirect3DDevice9::SetVertexShaderConstantF", sh_Dev_SetVertexShaderConstantF),
    D("IDirect3DDevice9::GetVertexShaderConstantF", sh_Dev_GetVertexShaderConstantF),
    D("IDirect3DDevice9::SetVertexShaderConstantI", sh_Dev_SetVertexShaderConstantI),
    D("IDirect3DDevice9::GetVertexShaderConstantI", sh_Dev_GetVertexShaderConstantI),
    D("IDirect3DDevice9::SetVertexShaderConstantB", sh_Dev_SetVertexShaderConstantB),
    D("IDirect3DDevice9::GetVertexShaderConstantB", sh_Dev_GetVertexShaderConstantB),
    D("IDirect3DDevice9::SetStreamSource", sh_Dev_SetStreamSource),
    D("IDirect3DDevice9::GetStreamSource", sh_Dev_GetStreamSource),
    D("IDirect3DDevice9::SetStreamSourceFreq", sh_Dev_SetStreamSourceFreq),
    D("IDirect3DDevice9::GetStreamSourceFreq", sh_Dev_GetStreamSourceFreq),
    D("IDirect3DDevice9::SetIndices", sh_Dev_SetIndices),
    D("IDirect3DDevice9::GetIndices", sh_Dev_GetIndices),
    D("IDirect3DDevice9::CreatePixelShader", sh_Dev_CreatePixelShader),
    D("IDirect3DDevice9::SetPixelShader", sh_Dev_SetPixelShader),
    D("IDirect3DDevice9::GetPixelShader", sh_Dev_GetPixelShader),
    D("IDirect3DDevice9::SetPixelShaderConstantF", sh_Dev_SetPixelShaderConstantF),
    D("IDirect3DDevice9::GetPixelShaderConstantF", sh_Dev_GetPixelShaderConstantF),
    D("IDirect3DDevice9::SetPixelShaderConstantI", sh_Dev_SetPixelShaderConstantI),
    D("IDirect3DDevice9::GetPixelShaderConstantI", sh_Dev_GetPixelShaderConstantI),
    D("IDirect3DDevice9::SetPixelShaderConstantB", sh_Dev_SetPixelShaderConstantB),
    D("IDirect3DDevice9::GetPixelShaderConstantB", sh_Dev_GetPixelShaderConstantB),
    D("IDirect3DDevice9::DrawRectPatch", sh_Dev_DrawRectPatch),
    D("IDirect3DDevice9::DrawTriPatch", sh_Dev_DrawTriPatch),
    D("IDirect3DDevice9::DeletePatch", sh_Dev_DeletePatch),
    D("IDirect3DDevice9::CreateQuery", sh_Dev_CreateQuery),
    D("IDirect3DSwapChain9::Present", sh_Swap_Present),
    D("IDirect3DSwapChain9::GetFrontBufferData", sh_Swap_GetFrontBufferData),
    D("IDirect3DSwapChain9::GetBackBuffer", sh_Swap_GetBackBuffer),
    D("IDirect3DSwapChain9::GetRasterStatus", sh_Swap_GetRasterStatus),
    D("IDirect3DSwapChain9::GetDisplayMode", sh_Swap_GetDisplayMode),
    D("IDirect3DSwapChain9::GetPresentParameters", sh_Swap_GetPresentParameters),
    D("IDirect3DSurface9::GetContainer", sh_Surface_GetContainer),
    D("IDirect3DSurface9::GetDesc", sh_Surface_GetDesc),
    D("IDirect3DSurface9::LockRect", sh_Surface_LockRect),
    D("IDirect3DSurface9::UnlockRect", sh_Surface_UnlockRect),
    D("IDirect3DSurface9::GetDC", sh_Surface_GetDC),
    D("IDirect3DSurface9::ReleaseDC", sh_Surface_ReleaseDC),
    D("IDirect3DTexture9::GetLevelDesc", sh_Tex_GetLevelDesc),
    D("IDirect3DTexture9::GetSurfaceLevel", sh_Tex_GetSurfaceLevel),
    D("IDirect3DTexture9::LockRect", sh_Tex_LockRect),
    D("IDirect3DTexture9::UnlockRect", sh_Tex_UnlockRect),
    D("IDirect3DTexture9::AddDirtyRect", sh_Tex_AddDirtyRect),
    D("IDirect3DCubeTexture9::GetLevelDesc", sh_Cube_GetLevelDesc),
    D("IDirect3DCubeTexture9::GetCubeMapSurface", sh_Cube_GetCubeMapSurface),
    D("IDirect3DCubeTexture9::LockRect", sh_Cube_LockRect),
    D("IDirect3DCubeTexture9::UnlockRect", sh_Cube_UnlockRect),
    D("IDirect3DCubeTexture9::AddDirtyRect", sh_Cube_AddDirtyRect),
    D("IDirect3DVolumeTexture9::GetLevelDesc", sh_VolTex_GetLevelDesc),
    D("IDirect3DVolumeTexture9::GetVolumeLevel", sh_VolTex_GetVolumeLevel),
    D("IDirect3DVolumeTexture9::LockBox", sh_VolTex_LockBox),
    D("IDirect3DVolumeTexture9::UnlockBox", sh_VolTex_UnlockBox),
    D("IDirect3DVolumeTexture9::AddDirtyBox", sh_VolTex_AddDirtyBox),
    D("IDirect3DVolume9::GetContainer", sh_Volume_GetContainer),
    D("IDirect3DVolume9::GetDesc", sh_Volume_GetDesc),
    D("IDirect3DVolume9::LockBox", sh_Volume_LockBox),
    D("IDirect3DVolume9::UnlockBox", sh_Volume_UnlockBox),
    D("IDirect3DVertexBuffer9::Lock", sh_Buffer_Lock),
    D("IDirect3DVertexBuffer9::Unlock", sh_Buffer_Unlock),
    D("IDirect3DVertexBuffer9::GetDesc", sh_VB_GetDesc),
    D("IDirect3DIndexBuffer9::Lock", sh_Buffer_Lock),
    D("IDirect3DIndexBuffer9::Unlock", sh_Buffer_Unlock),
    D("IDirect3DIndexBuffer9::GetDesc", sh_IB_GetDesc),
    D("IDirect3DVertexDeclaration9::GetDeclaration", sh_Decl_GetDeclaration),
    D("IDirect3DVertexShader9::GetFunction", sh_Shader_GetFunction),
    D("IDirect3DPixelShader9::GetFunction", sh_Shader_GetFunction),
    D("IDirect3DQuery9::GetType", sh_Query_GetType),
    D("IDirect3DQuery9::GetDataSize", sh_Query_GetDataSize),
    D("IDirect3DQuery9::Issue", sh_Query_Issue),
    D("IDirect3DQuery9::GetData", sh_Query_GetData),
    D("IDirect3DStateBlock9::Capture", sh_StateBlock_Capture),
    D("IDirect3DStateBlock9::Apply", sh_StateBlock_Apply),
    { NULL, NULL, NULL },
};

/* The methods every interface shares (IUnknown, IDirect3DResource9, GetDevice, the base texture's),
 * under each interface's name. */
static ShimDef g_common[K_KINDS * 16 + 1];
static char g_common_names[K_KINDS * 16][64];

static void add_common(unsigned* n, int kind, const char* method, Shim fn)
{
    for (unsigned i = 0; i < IFACES[kind].count; ++i)
        if (!strcmp(IFACES[kind].methods[i].name, method))
        {
            snprintf(g_common_names[*n], sizeof g_common_names[0], "%s::%s", IFACES[kind].iface, method);
            g_common[*n] = (ShimDef){ "d3d9.dll", g_common_names[*n], fn };
            (*n)++;
            return;
        }
}

void d3d9_init(void)
{
    unsigned n = 0;
    for (int k = 0; k < K_KINDS; ++k)
    {
        add_common(&n, k, "QueryInterface", sh_QueryInterface);
        add_common(&n, k, "AddRef", sh_AddRef);
        add_common(&n, k, "Release", sh_Release);
        if (k != K_D3D9 && k != K_DEVICE && k != K_SWAPCHAIN)
            add_common(&n, k, "GetDevice", sh_GetDevice);
        add_common(&n, k, "SetPrivateData", sh_SetPrivateData);
        add_common(&n, k, "GetPrivateData", sh_GetPrivateData);
        add_common(&n, k, "FreePrivateData", sh_FreePrivateData);
        add_common(&n, k, "SetPriority", sh_SetPriority);
        add_common(&n, k, "GetPriority", sh_GetPriority);
        add_common(&n, k, "PreLoad", sh_PreLoad);
        add_common(&n, k, "GetType", sh_GetType);
        add_common(&n, k, "SetLOD", sh_Tex_SetLOD);
        add_common(&n, k, "GetLOD", sh_Tex_GetLOD);
        add_common(&n, k, "GetLevelCount", sh_Tex_GetLevelCount);
        add_common(&n, k, "SetAutoGenFilterType", sh_Tex_SetAutoGenFilterType);
        add_common(&n, k, "GetAutoGenFilterType", sh_Tex_GetAutoGenFilterType);
        add_common(&n, k, "GenerateMipSubLevels", sh_Tex_GenerateMipSubLevels);
    }
    /* the swap chain's GetDevice */
    snprintf(g_common_names[n], sizeof g_common_names[0], "IDirect3DSwapChain9::GetDevice");
    g_common[n] = (ShimDef){ "d3d9.dll", g_common_names[n], sh_GetDevice };
    n++;
    thunk_register(g_common);
    thunk_register(D3D9);
}
