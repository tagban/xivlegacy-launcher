/* The Direct3D 9 back end on Metal (gfx9.h). Manual retain/release (built with -fno-objc-arc).
 *
 * Shaders: each vertex/pixel shader pair becomes one MSL library (sm3_msl.c) the first time a draw
 * uses it, per shape of the vertex inputs (Metal hands integer vertex formats only to integer
 * inputs). Pipelines are cached by everything Metal bakes into one: the pair, the vertex layout, the
 * render target formats, blending and write masks. Depth-stencil and sampler states have caches of
 * their own.
 *
 * Data: the vertex, index and constant bytes of each draw are copied into a ring (one buffer per frame
 * in flight), so guest memory can change the moment the draw returns, as D3D allows. Textures that
 * are only sampled live in shared storage and are filled with replaceRegion; render targets and
 * depth buffers are private.
 *
 * Passes: a render pass opens at the first draw or clear after the targets change. A clear of whole
 * targets before anything is drawn becomes the pass's load action; any other clear draws a quad.
 * Present ends the frame: the back buffer is drawn to the window's drawable.
 *
 * XIVLEGACY_SNAP=<folder>: every 120th frame's back buffer as a PNG (frame_000120.png ...);
 * XIVLEGACY_SNAP_EVERY=<n> for another interval. */
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx9.h"
#include "gthread.h"
#include "plat.h"
#include "sm3.h"
#include "sm3_msl.h"
#include "sm3_interp.h"

#define FRAMES 3
#define RING_SIZE (64u << 20)
#define RING_ALIGN 256u

/* D3DFORMAT values used here */
enum
{
    D_R8G8B8 = 20, D_A8R8G8B8 = 21, D_X8R8G8B8 = 22, D_R5G6B5 = 23, D_X1R5G5B5 = 24, D_A1R5G5B5 = 25, D_A4R4G4B4 = 26,
    D_A8 = 28, D_A2B10G10R10 = 31, D_A8B8G8R8 = 32, D_X8B8G8R8 = 33, D_G16R16 = 34, D_A2R10G10B10 = 35,
    D_A16B16G16R16 = 36, D_L8 = 50, D_A8L8 = 51, D_V8U8 = 60, D_Q8W8V8U8 = 63, D_D16_LOCKABLE = 70, D_D32 = 71,
    D_D24S8 = 75, D_D24X8 = 77, D_D16 = 80, D_L16 = 81, D_D24FS8 = 83, D_R16F = 111, D_G16R16F = 112,
    D_A16B16G16R16F = 113, D_R32F = 114, D_G32R32F = 115, D_A32B32G32R32F = 116,
};
#define FOURCC(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))
#define D_DXT1 FOURCC('D', 'X', 'T', '1')
#define D_DXT2 FOURCC('D', 'X', 'T', '2')
#define D_DXT3 FOURCC('D', 'X', 'T', '3')
#define D_DXT4 FOURCC('D', 'X', 'T', '4')
#define D_DXT5 FOURCC('D', 'X', 'T', '5')
#define D_INTZ FOURCC('I', 'N', 'T', 'Z')
#define D_NULL FOURCC('N', 'U', 'L', 'L')

/* CPU conversions on upload (formats Metal has no match for, as D3D lays them out) */
enum { CONV_NONE, CONV_565, CONV_1555, CONV_X1555, CONV_4444, CONV_RGB8 };

struct G9Tex
{
    id<MTLTexture> tex;  /* what is rendered to and uploaded into */
    id<MTLTexture> view; /* what shaders sample (a swizzled view, or tex) */
    int type, use;
    uint32_t fmt, w, h, d, levels;
    MTLPixelFormat pf;
    int conv;
    uint32_t block, bpp; /* bytes per 4x4 block (compressed) or per pixel, in Metal's layout */
};

struct G9Shader
{
    Sm3Shader s;
    uint32_t* tokens;
    uint32_t id;
    uint8_t in_usage[16], in_index[16];
    uint16_t in_mask;      /* vertex shader: the input registers declared */
    uint8_t tex_type[20];  /* sampler dcl types */
    uint32_t nconst;       /* float constants a draw must supply: past the highest one read (all with relative addressing) */
};

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_queue;
static CAMetalLayer* g_layer;
static SDL_MetalView g_view;
static SDL_Window* g_window;
static id<MTLLibrary> g_util;
static id<MTLRenderPipelineState> g_present_pipe;
static id<MTLSamplerState> g_linear, g_point;
static dispatch_semaphore_t g_frames_sem;
static id<MTLBuffer> g_ring[FRAMES];
static uint32_t g_slot, g_ring_off;
static NSMutableArray* g_spill; /* extra buffers when a frame outgrows its ring */
static uint32_t g_spills; /* this frame's (debugging: trace) */
static id<MTLCommandBuffer> g_cmd;
static id<MTLRenderCommandEncoder> g_enc;
static id<MTLBuffer> g_zero;    /* zeros, for vertex inputs the declaration does not feed */
static id<MTLTexture> g_dummy[3];

/* per-frame costs for the hitch log (g9_frame_costs) */
static uint32_t g_fc_compiles, g_fc_pipes, g_fc_uploads;
static uint64_t g_fc_compile_ns, g_fc_pipe_ns, g_fc_upload_ns, g_fc_upload_bytes;

/* debugging: live textures, to find one by its trace name (g9_snap_tex) */
#define MAX_LIVE 4096
static G9Tex* g_live[MAX_LIVE];
static void live_add(G9Tex* t)
{
    for (int i = 0; i < MAX_LIVE; ++i)
        if (!g_live[i])
        {
            g_live[i] = t;
            return;
        }
}


/* debugging: one frame's passes, copies and draws (g9_trace_next) */
static int g_trace;
void g9_trace_next(void) { g_trace = 2; }
static const char* tname(const G9Tex* t, char* b, size_t n)
{
    if (!t)
        return "-";
    snprintf(b, n, "%s%04x:%ux%u:f%u", t->use == G9_USE_RT ? "RT" : t->use == G9_USE_DEPTH ? "DS" : "T", (unsigned)(((uintptr_t)t >> 4) & 0xFFFF),
        t->w, t->h, t->fmt);
    return b;
}
#define TRACE(...) do { if (g_trace == 1) fprintf(stdout, __VA_ARGS__); } while (0)
static uint32_t g_next_shader_id = 1;
static uint64_t g_frame;
static int g_vsync = 1;

/* targets */
static G9Target g_rt[4];
static G9Tex* g_ds;
static int g_targets_dirty = 1;
static uint32_t g_clear_flags, g_clear_color, g_clear_stencil;
static float g_clear_z;

int g9_active(void) { return g_dev != nil; }

/* --- buffers the game seldom writes, kept whole on the GPU (g9_persistent) ------------------------
 * Each draw copied the vertices and indices it uses into the frame's ring: the drawing thread's
 * memmove was 5% of its time. A vertex or index buffer the game made without D3DUSAGE_DYNAMIC is
 * written once and drawn every frame: it is copied once, to a buffer of its own, and drawn from
 * there until the game writes it again (a new version: a new buffer; command buffers keep the old
 * one while the GPU still reads it). */
typedef struct
{
    uint32_t key, ver, size; /* key 0: free, ~0: free after a removal */
    id<MTLBuffer> buf;
} PBuf;
#define PBUF_SLOTS 32768u
static PBuf g_pbuf[PBUF_SLOTS];
static uint32_t g_pbuf_made;
static uint64_t g_pbuf_bytes;

int g9_persistent(void)
{
    static int on = -1;
    if (on < 0)
        on = getenv("XIVLEGACY_NOPBUF") == NULL;
    return on && g_dev != nil;
}

static PBuf* pbuf_find(uint32_t key, int insert)
{
    uint32_t h = (key * 2654435761u) % PBUF_SLOTS;
    PBuf* free_slot = NULL;
    for (uint32_t n = 0; n < PBUF_SLOTS; ++n, h = (h + 1) % PBUF_SLOTS)
    {
        PBuf* p = &g_pbuf[h];
        if (p->key == key)
            return p;
        if (p->key == ~0u && !free_slot)
            free_slot = p;
        if (p->key == 0)
        {
            if (!insert)
                return NULL;
            p = free_slot ? free_slot : p;
            p->key = key, p->ver = 0, p->size = 0, p->buf = nil;
            return p;
        }
    }
    return NULL;
}

/* the buffer for this key and version, made from the bytes if new; nil if the table is full */
static id<MTLBuffer> pbuf_get(uint32_t key, uint32_t ver, const void* data, uint32_t size)
{
    if (!key || !size)
        return nil;
    PBuf* p = pbuf_find(key, 1);
    if (!p)
        return nil;
    if (p->buf && p->ver == ver && p->size == size)
        return p->buf;
    [p->buf release];
    p->buf = [g_dev newBufferWithBytes:data length:size options:MTLResourceStorageModeShared];
    p->ver = ver, p->size = size;
    g_pbuf_made++, g_pbuf_bytes += size;
    return p->buf;
}

void g9_buffer_forget(uint32_t key)
{
    PBuf* p = pbuf_find(key, 0);
    if (!p)
        return;
    [p->buf release];
    p->buf = nil;
    p->key = ~0u;
}

/* --- small caches: key bytes -> object ------------------------------------------------------------- */

typedef struct Ent
{
    uint64_t h;
    void* key;
    size_t klen;
    id obj;
} Ent;

typedef struct Map
{
    Ent* e;
    size_t cap, n;
} Map;

/* a key's hash, 8 bytes a step (a draw looks up several keys of up to ~200 bytes; byte-wise FNV was a
 * visible part of g9_draw) */
static uint64_t fnv(const void* p, size_t n)
{
    const uint8_t* b = (const uint8_t*)p;
    uint64_t h = 14695981039346656037ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    if (i < n)
    {
        uint64_t w = 0;
        memcpy(&w, b + i, n - i);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
    }
    h ^= h >> 32;
    return h | 1;
}

static Ent* map_slot(Map* m, const void* key, size_t klen, uint64_t h)
{
    size_t i = h & (m->cap - 1);
    for (;;)
    {
        Ent* e = &m->e[i];
        if (!e->h || (e->h == h && e->klen == klen && !memcmp(e->key, key, klen)))
            return e;
        i = (i + 1) & (m->cap - 1);
    }
}

static id map_get(Map* m, const void* key, size_t klen)
{
    if (!m->cap)
        return nil;
    Ent* e = map_slot(m, key, klen, fnv(key, klen));
    return e->h ? e->obj : nil;
}

static void map_put(Map* m, const void* key, size_t klen, id obj) /* keeps a reference */
{
    if (m->n * 2 >= m->cap)
    {
        Map old = *m;
        m->cap = old.cap ? old.cap * 2 : 256;
        m->e = (Ent*)calloc(m->cap, sizeof *m->e);
        m->n = 0;
        for (size_t i = 0; i < old.cap; ++i)
            if (old.e[i].h)
            {
                *map_slot(m, old.e[i].key, old.e[i].klen, old.e[i].h) = old.e[i];
                m->n++;
            }
        free(old.e);
    }
    uint64_t h = fnv(key, klen);
    Ent* e = map_slot(m, key, klen, h);
    if (e->h)
    {
        [e->obj release];
        e->obj = [obj retain];
        return;
    }
    e->h = h;
    e->key = malloc(klen);
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->obj = [obj retain];
    m->n++;
}

static Map g_depth_states, g_samplers, g_util_pipes;

/* --- frames, the ring ------------------------------------------------------------------------------- */

static void frame_begin(void)
{
    if (g_cmd)
        return;
    dispatch_semaphore_wait(g_frames_sem, DISPATCH_TIME_FOREVER);
    g_cmd = [[g_queue commandBuffer] retain];
    dispatch_semaphore_t sem = g_frames_sem;
    [g_cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        (void)cb;
        dispatch_semaphore_signal(sem);
    }];
    g_ring_off = 0, g_spills = 0;
}

/* `n` bytes of this frame's ring: where they go, and the buffer and offset to bind */
static void* ring(size_t n, id<MTLBuffer>* buf, NSUInteger* off)
{
    frame_begin();
    uint32_t at = (g_ring_off + RING_ALIGN - 1) & ~(RING_ALIGN - 1);
    if (at + n > RING_SIZE)
    {
        g_spills++;
        id<MTLBuffer> b = [g_dev newBufferWithLength:n > (1u << 20) ? n : (1u << 20) options:MTLResourceStorageModeShared];
        [g_spill addObject:b];
        [b release];
        *buf = b;
        *off = 0;
        return [b contents];
    }
    g_ring_off = at + (uint32_t)n;
    *buf = g_ring[g_slot];
    *off = at;
    return (uint8_t*)[g_ring[g_slot] contents] + at;
}

static void flush_clear(void);

static void end_encoder(void)
{
    if (g_enc)
    {
        [g_enc endEncoding];
        [g_enc release];
        g_enc = nil;
    }
}

/* commits the frame's commands; wait: until the GPU has run them */
static void commit(int wait)
{
    flush_clear();
    end_encoder();
    if (!g_cmd)
        return;
    [g_cmd commit];
    if (wait)
        [g_cmd waitUntilCompleted];
    [g_cmd release];
    g_cmd = nil;
    g_slot = (g_slot + 1) % FRAMES;
    /* the spill buffers stay alive until their command buffer is done: the ring slot's reuse waits on
     * the semaphore, and the array keeps them for FRAMES frames */
    if ([g_spill count] > 64)
        [g_spill removeObjectsInRange:NSMakeRange(0, [g_spill count] - 64)];
    g_targets_dirty = 1;
}

/* --- formats --------------------------------------------------------------------------------------- */

static MTLPixelFormat pixel_format(uint32_t fmt, int* conv, uint32_t* block, uint32_t* bpp, MTLTextureSwizzleChannels* sw)
{
    *conv = CONV_NONE;
    *block = 0;
    *bpp = 4;
    *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue,
        MTLTextureSwizzleAlpha);
    switch (fmt)
    {
    case D_A8R8G8B8: return MTLPixelFormatBGRA8Unorm;
    case D_X8R8G8B8: sw->alpha = MTLTextureSwizzleOne; return MTLPixelFormatBGRA8Unorm;
    case D_A8B8G8R8: return MTLPixelFormatRGBA8Unorm;
    case D_X8B8G8R8: sw->alpha = MTLTextureSwizzleOne; return MTLPixelFormatRGBA8Unorm;
    case D_R8G8B8: *conv = CONV_RGB8; return MTLPixelFormatBGRA8Unorm;
    case D_R5G6B5: *conv = CONV_565; return MTLPixelFormatBGRA8Unorm;
    case D_X1R5G5B5: *conv = CONV_X1555; return MTLPixelFormatBGRA8Unorm;
    case D_A1R5G5B5: *conv = CONV_1555; return MTLPixelFormatBGRA8Unorm;
    case D_A4R4G4B4: *conv = CONV_4444; return MTLPixelFormatBGRA8Unorm;
    case D_A2R10G10B10: return MTLPixelFormatBGR10A2Unorm;
    case D_A2B10G10R10: return MTLPixelFormatRGB10A2Unorm;
    case D_A8: *bpp = 1; return MTLPixelFormatA8Unorm;
    case D_L8:
        *bpp = 1;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        return MTLPixelFormatR8Unorm;
    case D_A8L8:
        *bpp = 2;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleGreen);
        return MTLPixelFormatRG8Unorm;
    case D_L16:
        *bpp = 2;
        *sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleRed, MTLTextureSwizzleOne);
        return MTLPixelFormatR16Unorm;
    case D_V8U8: *bpp = 2; return MTLPixelFormatRG8Snorm;
    case D_Q8W8V8U8: return MTLPixelFormatRGBA8Snorm;
    case D_G16R16: return MTLPixelFormatRG16Unorm;
    case D_A16B16G16R16: *bpp = 8; return MTLPixelFormatRGBA16Unorm;
    case D_R16F: *bpp = 2; return MTLPixelFormatR16Float;
    case D_G16R16F: return MTLPixelFormatRG16Float;
    case D_A16B16G16R16F: *bpp = 8; return MTLPixelFormatRGBA16Float;
    case D_R32F: return MTLPixelFormatR32Float;
    case D_G32R32F: *bpp = 8; return MTLPixelFormatRG32Float;
    case D_A32B32G32R32F: *bpp = 16; return MTLPixelFormatRGBA32Float;
    case D_DXT1: *block = 8; return MTLPixelFormatBC1_RGBA;
    case D_DXT2: case D_DXT3: *block = 16; return MTLPixelFormatBC2_RGBA;
    case D_DXT4: case D_DXT5: *block = 16; return MTLPixelFormatBC3_RGBA;
    case D_D16: case D_D16_LOCKABLE: *bpp = 2; return MTLPixelFormatDepth16Unorm;
    case D_D32: return MTLPixelFormatDepth32Float;
    case D_D24S8: case D_D24X8: case D_D24FS8: case D_INTZ: *bpp = 8; return MTLPixelFormatDepth32Float_Stencil8;
    case D_NULL: return MTLPixelFormatInvalid;
    default: return MTLPixelFormatInvalid;
    }
}

static int has_stencil(MTLPixelFormat pf) { return pf == MTLPixelFormatDepth32Float_Stencil8; }
static int is_depth(MTLPixelFormat pf)
{
    return pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8;
}

static uint32_t expand(uint32_t v, int bits) { return (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)); }

static void convert_row(int conv, const uint8_t* s, uint8_t* d, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x, d += 4)
    {
        uint32_t v = conv == CONV_RGB8 ? 0 : (uint32_t)(s[2 * x] | s[2 * x + 1] << 8);
        switch (conv)
        {
        case CONV_RGB8: d[0] = s[3 * x], d[1] = s[3 * x + 1], d[2] = s[3 * x + 2], d[3] = 255; break;
        case CONV_565: d[0] = (uint8_t)expand(v & 31, 5), d[1] = (uint8_t)expand((v >> 5) & 63, 6), d[2] = (uint8_t)expand(v >> 11, 5), d[3] = 255; break;
        case CONV_1555: case CONV_X1555:
            d[0] = (uint8_t)expand(v & 31, 5), d[1] = (uint8_t)expand((v >> 5) & 31, 5), d[2] = (uint8_t)expand((v >> 10) & 31, 5);
            d[3] = conv == CONV_X1555 || (v & 0x8000) ? 255 : 0;
            break;
        case CONV_4444:
            d[0] = (uint8_t)((v & 15) * 17), d[1] = (uint8_t)(((v >> 4) & 15) * 17), d[2] = (uint8_t)(((v >> 8) & 15) * 17);
            d[3] = (uint8_t)((v >> 12) * 17);
            break;
        }
    }
}

/* --- textures -------------------------------------------------------------------------------------- */

static void level_size(const G9Tex* t, uint32_t level, uint32_t* w, uint32_t* h, uint32_t* d)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = t->h >> level ? t->h >> level : 1;
    *d = t->d >> level ? t->d >> level : 1;
}

G9Tex* g9_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    @autoreleasepool
    {
        G9Tex* t = (G9Tex*)calloc(1, sizeof *t);
        t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->d = d ? d : 1;
        t->levels = levels ? levels : 1;
        MTLTextureSwizzleChannels sw;
        t->pf = pixel_format(fmt, &t->conv, &t->block, &t->bpp, &sw);
        if (t->pf == MTLPixelFormatInvalid)
        {
            if (fmt != D_NULL)
            {
                char fc[5] = { 0 };
                if (fmt > 0xFF)
                    memcpy(fc, &fmt, 4);
                rt_log("[gfx9] texture format %s%u has no Metal match\n", fc, fc[0] ? 0 : fmt);
            }
            return t; /* kept: a NULL render target, or nothing drawn from it */
        }
        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.textureType = type == G9_TEX_CUBE ? MTLTextureTypeCube : type == G9_TEX_3D ? MTLTextureType3D : MTLTextureType2D;
        td.pixelFormat = t->pf;
        td.width = t->w;
        td.height = t->h;
        td.depth = type == G9_TEX_3D ? t->d : 1;
        td.mipmapLevelCount = t->levels;
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        if (use != G9_USE_SAMPLE)
        {
            td.usage |= MTLTextureUsageRenderTarget;
            td.storageMode = MTLStorageModePrivate;
        }
        else
            td.storageMode = MTLStorageModeShared;
        t->tex = [g_dev newTextureWithDescriptor:td];
        [td release];
        if (!t->tex)
        {
            rt_log("[gfx9] cannot create a %ux%u texture of format %u\n", t->w, t->h, fmt);
            return t;
        }
        int swizzled = sw.red != MTLTextureSwizzleRed || sw.green != MTLTextureSwizzleGreen || sw.blue != MTLTextureSwizzleBlue ||
                       sw.alpha != MTLTextureSwizzleAlpha;
        if (swizzled && !is_depth(t->pf))
            t->view = [t->tex newTextureViewWithPixelFormat:t->pf textureType:t->tex.textureType levels:NSMakeRange(0, t->levels)
                                                     slices:NSMakeRange(0, type == G9_TEX_CUBE ? 6 : 1) swizzle:sw];
        else
            t->view = [t->tex retain];
        live_add(t);
    return t;
    }
}

void g9_tex_destroy(G9Tex* t)
{
    if (!t)
        return;
    for (int i = 0; i < MAX_LIVE; ++i)
        if (g_live[i] == t)
            g_live[i] = NULL;
    [t->view release];
    [t->tex release];
    free(t);
}

static void tex_upload(G9Tex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch, uint32_t slice);

void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch, uint32_t slice)
{
    uint64_t t0 = rt_monotonic_ns();
    tex_upload(t, face, level, src, pitch, slice);
    g_fc_uploads++, g_fc_upload_ns += rt_monotonic_ns() - t0;
    if (t && level < t->levels)
    {
        uint32_t w, h, d;
        level_size(t, level, &w, &h, &d);
        g_fc_upload_bytes += (uint64_t)pitch * (t->block ? (h + 3) / 4 : h) * (t->type == G9_TEX_3D ? d : 1);
    }
}

static void tex_upload(G9Tex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch, uint32_t slice)
{
    if (!t || !t->tex || level >= t->levels || t->use != G9_USE_SAMPLE)
        return;
    uint32_t w, h, d;
    level_size(t, level, &w, &h, &d);
    MTLRegion r = MTLRegionMake3D(0, 0, 0, w, h, t->type == G9_TEX_3D ? d : 1);
    NSUInteger slicen = t->type == G9_TEX_CUBE ? face : 0;
    if (t->conv == CONV_NONE)
    {
        [t->tex replaceRegion:r mipmapLevel:level slice:slicen withBytes:src bytesPerRow:pitch
                bytesPerImage:t->type == G9_TEX_3D ? slice : 0];
        return;
    }
    uint32_t dpitch = w * 4, depth = t->type == G9_TEX_3D ? d : 1;
    uint8_t* tmp = (uint8_t*)malloc((size_t)dpitch * h * depth);
    for (uint32_t z = 0; z < depth; ++z)
        for (uint32_t y = 0; y < h; ++y)
            convert_row(t->conv, (const uint8_t*)src + (size_t)z * slice + (size_t)y * pitch, tmp + ((size_t)z * h + y) * dpitch, w);
    [t->tex replaceRegion:r mipmapLevel:level slice:slicen withBytes:tmp bytesPerRow:dpitch bytesPerImage:(NSUInteger)dpitch * h];
    free(tmp);
}

void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    flush_clear();
    if (!t || !t->tex || t->block || is_depth(t->pf))
        return;
    @autoreleasepool
    {
        uint32_t w, h, d;
        level_size(t, level, &w, &h, &d);
        uint32_t row = w * t->bpp;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:(NSUInteger)row * h options:MTLResourceStorageModeShared];
        frame_begin();
        end_encoder();
        id<MTLBlitCommandEncoder> b = [g_cmd blitCommandEncoder];
        [b copyFromTexture:t->tex sourceSlice:t->type == G9_TEX_CUBE ? face : 0 sourceLevel:level sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:(NSUInteger)row * h];
        [b endEncoding];
        commit(1);
        for (uint32_t y = 0; y < h; ++y)
            memcpy((uint8_t*)dst + (size_t)y * pitch, (uint8_t*)[buf contents] + (size_t)y * row, row < pitch ? row : pitch);
        [buf release];
    }
}

/* --- shaders and programs ---------------------------------------------------------------------------- */

G9Shader* g9_shader_create(const uint32_t* tokens, size_t n)
{
    G9Shader* s = (G9Shader*)calloc(1, sizeof *s);
    s->tokens = (uint32_t*)malloc(4 * n);
    memcpy(s->tokens, tokens, 4 * n);
    char err[128];
    if (!sm3_parse(s->tokens, n, &s->s, err, sizeof err))
    {
        rt_log("[gfx9] shader does not parse: %s\n", err);
        free(s->tokens);
        free(s);
        return NULL;
    }
    s->id = g_next_shader_id++;
    uint32_t all = s->s.pixel ? 224 : 256;
    for (size_t i = 0; i < s->s.nins; ++i)
    {
        const Sm3Ins* in = &s->s.ins[i];
        for (int k = 0; k < in->nsrc; ++k)
        {
            const Sm3Reg* r = &in->src[k];
            if (r->type == SM3_CONST2 || r->type == SM3_CONST3 || r->type == SM3_CONST4 || (r->type == SM3_CONST && r->rel))
                s->nconst = all;
            else if (r->type == SM3_CONST && r->num + 1u > s->nconst)
                s->nconst = r->num + 1u < all ? r->num + 1u : all;
        }
        if (in->op != SM3_DCL)
            continue;
        if (!s->s.pixel && in->dst.type == SM3_INPUT && in->dst.num < 16)
        {
            s->in_mask |= (uint16_t)(1u << in->dst.num);
            s->in_usage[in->dst.num] = in->usage;
            s->in_index[in->dst.num] = in->usage_index;
        }
        if (in->dst.type == SM3_SAMPLER && in->dst.num < 20)
            s->tex_type[in->dst.num] = in->tex_type ? in->tex_type : SM3_TEX_2D;
    }
    return s;
}

void g9_shader_destroy(G9Shader* s)
{
    /* programs keyed by its id stay in the cache: ids are never reused */
    if (!s)
        return;
    sm3_free(&s->s);
    free(s->tokens);
    free(s);
}

typedef struct ProgKey
{
    uint32_t vs, ps;
    uint8_t in_kind[16];
} ProgKey;

/* Shader pairs compile and pipelines build in the background: compiling a pair on the drawing
 * thread stalled the frame 50-130 ms whenever new scenery came into view (the world streams as the
 * player walks). A map entry holds g_pending meanwhile and the draws that need it are skipped; the
 * results come back through g_done, drained on the drawing thread (the maps are not thread-safe). */
static id g_pending;
static NSMutableArray* g_done; /* @[ @(kind), key NSData, result or NSNull ] */
enum { DONE_PROGRAM, DONE_PIPELINE };

static void async_done(int kind, const void* key, size_t klen, id result)
{
    NSArray* row = @[ @(kind), [NSData dataWithBytes:key length:klen], result ? result : [NSNull null] ];
    @synchronized(g_done)
    {
        [g_done addObject:row];
    }
}

static Map g_programs, g_pipelines;
static void drain_async(void)
{
    if (!g_done || ![g_done count])
        return;
    NSArray* rows;
    @synchronized(g_done)
    {
        rows = [[g_done copy] autorelease];
        [g_done removeAllObjects];
    }
    for (NSArray* row in rows)
    {
        NSData* key = row[1];
        map_put([row[0] intValue] == DONE_PROGRAM ? &g_programs : &g_pipelines, [key bytes], [key length], row[2]);
    }
}

static id<MTLLibrary> compile(const char* src, const char* what)
{
    NSError* e = nil;
    MTLCompileOptions* o = [[MTLCompileOptions alloc] init];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:[NSString stringWithUTF8String:src] options:o error:&e];
    [o release];
    if (!lib)
        rt_log("[gfx9] %s does not compile:\n%s\n", what, [[e localizedDescription] UTF8String]);
    return lib;
}

static id<MTLLibrary> program(G9Shader* vs, G9Shader* ps, const uint8_t in_kind[16])
{
    ProgKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id, k.ps = ps->id;
    memcpy(k.in_kind, in_kind, 16);
    id lib = map_get(&g_programs, &k, sizeof k);
    if (lib)
        return lib == (id)[NSNull null] || lib == g_pending ? nil : lib;
    char err[256];
    char* msl = sm3_msl_pair(&vs->s, &ps->s, in_kind, err, sizeof err);
    if (msl && getenv("XIVLEGACY_MSLDUMP"))
    {
        /* debugging: each translated pair's source, by shader ids (the ids traces print) */
        char path[1024];
        snprintf(path, sizeof path, "%s/vs%u_ps%u.metal", getenv("XIVLEGACY_MSLDUMP"), vs->id, ps->id);
        FILE* f = fopen(path, "w");
        if (f)
        {
            fputs(msl, f);
            fclose(f);
        }
    }
    if (!msl)
    {
        rt_log("[gfx9] shader pair %u/%u not translated: %s\n", vs->id, ps->id, err);
        map_put(&g_programs, &k, sizeof k, [NSNull null]);
        return nil;
    }
    g_fc_compiles++;
    map_put(&g_programs, &k, sizeof k, g_pending);
    NSString* src = [NSString stringWithUTF8String:msl];
    int show_src = getenv("XIVLEGACY_SHADER_SOURCE") != NULL;
    char* keep = show_src ? msl : NULL;
    if (!show_src)
        free(msl);
    MTLCompileOptions* o = [[MTLCompileOptions alloc] init];
    ProgKey kc = k;
    [g_dev newLibraryWithSource:src options:o completionHandler:^(id<MTLLibrary> l, NSError* e) {
        if (!l)
        {
            rt_log("[gfx9] shader pair %u/%u does not compile:\n%s\n", kc.vs, kc.ps, [[e localizedDescription] UTF8String]);
            if (keep)
                rt_log("%s\n", keep);
        }
        free(keep);
        async_done(DONE_PROGRAM, &kc, sizeof kc, l);
    }];
    [o release];
    return nil;
}

/* --- state objects ---------------------------------------------------------------------------------- */

static MTLCompareFunction compare(uint32_t f)
{
    static const MTLCompareFunction m[] = { MTLCompareFunctionAlways, MTLCompareFunctionNever, MTLCompareFunctionLess,
        MTLCompareFunctionEqual, MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
        MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways };
    return f < 9 ? m[f] : MTLCompareFunctionAlways;
}

static MTLStencilOperation stencil_op(uint32_t op)
{
    static const MTLStencilOperation m[] = { MTLStencilOperationKeep, MTLStencilOperationKeep, MTLStencilOperationZero,
        MTLStencilOperationReplace, MTLStencilOperationIncrementClamp, MTLStencilOperationDecrementClamp,
        MTLStencilOperationInvert, MTLStencilOperationIncrementWrap, MTLStencilOperationDecrementWrap };
    return op < 9 ? m[op] : MTLStencilOperationKeep;
}

typedef struct DepthKey
{
    uint8_t ztest, zwrite, zfunc, stencil, two_sided;
    uint8_t fail, zfail, pass, func, ccw_fail, ccw_zfail, ccw_pass, ccw_func;
    uint8_t pad[3];
    uint32_t mask, wmask;
} DepthKey;

static id<MTLDepthStencilState> depth_state(const uint32_t* rs, int has_depth, int stencil_ok)
{
    DepthKey k;
    memset(&k, 0, sizeof k);
    if (has_depth)
    {
        k.ztest = rs[7] != 0;
        k.zwrite = rs[7] != 0 && rs[14] != 0;
        k.zfunc = (uint8_t)rs[23];
    }
    if (stencil_ok && rs[52])
    {
        k.stencil = 1;
        k.fail = (uint8_t)rs[53], k.zfail = (uint8_t)rs[54], k.pass = (uint8_t)rs[55], k.func = (uint8_t)rs[56];
        k.two_sided = rs[185] != 0;
        if (k.two_sided)
            k.ccw_fail = (uint8_t)rs[186], k.ccw_zfail = (uint8_t)rs[187], k.ccw_pass = (uint8_t)rs[188], k.ccw_func = (uint8_t)rs[189];
        k.mask = rs[58], k.wmask = rs[59];
    }
    id s = map_get(&g_depth_states, &k, sizeof k);
    if (s)
        return s;
    MTLDepthStencilDescriptor* d = [[MTLDepthStencilDescriptor alloc] init];
    d.depthCompareFunction = k.ztest ? compare(k.zfunc) : MTLCompareFunctionAlways;
    d.depthWriteEnabled = k.zwrite;
    if (k.stencil)
    {
        MTLStencilDescriptor* f = [[MTLStencilDescriptor alloc] init];
        f.stencilCompareFunction = compare(k.func);
        f.stencilFailureOperation = stencil_op(k.fail);
        f.depthFailureOperation = stencil_op(k.zfail);
        f.depthStencilPassOperation = stencil_op(k.pass);
        f.readMask = k.mask & 0xFF;
        f.writeMask = k.wmask & 0xFF;
        d.frontFaceStencil = f;
        if (k.two_sided)
        {
            MTLStencilDescriptor* b = [[MTLStencilDescriptor alloc] init];
            b.stencilCompareFunction = compare(k.ccw_func);
            b.stencilFailureOperation = stencil_op(k.ccw_fail);
            b.depthFailureOperation = stencil_op(k.ccw_zfail);
            b.depthStencilPassOperation = stencil_op(k.ccw_pass);
            b.readMask = k.mask & 0xFF;
            b.writeMask = k.wmask & 0xFF;
            d.backFaceStencil = b;
            [b release];
        }
        else
            d.backFaceStencil = f;
        [f release];
    }
    id<MTLDepthStencilState> st = [g_dev newDepthStencilStateWithDescriptor:d];
    [d release];
    map_put(&g_depth_states, &k, sizeof k, st);
    [st release];
    return st;
}

static MTLSamplerAddressMode address(uint32_t a)
{
    switch (a)
    {
    case 2: return MTLSamplerAddressModeMirrorRepeat;
    case 3: return MTLSamplerAddressModeClampToEdge;
    case 4: return MTLSamplerAddressModeClampToBorderColor;
    case 5: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

typedef struct SampKey
{
    uint8_t u, v, w, mag, min, mip, aniso, maxlevel;
    uint32_t border;
} SampKey;

static id<MTLSamplerState> sampler(const uint32_t* ss)
{
    SampKey k = { (uint8_t)ss[1], (uint8_t)ss[2], (uint8_t)ss[3], (uint8_t)ss[5], (uint8_t)ss[6], (uint8_t)ss[7],
                  (uint8_t)(ss[10] > 16 ? 16 : ss[10]), (uint8_t)ss[9], ss[4] };
    if (k.min != 3 && k.mag != 3)
        k.aniso = 1;
    id s = map_get(&g_samplers, &k, sizeof k);
    if (s)
        return s;
    MTLSamplerDescriptor* d = [[MTLSamplerDescriptor alloc] init];
    d.sAddressMode = address(k.u);
    d.tAddressMode = address(k.v);
    d.rAddressMode = address(k.w);
    d.magFilter = k.mag >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = k.min >= 2 ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = k.mip == 0 ? MTLSamplerMipFilterNotMipmapped : k.mip == 1 ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.maxAnisotropy = k.aniso ? k.aniso : 1;
    d.lodMinClamp = k.maxlevel;
    /* Metal's three border colors: the nearest of them */
    uint32_t a = k.border >> 24, l = ((k.border >> 16 & 255) + (k.border >> 8 & 255) + (k.border & 255)) / 3;
    d.borderColor = a < 128 ? MTLSamplerBorderColorTransparentBlack : l < 128 ? MTLSamplerBorderColorOpaqueBlack : MTLSamplerBorderColorOpaqueWhite;
    id<MTLSamplerState> st = [g_dev newSamplerStateWithDescriptor:d];
    [d release];
    map_put(&g_samplers, &k, sizeof k, st);
    [st release];
    return st;
}

/* --- vertex formats --------------------------------------------------------------------------------- */

/* D3DDECLTYPE to the Metal vertex format, and the shader input kind it needs */
static MTLVertexFormat vertex_format(uint32_t type, uint8_t* kind)
{
    *kind = SM3_IN_FLOAT;
    switch (type)
    {
    case 0: return MTLVertexFormatFloat;
    case 1: return MTLVertexFormatFloat2;
    case 2: return MTLVertexFormatFloat3;
    case 3: return MTLVertexFormatFloat4;
    case 4: return MTLVertexFormatUChar4Normalized_BGRA;
    case 5: *kind = SM3_IN_UINT; return MTLVertexFormatUChar4;
    case 6: *kind = SM3_IN_INT; return MTLVertexFormatShort2;
    case 7: *kind = SM3_IN_INT; return MTLVertexFormatShort4;
    case 8: return MTLVertexFormatUChar4Normalized;
    case 9: return MTLVertexFormatShort2Normalized;
    case 10: return MTLVertexFormatShort4Normalized;
    case 11: return MTLVertexFormatUShort2Normalized;
    case 12: return MTLVertexFormatUShort4Normalized;
    case 13: return MTLVertexFormatUInt1010102Normalized; /* UDEC3: unnormalized in D3D; rare */
    case 14: return MTLVertexFormatInt1010102Normalized;
    case 15: return MTLVertexFormatHalf2;
    case 16: return MTLVertexFormatHalf4;
    default: return MTLVertexFormatInvalid;
    }
}

/* --- pipelines -------------------------------------------------------------------------------------- */

typedef struct PipeKey
{
    uint32_t vs, ps;
    uint8_t in_kind[16];
    uint8_t attr_fmt[16], attr_buf[16];
    uint16_t attr_off[16];
    uint16_t stride[17];
    uint8_t instanced[17];
    uint8_t blend, src, dst, op, srca, dsta, opa, pad;
    uint8_t wmask[4];
    uint32_t color[4], depth, stencil;
} PipeKey;

static MTLBlendFactor blend_factor(uint32_t f)
{
    switch (f)
    {
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: case 12: return MTLBlendFactorSourceAlpha;
    case 6: case 13: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return MTLBlendFactorSourceAlphaSaturated;
    case 14: return MTLBlendFactorBlendColor;
    case 15: return MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
    }
}

static MTLBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

static MTLColorWriteMask write_mask(uint32_t m)
{
    return (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) |
           (m & 8 ? MTLColorWriteMaskAlpha : 0);
}

static int blendable(MTLPixelFormat pf)
{
    return pf != MTLPixelFormatR32Float && pf != MTLPixelFormatRG32Float && pf != MTLPixelFormatRGBA32Float;
}

static id<MTLRenderPipelineState> pipeline(const PipeKey* k, id<MTLLibrary> lib)
{
    id p = map_get(&g_pipelines, k, sizeof *k);
    if (p)
        return p == (id)[NSNull null] || p == g_pending ? nil : p;
    uint64_t pipe_t0 = rt_monotonic_ns();
    g_fc_pipes++;
    int async = k->pad != 0xDB; /* the debug readback pipeline (cpuvs) is needed at once */
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [lib newFunctionWithName:@"vs_main"];
    id<MTLFunction> ff = [lib newFunctionWithName:@"fs_main"];
    d.vertexFunction = vf;
    d.fragmentFunction = ff;
    for (int i = 0; i < 4; ++i)
    {
        MTLRenderPipelineColorAttachmentDescriptor* c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k->color[i];
        if (k->color[i] == MTLPixelFormatInvalid)
            continue;
        c.writeMask = write_mask(k->wmask[i]);
        if (k->blend && blendable((MTLPixelFormat)k->color[i]))
        {
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = blend_factor(k->src);
            c.destinationRGBBlendFactor = blend_factor(k->src == 12 ? 6 : k->src == 13 ? 5 : k->dst);
            c.rgbBlendOperation = blend_op(k->op);
            c.sourceAlphaBlendFactor = blend_factor(k->srca);
            c.destinationAlphaBlendFactor = blend_factor(k->srca == 12 ? 6 : k->srca == 13 ? 5 : k->dsta);
            c.alphaBlendOperation = blend_op(k->opa);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (int r = 0; r < 16; ++r)
    {
        if (!k->attr_fmt[r])
            continue;
        vd.attributes[r].format = (MTLVertexFormat)k->attr_fmt[r];
        vd.attributes[r].offset = k->attr_off[r];
        vd.attributes[r].bufferIndex = k->attr_buf[r];
    }
    for (int s = 0; s < 17; ++s)
    {
        if (!k->stride[s])
            continue;
        int b = s == 16 ? 30 : s; /* 16: the zeros buffer */
        if (s == 16)
        {
            vd.layouts[b].stride = 16;
            vd.layouts[b].stepFunction = MTLVertexStepFunctionConstant;
            vd.layouts[b].stepRate = 0;
        }
        else
        {
            vd.layouts[b].stride = k->stride[s];
            vd.layouts[b].stepFunction = k->instanced[s] ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            vd.layouts[b].stepRate = k->instanced[s] ? k->instanced[s] : 1;
        }
    }
    d.vertexDescriptor = vd;
    if (async)
    {
        PipeKey kc = *k;
        map_put(&g_pipelines, k, sizeof *k, g_pending);
        [g_dev newRenderPipelineStateWithDescriptor:d completionHandler:^(id<MTLRenderPipelineState> st, NSError* e) {
            if (!st)
                rt_log("[gfx9] pipeline for shader pair %u/%u failed: %s\n", kc.vs, kc.ps, [[e localizedDescription] UTF8String]);
            async_done(DONE_PIPELINE, &kc, sizeof kc, st);
        }];
        [vf release];
        [ff release];
        [d release];
        g_fc_pipe_ns += rt_monotonic_ns() - pipe_t0;
        return nil;
    }
    NSError* e = nil;
    id<MTLRenderPipelineState> st = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!st)
        rt_log("[gfx9] pipeline for shader pair %u/%u failed: %s\n", k->vs, k->ps, [[e localizedDescription] UTF8String]);
    [vf release];
    [ff release];
    [d release];
    map_put(&g_pipelines, k, sizeof *k, st ? (id)st : (id)[NSNull null]);
    g_fc_pipe_ns += rt_monotonic_ns() - pipe_t0;
    [st release];
    return st;
}

/* --- passes, targets -------------------------------------------------------------------------------- */

static void target_size(uint32_t* w, uint32_t* h)
{
    *w = *h = 0;
    uint32_t d;
    if (g_rt[0].tex && g_rt[0].tex->tex)
        level_size(g_rt[0].tex, g_rt[0].level, w, h, &d);
    else if (g_ds && g_ds->tex)
        level_size(g_ds, 0, w, h, &d);
}

void g9_set_targets(const G9Target rt[4], G9Tex* depth)
{
    {
        char a[64], b[64], c1[64], c2[64], c3[64];
        TRACE("[trace] targets %s depth %s mrt %s %s %s\n", tname(rt[0].tex, a, sizeof a), tname(depth, b, sizeof b), tname(rt[1].tex, c1, sizeof c1),
            tname(rt[2].tex, c2, sizeof c2), tname(rt[3].tex, c3, sizeof c3));
    }
    if (!g_dev)
        return;
    if (!memcmp(rt, g_rt, sizeof g_rt) && depth == g_ds && !g_targets_dirty)
        return;
    flush_clear();
    end_encoder();
    memcpy(g_rt, rt, sizeof g_rt);
    g_ds = depth;
    g_targets_dirty = 1;
    g_clear_flags = 0;
}

/* the render pass for the current targets, opened now if it is not */
static int begin_pass(void)
{
    if (g_enc)
        return 1;
    int any = g_ds && g_ds->tex;
    for (int i = 0; i < 4; ++i)
        any |= g_rt[i].tex && g_rt[i].tex->tex != nil;
    if (!any)
        return 0;
    frame_begin();
    MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
    for (int i = 0; i < 4; ++i)
    {
        if (!g_rt[i].tex || !g_rt[i].tex->tex)
            continue;
        MTLRenderPassColorAttachmentDescriptor* c = p.colorAttachments[i];
        c.texture = g_rt[i].tex->tex;
        c.level = g_rt[i].level;
        c.slice = g_rt[i].tex->type == G9_TEX_CUBE ? g_rt[i].face : 0;
        if (g_clear_flags & 1)
        {
            c.loadAction = MTLLoadActionClear;
            uint32_t v = g_clear_color;
            c.clearColor = MTLClearColorMake((v >> 16 & 255) / 255.0, (v >> 8 & 255) / 255.0, (v & 255) / 255.0, (v >> 24) / 255.0);
        }
        else
            c.loadAction = MTLLoadActionLoad;
        c.storeAction = MTLStoreActionStore;
    }
    if (g_ds && g_ds->tex)
    {
        p.depthAttachment.texture = g_ds->tex;
        p.depthAttachment.loadAction = g_clear_flags & 2 ? MTLLoadActionClear : MTLLoadActionLoad;
        p.depthAttachment.clearDepth = g_clear_z;
        p.depthAttachment.storeAction = MTLStoreActionStore;
        if (has_stencil(g_ds->pf))
        {
            p.stencilAttachment.texture = g_ds->tex;
            p.stencilAttachment.loadAction = g_clear_flags & 4 ? MTLLoadActionClear : MTLLoadActionLoad;
            p.stencilAttachment.clearStencil = g_clear_stencil & 0xFF;
            p.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    }
    g_clear_flags = 0;
    g_enc = [[g_cmd renderCommandEncoderWithDescriptor:p] retain];
    [g_enc setFrontFacingWinding:MTLWindingClockwise];
    g_targets_dirty = 0;
    return 1;
}

/* A clear still waiting for the next pass (g9_clear defers a whole-target clear to the pass's load
 * action) carried out now: the targets are about to change or be read. Dropping it left last frame's
 * pixels wherever a frame drew nothing - cutscenes smeared their moving camera across the screen. */
static void flush_clear(void)
{
    if (!g_clear_flags || g_enc)
        return;
    if (begin_pass())
        end_encoder();
    g_clear_flags = 0;
}

static void set_viewport(const uint32_t vp[6])
{
    float zn, zf;
    memcpy(&zn, &vp[4], 4);
    memcpy(&zf, &vp[5], 4);
    MTLViewport v = { vp[0], vp[1], vp[2], vp[3], zn, zf };
    [g_enc setViewport:v];
}

/* the scissor rectangle, cut to the target (Metal refuses one outside it) */
static void set_scissor(const int32_t* r)
{
    uint32_t tw, th;
    target_size(&tw, &th);
    int32_t l = 0, t = 0, rr = (int32_t)tw, b = (int32_t)th;
    if (r)
    {
        l = r[0] > 0 ? r[0] : 0, t = r[1] > 0 ? r[1] : 0;
        rr = r[2] < (int32_t)tw ? r[2] : (int32_t)tw, b = r[3] < (int32_t)th ? r[3] : (int32_t)th;
    }
    if (rr <= l || b <= t)
        l = t = 0, rr = b = 0; /* an empty scissor: Metal takes a zero size */
    MTLScissorRect s = { (NSUInteger)l, (NSUInteger)t, (NSUInteger)(rr - l), (NSUInteger)(b - t) };
    [g_enc setScissorRect:s];
}

static uint32_t current_formats(uint32_t color[4], uint32_t* depth, uint32_t* stencil)
{
    for (int i = 0; i < 4; ++i)
        color[i] = g_rt[i].tex && g_rt[i].tex->tex ? (uint32_t)g_rt[i].tex->pf : MTLPixelFormatInvalid;
    *depth = g_ds && g_ds->tex ? (uint32_t)g_ds->pf : MTLPixelFormatInvalid;
    *stencil = g_ds && g_ds->tex && has_stencil(g_ds->pf) ? (uint32_t)g_ds->pf : MTLPixelFormatInvalid;
    return 0;
}

/* --- clears ---------------------------------------------------------------------------------------- */

static const char UTIL_MSL[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct QV { float4 pos [[position]]; float2 uv; };\n"
    "vertex QV quad_vs(uint vid [[vertex_id]], constant float4& uvx [[buffer(0)]])\n"
    "{\n"
    "    float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "    QV o; o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0); o.uv = uvx.xy + p * uvx.zw; return o;\n"
    "}\n"
    "struct CC { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; };\n"
    "struct CD { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]]; float z [[depth(any)]]; };\n"
    "fragment CC clear_fs(constant float4* u [[buffer(0)]]) { CC o; o.c0 = o.c1 = o.c2 = o.c3 = u[0]; return o; }\n"
    "fragment CD clear_depth_fs(constant float4* u [[buffer(0)]]) { CD o; o.c0 = o.c1 = o.c2 = o.c3 = u[0]; o.z = u[1].x; return o; }\n"
    "fragment float4 copy_fs(QV in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, in.uv); }\n";

typedef struct UtilKey
{
    uint32_t kind; /* 0 clear, 1 clear with depth, 2 copy */
    uint32_t color[4], depth, stencil;
    uint8_t wmask[4];
} UtilKey;

static id<MTLRenderPipelineState> util_pipeline(const UtilKey* k)
{
    id p = map_get(&g_util_pipes, k, sizeof *k);
    if (p)
        return p == (id)[NSNull null] ? nil : p;
    MTLRenderPipelineDescriptor* d = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vf = [g_util newFunctionWithName:@"quad_vs"];
    id<MTLFunction> ff = [g_util newFunctionWithName:k->kind == 0 ? @"clear_fs" : k->kind == 1 ? @"clear_depth_fs" : @"copy_fs"];
    d.vertexFunction = vf;
    d.fragmentFunction = ff;
    for (int i = 0; i < 4; ++i)
    {
        d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->color[i];
        d.colorAttachments[i].writeMask = write_mask(k->wmask[i]);
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->depth;
    d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->stencil;
    NSError* e = nil;
    id<MTLRenderPipelineState> st = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!st)
        rt_log("[gfx9] utility pipeline %u: %s\n", k->kind, [[e localizedDescription] UTF8String]);
    [vf release];
    [ff release];
    [d release];
    map_put(&g_util_pipes, k, sizeof *k, st ? (id)st : (id)[NSNull null]);
    [st release];
    return st;
}

void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t viewport[6], const int32_t* scissor)
{
    TRACE("[trace] clear flags %u color %08x\n", flags, color);
    if (!g_dev)
        return;
    @autoreleasepool
    {
        if (!g_ds || !g_ds->tex)
            flags &= 1;
        else if (!has_stencil(g_ds->pf))
            flags &= 3;
        if (!flags)
            return;
        uint32_t tw, th;
        target_size(&tw, &th);
        int whole = !nrects && viewport[0] == 0 && viewport[1] == 0 && viewport[2] >= tw && viewport[3] >= th &&
                    (!scissor || (scissor[0] <= 0 && scissor[1] <= 0 && scissor[2] >= (int32_t)tw && scissor[3] >= (int32_t)th));
        if (whole && !g_enc)
        {
            /* the next pass starts cleared */
            g_clear_flags |= flags;
            if (flags & 1)
                g_clear_color = color;
            if (flags & 2)
                g_clear_z = z;
            if (flags & 4)
                g_clear_stencil = stencil;
            return;
        }
        if (whole)
        {
            /* drawn into already: end the pass, start a cleared one (keeping what is not cleared) */
            end_encoder();
            g_clear_flags = flags, g_clear_color = color, g_clear_z = z, g_clear_stencil = stencil;
            begin_pass();
            return;
        }
        if (!begin_pass())
            return;
        UtilKey k;
        memset(&k, 0, sizeof k);
        k.kind = flags & 2 ? 1 : 0;
        current_formats(k.color, &k.depth, &k.stencil);
        for (int i = 0; i < 4; ++i)
            k.wmask[i] = flags & 1 ? 15 : 0;
        id<MTLRenderPipelineState> p = util_pipeline(&k);
        if (!p)
            return;
        [g_enc setRenderPipelineState:p];
        DepthKey dk;
        memset(&dk, 0, sizeof dk);
        uint32_t rs[256] = { 0 };
        rs[7] = (flags & 2) != 0, rs[14] = (flags & 2) != 0, rs[23] = 8;
        rs[52] = (flags & 4) != 0, rs[53] = rs[54] = rs[55] = 3, rs[56] = 8, rs[58] = rs[59] = 0xFF;
        [g_enc setDepthStencilState:depth_state(rs, (flags & 2) != 0, (flags & 4) != 0)];
        [g_enc setStencilReferenceValue:stencil & 0xFF];
        [g_enc setCullMode:MTLCullModeNone];
        [g_enc setTriangleFillMode:MTLTriangleFillModeFill];
        [g_enc setDepthBias:0 slopeScale:0 clamp:0];
        float u[8] = { (color >> 16 & 255) / 255.0f, (color >> 8 & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f, z, 0, 0, 0 };
        float uvx[4] = { 0, 0, 1, 1 };
        [g_enc setFragmentBytes:u length:sizeof u atIndex:0];
        [g_enc setVertexBytes:uvx length:sizeof uvx atIndex:0];
        /* the viewport with the depth range at full: the clear's depth is the value itself */
        uint32_t vp[6];
        memcpy(vp, viewport, sizeof vp);
        float zn = 0.0f, zf = 1.0f;
        memcpy(&vp[4], &zn, 4);
        memcpy(&vp[5], &zf, 4);
        set_viewport(vp);
        uint32_t n = nrects ? nrects : 1;
        for (uint32_t i = 0; i < n; ++i)
        {
            int32_t r[4] = { (int32_t)viewport[0], (int32_t)viewport[1], (int32_t)(viewport[0] + viewport[2]), (int32_t)(viewport[1] + viewport[3]) };
            if (nrects)
            {
                r[0] = rects[4 * i] > r[0] ? rects[4 * i] : r[0];
                r[1] = rects[4 * i + 1] > r[1] ? rects[4 * i + 1] : r[1];
                r[2] = rects[4 * i + 2] < r[2] ? rects[4 * i + 2] : r[2];
                r[3] = rects[4 * i + 3] < r[3] ? rects[4 * i + 3] : r[3];
            }
            if (scissor)
            {
                r[0] = scissor[0] > r[0] ? scissor[0] : r[0];
                r[1] = scissor[1] > r[1] ? scissor[1] : r[1];
                r[2] = scissor[2] < r[2] ? scissor[2] : r[2];
                r[3] = scissor[3] < r[3] ? scissor[3] : r[3];
            }
            set_scissor(r);
            [g_enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    }
}

/* --- draws ----------------------------------------------------------------------------------------- */

static uint32_t vertex_count(uint32_t prim, uint32_t n)
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

static MTLPrimitiveType prim_type(uint32_t prim)
{
    switch (prim)
    {
    case 1: return MTLPrimitiveTypePoint;
    case 2: return MTLPrimitiveTypeLine;
    case 3: return MTLPrimitiveTypeLineStrip;
    case 5: return MTLPrimitiveTypeTriangleStrip;
    default: return MTLPrimitiveTypeTriangle; /* lists, and fans made lists */
    }
}

static id<MTLTexture> dummy(int type)
{
    int i = type == SM3_TEX_CUBE ? 1 : type == SM3_TEX_VOLUME ? 2 : 0;
    if (!g_dummy[i])
    {
        MTLTextureDescriptor* td = [[MTLTextureDescriptor alloc] init];
        td.textureType = i == 1 ? MTLTextureTypeCube : i == 2 ? MTLTextureType3D : MTLTextureType2D;
        td.pixelFormat = MTLPixelFormatRGBA8Unorm;
        td.width = td.height = 1;
        td.depth = 1;
        td.storageMode = MTLStorageModeShared;
        g_dummy[i] = [g_dev newTextureWithDescriptor:td];
        [td release];
        uint8_t zero[4] = { 0, 0, 0, 0 };
        for (int s = 0; s < (i == 1 ? 6 : 1); ++s)
            [g_dummy[i] replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:s withBytes:zero bytesPerRow:4 bytesPerImage:4];
    }
    return g_dummy[i];
}


/* debugging (g9_debug): 1 draws triangles as lines, 2 samples mid-gray in place of 2D textures, 3 in
 * place of render targets read as textures */
static int g_debug_mode;
void g9_debug(int mode) { g_debug_mode = mode; }
static uint32_t g_skip_vs; /* debugging: draws with this vertex shader id skipped (g9_skip_vs) */
void g9_skip_vs(uint32_t id) { g_skip_vs = id; }
static uint32_t g_only_vs;
void g9_only_vs(uint32_t id) { g_only_vs = id; }
static uint32_t g_cpuvs; /* debugging: the next draw with this vertex shader id checked against the CPU interpreter */
void g9_cpu_check_vs(uint32_t id) { g_cpuvs = id; }
static uint32_t g_dump_vs, g_dump_left; /* debugging: the next draws with this vertex shader id described (g9_dump_vs) */
void g9_dump_vs(uint32_t id, uint32_t count) { g_dump_vs = id, g_dump_left = count; }

static void dump_draw(const G9Draw* d)
{
    printf("[drawdump] vs%u ps%u prim %u x%u instances %u indices %s size %u base %d\n", d->vs->id, d->ps->id, d->prim, d->prims,
        d->instances, d->indices ? "yes" : "no", d->index_size, d->base_vertex);
    for (uint32_t i = 0; i < d->nelems; ++i)
        printf("[drawdump]   elem stream %u type %u usage %u index %u offset %u\n", d->elems[i].stream, d->elems[i].type,
            d->elems[i].usage, d->elems[i].index, d->elems[i].offset);
    for (int r = 0; r < 16; ++r)
        if (d->vs->in_mask & (1u << r))
            printf("[drawdump]   v%d wants usage %u index %u\n", r, d->vs->in_usage[r], d->vs->in_index[r]);
    for (int si = 0; si < 16; ++si)
        if (d->streams[si].data)
        {
            const G9Stream* st = &d->streams[si];
            printf("[drawdump]   stream %d stride %u size %u instanced %u\n", si, st->stride, st->size, st->instanced);
            for (uint32_t v = 0; v < 3 && st->stride && (v + 1) * st->stride <= st->size; ++v)
            {
                printf("[drawdump]     vtx %u:", v);
                const uint8_t* p = st->data + v * st->stride;
                for (uint32_t k = 0; k + 4 <= st->stride && k < 96; k += 4)
                {
                    uint32_t w;
                    float f;
                    memcpy(&w, p + k, 4);
                    memcpy(&f, p + k, 4);
                    printf(" %08x(%g)", w, f);
                }
                printf("\n");
            }
        }
    printf("[drawdump]   b:");
    for (int i = 0; i < 16; ++i)
        printf(" %u", d->vsb[i]);
    printf("\n[drawdump]   i0..3:");
    for (int i = 0; i < 16; ++i)
        printf(" %d", d->vsi[i]);
    printf("\n");
    fflush(stdout);
}

static id<MTLTexture> gray2d(void)
{
    static id<MTLTexture> t;
    if (!t)
    {
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:1 height:1 mipmapped:NO];
        td.storageMode = MTLStorageModeShared;
        t = [g_dev newTextureWithDescriptor:td];
        uint8_t px[4] = { 128, 128, 128, 255 };
        [t replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:px bytesPerRow:4];
    }
    return t;
}

static id<MTLTexture> texture_for(G9Tex* t, int type)
{
    int want = type == SM3_TEX_CUBE ? G9_TEX_CUBE : type == SM3_TEX_VOLUME ? G9_TEX_3D : G9_TEX_2D;
    if (g_debug_mode == 2 && want == G9_TEX_2D && t && t->view && t->type == want && !(t->use == G9_USE_RT || t->use == G9_USE_DEPTH))
        return gray2d();
    if (g_debug_mode == 3 && want == G9_TEX_2D && t && t->view && t->type == want && (t->use == G9_USE_RT || t->use == G9_USE_DEPTH))
        return gray2d();
    if (!t || !t->view || t->type != want)
        return dummy(type);
    return t->view;
}

/* debugging: an input register's value for vertex v, decoded as the Metal vertex descriptor delivers it */
static void decode_elem(const G9Draw* d, const G9Elem* e, uint32_t v, uint32_t inst, float out[4])
{
    out[0] = out[1] = out[2] = 0, out[3] = 1;
    const G9Stream* st = &d->streams[e->stream];
    if (!st->data)
    {
        out[3] = 0;
        return;
    }
    uint32_t at = (st->instanced ? inst / st->instanced : v) * st->stride + e->offset;
    if (at + 4 > st->size)
        return;
    const uint8_t* p = st->data + at;
    float f[4];
    int16_t sh[4];
    uint16_t us[4];
    _Float16 h[4];
    switch (e->type)
    {
    case 0: case 1: case 2: case 3:
        memcpy(f, p, 4u * (e->type + 1));
        for (uint32_t k = 0; k <= e->type; ++k)
            out[k] = f[k];
        break;
    case 4: out[0] = p[2] / 255.0f, out[1] = p[1] / 255.0f, out[2] = p[0] / 255.0f, out[3] = p[3] / 255.0f; break;
    case 5: for (int k = 0; k < 4; ++k) out[k] = p[k]; break;
    case 6: memcpy(sh, p, 4); out[0] = sh[0], out[1] = sh[1]; break;
    case 7: memcpy(sh, p, 8); for (int k = 0; k < 4; ++k) out[k] = sh[k]; break;
    case 8: for (int k = 0; k < 4; ++k) out[k] = p[k] / 255.0f; break;
    case 9: memcpy(sh, p, 4); for (int k = 0; k < 2; ++k) out[k] = fmaxf(sh[k] / 32767.0f, -1); break;
    case 10: memcpy(sh, p, 8); for (int k = 0; k < 4; ++k) out[k] = fmaxf(sh[k] / 32767.0f, -1); break;
    case 11: memcpy(us, p, 4); out[0] = us[0] / 65535.0f, out[1] = us[1] / 65535.0f; break;
    case 12: memcpy(us, p, 8); for (int k = 0; k < 4; ++k) out[k] = us[k] / 65535.0f; break;
    case 15: memcpy(h, p, 4); out[0] = (float)h[0], out[1] = (float)h[1]; break;
    case 16: memcpy(h, p, 8); for (int k = 0; k < 4; ++k) out[k] = (float)h[k]; break;
    default: break;
    }
}

static void cpu_compare(const G9Draw* d, G9Shader* vs, const float* gpu, uint32_t nverts, uint32_t inst)
{
    static Sm3VsConsts k;
    memcpy(k.c, d->vsc, sizeof k.c);
    memcpy(k.i, d->vsi, sizeof k.i);
    memcpy(k.b, d->vsb, sizeof k.b);
    uint32_t checked = 0, bad = 0, unsup = 0;
    float worst = 0;
    for (uint32_t v = 0; v < nverts && checked < 20000; ++v)
    {
        const float* g = gpu + 4 * v;
        if (g[0] == 0 && g[1] == 0 && g[2] == 0 && g[3] == 0)
            continue; /* not drawn (no index reached it) */
        float in[16][4];
        memset(in, 0, sizeof in);
        for (int r = 0; r < 16; ++r)
        {
            if (!(vs->in_mask & (1u << r)))
                continue;
            for (uint32_t i = 0; i < d->nelems; ++i)
                if (d->elems[i].usage == vs->in_usage[r] && d->elems[i].index == vs->in_index[r])
                {
                    decode_elem(d, &d->elems[i], v, inst, in[r]);
                    break;
                }
        }
        Sm3VsResult res;
        sm3_run_vs(&vs->s, (const float(*)[4])in, &k, &res);
        unsup = res.unsupported ? (uint32_t)res.unsupported : unsup;
        checked++;
        float diff = 0, mag = 1;
        for (int c = 0; c < 4; ++c)
        {
            diff = fmaxf(diff, fabsf(res.pos[c] - g[c]));
            mag = fmaxf(mag, fabsf(res.pos[c]));
        }
        float rel = diff / mag;
        if (rel > worst)
            worst = rel;
        if (rel > 1e-3f && bad++ < 6)
        {
            printf("[cpuvs] vtx %u cpu %g %g %g %g gpu %g %g %g %g\n", v, res.pos[0], res.pos[1], res.pos[2], res.pos[3], g[0], g[1], g[2], g[3]);
            for (int r = 0; r < 16; ++r)
                if (vs->in_mask & (1u << r))
                    printf("[cpuvs]     v%d %g %g %g %g\n", r, in[r][0], in[r][1], in[r][2], in[r][3]);
        }
    }
    printf("[cpuvs] vs%u ps%u instance %u/%u: %u vertices checked, %u differ (worst relative %g)%s\n", vs->id, d->ps->id, inst,
        d->instances ? d->instances : 1, checked, bad, worst,
        unsup ? " (the interpreter skipped an instruction)" : "");
    fflush(stdout);
}

static uint64_t g_draws, g_skipped;

void g9_frame_costs(uint32_t* compiles, uint64_t* compile_ns, uint32_t* pipes, uint64_t* pipe_ns, uint32_t* uploads, uint64_t* upload_bytes,
    uint64_t* upload_ns)
{
    *compiles = g_fc_compiles, *compile_ns = g_fc_compile_ns, *pipes = g_fc_pipes, *pipe_ns = g_fc_pipe_ns;
    *uploads = g_fc_uploads, *upload_bytes = g_fc_upload_bytes, *upload_ns = g_fc_upload_ns;
    g_fc_compiles = g_fc_pipes = g_fc_uploads = 0;
    g_fc_compile_ns = g_fc_pipe_ns = g_fc_upload_ns = g_fc_upload_bytes = 0;
}

void g9_draw(const G9Draw* d)
{
    if (!g_dev || !d->vs || !d->ps || !d->prims)
        return;
    @autoreleasepool
    {
        drain_async();
        G9Shader* vs = d->vs;
        G9Shader* ps = d->ps;
        if (g_skip_vs && vs->id == g_skip_vs)
            return;
        if (g_only_vs && vs->id != g_only_vs && g_rt[0].tex && g_rt[0].tex->w >= 640)
            return; /* debugging: only one shader's geometry in the full-size color passes */
        if (g_dump_left && vs->id == g_dump_vs)
        {
            g_dump_left--;
            dump_draw(d);
        }
        /* the vertex inputs from the declaration */
        PipeKey k;
        memset(&k, 0, sizeof k);
        k.vs = vs->id, k.ps = ps->id;
        int uses_zero = 0;
        for (int r = 0; r < 16; ++r)
        {
            if (!(vs->in_mask & (1u << r)))
                continue;
            const G9Elem* e = NULL;
            for (uint32_t i = 0; i < d->nelems; ++i)
                if (d->elems[i].usage == vs->in_usage[r] && d->elems[i].index == vs->in_index[r])
                {
                    e = &d->elems[i];
                    break;
                }
            if (e && g_debug_mode == 5 && e->usage == 10 && e->type == 16)
                e = NULL; /* debugging: half4 vertex colors read as zero */
            MTLVertexFormat f = e ? vertex_format(e->type, &k.in_kind[r]) : MTLVertexFormatInvalid;
            if (!e || f == MTLVertexFormatInvalid || e->stream >= 16 || !d->streams[e->stream].data)
            {
                k.attr_fmt[r] = MTLVertexFormatFloat4;
                k.attr_buf[r] = 30;
                k.in_kind[r] = SM3_IN_FLOAT;
                uses_zero = 1;
                continue;
            }
            k.attr_fmt[r] = (uint8_t)f;
            k.attr_buf[r] = e->stream;
            k.attr_off[r] = e->offset;
            k.stride[e->stream] = (uint16_t)(d->streams[e->stream].stride ? d->streams[e->stream].stride : 4);
            k.instanced[e->stream] = (uint8_t)d->streams[e->stream].instanced;
        }
        if (uses_zero)
            k.stride[16] = 16;
        const uint32_t* rs = d->rs;
        k.blend = rs[27] != 0;
        k.src = (uint8_t)rs[19], k.dst = (uint8_t)rs[20], k.op = (uint8_t)rs[171];
        if (rs[206])
            k.srca = (uint8_t)rs[207], k.dsta = (uint8_t)rs[208], k.opa = (uint8_t)rs[209];
        else
            k.srca = k.src, k.dsta = k.dst, k.opa = k.op;
        k.wmask[0] = (uint8_t)(rs[168] & 15), k.wmask[1] = (uint8_t)(rs[190] & 15), k.wmask[2] = (uint8_t)(rs[191] & 15);
        k.wmask[3] = (uint8_t)(rs[192] & 15);
        current_formats(k.color, &k.depth, &k.stencil);
        if (!k.blend)
            k.src = k.dst = k.op = k.srca = k.dsta = k.opa = 0;

        int vtx_debug = g_cpuvs && vs->id == g_cpuvs && d->indices;
        id<MTLLibrary> lib = nil;
        if (vtx_debug)
        {
            /* a one-off translation that also stores the output positions (not cached) */
            char err[256];
            sm3_msl_vtx_debug = 1;
            char* msl = sm3_msl_pair(&vs->s, &ps->s, k.in_kind, err, sizeof err);
            sm3_msl_vtx_debug = 0;
            if (msl)
            {
                lib = [compile(msl, "a debug shader pair") autorelease];
                free(msl);
            }
            k.pad = 0xDB;
        }
        else
            lib = program(vs, ps, k.in_kind);
        if (!lib)
        {
            g_skipped++;
            return;
        }
        id<MTLRenderPipelineState> pipe = pipeline(&k, lib);
        if (!pipe || !begin_pass())
        {
            g_skipped++;
            return;
        }
        id<MTLRenderCommandEncoder> e = g_enc;
        [e setRenderPipelineState:pipe];
        [e setDepthStencilState:depth_state(rs, k.depth != MTLPixelFormatInvalid, k.stencil != MTLPixelFormatInvalid)];
        [e setStencilReferenceValue:rs[57] & 0xFF];
        [e setCullMode:rs[22] == 2 ? MTLCullModeFront : rs[22] == 3 ? MTLCullModeBack : MTLCullModeNone];
        [e setTriangleFillMode:rs[8] == 2 || g_debug_mode == 1 ? MTLTriangleFillModeLines : MTLTriangleFillModeFill];
        float bias, slope;
        memcpy(&bias, &rs[195], 4);
        memcpy(&slope, &rs[175], 4);
        [e setDepthBias:bias * 16777216.0f slopeScale:slope clamp:0];
        uint32_t bc = rs[193];
        [e setBlendColorRed:(bc >> 16 & 255) / 255.0f green:(bc >> 8 & 255) / 255.0f blue:(bc & 255) / 255.0f alpha:(bc >> 24) / 255.0f];
        set_viewport(d->viewport);
        set_scissor(rs[174] ? d->scissor : NULL);

        /* streams */
        id<MTLBuffer> buf;
        NSUInteger off;
        for (int s = 0; s < 16; ++s)
        {
            if (!k.stride[s] || !d->streams[s].data || !d->streams[s].size)
                continue;
            if (d->streams[s].key)
            {
                id<MTLBuffer> pb = pbuf_get(d->streams[s].key, d->streams[s].ver, d->streams[s].whole, d->streams[s].whole_size);
                if (pb)
                {
                    [e setVertexBuffer:pb offset:d->streams[s].offset atIndex:s];
                    continue;
                }
            }
            void* p = ring(d->streams[s].size, &buf, &off);
            memcpy(p, d->streams[s].data, d->streams[s].size);
            [e setVertexBuffer:buf offset:off atIndex:s];
        }
        if (uses_zero)
            [e setVertexBuffer:g_zero offset:0 atIndex:30];

        /* constants and the fixups */
        Sm3Fix fx;
        memset(&fx, 0, sizeof fx);
        float vw = (float)d->viewport[2], vh = (float)d->viewport[3];
        /* D3D9's pixel centers sit on integer coordinates, Metal's on half: geometry moves half a pixel
         * right and down */
        fx.posfix[0] = vw > 0 ? 1.0f / vw : 0, fx.posfix[1] = vh > 0 ? -1.0f / vh : 0;
        fx.alpha_func = rs[15] ? rs[25] : 8;
        fx.alpha_ref = (rs[24] & 255) / 255.0f;
        fx.srgb_write = rs[194] != 0;
        for (int n = 0; n < 16; ++n)
            if (d->ss[n][11])
                fx.srgb_mask |= 1u << n;
        uint32_t vsrgb = 0;
        for (int n = 0; n < 4; ++n)
            if (d->ss[16 + n][11])
                vsrgb |= 1u << n;
        /* the float constants as far as the shaders read them */
        size_t vsn = 16 * (vs->nconst ? vs->nconst : 1), psn = 16 * (ps->nconst ? ps->nconst : 1);
        void* p = ring(vsn, &buf, &off);
        memcpy(p, d->vsc, vsn);
        [e setVertexBuffer:buf offset:off atIndex:16];
        [e setVertexBytes:d->vsi length:256 atIndex:17];
        [e setVertexBytes:d->vsb length:64 atIndex:18];
        Sm3Fix vfx = fx;
        vfx.srgb_mask = vsrgb;
        [e setVertexBytes:&vfx length:sizeof vfx atIndex:19];
        [e setFragmentBytes:d->psc length:psn atIndex:0];
        [e setFragmentBytes:d->psi length:256 atIndex:1];
        [e setFragmentBytes:d->psb length:64 atIndex:2];
        [e setFragmentBytes:&fx length:sizeof fx atIndex:3];

        /* textures and samplers the shaders declare */
        for (int n = 0; n < 16; ++n)
            if (ps->tex_type[n])
            {
                [e setFragmentTexture:texture_for(d->tex[n], ps->tex_type[n]) atIndex:n];
                [e setFragmentSamplerState:sampler(d->ss[n]) atIndex:n];
            }
        for (int n = 0; n < 4; ++n)
            if (vs->tex_type[n])
            {
                [e setVertexTexture:texture_for(d->tex[16 + n], vs->tex_type[n]) atIndex:n];
                [e setVertexSamplerState:sampler(d->ss[16 + n]) atIndex:n];
            }

        if (g_trace == 1)
        {
            char line[640];
            float mnz, mxz;
            memcpy(&mnz, &d->viewport[4], 4);
            memcpy(&mxz, &d->viewport[5], 4);
            size_t o = (size_t)snprintf(line, sizeof line, "[trace] draw vs%u ps%u prim %u x%u blend %u(%u,%u,%u) zw %u vp %u,%u %ux%u z %g..%g sc %d cull %u",
                vs->id, ps->id, d->prim, d->prims, k.blend, k.src, k.dst, k.op, rs[14], d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3],
                mnz, mxz, rs[174] ? 1 : 0, rs[22]);
            o += (size_t)snprintf(line + o, sizeof line - o, " at %u/%u/%u ps154 %08x at181 %08x", rs[15], rs[25], rs[24] & 255, rs[154], rs[181]);
            for (int n = 0; n < 16 && o < sizeof line - 80; ++n)
                if (ps->tex_type[n])
                {
                    char b[64];
                    o += (size_t)snprintf(line + o, sizeof line - o, " s%d=%s", n, tname(d->tex[n], b, sizeof b));
                }
            fprintf(stdout, "%s\n", line);
        }
        id<MTLBuffer> dbg = nil;
        uint32_t dbg_n = 0;
        if (vtx_debug)
        {
            uint32_t cnt = vertex_count(d->prim, d->prims), hi = 0;
            for (uint32_t i = 0; i < cnt; ++i)
            {
                int32_t vi = (int32_t)(d->index_size == 4 ? ((const uint32_t*)d->indices)[i] : ((const uint16_t*)d->indices)[i]) + d->base_vertex;
                if (vi > (int32_t)hi)
                    hi = (uint32_t)vi;
            }
            dbg_n = hi + 1;
            uint32_t ni = d->instances ? d->instances : 1;
            dbg = [[g_dev newBufferWithLength:(NSUInteger)dbg_n * ni * 16 options:MTLResourceStorageModeShared] autorelease];
            memset([dbg contents], 0, (size_t)dbg_n * ni * 16);
            [e setVertexBuffer:dbg offset:0 atIndex:29];
            [e setVertexBytes:&dbg_n length:4 atIndex:28];
        }
        /* the draw */
        uint32_t count = vertex_count(d->prim, d->prims);
        if (!count)
            return;
        NSUInteger inst = d->instances ? d->instances : 1;
        id<MTLBuffer> pib = nil;

        if (d->prim == 6)
        {
            /* a fan: as a list */
            uint32_t tris = d->prims, n = tris * 3;
            uint32_t* idx = (uint32_t*)ring(4 * n, &buf, &off);
            for (uint32_t t = 0; t < tris; ++t)
            {
                uint32_t a = 0, b = t + 1, c = t + 2;
                if (d->indices)
                {
                    if (d->index_size == 2)
                        a = ((const uint16_t*)d->indices)[a], b = ((const uint16_t*)d->indices)[b], c = ((const uint16_t*)d->indices)[c];
                    else
                        a = ((const uint32_t*)d->indices)[a], b = ((const uint32_t*)d->indices)[b], c = ((const uint32_t*)d->indices)[c];
                }
                if (d->indices)
                {
                    int32_t bv = d->base_vertex;
                    a = (uint32_t)((int32_t)a + bv), b = (uint32_t)((int32_t)b + bv), c = (uint32_t)((int32_t)c + bv);
                }
                idx[3 * t] = a, idx[3 * t + 1] = b, idx[3 * t + 2] = c;
            }
            [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:n indexType:MTLIndexTypeUInt32 indexBuffer:buf
                   indexBufferOffset:off instanceCount:inst baseVertex:0 baseInstance:0];
        }
        else if (d->indices && d->ikey && d->base_vertex >= 0 &&
                 (pib = pbuf_get(d->ikey, d->iver, d->iwhole, d->iwhole_size)) != nil)
        {
            /* the game's index buffer as it is, its base vertex as Metal's */
            [e drawIndexedPrimitives:prim_type(d->prim) indexCount:count
                           indexType:d->index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:pib
                   indexBufferOffset:d->ioffset instanceCount:inst baseVertex:d->base_vertex baseInstance:0];
        }
        else if (d->indices && d->base_vertex == 0)
        {
            size_t bytes = (size_t)count * d->index_size;
            void* ip = ring(bytes, &buf, &off);
            memcpy(ip, d->indices, bytes);
            [e drawIndexedPrimitives:prim_type(d->prim) indexCount:count
                           indexType:d->index_size == 4 ? MTLIndexTypeUInt32 : MTLIndexTypeUInt16 indexBuffer:buf
                   indexBufferOffset:off instanceCount:inst baseVertex:0 baseInstance:0];
        }
        else if (d->indices)
        {
            /* the base vertex applied to the indices as they are copied (it is negative here: the
             * streams start at the lowest vertex used), as 32-bit indices */
            uint32_t* ip = (uint32_t*)ring((size_t)count * 4, &buf, &off);
            for (uint32_t i = 0; i < count; ++i)
            {
                int32_t v = (d->index_size == 4 ? (int32_t)((const uint32_t*)d->indices)[i] : (int32_t)((const uint16_t*)d->indices)[i]) +
                            d->base_vertex;
                ip[i] = v < 0 ? 0u : (uint32_t)v;
            }
            [e drawIndexedPrimitives:prim_type(d->prim) indexCount:count indexType:MTLIndexTypeUInt32 indexBuffer:buf
                   indexBufferOffset:off instanceCount:inst baseVertex:0 baseInstance:0];
        }
        else
            [e drawPrimitives:prim_type(d->prim) vertexStart:0 vertexCount:count instanceCount:inst];
        g_draws++;
        if (dbg)
        {
            g_cpuvs = 0;
            commit(1);
            for (uint32_t ii = 0; ii < (d->instances ? d->instances : 1) && ii < 8; ++ii)
                cpu_compare(d, vs, (const float*)[dbg contents] + (size_t)ii * dbg_n * 4, dbg_n, ii);
            /* the triangles the indices make, from the GPU's own positions: how many span most of the
             * screen (wrong indices join far-apart vertices) */
            {
                const float* P = (const float*)[dbg contents];
                uint32_t cnt = vertex_count(d->prim, d->prims), tris = 0, huge = 0, behind = 0;
                for (uint32_t t = 0; t + 2 < cnt; t += d->prim == 5 ? 1 : 3)
                {
                    uint32_t ix[3];
                    for (int q = 0; q < 3; ++q)
                    {
                        int32_t vi = (int32_t)(d->index_size == 4 ? ((const uint32_t*)d->indices)[t + q] : ((const uint16_t*)d->indices)[t + q]) +
                                     d->base_vertex;
                        ix[q] = vi < 0 ? 0 : (uint32_t)vi;
                    }
                    float sx[3], sy[3];
                    int ok = 1;
                    for (int q = 0; q < 3; ++q)
                    {
                        const float* p = P + 4 * ix[q];
                        if (p[3] <= 1e-4f)
                            ok = 0;
                        else
                            sx[q] = p[0] / p[3], sy[q] = p[1] / p[3];
                    }
                    tris++;
                    if (!ok)
                    {
                        behind++;
                        continue;
                    }
                    float e = 0;
                    for (int q = 0; q < 3; ++q)
                    {
                        float dx = sx[q] - sx[(q + 1) % 3], dy = sy[q] - sy[(q + 1) % 3];
                        e = fmaxf(e, sqrtf(dx * dx + dy * dy));
                    }
                    if (e > 1.0f)
                        huge++;
                }
                printf("[cpuvs] triangles %u: %u span over half the screen, %u touch the camera plane (start-relative indices %s)\n", tris, huge,
                    behind, d->index_size == 4 ? "32-bit" : "16-bit");
            }
        }
    }
}

/* --- copies ---------------------------------------------------------------------------------------- */

void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t* srect, G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t* drect, int linear)
{
    {
        char a[64], b[64];
        TRACE("[trace] stretch %s L%u -> %s L%u %s%s\n", tname(src, a, sizeof a), slevel, tname(dst, b, sizeof b), dlevel, srect ? "srect " : "",
            drect ? "drect" : "");
    }
    if (!g_dev || !src || !dst || !src->tex || !dst->tex)
        return;
    @autoreleasepool
    {
        uint32_t sw, sh, sd, dw, dh, dd;
        level_size(src, slevel, &sw, &sh, &sd);
        level_size(dst, dlevel, &dw, &dh, &dd);
        int32_t s[4] = { 0, 0, (int32_t)sw, (int32_t)sh }, t[4] = { 0, 0, (int32_t)dw, (int32_t)dh };
        if (srect)
            memcpy(s, srect, sizeof s);
        if (drect)
            memcpy(t, drect, sizeof t);
        if (s[2] <= s[0] || s[3] <= s[1] || t[2] <= t[0] || t[3] <= t[1])
            return;
        frame_begin();
        flush_clear();
        end_encoder();
        g_targets_dirty = 1;
        int same = s[2] - s[0] == t[2] - t[0] && s[3] - s[1] == t[3] - t[1] && src->pf == dst->pf;
        if (same || is_depth(src->pf) || is_depth(dst->pf))
        {
            if (!same)
                return; /* a depth stretch: not done */
            id<MTLBlitCommandEncoder> b = [g_cmd blitCommandEncoder];
            [b copyFromTexture:src->tex sourceSlice:src->type == G9_TEX_CUBE ? sface : 0 sourceLevel:slevel
                  sourceOrigin:MTLOriginMake((NSUInteger)s[0], (NSUInteger)s[1], 0)
                    sourceSize:MTLSizeMake((NSUInteger)(s[2] - s[0]), (NSUInteger)(s[3] - s[1]), 1)
                     toTexture:dst->tex destinationSlice:dst->type == G9_TEX_CUBE ? dface : 0 destinationLevel:dlevel
             destinationOrigin:MTLOriginMake((NSUInteger)t[0], (NSUInteger)t[1], 0)];
            [b endEncoding];
            return;
        }
        MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = dst->tex;
        p.colorAttachments[0].level = dlevel;
        p.colorAttachments[0].slice = dst->type == G9_TEX_CUBE ? dface : 0;
        p.colorAttachments[0].loadAction = MTLLoadActionLoad;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> e = [g_cmd renderCommandEncoderWithDescriptor:p];
        UtilKey k;
        memset(&k, 0, sizeof k);
        k.kind = 2;
        k.color[0] = (uint32_t)dst->pf;
        k.wmask[0] = 15;
        id<MTLRenderPipelineState> pipe = util_pipeline(&k);
        if (pipe)
        {
            [e setRenderPipelineState:pipe];
            MTLViewport v = { (double)t[0], (double)t[1], (double)(t[2] - t[0]), (double)(t[3] - t[1]), 0, 1 };
            [e setViewport:v];
            /* uv across the viewport: the source rectangle (the triangle spans twice the viewport) */
            float uvx[4] = { (float)s[0] / sw, (float)s[1] / sh, (float)(s[2] - s[0]) / sw, (float)(s[3] - s[1]) / sh };
            [e setVertexBytes:uvx length:sizeof uvx atIndex:0];
            id<MTLTexture> view = src->view;
            if (src->type == G9_TEX_CUBE)
                view = [[src->tex newTextureViewWithPixelFormat:src->pf textureType:MTLTextureType2D levels:NSMakeRange(slevel, 1)
                                                         slices:NSMakeRange(sface, 1)] autorelease];
            [e setFragmentTexture:view atIndex:0];
            [e setFragmentSamplerState:linear ? g_linear : g_point atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [e endEncoding];
    }
}

/* --- present --------------------------------------------------------------------------------------- */

/* a one-off frame capture asked for by path (g9_snap_next): taken at the next present */
static char g_snap_path[1024];
static volatile int g_snap_pending;

void g9_snap_next(const char* path)
{
    snprintf(g_snap_path, sizeof g_snap_path, "%s", path);
    g_snap_pending = 1;
}

/* debugging: any 2D texture level 0 as a PNG, float formats scaled per channel from their range
 * (logged) to 0..255 */
static uint32_t g_snaptex_id;
static char g_snaptex_path[1024];
void g9_snap_tex(uint32_t id, const char* path)
{
    snprintf(g_snaptex_path, sizeof g_snaptex_path, "%s", path);
    g_snaptex_id = id;
}

static void save_png_any(G9Tex* t, const char* path)
{
    uint32_t w = t->w, h = t->h, bpp = t->bpp ? t->bpp : 4;
    int ch = 0, f32 = 0, f16 = 0;
    switch (t->pf)
    {
    case MTLPixelFormatR32Float: ch = 1, f32 = 1; break;
    case MTLPixelFormatRG32Float: ch = 2, f32 = 1; break;
    case MTLPixelFormatRGBA32Float: ch = 4, f32 = 1; break;
    case MTLPixelFormatR16Float: ch = 1, f16 = 1; break;
    case MTLPixelFormatRG16Float: ch = 2, f16 = 1; break;
    case MTLPixelFormatRGBA16Float: ch = 4, f16 = 1; break;
    default: break;
    }
    uint8_t* raw = (uint8_t*)malloc((size_t)w * h * bpp);
    g9_tex_read(t, 0, 0, raw, w * bpp);
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    if (!ch)
        memcpy(px, raw, (size_t)w * h * 4);
    else
    {
        float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
        size_t n = (size_t)w * h;
        for (int pass = 0; pass < 2; ++pass)
            for (size_t i = 0; i < n; ++i)
                for (int c = 0; c < 4; ++c)
                {
                    float v = 0;
                    if (c < ch)
                        v = f32 ? ((float*)raw)[i * ch + c] : (float)((_Float16*)raw)[i * ch + c];
                    if (pass == 0 && c < ch && v == v)
                        lo[c] = v < lo[c] ? v : lo[c], hi[c] = v > hi[c] ? v : hi[c];
                    if (pass == 1)
                    {
                        float k = c < ch && hi[c] > lo[c] ? (v - lo[c]) / (hi[c] - lo[c]) : 0;
                        /* BGRA order: R->2, G->1, B->0; one channel shows as gray */
                        uint8_t b = (uint8_t)(k * 255.0f);
                        if (ch == 1)
                        {
                            if (c == 0)
                                px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = b;
                        }
                        else if (c < 3)
                            px[i * 4 + (2 - c)] = b;
                    }
                }
        rt_log("[gfx9] snaptex ranges r %g..%g g %g..%g b %g..%g a %g..%g\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
    }
    free(raw);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (dst)
    {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
        rt_log("[gfx9] texture %s saved to %s\n", "", path);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    free(px);
}

static void save_png(G9Tex* bb, const char* path);

static void save_png_any(G9Tex* t, const char* path);
static void snap(G9Tex* bb)
{
    if (g_snaptex_id)
    {
        for (int i = 0; i < MAX_LIVE; ++i)
            if (g_live[i] && (((uintptr_t)g_live[i] >> 4) & 0xFFFF) == g_snaptex_id && g_live[i]->type == G9_TEX_2D)
            {
                save_png_any(g_live[i], g_snaptex_path);
                break;
            }
        g_snaptex_id = 0;
    }
    if (g_snap_pending && bb->pf == MTLPixelFormatBGRA8Unorm)
    {
        g_snap_pending = 0;
        save_png(bb, g_snap_path);
    }
    static const char* dir;
    static int checked;
    static uint64_t every = 120;
    if (!checked)
    {
        checked = 1, dir = getenv("XIVLEGACY_SNAP");
        if (getenv("XIVLEGACY_SNAP_EVERY") && atoi(getenv("XIVLEGACY_SNAP_EVERY")) > 0)
            every = (uint64_t)atoi(getenv("XIVLEGACY_SNAP_EVERY"));
    }
    if (!dir || g_frame % every || bb->pf != MTLPixelFormatBGRA8Unorm)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.png", dir, (unsigned long long)g_frame);
    save_png(bb, path);
}

static void save_png(G9Tex* bb, const char* path)
{
    uint32_t w = bb->w, h = bb->h;
    uint8_t* px = (uint8_t*)malloc((size_t)w * h * 4);
    g9_tex_read(bb, 0, 0, px, w * 4);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    /* BGRA bytes, alpha ignored (X8R8G8B8 back buffers carry garbage there) */
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, (size_t)w * 4, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
    if (dst)
    {
        CGImageDestinationAddImage(dst, img, NULL);
        CGImageDestinationFinalize(dst);
        CFRelease(dst);
        rt_log("[gfx9] frame %llu saved to %s\n", (unsigned long long)g_frame, path);
    }
    CFRelease(url);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    free(px);
}

/* XIVLEGACY_VISLOG: each presented frame copied to the CPU without a stall, shrunk to 64x36, and how
 * much it changed from the frame before logged with the time (when the screen answers input) */
#define VIS_W 64
#define VIS_H 36
static void vislog(G9Tex* bb)
{
    static int on = -1;
    static id<MTLBuffer> bufs[4];
    static uint8_t prev[VIS_W * VIS_H * 3];
    static int have_prev;
    if (on < 0)
        on = getenv("XIVLEGACY_VISLOG") != NULL;
    if (!on || !bb || !bb->tex || bb->pf != MTLPixelFormatBGRA8Unorm || !g_cmd)
        return;
    uint32_t w = bb->w, h = bb->h;
    int slot = (int)(g_frame & 3);
    if (!bufs[slot] || [bufs[slot] length] < (NSUInteger)w * h * 4)
    {
        [bufs[slot] release];
        bufs[slot] = [g_dev newBufferWithLength:(NSUInteger)w * h * 4 options:MTLResourceStorageModeShared];
    }
    end_encoder();
    id<MTLBlitCommandEncoder> b = [g_cmd blitCommandEncoder];
    [b copyFromTexture:bb->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:bufs[slot] destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:(NSUInteger)w * h * 4];
    [b endEncoding];
    id<MTLBuffer> buf = bufs[slot];
    unsigned long long frame = g_frame;
    double t = (double)rt_monotonic_ns() * 1e-9;
    [g_cmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        (void)cb;
        const uint8_t* px = (const uint8_t*)[buf contents];
        uint8_t th[VIS_W * VIS_H * 3];
        double diff = 0;
        for (int y = 0; y < VIS_H; ++y)
            for (int x = 0; x < VIS_W; ++x)
            {
                unsigned r = 0, g2 = 0, bl = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                    {
                        const uint8_t* q = px + ((size_t)(y * h / VIS_H + sy * h / VIS_H / 4) * w + (size_t)(x * w / VIS_W + sx * w / VIS_W / 4)) * 4;
                        bl += q[0], g2 += q[1], r += q[2];
                    }
                uint8_t* o = th + (y * VIS_W + x) * 3;
                o[0] = (uint8_t)(r / 16), o[1] = (uint8_t)(g2 / 16), o[2] = (uint8_t)(bl / 16);
            }
        if (have_prev)
            for (int i = 0; i < VIS_W * VIS_H * 3; ++i)
                diff += abs((int)th[i] - (int)prev[i]);
        memcpy(prev, th, sizeof th);
        have_prev = 1;
        rt_log("[vis] frame %llu t %.3f diff %.2f\n", frame, t, diff / (VIS_W * VIS_H * 3));
    }];
}

void g9_present(G9Tex* bb)
{
    if (g_trace)
    {
        char a[64];
        TRACE("[trace] present %s ring %u bytes, %u spills\n", tname(bb, a, sizeof a), g_ring_off, g_spills);
        g_trace--;
    }
    if (!g_dev)
        return;
    @autoreleasepool
    {
        g_frame++;
        flush_clear();
        if (bb && bb->tex)
            snap(bb);
        frame_begin();
        end_encoder();
        if (g_layer && bb && bb->tex)
        {
            int pw = 0, ph = 0;
            if (g_window)
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
            if (pw > 0 && ph > 0 && (g_layer.drawableSize.width != pw || g_layer.drawableSize.height != ph))
                g_layer.drawableSize = CGSizeMake(pw, ph);
            id<CAMetalDrawable> drawable = [g_layer nextDrawable];
            if (drawable)
            {
                MTLRenderPassDescriptor* p = [MTLRenderPassDescriptor renderPassDescriptor];
                p.colorAttachments[0].texture = drawable.texture;
                p.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                p.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> e = [g_cmd renderCommandEncoderWithDescriptor:p];
                [e setRenderPipelineState:g_present_pipe];
                float uvx[4] = { 0, 0, 1, 1 };
                [e setVertexBytes:uvx length:sizeof uvx atIndex:0];
                [e setFragmentTexture:bb->view atIndex:0];
                [e setFragmentSamplerState:g_linear atIndex:0];
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [e endEncoding];
                [g_cmd presentDrawable:drawable];
            }
        }
        vislog(bb);
        commit(0);
        if (g_frame == 1 || g_frame % 1024 == 0)
            rt_log("[gfx9] frame %llu: %llu draws drawn, %llu skipped so far\n", (unsigned long long)g_frame,
                (unsigned long long)g_draws, (unsigned long long)g_skipped);
    }
}

/* --- init ------------------------------------------------------------------------------------------ */

static void make_view(void* arg)
{
    (void)arg;
    g_view = SDL_Metal_CreateView(g_window);
    g_layer = g_view ? (CAMetalLayer*)SDL_Metal_GetLayer(g_view) : nil;
}

int g9_init(void* window, int vsync)
{
    if (g_dev)
        return 1;
    if (getenv("XIVLEGACY_NOGFX"))
        return 0;
    @autoreleasepool
    {
        g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev)
        {
            rt_log("[gfx9] no Metal device: drawing nothing\n");
            return 0;
        }
        g_vsync = vsync;
        g_queue = [g_dev newCommandQueue];
        g_frames_sem = dispatch_semaphore_create(FRAMES);
        for (int i = 0; i < FRAMES; ++i)
            g_ring[i] = [g_dev newBufferWithLength:RING_SIZE options:MTLResourceStorageModeShared];
        g_spill = [[NSMutableArray alloc] init];
        g_pending = [[NSObject alloc] init];
        g_done = [[NSMutableArray alloc] init];
        g_zero = [g_dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        memset([g_zero contents], 0, 256);
        g_util = compile(UTIL_MSL, "the utility shaders");
        MTLRenderPipelineDescriptor* pd = [[MTLRenderPipelineDescriptor alloc] init];
        id<MTLFunction> vf = [g_util newFunctionWithName:@"quad_vs"], ff = [g_util newFunctionWithName:@"copy_fs"];
        pd.vertexFunction = vf;
        pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_present_pipe = [g_dev newRenderPipelineStateWithDescriptor:pd error:NULL];
        [vf release];
        [ff release];
        [pd release];
        MTLSamplerDescriptor* sd = [[MTLSamplerDescriptor alloc] init];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        g_linear = [g_dev newSamplerStateWithDescriptor:sd];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        g_point = [g_dev newSamplerStateWithDescriptor:sd];
        [sd release];
        if (window)
        {
            g_window = (SDL_Window*)window;
            if (SDL_IsMainThread())
                make_view(NULL);
            else
            {
                /* the view is the main thread's to make: let it run (and pump messages) meanwhile */
                int held = gt_holds();
                if (held)
                    gt_unlock();
                SDL_RunOnMainThread(make_view, NULL, true);
                if (held)
                    gt_lock();
            }
            if (g_layer)
            {
                g_layer.device = g_dev;
                g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                g_layer.framebufferOnly = YES;
                g_layer.displaySyncEnabled = vsync ? YES : NO;
                int pw = 0, ph = 0;
                SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
                if (pw > 0 && ph > 0)
                    g_layer.drawableSize = CGSizeMake(pw, ph);
            }
            else
                rt_log("[gfx9] no Metal layer for the window (%s): drawing offscreen\n", SDL_GetError());
        }
        rt_log("[gfx9] Metal on %s%s\n", [[g_dev name] UTF8String], g_layer ? "" : ", offscreen");
        return 1;
    }
}
