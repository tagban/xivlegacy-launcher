/* The Direct3D 9 back end on wgpu (gfx9.h; webgpu.h from wgpu-native): one code path for Direct3D 12 on
 * Windows, Vulkan on Linux and Metal on macOS (where gfx9_metal.m stays the default).
 *
 * Shaders: each vertex/pixel shader pair becomes WGSL (sm3_wgsl.c) the first time a draw uses it, per
 * shape of the vertex inputs (integer formats reach only integer inputs; inputs nothing feeds read zero)
 * and per sampler bound to a depth texture. Pipelines - everything WebGPU bakes into one: the pair, the
 * vertex layout, target formats, blending, write masks, depth-stencil state, culling, fill, depth bias,
 * topology and the texture binding layout - are built by worker threads; the draws that need one are
 * skipped until it is ready, as the Metal back end skips draws while Metal compiles.
 *
 * Data: the vertex, index and constant bytes a draw needs are copied into a ring in memory, and the ring
 * goes to the GPU with one queue write per submit, ordered before that submit's commands; it starts again
 * empty after every submit, so guest memory can change the moment a draw returns, as D3D allows.
 * Textures are written through the queue too; a write to a texture that draws recorded but not yet
 * submitted have read submits those first.
 *
 * Passes: a render pass opens at the first draw or clear after the targets change. A clear of whole
 * targets before anything is drawn becomes the pass's load operation; any other clear draws a quad.
 * WebGPU forbids sampling a texture the pass draws to (D3D allows it): such a draw samples a copy, made
 * at most once a frame. Present draws the back buffer to the window's surface.
 *
 * Formats WebGPU lacks (16-bit color, 24-bit RGB) are widened to BGRA8 on upload; luminance, alpha-only
 * and X8 formats read through swizzled views where the adapter has swizzles, else widened too.
 *
 * XIVLEGACY_SNAP=<folder>: every 120th frame's back buffer as a PNG (XIVLEGACY_SNAP_EVERY=<n>);
 * XIVLEGACY_WGSLDUMP=<folder>: each pair's WGSL. */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <SDL3/SDL_metal.h>
#endif
#include <webgpu.h>
#include <wgpu.h>

#include "gfx9.h"
#include "gthread.h"
#include "plat.h"
#include "sm3.h"
#include "sm3_wgsl.h"

#define RING_CHUNK (16u << 20)
#define MAX_LIVE 4096

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

/* CPU conversions on upload, to BGRA8 (or noted) */
enum { CONV_NONE, CONV_565, CONV_1555, CONV_X1555, CONV_4444, CONV_RGB8, CONV_A8, CONV_L8, CONV_A8L8, CONV_L16, CONV_BGR10, CONV_DXT1, CONV_DXT3, CONV_DXT5 };

/* texture binding kinds (3 bits per sampler slot in a layout mask) */
enum { K_NONE, K_2D, K_CUBE, K_3D, K_DEPTH, K_U2D, K_UCUBE, K_U3D };

static const WGPUStringView NOLABEL = { NULL, WGPU_STRLEN };
static WGPUStringView sv(const char* s) { return (WGPUStringView){ s, WGPU_STRLEN }; }

struct G9Tex
{
    WGPUTexture tex;
    WGPUTextureView view;     /* what shaders sample (NULL: not sampleable) */
    WGPUTextureView* targets; /* face * levels + level, made when first drawn to */
    WGPUTextureFormat format;
    int type, use, conv, depth, stencil, unfilterable;
    uint32_t fmt, w, h, d, levels;
    uint32_t block, bpp;      /* bytes per 4x4 block (compressed) or per texel, in WebGPU's layout */
    uint64_t used;            /* the serial of the last encoder that referenced it */
    uint64_t uid;             /* never reused (bind group keys) */
    uint64_t writes;          /* passes drawn into it */
    /* the copy sampled while it is also a target */
    WGPUTexture shadow;
    WGPUTextureView shadow_view;
    uint64_t shadow_uid, shadow_at, shadow_frame;
};

struct G9Shader
{
    Sm3Shader s;
    uint32_t* tokens;
    uint32_t id;
    uint8_t in_usage[16], in_index[16];
    uint16_t in_mask;
    uint8_t tex_type[20];
    uint32_t nconst;
    uint8_t colors; /* pixel shader: color outputs written */
};

static WGPUInstance g_inst;
static WGPUAdapter g_adapter;
static WGPUDevice g_dev;
static WGPUQueue g_queue;
static WGPUSurface g_surface;
static WGPUTextureFormat g_surface_fmt = WGPUTextureFormat_BGRA8Unorm;
static WGPUPresentMode g_present_mode = WGPUPresentMode_Fifo;
static SDL_Window* g_window;
static uint32_t g_surface_w, g_surface_h;
static int g_vsync = 1, g_has_bc, g_has_swizzle, g_has_line, g_has_d32s8, g_has_unorm16, g_has_f32filter, g_has_border;
static _Atomic uint32_t g_errors;
static _Thread_local uint32_t t_errors;

static WGPUCommandEncoder g_enc;
static WGPURenderPassEncoder g_pass;
static uint64_t g_serial = 1;
static uint64_t g_uids;
static uint64_t g_frame;
static uint64_t g_draws, g_skipped;

static uint32_t g_fc_compiles, g_fc_pipes, g_fc_uploads;
static uint64_t g_fc_compile_ns, g_fc_pipe_ns, g_fc_upload_ns, g_fc_upload_bytes;

static G9Tex* g_live[MAX_LIVE];
static uint32_t g_next_shader_id = 1;

int g9_active(void) { return g_dev != NULL; }

static void on_error(WGPUDevice const* device, WGPUErrorType type, WGPUStringView message, void* u1, void* u2)
{
    (void)device, (void)u1, (void)u2;
    uint32_t n = atomic_fetch_add(&g_errors, 1);
    t_errors++;
    if (n < 50 || n % 1000 == 0)
        rt_log("[gfx9] wgpu error %d (%u so far): %.*s\n", (int)type, n + 1,
            message.data ? (int)(message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0, message.data ? message.data : "");
}

/* --- debugging: trace --------------------------------------------------------------------------------- */

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

/* --- small caches: key bytes -> object ---------------------------------------------------------------- */

typedef struct Ent
{
    uint64_t h;
    void* key;
    size_t klen;
    void* obj;
} Ent;

typedef struct Map
{
    Ent* e;
    size_t cap, n;
} Map;

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

static void* map_get(Map* m, const void* key, size_t klen)
{
    if (!m->cap)
        return NULL;
    Ent* e = map_slot(m, key, klen, fnv(key, klen));
    return e->h ? e->obj : NULL;
}

static void map_put(Map* m, const void* key, size_t klen, void* obj)
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
        e->obj = obj;
        return;
    }
    e->h = h;
    e->key = malloc(klen);
    memcpy(e->key, key, klen);
    e->klen = klen;
    e->obj = obj;
    m->n++;
}

static void map_clear(Map* m, void (*release)(void*))
{
    for (size_t i = 0; i < m->cap; ++i)
        if (m->e[i].h)
        {
            if (release && m->e[i].obj)
                release(m->e[i].obj);
            free(m->e[i].key);
        }
    free(m->e);
    m->e = NULL, m->cap = m->n = 0;
}

/* --- the upload ring: memory here, written to the GPU once per submit ------------------------------------ */

typedef struct Chunk
{
    WGPUBuffer gpu;
    uint8_t* cpu;
    uint32_t used, size;
    WGPUBindGroup group0; /* the draws' uniforms, at dynamic offsets into this chunk */
    WGPUBindGroup util;   /* the clears' */
} Chunk;
static Chunk* g_chunks;
static uint32_t g_nchunks, g_cur_chunk;

static Chunk* ring(size_t n, size_t align, uint64_t* off, void** cpu)
{
    for (;; g_cur_chunk++)
    {
        if (g_cur_chunk == g_nchunks)
        {
            uint32_t size = n + align > RING_CHUNK ? (uint32_t)((n + align + 255) & ~(size_t)255) : RING_CHUNK;
            g_chunks = (Chunk*)realloc(g_chunks, (g_nchunks + 1) * sizeof(Chunk));
            Chunk* c = &g_chunks[g_nchunks++];
            memset(c, 0, sizeof *c);
            WGPUBufferDescriptor d = { 0 };
            d.label = NOLABEL;
            d.size = size;
            d.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_Vertex | WGPUBufferUsage_Index | WGPUBufferUsage_CopyDst;
            c->gpu = wgpuDeviceCreateBuffer(g_dev, &d);
            c->cpu = (uint8_t*)malloc(size);
            c->size = size;
        }
        Chunk* c = &g_chunks[g_cur_chunk];
        uint32_t at = (uint32_t)((c->used + align - 1) & ~(align - 1));
        if (at + n <= c->size)
        {
            c->used = at + (uint32_t)n;
            *off = at;
            *cpu = c->cpu + at;
            return c;
        }
    }
}

static int begin_pass(void);
static void end_pass(void);
static void flush_clear(void);

static WGPUCommandEncoder enc(void)
{
    if (!g_enc)
    {
        WGPUCommandEncoderDescriptor d = { 0 };
        d.label = NOLABEL;
        g_enc = wgpuDeviceCreateCommandEncoder(g_dev, &d);
    }
    return g_enc;
}

static int g_targets_dirty = 1;
static uint32_t g_submits;
/* XIVLEGACY_WGPU_STATS: per-frame averages every 120 frames */
static uint64_t g_st_submits, g_st_passes, g_st_shadows, g_st_ring, g_st_uploads_submit;

/* the ring's bytes and the recorded commands to the GPU; wait: until it has run them */
static void submit(int wait)
{
    flush_clear();
    end_pass();
    for (uint32_t i = 0; i < g_nchunks; ++i)
        if (g_chunks[i].used)
        {
            g_st_ring += g_chunks[i].used;
            wgpuQueueWriteBuffer(g_queue, g_chunks[i].gpu, 0, g_chunks[i].cpu, (g_chunks[i].used + 3u) & ~3u);
            g_chunks[i].used = 0;
        }
    g_cur_chunk = 0;
    if (g_enc)
    {
        WGPUCommandBufferDescriptor d = { 0 };
        d.label = NOLABEL;
        uint32_t before = t_errors;
        WGPUCommandBuffer cb = wgpuCommandEncoderFinish(g_enc, &d);
        wgpuCommandEncoderRelease(g_enc);
        g_enc = NULL;
        if (t_errors != before)
        {
            /* an invalid command buffer: wgpu-native aborts the process if it is submitted */
            rt_log("[gfx9] commands dropped (invalid; see the error above)\n");
            wgpuCommandBufferRelease(cb);
        }
        else
        {
            WGPUSubmissionIndex idx = wgpuQueueSubmitForIndex(g_queue, 1, &cb);
            wgpuCommandBufferRelease(cb);
            g_submits++, g_st_submits++;
            if (wait)
                wgpuDevicePoll(g_dev, 1, &idx);
        }
    }
    g_serial++;
    g_targets_dirty = 1;
}

/* --- formats -------------------------------------------------------------------------------------------- */

typedef struct Swz
{
    WGPUComponentSwizzle r, g, b, a;
    int on;
} Swz;

static WGPUTextureFormat texture_format(uint32_t fmt, int use, int* conv, uint32_t* block, uint32_t* bpp, Swz* sw)
{
    *conv = CONV_NONE, *block = 0, *bpp = 4;
    memset(sw, 0, sizeof *sw);
    int sampled = use == G9_USE_SAMPLE;
    switch (fmt)
    {
    case D_A8R8G8B8: return WGPUTextureFormat_BGRA8Unorm;
    case D_X8R8G8B8: case D_X8B8G8R8:
        if (g_has_swizzle)
            *sw = (Swz){ WGPUComponentSwizzle_R, WGPUComponentSwizzle_G, WGPUComponentSwizzle_B, WGPUComponentSwizzle_One, 1 };
        /* else the shaders read its alpha as 1 (Sm3wFix.one_mask) */
        return fmt == D_X8R8G8B8 ? WGPUTextureFormat_BGRA8Unorm : WGPUTextureFormat_RGBA8Unorm;
    case D_A8B8G8R8: return WGPUTextureFormat_RGBA8Unorm;
    case D_R8G8B8: *conv = CONV_RGB8; return WGPUTextureFormat_BGRA8Unorm;
    case D_R5G6B5: *conv = sampled ? CONV_565 : CONV_NONE; return WGPUTextureFormat_BGRA8Unorm;
    case D_X1R5G5B5: *conv = sampled ? CONV_X1555 : CONV_NONE; return WGPUTextureFormat_BGRA8Unorm;
    case D_A1R5G5B5: *conv = sampled ? CONV_1555 : CONV_NONE; return WGPUTextureFormat_BGRA8Unorm;
    case D_A4R4G4B4: *conv = sampled ? CONV_4444 : CONV_NONE; return WGPUTextureFormat_BGRA8Unorm;
    case D_A2R10G10B10: *conv = sampled ? CONV_BGR10 : CONV_NONE; return WGPUTextureFormat_RGB10A2Unorm;
    case D_A2B10G10R10: return WGPUTextureFormat_RGB10A2Unorm;
    case D_A8:
        if (g_has_swizzle)
        {
            *sw = (Swz){ WGPUComponentSwizzle_Zero, WGPUComponentSwizzle_Zero, WGPUComponentSwizzle_Zero, WGPUComponentSwizzle_R, 1 };
            *bpp = 1;
            return WGPUTextureFormat_R8Unorm;
        }
        *conv = CONV_A8;
        return WGPUTextureFormat_BGRA8Unorm;
    case D_L8:
        if (g_has_swizzle)
        {
            *sw = (Swz){ WGPUComponentSwizzle_R, WGPUComponentSwizzle_R, WGPUComponentSwizzle_R, WGPUComponentSwizzle_One, 1 };
            *bpp = 1;
            return WGPUTextureFormat_R8Unorm;
        }
        *conv = CONV_L8;
        return WGPUTextureFormat_BGRA8Unorm;
    case D_A8L8:
        if (g_has_swizzle)
        {
            *sw = (Swz){ WGPUComponentSwizzle_R, WGPUComponentSwizzle_R, WGPUComponentSwizzle_R, WGPUComponentSwizzle_G, 1 };
            *bpp = 2;
            return WGPUTextureFormat_RG8Unorm;
        }
        *conv = CONV_A8L8;
        return WGPUTextureFormat_BGRA8Unorm;
    case D_L16:
        if (g_has_swizzle && g_has_unorm16)
        {
            *sw = (Swz){ WGPUComponentSwizzle_R, WGPUComponentSwizzle_R, WGPUComponentSwizzle_R, WGPUComponentSwizzle_One, 1 };
            *bpp = 2;
            return WGPUTextureFormat_R16Unorm;
        }
        *conv = CONV_L16;
        return WGPUTextureFormat_BGRA8Unorm;
    case D_V8U8: *bpp = 2; return WGPUTextureFormat_RG8Snorm;
    case D_Q8W8V8U8: return WGPUTextureFormat_RGBA8Snorm;
    case D_G16R16: return g_has_unorm16 ? WGPUTextureFormat_RG16Unorm : WGPUTextureFormat_Undefined;
    case D_A16B16G16R16: *bpp = 8; return g_has_unorm16 ? WGPUTextureFormat_RGBA16Unorm : WGPUTextureFormat_Undefined;
    case D_R16F: *bpp = 2; return WGPUTextureFormat_R16Float;
    case D_G16R16F: return WGPUTextureFormat_RG16Float;
    case D_A16B16G16R16F: *bpp = 8; return WGPUTextureFormat_RGBA16Float;
    case D_R32F: return WGPUTextureFormat_R32Float;
    case D_G32R32F: *bpp = 8; return WGPUTextureFormat_RG32Float;
    case D_A32B32G32R32F: *bpp = 16; return WGPUTextureFormat_RGBA32Float;
    case D_DXT1:
        if (!g_has_bc)
            return *conv = CONV_DXT1, WGPUTextureFormat_BGRA8Unorm;
        *block = 8;
        return WGPUTextureFormat_BC1RGBAUnorm;
    case D_DXT2: case D_DXT3:
        if (!g_has_bc)
            return *conv = CONV_DXT3, WGPUTextureFormat_BGRA8Unorm;
        *block = 16;
        return WGPUTextureFormat_BC2RGBAUnorm;
    case D_DXT4: case D_DXT5:
        if (!g_has_bc)
            return *conv = CONV_DXT5, WGPUTextureFormat_BGRA8Unorm;
        *block = 16;
        return WGPUTextureFormat_BC3RGBAUnorm;
    case D_D16: case D_D16_LOCKABLE: *bpp = 2; return WGPUTextureFormat_Depth16Unorm;
    case D_D32: return WGPUTextureFormat_Depth32Float;
    case D_D24S8: case D_D24X8: case D_D24FS8: case D_INTZ:
        *bpp = 8;
        return g_has_d32s8 ? WGPUTextureFormat_Depth32FloatStencil8 : WGPUTextureFormat_Depth24PlusStencil8;
    default: return WGPUTextureFormat_Undefined;
    }
}

static int fmt_depth(WGPUTextureFormat f)
{
    return f == WGPUTextureFormat_Depth16Unorm || f == WGPUTextureFormat_Depth32Float || f == WGPUTextureFormat_Depth32FloatStencil8 ||
           f == WGPUTextureFormat_Depth24PlusStencil8 || f == WGPUTextureFormat_Depth24Plus;
}
static int fmt_stencil(WGPUTextureFormat f) { return f == WGPUTextureFormat_Depth32FloatStencil8 || f == WGPUTextureFormat_Depth24PlusStencil8; }
static int fmt_float32(WGPUTextureFormat f)
{
    return f == WGPUTextureFormat_R32Float || f == WGPUTextureFormat_RG32Float || f == WGPUTextureFormat_RGBA32Float;
}

static uint32_t expand(uint32_t v, int bits) { return (v << (8 - bits)) | (v >> (2 * bits - 8 > 0 ? 2 * bits - 8 : 0)); }

/* one row of a D3D format into BGRA8 (or swapped RGB10A2) */
static void convert_row(int conv, const uint8_t* s, uint8_t* d, uint32_t w)
{
    for (uint32_t x = 0; x < w; ++x, d += 4)
    {
        uint32_t v = 0;
        switch (conv)
        {
        case CONV_RGB8: d[0] = s[3 * x], d[1] = s[3 * x + 1], d[2] = s[3 * x + 2], d[3] = 255; break;
        case CONV_A8: d[0] = d[1] = d[2] = 0, d[3] = s[x]; break;
        case CONV_L8: d[0] = d[1] = d[2] = s[x], d[3] = 255; break;
        case CONV_A8L8: d[0] = d[1] = d[2] = s[2 * x], d[3] = s[2 * x + 1]; break;
        case CONV_L16: d[0] = d[1] = d[2] = s[2 * x + 1], d[3] = 255; break;
        case CONV_BGR10:
            memcpy(&v, s + 4 * x, 4);
            v = (v & 0xC00FFC00u) | ((v & 0x3FFu) << 20) | ((v >> 20) & 0x3FFu);
            memcpy(d, &v, 4);
            break;
        default:
            v = (uint32_t)(s[2 * x] | s[2 * x + 1] << 8);
            switch (conv)
            {
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
}

/* a DXT1/3/5 4x4 block to BGRA8 texels at d (row pitch dp), for adapters without BC formats */
static void color565(uint16_t c, uint8_t* o) { o[0] = (uint8_t)expand(c & 31, 5), o[1] = (uint8_t)expand((c >> 5) & 63, 6), o[2] = (uint8_t)expand(c >> 11, 5); }
static void dxt_block(int conv, const uint8_t* s, uint8_t* d, uint32_t dp, uint32_t bw, uint32_t bh)
{
    const uint8_t* cb = conv == CONV_DXT1 ? s : s + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint8_t pal[4][4];
    color565(c0, pal[0]), color565(c1, pal[1]);
    pal[0][3] = pal[1][3] = 255;
    if (conv != CONV_DXT1 || c0 > c1)
        for (int k = 0; k < 3; ++k)
            pal[2][k] = (uint8_t)((2 * pal[0][k] + pal[1][k]) / 3), pal[3][k] = (uint8_t)((pal[0][k] + 2 * pal[1][k]) / 3);
    else
        for (int k = 0; k < 3; ++k)
            pal[2][k] = (uint8_t)((pal[0][k] + pal[1][k]) / 2), pal[3][k] = 0;
    pal[2][3] = 255, pal[3][3] = conv == CONV_DXT1 && c0 <= c1 ? 0 : 255;
    uint8_t alpha[8];
    if (conv == CONV_DXT5)
    {
        alpha[0] = s[0], alpha[1] = s[1];
        for (int k = 2; k < 8; ++k)
            alpha[k] = s[0] > s[1] ? (uint8_t)(((8 - k) * s[0] + (k - 1) * s[1]) / 7)
                     : k < 6       ? (uint8_t)(((6 - k) * s[0] + (k - 1) * s[1]) / 5)
                     : k == 6      ? 0
                                   : 255;
    }
    uint64_t abits = 0;
    if (conv == CONV_DXT5)
        for (int k = 0; k < 6; ++k)
            abits |= (uint64_t)s[2 + k] << (8 * k);
    uint32_t idx = (uint32_t)(cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24);
    for (uint32_t y = 0; y < bh; ++y)
        for (uint32_t x = 0; x < bw; ++x)
        {
            uint32_t i = y * 4 + x;
            uint8_t* o = d + y * dp + x * 4;
            memcpy(o, pal[(idx >> (2 * i)) & 3], 4);
            if (conv == CONV_DXT3)
                o[3] = (uint8_t)(((s[i / 2] >> (4 * (i & 1))) & 15) * 17);
            else if (conv == CONV_DXT5)
                o[3] = alpha[(abits >> (3 * i)) & 7];
        }
}

/* --- textures ------------------------------------------------------------------------------------------- */

static void level_size(const G9Tex* t, uint32_t level, uint32_t* w, uint32_t* h, uint32_t* d)
{
    *w = t->w >> level ? t->w >> level : 1;
    *h = t->h >> level ? t->h >> level : 1;
    *d = t->d >> level ? t->d >> level : 1;
}

static WGPUTextureView make_view(G9Tex* t, WGPUTexture tex, const Swz* sw)
{
    WGPUTextureViewDescriptor vd = { 0 };
    vd.label = NOLABEL;
    vd.format = t->depth ? WGPUTextureFormat_Undefined : t->format; /* a depth aspect's own format */
    vd.dimension = t->type == G9_TEX_CUBE ? WGPUTextureViewDimension_Cube : t->type == G9_TEX_3D ? WGPUTextureViewDimension_3D : WGPUTextureViewDimension_2D;
    vd.mipLevelCount = t->levels;
    vd.arrayLayerCount = t->type == G9_TEX_CUBE ? 6 : 1;
    vd.aspect = t->depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
    vd.usage = WGPUTextureUsage_TextureBinding;
    WGPUTextureComponentSwizzleDescriptor sd = { 0 };
    if (sw && sw->on)
    {
        sd.chain.sType = WGPUSType_TextureComponentSwizzleDescriptor;
        sd.swizzle.r = sw->r, sd.swizzle.g = sw->g, sd.swizzle.b = sw->b, sd.swizzle.a = sw->a;
        vd.nextInChain = &sd.chain;
    }
    return wgpuTextureCreateView(tex, &vd);
}

static WGPUTexture make_texture(const G9Tex* t, WGPUTextureUsage usage)
{
    WGPUTextureDescriptor d = { 0 };
    d.label = NOLABEL;
    d.dimension = t->type == G9_TEX_3D ? WGPUTextureDimension_3D : WGPUTextureDimension_2D;
    d.size.width = t->w;
    d.size.height = t->h;
    d.size.depthOrArrayLayers = t->type == G9_TEX_CUBE ? 6 : t->type == G9_TEX_3D ? t->d : 1;
    d.format = t->format;
    d.mipLevelCount = t->levels;
    d.sampleCount = 1;
    d.usage = usage;
    return wgpuDeviceCreateTexture(g_dev, &d);
}

G9Tex* g9_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int use)
{
    if (!g_dev)
        return NULL;
    G9Tex* t = (G9Tex*)calloc(1, sizeof *t);
    t->type = type, t->use = use, t->fmt = fmt, t->w = w ? w : 1, t->h = h ? h : 1, t->d = d ? d : 1;
    t->levels = levels ? levels : 1;
    Swz sw;
    t->format = texture_format(fmt, use, &t->conv, &t->block, &t->bpp, &sw);
    t->uid = ++g_uids, t->shadow_uid = ++g_uids;
    if (t->format == WGPUTextureFormat_Undefined)
    {
        if (fmt != D_NULL)
        {
            char fc[5] = { 0 };
            if (fmt > 0xFF)
                memcpy(fc, &fmt, 4);
            rt_log("[gfx9] texture format %s%u has no wgpu match\n", fc, fc[0] ? 0 : fmt);
        }
        return t;
    }
    /* WebGPU wants a compressed texture's top level in whole blocks: others are decoded on upload */
    if (t->block && (t->w % 4 || t->h % 4))
    {
        t->conv = fmt == D_DXT1 ? CONV_DXT1 : fmt == D_DXT2 || fmt == D_DXT3 ? CONV_DXT3 : CONV_DXT5;
        t->format = WGPUTextureFormat_BGRA8Unorm, t->block = 0, t->bpp = 4;
    }
    t->depth = fmt_depth(t->format);
    t->stencil = fmt_stencil(t->format);
    t->unfilterable = fmt_float32(t->format) && !g_has_f32filter;
    WGPUTextureUsage usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    if (use != G9_USE_SAMPLE)
        usage |= WGPUTextureUsage_RenderAttachment;
    if (t->depth && t->format == WGPUTextureFormat_Depth24PlusStencil8)
        usage &= ~(WGPUTextureUsage_CopySrc | WGPUTextureUsage_CopyDst);
    uint32_t before = t_errors;
    t->tex = make_texture(t, usage);
    if (!t->tex || t_errors != before)
    {
        rt_log("[gfx9] cannot create a %ux%u texture of format %u\n", t->w, t->h, fmt);
        if (t->tex)
            wgpuTextureRelease(t->tex);
        t->tex = NULL;
        return t;
    }
    t->view = make_view(t, t->tex, &sw);
    t->targets = (WGPUTextureView*)calloc((type == G9_TEX_CUBE ? 6 : 1) * t->levels, sizeof(WGPUTextureView));
    for (int i = 0; i < MAX_LIVE; ++i)
        if (!g_live[i])
        {
            g_live[i] = t;
            break;
        }
    return t;
}

static G9Target g_rt[4];
static G9Tex* g_ds;

void g9_tex_destroy(G9Tex* t)
{
    if (!t)
        return;
    for (int i = 0; i < MAX_LIVE; ++i)
        if (g_live[i] == t)
            g_live[i] = NULL;
    for (int i = 0; i < 4; ++i)
        if (g_rt[i].tex == t)
        {
            submit(0);
            g_rt[i].tex = NULL;
        }
    if (g_ds == t)
    {
        submit(0);
        g_ds = NULL;
    }
    if (t->targets)
    {
        uint32_t n = (t->type == G9_TEX_CUBE ? 6 : 1) * t->levels;
        for (uint32_t i = 0; i < n; ++i)
            if (t->targets[i])
                wgpuTextureViewRelease(t->targets[i]);
        free(t->targets);
    }
    if (t->view)
        wgpuTextureViewRelease(t->view);
    if (t->shadow_view)
        wgpuTextureViewRelease(t->shadow_view);
    if (t->shadow)
        wgpuTextureRelease(t->shadow);
    if (t->tex)
        wgpuTextureRelease(t->tex);
    free(t);
}

static WGPUTextureView target_view(G9Tex* t, uint32_t face, uint32_t level)
{
    if (!t->targets || level >= t->levels)
        return NULL;
    WGPUTextureView* v = &t->targets[(t->type == G9_TEX_CUBE ? face : 0) * t->levels + level];
    if (!*v)
    {
        WGPUTextureViewDescriptor vd = { 0 };
        vd.label = NOLABEL;
        vd.format = t->format;
        vd.dimension = WGPUTextureViewDimension_2D;
        vd.baseMipLevel = level, vd.mipLevelCount = 1;
        vd.baseArrayLayer = t->type == G9_TEX_CUBE ? face : 0, vd.arrayLayerCount = 1;
        vd.aspect = WGPUTextureAspect_All;
        vd.usage = WGPUTextureUsage_RenderAttachment;
        *v = wgpuTextureCreateView(t->tex, &vd);
    }
    return *v;
}

static void tex_upload(G9Tex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch, uint32_t slice)
{
    if (!t || !t->tex || level >= t->levels || t->use != G9_USE_SAMPLE)
        return;
    uint32_t w, h, d;
    level_size(t, level, &w, &h, &d);
    uint32_t depth = t->type == G9_TEX_3D ? d : 1;
    uint32_t pw = w, ph = h, rows = h, row_bytes = w * t->bpp, dpitch = pitch;
    const uint8_t* data = (const uint8_t*)src;
    uint8_t* tmp = NULL;
    if (t->block)
    {
        pw = (w + 3) & ~3u, ph = (h + 3) & ~3u; /* whole blocks (a small level's physical size) */
        rows = ph / 4, row_bytes = pw / 4 * t->block;
    }
    if (t->conv >= CONV_DXT1)
    {
        /* decoded: a compressed level of D3D's to BGRA8 */
        uint32_t bsz = t->conv == CONV_DXT1 ? 8 : 16, bw = (w + 3) / 4, bh = (h + 3) / 4;
        row_bytes = w * 4, dpitch = row_bytes;
        tmp = (uint8_t*)malloc((size_t)row_bytes * h * depth);
        for (uint32_t z = 0; z < depth; ++z)
            for (uint32_t by = 0; by < bh; ++by)
                for (uint32_t bx = 0; bx < bw; ++bx)
                    dxt_block(t->conv, data + (size_t)z * slice + (size_t)by * pitch + (size_t)bx * bsz,
                        tmp + ((size_t)z * h + by * 4) * row_bytes + bx * 16, row_bytes, w - bx * 4 < 4 ? w - bx * 4 : 4, h - by * 4 < 4 ? h - by * 4 : 4);
        data = tmp, slice = row_bytes * h;
    }
    else if (t->conv != CONV_NONE)
    {
        row_bytes = w * 4, dpitch = row_bytes;
        tmp = (uint8_t*)malloc((size_t)row_bytes * h * depth);
        for (uint32_t z = 0; z < depth; ++z)
            for (uint32_t y = 0; y < h; ++y)
                convert_row(t->conv, data + (size_t)z * slice + (size_t)y * pitch, tmp + ((size_t)z * h + y) * row_bytes, w);
        data = tmp, slice = row_bytes * h;
    }
    else if (depth > 1 && (slice % pitch || slice / pitch < rows))
    {
        /* slices not a whole number of rows apart: packed */
        tmp = (uint8_t*)malloc((size_t)pitch * rows * depth);
        for (uint32_t z = 0; z < depth; ++z)
            memcpy(tmp + (size_t)z * pitch * rows, data + (size_t)z * slice, (size_t)pitch * rows);
        data = tmp, slice = pitch * rows;
    }
    if (t->used == g_serial)
        submit(0), g_st_uploads_submit++; /* draws recorded with the old contents go first */
    WGPUTexelCopyTextureInfo dst = { 0 };
    dst.texture = t->tex;
    dst.mipLevel = level;
    dst.origin = (WGPUOrigin3D){ 0, 0, t->type == G9_TEX_CUBE ? face : 0 };
    dst.aspect = WGPUTextureAspect_All;
    uint32_t rpi = depth > 1 ? slice / dpitch : rows;
    WGPUTexelCopyBufferLayout layout = { 0, dpitch, rpi };
    WGPUExtent3D size = { pw, ph, depth };
    size_t bytes = (size_t)dpitch * rpi * (depth - 1) + (size_t)dpitch * (rows - 1) + row_bytes;
    wgpuQueueWriteTexture(g_queue, &dst, data, bytes, &layout, &size);
    free(tmp);
}

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

static void on_mapped(WGPUMapAsyncStatus status, WGPUStringView message, void* u1, void* u2)
{
    (void)message, (void)u2;
    *(int*)u1 = status == WGPUMapAsyncStatus_Success ? 1 : -1;
}

void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    if (!g_dev || !t || !t->tex || t->block || t->depth || level >= t->levels)
        return;
    uint32_t w, h, d;
    level_size(t, level, &w, &h, &d);
    uint32_t row = w * t->bpp, prow = (row + 255u) & ~255u;
    WGPUBufferDescriptor bd = { 0 };
    bd.label = NOLABEL;
    bd.size = (uint64_t)prow * h;
    bd.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    WGPUBuffer buf = wgpuDeviceCreateBuffer(g_dev, &bd);
    flush_clear();
    end_pass();
    WGPUTexelCopyTextureInfo src = { t->tex, level, { 0, 0, t->type == G9_TEX_CUBE ? face : 0 }, WGPUTextureAspect_All };
    WGPUTexelCopyBufferInfo out = { { 0, prow, h }, buf };
    WGPUExtent3D size = { w, h, 1 };
    wgpuCommandEncoderCopyTextureToBuffer(enc(), &src, &out, &size);
    submit(1);
    int done = 0;
    WGPUBufferMapCallbackInfo cb = { 0 };
    cb.mode = WGPUCallbackMode_AllowProcessEvents;
    cb.callback = on_mapped;
    cb.userdata1 = &done;
    wgpuBufferMapAsync(buf, WGPUMapMode_Read, 0, (size_t)prow * h, cb);
    while (!done)
    {
        wgpuDevicePoll(g_dev, 1, NULL);
        wgpuInstanceProcessEvents(g_inst);
    }
    if (done > 0)
    {
        const uint8_t* p = (const uint8_t*)wgpuBufferGetConstMappedRange(buf, 0, (size_t)prow * h);
        for (uint32_t y = 0; p && y < h; ++y)
            memcpy((uint8_t*)dst + (size_t)y * pitch, p + (size_t)y * prow, row < pitch ? row : pitch);
        wgpuBufferUnmap(buf);
    }
    wgpuBufferRelease(buf);
}

/* --- shaders --------------------------------------------------------------------------------------------- */

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
    s->colors = 1;
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
            if (r->type == SM3_CONST && k == 1 && in->op >= SM3_M4x4 && in->op <= SM3_M3x2 && r->num + 4u > s->nconst)
                s->nconst = r->num + 4u < all ? r->num + 4u : all; /* the matrix's rows */
        }
        if (in->has_dst && in->dst.type == SM3_COLOROUT && in->dst.num + 1u > s->colors)
            s->colors = (uint8_t)(in->dst.num + 1u > 4 ? 4 : in->dst.num + 1u);
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
    /* samplers used without a dcl (ps 2.0): 2D, as the translator takes them */
    for (size_t i = 0; i < s->s.nins; ++i)
    {
        const Sm3Ins* in = &s->s.ins[i];
        if ((in->op == SM3_TEX || in->op == SM3_TEXLDL || in->op == SM3_TEXLDD) && in->nsrc >= 2 && in->src[1].type == SM3_SAMPLER &&
            in->src[1].num < 20 && !s->tex_type[in->src[1].num])
            s->tex_type[in->src[1].num] = SM3_TEX_2D;
    }
    return s;
}

void g9_shader_destroy(G9Shader* s)
{
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
    uint32_t depth_mask;
} ProgKey;

#define FAILED ((void*)1)
static Map g_programs; /* ProgKey -> WGSL source (malloc'd) or FAILED */

static const char* program(G9Shader* vs, G9Shader* ps, const uint8_t in_kind[16], uint32_t depth_mask)
{
    ProgKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id, k.ps = ps->id, k.depth_mask = depth_mask;
    memcpy(k.in_kind, in_kind, 16);
    char* src = (char*)map_get(&g_programs, &k, sizeof k);
    if (src)
        return src == FAILED ? NULL : src;
    uint64_t t0 = rt_monotonic_ns();
    char err[256];
    src = sm3_wgsl_pair(&vs->s, &ps->s, in_kind, depth_mask, err, sizeof err);
    g_fc_compiles++, g_fc_compile_ns += rt_monotonic_ns() - t0;
    if (!src)
    {
        rt_log("[gfx9] shader pair %u/%u not translated: %s\n", vs->id, ps->id, err);
        map_put(&g_programs, &k, sizeof k, FAILED);
        return NULL;
    }
    if (getenv("XIVLEGACY_WGSLDUMP"))
    {
        char path[1024];
        snprintf(path, sizeof path, "%s/vs%u_ps%u.wgsl", getenv("XIVLEGACY_WGSLDUMP"), vs->id, ps->id);
        FILE* f = fopen(path, "w");
        if (f)
        {
            fputs(src, f);
            fclose(f);
        }
    }
    map_put(&g_programs, &k, sizeof k, src);
    return src;
}

/* --- state conversions ----------------------------------------------------------------------------------- */

static WGPUCompareFunction compare(uint32_t f)
{
    switch (f)
    {
    case 1: return WGPUCompareFunction_Never;
    case 2: return WGPUCompareFunction_Less;
    case 3: return WGPUCompareFunction_Equal;
    case 4: return WGPUCompareFunction_LessEqual;
    case 5: return WGPUCompareFunction_Greater;
    case 6: return WGPUCompareFunction_NotEqual;
    case 7: return WGPUCompareFunction_GreaterEqual;
    default: return WGPUCompareFunction_Always;
    }
}

static WGPUStencilOperation stencil_op(uint32_t op)
{
    switch (op)
    {
    case 2: return WGPUStencilOperation_Zero;
    case 3: return WGPUStencilOperation_Replace;
    case 4: return WGPUStencilOperation_IncrementClamp;
    case 5: return WGPUStencilOperation_DecrementClamp;
    case 6: return WGPUStencilOperation_Invert;
    case 7: return WGPUStencilOperation_IncrementWrap;
    case 8: return WGPUStencilOperation_DecrementWrap;
    default: return WGPUStencilOperation_Keep;
    }
}

static WGPUBlendFactor blend_factor(uint32_t f)
{
    switch (f)
    {
    case 1: return WGPUBlendFactor_Zero;
    case 2: return WGPUBlendFactor_One;
    case 3: return WGPUBlendFactor_Src;
    case 4: return WGPUBlendFactor_OneMinusSrc;
    case 5: case 12: return WGPUBlendFactor_SrcAlpha;
    case 6: case 13: return WGPUBlendFactor_OneMinusSrcAlpha;
    case 7: return WGPUBlendFactor_DstAlpha;
    case 8: return WGPUBlendFactor_OneMinusDstAlpha;
    case 9: return WGPUBlendFactor_Dst;
    case 10: return WGPUBlendFactor_OneMinusDst;
    case 11: return WGPUBlendFactor_SrcAlphaSaturated;
    case 14: return WGPUBlendFactor_Constant;
    case 15: return WGPUBlendFactor_OneMinusConstant;
    default: return WGPUBlendFactor_One;
    }
}

static WGPUBlendOperation blend_op(uint32_t op)
{
    switch (op)
    {
    case 2: return WGPUBlendOperation_Subtract;
    case 3: return WGPUBlendOperation_ReverseSubtract;
    case 4: return WGPUBlendOperation_Min;
    case 5: return WGPUBlendOperation_Max;
    default: return WGPUBlendOperation_Add;
    }
}

static WGPUVertexFormat vertex_format(uint32_t type, uint8_t* kind, uint32_t* size)
{
    static const uint8_t sizes[17] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
    *kind = SM3W_IN_FLOAT;
    *size = type < 17 ? sizes[type] : 0;
    switch (type)
    {
    case 0: return WGPUVertexFormat_Float32;
    case 1: return WGPUVertexFormat_Float32x2;
    case 2: return WGPUVertexFormat_Float32x3;
    case 3: return WGPUVertexFormat_Float32x4;
    case 4: return WGPUVertexFormat_Unorm8x4BGRA;
    case 5: *kind = SM3W_IN_UINT; return WGPUVertexFormat_Uint8x4;
    case 6: *kind = SM3W_IN_INT; return WGPUVertexFormat_Sint16x2;
    case 7: *kind = SM3W_IN_INT; return WGPUVertexFormat_Sint16x4;
    case 8: return WGPUVertexFormat_Unorm8x4;
    case 9: return WGPUVertexFormat_Snorm16x2;
    case 10: return WGPUVertexFormat_Snorm16x4;
    case 11: return WGPUVertexFormat_Unorm16x2;
    case 12: return WGPUVertexFormat_Unorm16x4;
    case 13: return WGPUVertexFormat_Unorm10_10_10_2; /* UDEC3: unnormalized in D3D; rare */
    case 15: return WGPUVertexFormat_Float16x2;
    case 16: return WGPUVertexFormat_Float16x4;
    default: return (WGPUVertexFormat)0; /* DEC3N: none in WebGPU */
    }
}

static WGPUPrimitiveTopology topology(uint32_t prim)
{
    switch (prim)
    {
    case 1: return WGPUPrimitiveTopology_PointList;
    case 2: return WGPUPrimitiveTopology_LineList;
    case 3: return WGPUPrimitiveTopology_LineStrip;
    case 5: return WGPUPrimitiveTopology_TriangleStrip;
    default: return WGPUPrimitiveTopology_TriangleList; /* lists, and fans made lists */
    }
}

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

/* --- binding layouts -------------------------------------------------------------------------------------- */

static WGPUBindGroupLayout g_bgl0, g_bgl_util;
static Map g_layouts1, g_pipe_layouts, g_groups1, g_samplers;

static void make_bgl0(void)
{
    WGPUBindGroupLayoutEntry e[2];
    memset(e, 0, sizeof e);
    e[0].binding = 0;
    e[0].visibility = WGPUShaderStage_Vertex;
    e[0].buffer.type = WGPUBufferBindingType_Uniform;
    e[0].buffer.hasDynamicOffset = 1;
    e[1].binding = 1;
    e[1].visibility = WGPUShaderStage_Fragment;
    e[1].buffer.type = WGPUBufferBindingType_Uniform;
    e[1].buffer.hasDynamicOffset = 1;
    WGPUBindGroupLayoutDescriptor d = { 0 };
    d.label = NOLABEL;
    d.entryCount = 2;
    d.entries = e;
    g_bgl0 = wgpuDeviceCreateBindGroupLayout(g_dev, &d);
    memset(e, 0, sizeof e);
    e[0].binding = 0;
    e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    e[0].buffer.type = WGPUBufferBindingType_Uniform;
    e[0].buffer.hasDynamicOffset = 1;
    e[0].buffer.minBindingSize = 32;
    d.entryCount = 1;
    g_bgl_util = wgpuDeviceCreateBindGroupLayout(g_dev, &d);
}

static unsigned slot_kind(uint64_t mask, int slot) { return (unsigned)(mask >> (3 * slot)) & 7u; }

/* group 1 for a texture mask: 3 bits per sampler slot (0..15 pixel, 16..19 vertex) */
static WGPUBindGroupLayout layout1(uint64_t mask)
{
    WGPUBindGroupLayout l = (WGPUBindGroupLayout)map_get(&g_layouts1, &mask, sizeof mask);
    if (l)
        return l;
    WGPUBindGroupLayoutEntry e[40];
    memset(e, 0, sizeof e);
    int n = 0;
    for (int s = 0; s < 20; ++s)
    {
        unsigned k = slot_kind(mask, s);
        if (!k)
            continue;
        uint32_t b = s < 16 ? 2u * (uint32_t)s : 32u + 2u * (uint32_t)(s - 16);
        WGPUShaderStage vis = s < 16 ? WGPUShaderStage_Fragment : WGPUShaderStage_Vertex;
        e[n].binding = b;
        e[n].visibility = vis;
        e[n].texture.sampleType = k == K_DEPTH ? WGPUTextureSampleType_Depth : k >= K_U2D ? WGPUTextureSampleType_UnfilterableFloat : WGPUTextureSampleType_Float;
        e[n].texture.viewDimension = k == K_CUBE || k == K_UCUBE ? WGPUTextureViewDimension_Cube
            : k == K_3D || k == K_U3D                       ? WGPUTextureViewDimension_3D
                                                            : WGPUTextureViewDimension_2D;
        n++;
        e[n].binding = b + 1;
        e[n].visibility = vis;
        e[n].sampler.type = k == K_DEPTH || k >= K_U2D ? WGPUSamplerBindingType_NonFiltering : WGPUSamplerBindingType_Filtering;
        n++;
    }
    WGPUBindGroupLayoutDescriptor d = { 0 };
    d.label = NOLABEL;
    d.entryCount = (size_t)n;
    d.entries = e;
    l = wgpuDeviceCreateBindGroupLayout(g_dev, &d);
    map_put(&g_layouts1, &mask, sizeof mask, l);
    return l;
}

static WGPUPipelineLayout pipeline_layout(uint64_t mask)
{
    WGPUPipelineLayout l = (WGPUPipelineLayout)map_get(&g_pipe_layouts, &mask, sizeof mask);
    if (l)
        return l;
    WGPUBindGroupLayout g[2] = { g_bgl0, mask ? layout1(mask) : NULL };
    WGPUPipelineLayoutDescriptor d = { 0 };
    d.label = NOLABEL;
    d.bindGroupLayoutCount = mask ? 2 : 1;
    d.bindGroupLayouts = g;
    l = wgpuDeviceCreatePipelineLayout(g_dev, &d);
    map_put(&g_pipe_layouts, &mask, sizeof mask, l);
    return l;
}

static void release_group(void* g) { wgpuBindGroupRelease((WGPUBindGroup)g); }

/* --- samplers ---------------------------------------------------------------------------------------------- */

static WGPUAddressMode address(uint32_t a, int* border)
{
    switch (a)
    {
    case 2: return WGPUAddressMode_MirrorRepeat;
    case 3: case 5: return WGPUAddressMode_ClampToEdge; /* MIRRORONCE: none in WebGPU */
    case 4:
        if (g_has_border)
        {
            *border = 1;
            return (WGPUAddressMode)WGPUNativeAddressMode_ClampToBorder;
        }
        return WGPUAddressMode_ClampToEdge;
    default: return WGPUAddressMode_Repeat;
    }
}

typedef struct SampKey
{
    uint8_t u, v, w, mag, min, mip, aniso, maxlevel;
    uint32_t border;
    uint32_t nonfiltering;
} SampKey;

static WGPUSampler sampler(const uint32_t* ss, int nonfiltering)
{
    SampKey k = { (uint8_t)ss[1], (uint8_t)ss[2], (uint8_t)ss[3], (uint8_t)ss[5], (uint8_t)ss[6], (uint8_t)ss[7],
                  (uint8_t)(ss[10] > 16 ? 16 : ss[10]), (uint8_t)(ss[9] > 15 ? 15 : ss[9]), ss[4], (uint32_t)nonfiltering };
    if (k.min != 3 && k.mag != 3)
        k.aniso = 1;
    if (nonfiltering)
        k.mag = k.min = 1, k.mip = k.mip ? 1 : 0, k.aniso = 1;
    int b = 0;
    address(k.u, &b), address(k.v, &b), address(k.w, &b);
    if (!b)
        k.border = 0;
    WGPUSampler s = (WGPUSampler)map_get(&g_samplers, &k, sizeof k);
    if (s)
        return s;
    WGPUSamplerDescriptor d = { 0 };
    d.label = NOLABEL;
    int border = 0;
    d.addressModeU = address(k.u, &border);
    d.addressModeV = address(k.v, &border);
    d.addressModeW = address(k.w, &border);
    d.magFilter = k.mag >= 2 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.minFilter = k.min >= 2 ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.mipmapFilter = k.mip == 2 ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    d.lodMinClamp = k.maxlevel;
    d.lodMaxClamp = k.mip == 0 ? (float)k.maxlevel : 32.0f;
    /* anisotropy only with every filter linear (WebGPU's rule) */
    d.maxAnisotropy = k.aniso > 1 && d.magFilter == WGPUFilterMode_Linear && d.minFilter == WGPUFilterMode_Linear &&
            d.mipmapFilter == WGPUMipmapFilterMode_Linear
        ? k.aniso
        : 1;
    WGPUSamplerDescriptorExtras ex = { 0 };
    if (border)
    {
        uint32_t a = k.border >> 24, l = ((k.border >> 16 & 255) + (k.border >> 8 & 255) + (k.border & 255)) / 3;
        ex.chain.sType = (WGPUSType)WGPUSType_SamplerDescriptorExtras;
        ex.samplerBorderColor = a < 128 ? WGPUSamplerBorderColor_TransparentBlack : l < 128 ? WGPUSamplerBorderColor_OpaqueBlack : WGPUSamplerBorderColor_OpaqueWhite;
        d.nextInChain = &ex.chain;
    }
    s = wgpuDeviceCreateSampler(g_dev, &d);
    map_put(&g_samplers, &k, sizeof k, s);
    return s;
}

/* --- pipelines, built by worker threads --------------------------------------------------------------------- */

typedef struct PipeKey
{
    uint32_t vs, ps;
    uint8_t in_kind[16];
    uint8_t attr_fmt[16], attr_slot[16];
    uint16_t attr_off[16];
    uint16_t stride[16];
    uint8_t instanced[16];
    uint8_t nslots, topo, strip, cull, fill;
    uint8_t blend, src, dst, op, srca, dsta, opa, colors;
    uint8_t wmask[4];
    uint8_t ztest, zwrite, zfunc, stencil, two_sided, fail, zfail, pass, func, ccw_fail, ccw_zfail, ccw_pass, ccw_func, smask, swmask, pad;
    uint32_t color[4], depth;
    int32_t bias;
    float slope;
    uint32_t depth_mask;
    uint64_t tex_mask;
} PipeKey;

typedef struct Pipe
{
    void* _Atomic state; /* NULL while building, FAILED, or the WGPURenderPipeline */
} Pipe;

typedef struct Job
{
    struct Job* next;
    PipeKey k;
    char* src;
    WGPUPipelineLayout layout;
    Pipe* e;
} Job;

static Map g_pipes;
static SDL_Mutex* g_job_lock;
static SDL_Condition* g_job_ready;
static Job *g_jobs, *g_jobs_tail;
static int g_workers;
#define WORKERS 3

static WGPUShaderModule shader_module(const char* src)
{
    WGPUShaderSourceWGSL w = { 0 };
    w.chain.sType = WGPUSType_ShaderSourceWGSL;
    w.code = sv(src);
    WGPUShaderModuleDescriptor d = { 0 };
    d.nextInChain = &w.chain;
    d.label = NOLABEL;
    return wgpuDeviceCreateShaderModule(g_dev, &d);
}

static WGPURenderPipeline build_pipeline(const PipeKey* k, const char* src, WGPUPipelineLayout layout)
{
    uint32_t before = t_errors;
    WGPUShaderModule m = shader_module(src);
    WGPUVertexAttribute at[16];
    WGPUVertexBufferLayout vb[16];
    memset(at, 0, sizeof at);
    memset(vb, 0, sizeof vb);
    int na = 0;
    for (int s = 0; s < k->nslots; ++s)
    {
        vb[s].stepMode = k->instanced[s] ? WGPUVertexStepMode_Instance : WGPUVertexStepMode_Vertex;
        vb[s].arrayStride = k->stride[s];
        vb[s].attributes = &at[na];
        for (int r = 0; r < 16; ++r)
            if (k->attr_fmt[r] && k->attr_slot[r] == s)
            {
                at[na].format = (WGPUVertexFormat)k->attr_fmt[r];
                at[na].offset = k->attr_off[r];
                at[na].shaderLocation = (uint32_t)r;
                na++;
                vb[s].attributeCount++;
            }
    }
    WGPUBlendState bs;
    memset(&bs, 0, sizeof bs);
    if (k->blend)
    {
        int minmax = k->op == 4 || k->op == 5, minmaxa = k->opa == 4 || k->opa == 5; /* the factors must be one */
        bs.color.operation = blend_op(k->op);
        bs.color.srcFactor = minmax ? WGPUBlendFactor_One : blend_factor(k->src);
        bs.color.dstFactor = minmax ? WGPUBlendFactor_One : blend_factor(k->src == 12 ? 6 : k->src == 13 ? 5 : k->dst);
        bs.alpha.operation = blend_op(k->opa);
        bs.alpha.srcFactor = minmaxa ? WGPUBlendFactor_One : blend_factor(k->srca);
        bs.alpha.dstFactor = minmaxa ? WGPUBlendFactor_One : blend_factor(k->srca == 12 ? 6 : k->srca == 13 ? 5 : k->dsta);
    }
    WGPUColorTargetState ct[4];
    memset(ct, 0, sizeof ct);
    int nt = 0;
    for (int i = 0; i < 4; ++i)
    {
        ct[i].format = (WGPUTextureFormat)k->color[i];
        if (!k->color[i])
            continue;
        nt = i + 1;
        uint32_t wm = i < k->colors ? k->wmask[i] : 0; /* no output there: nothing written */
        ct[i].writeMask = (wm & 1 ? WGPUColorWriteMask_Red : 0) | (wm & 2 ? WGPUColorWriteMask_Green : 0) | (wm & 4 ? WGPUColorWriteMask_Blue : 0) |
                          (wm & 8 ? WGPUColorWriteMask_Alpha : 0);
        if (k->blend && !fmt_float32(ct[i].format))
            ct[i].blend = &bs;
    }
    WGPUFragmentState fs = { 0 };
    fs.module = m;
    fs.entryPoint = sv("fs_main");
    fs.targetCount = (size_t)nt;
    fs.targets = ct;
    WGPUDepthStencilState ds = { 0 };
    if (k->depth)
    {
        ds.format = (WGPUTextureFormat)k->depth;
        ds.depthCompare = k->ztest ? compare(k->zfunc) : WGPUCompareFunction_Always;
        ds.depthWriteEnabled = k->zwrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        WGPUStencilFaceState keep = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep };
        ds.stencilFront = ds.stencilBack = keep;
        if (k->stencil)
        {
            ds.stencilFront = (WGPUStencilFaceState){ compare(k->func), stencil_op(k->fail), stencil_op(k->zfail), stencil_op(k->pass) };
            ds.stencilBack = k->two_sided ? (WGPUStencilFaceState){ compare(k->ccw_func), stencil_op(k->ccw_fail), stencil_op(k->ccw_zfail), stencil_op(k->ccw_pass) }
                                          : ds.stencilFront;
            ds.stencilReadMask = k->smask;
            ds.stencilWriteMask = k->swmask;
        }
        ds.depthBias = k->bias;
        ds.depthBiasSlopeScale = k->slope;
    }
    WGPUPrimitiveStateExtras px = { 0 };
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.label = NOLABEL;
    pd.layout = layout;
    pd.vertex.module = m;
    pd.vertex.entryPoint = sv("vs_main");
    pd.vertex.bufferCount = k->nslots;
    pd.vertex.buffers = vb;
    pd.primitive.topology = (WGPUPrimitiveTopology)k->topo;
    pd.primitive.stripIndexFormat = (WGPUIndexFormat)k->strip;
    pd.primitive.frontFace = WGPUFrontFace_CW; /* D3D's front faces are clockwise on screen */
    pd.primitive.cullMode = k->cull == 2 ? WGPUCullMode_Front : k->cull == 3 ? WGPUCullMode_Back : WGPUCullMode_None;
    if (k->fill == 2 && g_has_line)
    {
        px.chain.sType = (WGPUSType)WGPUSType_PrimitiveStateExtras;
        px.polygonMode = WGPUPolygonMode_Line;
        pd.primitive.nextInChain = &px.chain;
    }
    pd.depthStencil = k->depth ? &ds : NULL;
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
    wgpuShaderModuleRelease(m);
    if (p && t_errors != before)
        wgpuRenderPipelineRelease(p), p = NULL;
    if (!p)
    {
        rt_log("[gfx9] pipeline for shader pair %u/%u failed\n", k->vs, k->ps);
        if (getenv("XIVLEGACY_SHADER_SOURCE"))
            rt_log("%s\n", src);
    }
    return p;
}

static void run_job(Job* j)
{
    WGPURenderPipeline p = build_pipeline(&j->k, j->src, j->layout);
    atomic_store(&j->e->state, p ? (void*)p : FAILED);
    free(j->src);
    free(j);
}

static int SDLCALL pipe_worker(void* arg)
{
    (void)arg;
    for (;;)
    {
        SDL_LockMutex(g_job_lock);
        while (!g_jobs)
            SDL_WaitCondition(g_job_ready, g_job_lock);
        Job* j = g_jobs;
        g_jobs = j->next;
        if (!g_jobs)
            g_jobs_tail = NULL;
        SDL_UnlockMutex(g_job_lock);
        run_job(j);
    }
    return 0;
}

/* the pipeline for a key, queued for building the first time (NULL until it is built) */
static WGPURenderPipeline pipeline(const PipeKey* k, const char* src, int sync)
{
    Pipe* e = (Pipe*)map_get(&g_pipes, k, sizeof *k);
    if (e)
    {
        void* st = atomic_load(&e->state);
        return st == FAILED ? NULL : (WGPURenderPipeline)st;
    }
    uint64_t t0 = rt_monotonic_ns();
    g_fc_pipes++;
    e = (Pipe*)calloc(1, sizeof *e);
    map_put(&g_pipes, k, sizeof *k, e);
    Job* j = (Job*)calloc(1, sizeof *j);
    j->k = *k, j->e = e, j->layout = pipeline_layout(k->tex_mask);
    j->src = strdup(src);
    if (!g_workers && !sync)
    {
        g_job_lock = SDL_CreateMutex(), g_job_ready = SDL_CreateCondition();
        for (int i = 0; i < WORKERS; ++i)
        {
            SDL_Thread* t = SDL_CreateThread(pipe_worker, "wgpu pipelines", NULL);
            if (t)
                SDL_DetachThread(t), g_workers++;
        }
    }
    if (sync || !g_workers)
        run_job(j);
    else
    {
        SDL_LockMutex(g_job_lock);
        if (g_jobs_tail)
            g_jobs_tail->next = j, g_jobs_tail = j;
        else
            g_jobs = g_jobs_tail = j;
        SDL_SignalCondition(g_job_ready);
        SDL_UnlockMutex(g_job_lock);
    }
    g_fc_pipe_ns += rt_monotonic_ns() - t0;
    void* st = atomic_load(&e->state);
    return st == FAILED ? NULL : (WGPURenderPipeline)st;
}

/* --- passes, targets ------------------------------------------------------------------------------------------ */

static uint32_t g_clear_flags, g_clear_color, g_clear_stencil;

/* what the open pass has set, so a draw sets only what changes (wgpu records and checks every call) */
static struct
{
    WGPURenderPipeline pipe;
    WGPUBindGroup group1;
    float vp[6];
    uint32_t sc[4], stencil_ref, blend;
    uint8_t vp_set, sc_set, ref_set, blend_set;
} g_ps;
static float g_clear_z;

static void target_size(uint32_t* w, uint32_t* h)
{
    *w = *h = 0;
    uint32_t d;
    for (int i = 0; i < 4; ++i)
        if (g_rt[i].tex && g_rt[i].tex->tex)
        {
            level_size(g_rt[i].tex, g_rt[i].level, w, h, &d);
            return;
        }
    if (g_ds && g_ds->tex)
        level_size(g_ds, 0, w, h, &d);
}

/* the depth attachment: the bound one, or (D3D lets a depth buffer be larger than the color target;
 * WebGPU wants them equal) a scratch one of the color target's size kept per depth buffer and size */
typedef struct Scratch
{
    G9Tex* ds;
    uint32_t w, h;
    WGPUTexture tex;
    WGPUTextureView view;
} Scratch;
static Scratch g_scratch[8];

static WGPUTextureView depth_attachment(void)
{
    if (!g_ds || !g_ds->tex)
        return NULL;
    uint32_t w, h;
    target_size(&w, &h);
    if (g_ds->w == w && g_ds->h == h)
        return target_view(g_ds, 0, 0);
    for (int i = 0; i < 8; ++i)
        if (g_scratch[i].ds == g_ds && g_scratch[i].w == w && g_scratch[i].h == h && g_scratch[i].tex)
            return g_scratch[i].view;
    static int next;
    Scratch* s = &g_scratch[next++ & 7];
    if (s->view)
        wgpuTextureViewRelease(s->view);
    if (s->tex)
        wgpuTextureRelease(s->tex);
    WGPUTextureDescriptor d = { 0 };
    d.label = NOLABEL;
    d.dimension = WGPUTextureDimension_2D;
    d.size = (WGPUExtent3D){ w, h, 1 };
    d.format = g_ds->format;
    d.mipLevelCount = 1, d.sampleCount = 1;
    d.usage = WGPUTextureUsage_RenderAttachment;
    s->tex = wgpuDeviceCreateTexture(g_dev, &d);
    s->view = wgpuTextureCreateView(s->tex, NULL);
    s->ds = g_ds, s->w = w, s->h = h;
    return s->view;
}

static int has_color(int i) { return g_rt[i].tex && g_rt[i].tex->tex && g_rt[i].tex->format != WGPUTextureFormat_Undefined; }

static void current_formats(uint32_t color[4], uint32_t* depth)
{
    for (int i = 0; i < 4; ++i)
        color[i] = has_color(i) ? (uint32_t)g_rt[i].tex->format : 0;
    *depth = g_ds && g_ds->tex ? (uint32_t)g_ds->format : 0;
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
    end_pass();
    memcpy(g_rt, rt, sizeof g_rt);
    g_ds = depth;
    g_targets_dirty = 1;
    g_clear_flags = 0;
}

static void end_pass(void)
{
    if (g_pass)
    {
        wgpuRenderPassEncoderEnd(g_pass);
        wgpuRenderPassEncoderRelease(g_pass);
        g_pass = NULL;
    }
}

static int begin_pass(void)
{
    if (g_pass)
        return 1;
    int any = g_ds && g_ds->tex;
    for (int i = 0; i < 4; ++i)
        any |= has_color(i);
    if (!any)
        return 0;
    WGPURenderPassColorAttachment ca[4];
    memset(ca, 0, sizeof ca);
    int nc = 0;
    uint32_t v = g_clear_color;
    for (int i = 0; i < 4; ++i)
    {
        ca[i].depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        if (!has_color(i))
            continue;
        ca[i].view = target_view(g_rt[i].tex, g_rt[i].face, g_rt[i].level);
        ca[i].storeOp = WGPUStoreOp_Store;
        if (g_clear_flags & 1)
        {
            ca[i].loadOp = WGPULoadOp_Clear;
            ca[i].clearValue = (WGPUColor){ (v >> 16 & 255) / 255.0, (v >> 8 & 255) / 255.0, (v & 255) / 255.0, (v >> 24) / 255.0 };
        }
        else
            ca[i].loadOp = WGPULoadOp_Load;
        g_rt[i].tex->used = g_serial;
        g_rt[i].tex->writes++;
        nc = i + 1;
    }
    WGPURenderPassDepthStencilAttachment da = { 0 };
    WGPURenderPassDescriptor p = { 0 };
    p.label = NOLABEL;
    p.colorAttachmentCount = (size_t)nc;
    p.colorAttachments = ca;
    WGPUTextureView dv = depth_attachment();
    if (dv)
    {
        da.view = dv;
        da.depthLoadOp = g_clear_flags & 2 ? WGPULoadOp_Clear : WGPULoadOp_Load;
        da.depthStoreOp = WGPUStoreOp_Store;
        da.depthClearValue = g_clear_z;
        if (g_ds->stencil)
        {
            da.stencilLoadOp = g_clear_flags & 4 ? WGPULoadOp_Clear : WGPULoadOp_Load;
            da.stencilStoreOp = WGPUStoreOp_Store;
            da.stencilClearValue = g_clear_stencil & 0xFF;
        }
        p.depthStencilAttachment = &da;
        g_ds->used = g_serial;
        g_ds->writes++;
    }
    g_clear_flags = 0;
    g_pass = wgpuCommandEncoderBeginRenderPass(enc(), &p);
    g_st_passes++;
    memset(&g_ps, 0, sizeof g_ps);
    g_targets_dirty = 0;
    return 1;
}

/* a clear still waiting for the next pass carried out now (see gfx9_metal.m's flush_clear) */
static void flush_clear(void)
{
    if (!g_clear_flags || g_pass)
        return;
    if (begin_pass())
        end_pass();
    g_clear_flags = 0;
}

static void set_viewport(const uint32_t vp[6])
{
    float zn, zf;
    memcpy(&zn, &vp[4], 4);
    memcpy(&zf, &vp[5], 4);
    uint32_t tw, th;
    target_size(&tw, &th);
    float x = (float)vp[0], y = (float)vp[1], w = (float)vp[2], h = (float)vp[3];
    /* WebGPU wants the viewport inside the target (D3D does too, mostly) */
    if (x > tw)
        x = (float)tw;
    if (y > th)
        y = (float)th;
    if (x + w > tw)
        w = tw - x;
    if (y + h > th)
        h = th - y;
    if (w < 1)
        w = 1;
    if (h < 1)
        h = 1;
    zn = zn < 0 ? 0 : zn > 1 ? 1 : zn;
    zf = zf < zn ? zn : zf > 1 ? 1 : zf;
    float v[6] = { x, y, w, h, zn, zf };
    if (g_ps.vp_set && !memcmp(v, g_ps.vp, sizeof v))
        return;
    memcpy(g_ps.vp, v, sizeof v), g_ps.vp_set = 1;
    wgpuRenderPassEncoderSetViewport(g_pass, x, y, w, h, zn, zf);
}

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
        l = t = rr = b = 0;
    uint32_t sc[4] = { (uint32_t)l, (uint32_t)t, (uint32_t)(rr - l), (uint32_t)(b - t) };
    if (g_ps.sc_set && !memcmp(sc, g_ps.sc, sizeof sc))
        return;
    memcpy(g_ps.sc, sc, sizeof sc), g_ps.sc_set = 1;
    wgpuRenderPassEncoderSetScissorRect(g_pass, sc[0], sc[1], sc[2], sc[3]);
}

/* --- clears, copies, present: the utility shaders --------------------------------------------------------------- */

static const char UTIL_WGSL[] =
    "struct CU { color: vec4<f32>, z: vec4<f32> };\n"
    "@group(0) @binding(0) var<uniform> cu: CU;\n"
    "@vertex fn clear_vs(@builtin(vertex_index) vid: u32) -> @builtin(position) vec4<f32> {\n"
    "    let p = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));\n"
    "    return vec4<f32>(p * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), cu.z.x, 1.0);\n"
    "}\n"
    "struct CO { @location(0) c0: vec4<f32>, @location(1) c1: vec4<f32>, @location(2) c2: vec4<f32>, @location(3) c3: vec4<f32> };\n"
    "@fragment fn clear_fs() -> CO { var o: CO; o.c0 = cu.color; o.c1 = cu.color; o.c2 = cu.color; o.c3 = cu.color; return o; }\n";

static const char COPY_WGSL[] =
    "struct PO { @builtin(position) pos: vec4<f32>, @location(0) uv: vec2<f32> };\n"
    "struct CP { uvx: vec4<f32>, alpha1: vec4<f32> };\n"
    "@group(0) @binding(0) var<uniform> cp: CP;\n"
    "@group(0) @binding(1) var t: texture_2d<f32>;\n"
    "@group(0) @binding(2) var s: sampler;\n"
    "@vertex fn copy_vs(@builtin(vertex_index) vid: u32) -> PO {\n"
    "    let p = vec2<f32>(f32((vid << 1u) & 2u), f32(vid & 2u));\n"
    "    var o: PO; o.pos = vec4<f32>(p * vec2<f32>(2.0, -2.0) + vec2<f32>(-1.0, 1.0), 0.0, 1.0); o.uv = cp.uvx.xy + p * cp.uvx.zw; return o;\n"
    "}\n"
    "@fragment fn copy_fs(i: PO) -> @location(0) vec4<f32> {\n"
    "    let c = textureSample(t, s, i.uv);\n"
    "    return vec4<f32>(c.rgb, max(c.a, cp.alpha1.x));\n"
    "}\n";

static WGPUShaderModule g_util_module, g_copy_module;
static WGPURenderPipeline g_present_pipe;
static WGPUBindGroupLayout g_copy_bgl;
static WGPUPipelineLayout g_util_layout, g_copy_layout;
static WGPUSampler g_linear, g_point;
static Map g_util_pipes, g_copy_pipes;

typedef struct UtilKey
{
    uint32_t flags; /* D3DCLEAR */
    uint32_t color[4], depth;
} UtilKey;

static WGPURenderPipeline clear_pipeline(const UtilKey* k)
{
    WGPURenderPipeline p = (WGPURenderPipeline)map_get(&g_util_pipes, k, sizeof *k);
    if (p)
        return p == FAILED ? NULL : p;
    WGPUColorTargetState ct[4];
    memset(ct, 0, sizeof ct);
    int nt = 0;
    for (int i = 0; i < 4; ++i)
        if ((ct[i].format = (WGPUTextureFormat)k->color[i]))
            ct[i].writeMask = k->flags & 1 ? WGPUColorWriteMask_All : WGPUColorWriteMask_None, nt = i + 1;
    WGPUFragmentState fs = { 0 };
    fs.module = g_util_module;
    fs.entryPoint = sv("clear_fs");
    fs.targetCount = (size_t)nt;
    fs.targets = ct;
    WGPUDepthStencilState ds = { 0 };
    WGPUStencilFaceState keep = { WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep };
    if (k->depth)
    {
        ds.format = (WGPUTextureFormat)k->depth;
        ds.depthCompare = WGPUCompareFunction_Always;
        ds.depthWriteEnabled = k->flags & 2 ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        ds.stencilFront = ds.stencilBack = keep;
        if ((k->flags & 4) && fmt_stencil(ds.format))
        {
            WGPUStencilFaceState rep = { WGPUCompareFunction_Always, WGPUStencilOperation_Replace, WGPUStencilOperation_Replace, WGPUStencilOperation_Replace };
            ds.stencilFront = ds.stencilBack = rep;
            ds.stencilReadMask = ds.stencilWriteMask = 0xFF;
        }
    }
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.label = NOLABEL;
    pd.layout = g_util_layout;
    pd.vertex.module = g_util_module;
    pd.vertex.entryPoint = sv("clear_vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.depthStencil = k->depth ? &ds : NULL;
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    uint32_t before = t_errors;
    p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
    if (p && before != t_errors)
        wgpuRenderPipelineRelease(p), p = NULL;
    map_put(&g_util_pipes, k, sizeof *k, p ? (void*)p : FAILED);
    return p;
}

static WGPURenderPipeline copy_pipeline(WGPUTextureFormat f)
{
    WGPURenderPipeline p = (WGPURenderPipeline)map_get(&g_copy_pipes, &f, sizeof f);
    if (p)
        return p == FAILED ? NULL : p;
    WGPUColorTargetState ct = { 0 };
    ct.format = f;
    ct.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs = { 0 };
    fs.module = g_copy_module;
    fs.entryPoint = sv("copy_fs");
    fs.targetCount = 1;
    fs.targets = &ct;
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.label = NOLABEL;
    pd.layout = g_copy_layout;
    pd.vertex.module = g_copy_module;
    pd.vertex.entryPoint = sv("copy_vs");
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CCW;
    pd.multisample.count = 1;
    pd.multisample.mask = 0xFFFFFFFFu;
    pd.fragment = &fs;
    uint32_t before = t_errors;
    p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
    if (p && before != t_errors)
        wgpuRenderPipelineRelease(p), p = NULL;
    map_put(&g_copy_pipes, &f, sizeof f, p ? (void*)p : FAILED);
    return p;
}

/* a bind group for a copy: uniforms at off in the chunk, the view, the sampler (made per copy: few a frame) */
static WGPUBindGroup copy_group(Chunk* c, uint64_t off, WGPUTextureView view, WGPUSampler smp)
{
    WGPUBindGroupEntry e[3];
    memset(e, 0, sizeof e);
    e[0].binding = 0, e[0].buffer = c->gpu, e[0].offset = off, e[0].size = 32;
    e[1].binding = 1, e[1].textureView = view;
    e[2].binding = 2, e[2].sampler = smp;
    WGPUBindGroupDescriptor bd = { 0 };
    bd.label = NOLABEL;
    bd.layout = g_copy_bgl;
    bd.entryCount = 3;
    bd.entries = e;
    return wgpuDeviceCreateBindGroup(g_dev, &bd);
}

static WGPUBindGroup util_group(Chunk* c)
{
    if (!c->util)
    {
        WGPUBindGroupEntry e = { 0 };
        e.binding = 0, e.buffer = c->gpu, e.offset = 0, e.size = 32;
        WGPUBindGroupDescriptor bd = { 0 };
        bd.label = NOLABEL;
        bd.layout = g_bgl_util;
        bd.entryCount = 1;
        bd.entries = &e;
        c->util = wgpuDeviceCreateBindGroup(g_dev, &bd);
    }
    return c->util;
}

void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t viewport[6], const int32_t* scissor)
{
    TRACE("[trace] clear flags %u color %08x\n", flags, color);
    if (!g_dev)
        return;
    if (!g_ds || !g_ds->tex)
        flags &= 1;
    else if (!g_ds->stencil)
        flags &= 3;
    if (!flags)
        return;
    uint32_t tw, th;
    target_size(&tw, &th);
    int whole = !nrects && viewport[0] == 0 && viewport[1] == 0 && viewport[2] >= tw && viewport[3] >= th &&
                (!scissor || (scissor[0] <= 0 && scissor[1] <= 0 && scissor[2] >= (int32_t)tw && scissor[3] >= (int32_t)th));
    if (whole && !g_pass)
    {
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
        end_pass();
        g_clear_flags = flags, g_clear_color = color, g_clear_z = z, g_clear_stencil = stencil;
        begin_pass();
        return;
    }
    if (!begin_pass())
        return;
    UtilKey k;
    memset(&k, 0, sizeof k);
    k.flags = flags;
    current_formats(k.color, &k.depth);
    WGPURenderPipeline p = clear_pipeline(&k);
    if (!p)
        return;
    uint64_t off;
    void* cpu;
    Chunk* c = ring(32, 256, &off, &cpu);
    float u[8] = { (color >> 16 & 255) / 255.0f, (color >> 8 & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f, z, 0, 0, 0 };
    memcpy(cpu, u, sizeof u);
    wgpuRenderPassEncoderSetPipeline(g_pass, p);
    g_ps.pipe = NULL, g_ps.group1 = NULL, g_ps.ref_set = 0;
    uint32_t dyn = (uint32_t)off;
    wgpuRenderPassEncoderSetBindGroup(g_pass, 0, util_group(c), 1, &dyn);
    wgpuRenderPassEncoderSetStencilReference(g_pass, stencil & 0xFF);
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
        wgpuRenderPassEncoderDraw(g_pass, 3, 1, 0, 0);
    }
}

/* --- draws ------------------------------------------------------------------------------------------------- */

static int g_debug_mode;
void g9_debug(int mode) { g_debug_mode = mode; }
static uint32_t g_skip_vs, g_only_vs, g_dump_vs, g_dump_left;
void g9_skip_vs(uint32_t id) { g_skip_vs = id; }
void g9_only_vs(uint32_t id) { g_only_vs = id; }
void g9_dump_vs(uint32_t id, uint32_t count) { g_dump_vs = id, g_dump_left = count; }
void g9_cpu_check_vs(uint32_t id)
{
    (void)id;
    rt_log("[gfx9] cpuvs: the Metal back end's (not on wgpu)\n");
}

static void dump_draw(const G9Draw* d)
{
    printf("[drawdump] vs%u ps%u prim %u x%u instances %u indices %s size %u base %d\n", d->vs->id, d->ps->id, d->prim, d->prims, d->instances,
        d->indices ? "yes" : "no", d->index_size, d->base_vertex);
    for (uint32_t i = 0; i < d->nelems; ++i)
        printf("[drawdump]   elem stream %u type %u usage %u index %u offset %u\n", d->elems[i].stream, d->elems[i].type, d->elems[i].usage,
            d->elems[i].index, d->elems[i].offset);
    for (int r = 0; r < 16; ++r)
        if (d->vs->in_mask & (1u << r))
            printf("[drawdump]   v%d wants usage %u index %u\n", r, d->vs->in_usage[r], d->vs->in_index[r]);
    for (int si = 0; si < 16; ++si)
        if (d->streams[si].data)
            printf("[drawdump]   stream %d stride %u size %u instanced %u\n", si, d->streams[si].stride, d->streams[si].size, d->streams[si].instanced);
    fflush(stdout);
}

static G9Tex* g_dummy[3];
static G9Tex* g_gray;

static G9Tex* dummy(int type)
{
    int i = type == SM3_TEX_CUBE ? 1 : type == SM3_TEX_VOLUME ? 2 : 0;
    if (!g_dummy[i])
    {
        g_dummy[i] = g9_tex_create(i == 1 ? G9_TEX_CUBE : i == 2 ? G9_TEX_3D : G9_TEX_2D, D_A8B8G8R8, 1, 1, 1, 1, G9_USE_SAMPLE);
        uint8_t zero[4] = { 0, 0, 0, 0 };
        for (int f = 0; f < (i == 1 ? 6 : 1); ++f)
            g9_tex_upload(g_dummy[i], (uint32_t)f, 0, zero, 4, 4);
    }
    return g_dummy[i];
}

static G9Tex* gray2d(void)
{
    if (!g_gray)
    {
        g_gray = g9_tex_create(G9_TEX_2D, D_A8B8G8R8, 1, 1, 1, 1, G9_USE_SAMPLE);
        uint8_t px[4] = { 128, 128, 128, 255 };
        g9_tex_upload(g_gray, 0, 0, px, 4, 4);
    }
    return g_gray;
}

/* the texture a sampler of the shader's type reads */
static G9Tex* texture_for(G9Tex* t, int type)
{
    int want = type == SM3_TEX_CUBE ? G9_TEX_CUBE : type == SM3_TEX_VOLUME ? G9_TEX_3D : G9_TEX_2D;
    if (g_debug_mode == 2 && want == G9_TEX_2D && t && t->view && t->type == want && t->use == G9_USE_SAMPLE)
        return gray2d();
    if (g_debug_mode == 3 && want == G9_TEX_2D && t && t->view && t->type == want && t->use != G9_USE_SAMPLE)
        return gray2d();
    if (!t || !t->view || t->type != want)
        return dummy(type);
    return t;
}

static int is_target(const G9Tex* t)
{
    if (t == g_ds)
        return 1;
    for (int i = 0; i < 4; ++i)
        if (g_rt[i].tex == t)
            return 1;
    return 0;
}

/* what to sample t through: its own view, or when the pass draws to it, a copy made at most once a
 * frame since it was last drawn to (ends the pass for the copy) */
static WGPUTextureView sample_view(G9Tex* t, uint64_t* uid)
{
    *uid = t->uid;
    if (!is_target(t) || !t->tex)
        return t->view;
    *uid = t->shadow_uid;
    if (!t->shadow)
    {
        t->shadow = make_texture(t, WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst);
        Swz sw;
        int conv;
        uint32_t block, bpp;
        texture_format(t->fmt, t->use, &conv, &block, &bpp, &sw);
        t->shadow_view = make_view(t, t->shadow, &sw);
        t->shadow_at = ~0ull;
    }
    if (t->shadow_at != t->writes && t->shadow_frame != g_frame + 1)
    {
        t->shadow_frame = g_frame + 1;
        flush_clear();
        end_pass();
        uint32_t layers = t->type == G9_TEX_CUBE ? 6 : 1;
        for (uint32_t level = 0; level < t->levels; ++level)
        {
            uint32_t w, h, d;
            level_size(t, level, &w, &h, &d);
            WGPUTexelCopyTextureInfo s = { t->tex, level, { 0, 0, 0 }, WGPUTextureAspect_All };
            WGPUTexelCopyTextureInfo dd = { t->shadow, level, { 0, 0, 0 }, WGPUTextureAspect_All };
            WGPUExtent3D size = { w, h, t->type == G9_TEX_3D ? d : layers };
            wgpuCommandEncoderCopyTextureToTexture(enc(), &s, &dd, &size);
        }
        t->shadow_at = t->writes;
        g_st_shadows++;
        t->used = g_serial;
    }
    return t->shadow_view;
}

static unsigned tex_kind(const G9Tex* t)
{
    if (t->depth)
        return t->type == G9_TEX_2D ? K_DEPTH : K_NONE;
    unsigned k = t->type == G9_TEX_CUBE ? K_CUBE : t->type == G9_TEX_3D ? K_3D : K_2D;
    return t->unfilterable ? k + (K_U2D - K_2D) : k;
}

void g9_frame_costs(uint32_t* compiles, uint64_t* compile_ns, uint32_t* pipes, uint64_t* pipe_ns, uint32_t* uploads, uint64_t* upload_bytes,
    uint64_t* upload_ns)
{
    *compiles = g_fc_compiles, *compile_ns = g_fc_compile_ns, *pipes = g_fc_pipes, *pipe_ns = g_fc_pipe_ns;
    *uploads = g_fc_uploads, *upload_bytes = g_fc_upload_bytes, *upload_ns = g_fc_upload_ns;
    g_fc_compiles = g_fc_pipes = g_fc_uploads = 0;
    g_fc_compile_ns = g_fc_pipe_ns = g_fc_upload_ns = g_fc_upload_bytes = 0;
}

static WGPUBindGroup group0(Chunk* c)
{
    if (!c->group0)
    {
        WGPUBindGroupEntry e[2];
        memset(e, 0, sizeof e);
        e[0].binding = 0, e[0].buffer = c->gpu, e[0].offset = 0, e[0].size = SM3W_VS_SIZE;
        e[1].binding = 1, e[1].buffer = c->gpu, e[1].offset = 0, e[1].size = SM3W_PS_SIZE;
        WGPUBindGroupDescriptor bd = { 0 };
        bd.label = NOLABEL;
        bd.layout = g_bgl0;
        bd.entryCount = 2;
        bd.entries = e;
        c->group0 = wgpuDeviceCreateBindGroup(g_dev, &bd);
    }
    return c->group0;
}

#define VS_BLOCK ((SM3W_VS_SIZE + 255u) & ~255u)

void g9_draw(const G9Draw* d)
{
    if (!g_dev || !d->vs || !d->ps || !d->prims)
        return;
    G9Shader* vs = d->vs;
    G9Shader* ps = d->ps;
    if (g_skip_vs && vs->id == g_skip_vs)
        return;
    if (g_only_vs && vs->id != g_only_vs && g_rt[0].tex && g_rt[0].tex->w >= 640)
        return;
    if (g_dump_left && vs->id == g_dump_vs)
    {
        g_dump_left--;
        dump_draw(d);
    }
    uint32_t count = vertex_count(d->prim, d->prims);
    uint32_t inst = d->instances ? d->instances : 1;
    if (!count)
        return;

    /* the vertex inputs from the declaration: streams packed into consecutive buffer slots */
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs->id, k.ps = ps->id;
    int slot_of[16];
    uint32_t need[16]; /* bytes a slot's attributes reach into a vertex */
    for (int s = 0; s < 16; ++s)
        slot_of[s] = -1, need[s] = 0;
    int stream_of[16];
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
        uint32_t size = 0;
        WGPUVertexFormat f = e ? vertex_format(e->type, &k.in_kind[r], &size) : (WGPUVertexFormat)0;
        const G9Stream* st = e && e->stream < 16 ? &d->streams[e->stream] : NULL;
        uint32_t stride = st ? st->stride : 0;
        if (!e || !f || !st || !st->data || !st->size || stride % 4 || stride > 2048 || (stride && e->offset + size > stride) ||
            e->offset % (size < 4 ? size : 4) || (st->instanced > 1))
        {
            k.in_kind[r] = SM3W_IN_ZERO;
            continue;
        }
        int s = slot_of[e->stream];
        if (s < 0)
        {
            s = slot_of[e->stream] = k.nslots++;
            stream_of[s] = e->stream;
            k.stride[s] = (uint16_t)stride;
            k.instanced[s] = (uint8_t)(st->instanced ? 1 : 0);
        }
        k.attr_fmt[r] = (uint8_t)f;
        k.attr_slot[r] = (uint8_t)s;
        k.attr_off[r] = e->offset;
        if (e->offset + size > need[s])
            need[s] = e->offset + size;
    }
    const uint32_t* rs = d->rs;
    k.blend = rs[27] != 0;
    k.src = (uint8_t)rs[19], k.dst = (uint8_t)rs[20], k.op = (uint8_t)rs[171];
    if (rs[206])
        k.srca = (uint8_t)rs[207], k.dsta = (uint8_t)rs[208], k.opa = (uint8_t)rs[209];
    else
        k.srca = k.src, k.dsta = k.dst, k.opa = k.op;
    if (!k.blend)
        k.src = k.dst = k.op = k.srca = k.dsta = k.opa = 0;
    k.wmask[0] = (uint8_t)(rs[168] & 15), k.wmask[1] = (uint8_t)(rs[190] & 15), k.wmask[2] = (uint8_t)(rs[191] & 15);
    k.wmask[3] = (uint8_t)(rs[192] & 15);
    k.colors = ps->colors;
    current_formats(k.color, &k.depth);
    k.topo = (uint8_t)topology(d->prim);
    /* indexed strips: the pipeline names the index format (base vertex applied: 32-bit) */
    if (d->indices && (d->prim == 3 || d->prim == 5))
        k.strip = (uint8_t)(d->index_size == 4 || d->base_vertex ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16);
    k.cull = (uint8_t)(rs[22] == 2 || rs[22] == 3 ? rs[22] : 1);
    k.fill = (uint8_t)(rs[8] == 2 || g_debug_mode == 1 ? 2 : 3);
    if (k.depth)
    {
        k.ztest = rs[7] != 0;
        k.zwrite = rs[7] != 0 && rs[14] != 0;
        k.zfunc = (uint8_t)rs[23];
        if (fmt_stencil((WGPUTextureFormat)k.depth) && rs[52])
        {
            k.stencil = 1;
            k.fail = (uint8_t)rs[53], k.zfail = (uint8_t)rs[54], k.pass = (uint8_t)rs[55], k.func = (uint8_t)rs[56];
            k.two_sided = rs[185] != 0;
            if (k.two_sided)
                k.ccw_fail = (uint8_t)rs[186], k.ccw_zfail = (uint8_t)rs[187], k.ccw_pass = (uint8_t)rs[188], k.ccw_func = (uint8_t)rs[189];
            k.smask = (uint8_t)rs[58], k.swmask = (uint8_t)rs[59];
        }
        float bias, slope;
        memcpy(&bias, &rs[195], 4);
        memcpy(&slope, &rs[175], 4);
        /* no depth bias on points and lines (WebGPU's rule) */
        if (k.topo == WGPUPrimitiveTopology_TriangleList || k.topo == WGPUPrimitiveTopology_TriangleStrip)
        {
            float b = bias * (k.depth == WGPUTextureFormat_Depth16Unorm ? 65535.0f : 16777216.0f);
            k.bias = (int32_t)(b > 2e9f ? 2e9f : b < -2e9f ? -2e9f : b);
            k.slope = slope;
        }
    }

    /* textures the shaders declare: their binding kinds, and the views (copies made before the pass) */
    G9Tex* tex[20] = { 0 };
    WGPUTextureView views[20] = { 0 };
    uint64_t uids[20] = { 0 };
    for (int n = 0; n < 20; ++n)
    {
        int type = n < 16 ? ps->tex_type[n] : vs->tex_type[n - 16];
        if (!type)
            continue;
        G9Tex* t = texture_for(d->tex[n], type);
        if (!t || !t->view)
            continue;
        unsigned kind = tex_kind(t);
        if (!kind || (kind == K_DEPTH && type != SM3_TEX_2D))
            t = dummy(type), kind = tex_kind(t);
        tex[n] = t;
        k.tex_mask |= (uint64_t)kind << (3 * n);
        if (kind == K_DEPTH)
            k.depth_mask |= 1u << n; /* bit n: s<n>; 16 + n: vertex sampler n (sm3_wgsl.h) */
    }

    const char* src = program(vs, ps, k.in_kind, k.depth_mask);
    if (!src)
    {
        g_skipped++;
        return;
    }
    WGPURenderPipeline pipe = pipeline(&k, src, 0);
    if (!pipe)
    {
        g_skipped++;
        return;
    }
    for (int n = 0; n < 20; ++n)
        if (tex[n])
            views[n] = sample_view(tex[n], &uids[n]);
    if (!begin_pass())
    {
        g_skipped++;
        return;
    }
    WGPURenderPassEncoder e = g_pass;
    if (g_ps.pipe != pipe)
        wgpuRenderPassEncoderSetPipeline(e, pipe), g_ps.pipe = pipe;
    if (k.stencil && (!g_ps.ref_set || g_ps.stencil_ref != (rs[57] & 0xFF)))
        wgpuRenderPassEncoderSetStencilReference(e, rs[57] & 0xFF), g_ps.stencil_ref = rs[57] & 0xFF, g_ps.ref_set = 1;
    if (k.blend && (k.src == 14 || k.src == 15 || k.dst == 14 || k.dst == 15 || k.srca >= 14 || k.dsta >= 14))
    {
        uint32_t bc = rs[193];
        WGPUColor c = { (bc >> 16 & 255) / 255.0, (bc >> 8 & 255) / 255.0, (bc & 255) / 255.0, (bc >> 24) / 255.0 };
        if (!g_ps.blend_set || g_ps.blend != bc)
            wgpuRenderPassEncoderSetBlendConstant(e, &c), g_ps.blend = bc, g_ps.blend_set = 1;
    }
    set_viewport(d->viewport);
    set_scissor(rs[174] ? d->scissor : NULL);

    /* the uniforms: both stages' blocks in one ring allocation */
    uint64_t off;
    void* cpu;
    Chunk* c = ring(VS_BLOCK + SM3W_PS_SIZE, 256, &off, &cpu);
    uint8_t* vu = (uint8_t*)cpu;
    uint8_t* pu = vu + VS_BLOCK;
    Sm3wFix fx;
    memset(&fx, 0, sizeof fx);
    float vw = (float)d->viewport[2], vh = (float)d->viewport[3];
    /* D3D9's pixel centers sit on integer coordinates, WebGPU's on half: geometry moves half a pixel
     * right and down */
    fx.posfix[0] = vw > 0 ? 1.0f / vw : 0, fx.posfix[1] = vh > 0 ? -1.0f / vh : 0;
    fx.alpha_func = rs[15] ? rs[25] : 8;
    fx.alpha_ref = (rs[24] & 255) / 255.0f;
    fx.srgb_write = rs[194] != 0;
    for (int n = 0; n < 16; ++n)
        if (d->ss[n][11])
            fx.srgb_mask |= 1u << n;
    Sm3wFix vfx = fx;
    vfx.srgb_mask = 0;
    for (int n = 0; n < 4; ++n)
        if (d->ss[16 + n][11])
            vfx.srgb_mask |= 1u << n;
    if (!g_has_swizzle)
        for (int n = 0; n < 20; ++n)
            if (tex[n] && (tex[n]->fmt == D_X8R8G8B8 || tex[n]->fmt == D_X8B8G8R8))
            {
                if (n < 16)
                    fx.one_mask |= 1u << n;
                else
                    vfx.one_mask |= 1u << (n - 16);
            }
    memcpy(vu, &vfx, sizeof vfx);
    memcpy(vu + SM3W_OFF_I, d->vsi, 256);
    memcpy(vu + SM3W_OFF_B, d->vsb, 64);
    memcpy(vu + SM3W_OFF_C, d->vsc, 16 * (vs->nconst ? vs->nconst : 1));
    memcpy(pu, &fx, sizeof fx);
    memcpy(pu + SM3W_OFF_I, d->psi, 256);
    memcpy(pu + SM3W_OFF_B, d->psb, 64);
    memcpy(pu + SM3W_OFF_C, d->psc, 16 * (ps->nconst ? ps->nconst : 1));
    uint32_t dyn[2] = { (uint32_t)off, (uint32_t)off + VS_BLOCK };
    wgpuRenderPassEncoderSetBindGroup(e, 0, group0(c), 2, dyn);

    /* group 1: textures and samplers, kept per (layout, views, samplers) */
    if (k.tex_mask)
    {
        struct
        {
            uint64_t mask;
            uint64_t view[20];
            WGPUSampler samp[20];
        } key;
        memset(&key, 0, sizeof key);
        key.mask = k.tex_mask;
        WGPUBindGroupEntry be[40];
        memset(be, 0, sizeof be);
        int nb = 0;
        for (int n = 0; n < 20; ++n)
        {
            unsigned kind = slot_kind(k.tex_mask, n);
            if (!kind)
                continue;
            WGPUSampler sm = sampler(d->ss[n], kind == K_DEPTH || kind >= K_U2D);
            uint32_t b = n < 16 ? 2u * (uint32_t)n : 32u + 2u * (uint32_t)(n - 16);
            be[nb].binding = b, be[nb].textureView = views[n], nb++;
            be[nb].binding = b + 1, be[nb].sampler = sm, nb++;
            key.view[n] = uids[n], key.samp[n] = sm;
            tex[n]->used = g_serial;
        }
        WGPUBindGroup g = (WGPUBindGroup)map_get(&g_groups1, &key, sizeof key);
        if (!g)
        {
            if (g_groups1.n >= 16384)
                map_clear(&g_groups1, release_group), g_ps.group1 = NULL;
            WGPUBindGroupDescriptor bd = { 0 };
            bd.label = NOLABEL;
            bd.layout = layout1(k.tex_mask);
            bd.entryCount = (size_t)nb;
            bd.entries = be;
            g = wgpuDeviceCreateBindGroup(g_dev, &bd);
            map_put(&g_groups1, &key, sizeof key, g);
        }
        if (g_ps.group1 != g)
            wgpuRenderPassEncoderSetBindGroup(e, 1, g, 0, NULL), g_ps.group1 = g;
    }

    /* streams: copied into the ring, long enough for what a draw of this size reads (WebGPU checks) */
    for (int s = 0; s < k.nslots; ++s)
    {
        const G9Stream* st = &d->streams[stream_of[s]];
        uint32_t n = k.instanced[s] ? inst : d->indices ? 0 : count;
        uint64_t want = n ? (uint64_t)(n - 1) * st->stride + need[s] : 0;
        uint64_t size = st->size > want ? st->size : want;
        size = (size + 3) & ~3ull;
        void* p;
        Chunk* vc = ring(size, 16, &off, &p);
        memcpy(p, st->data, st->size);
        wgpuRenderPassEncoderSetVertexBuffer(e, (uint32_t)s, vc->gpu, off, size);
    }

    if (g_trace == 1)
    {
        char line[640];
        float mnz, mxz;
        memcpy(&mnz, &d->viewport[4], 4);
        memcpy(&mxz, &d->viewport[5], 4);
        size_t o = (size_t)snprintf(line, sizeof line, "[trace] draw vs%u ps%u prim %u x%u blend %u(%u,%u,%u) zw %u vp %u,%u %ux%u z %g..%g sc %d cull %u",
            vs->id, ps->id, d->prim, d->prims, k.blend, k.src, k.dst, k.op, rs[14], d->viewport[0], d->viewport[1], d->viewport[2], d->viewport[3], mnz,
            mxz, rs[174] ? 1 : 0, rs[22]);
        for (int n = 0; n < 16 && o < sizeof line - 80; ++n)
            if (ps->tex_type[n])
            {
                char b[64];
                o += (size_t)snprintf(line + o, sizeof line - o, " s%d=%s", n, tname(d->tex[n], b, sizeof b));
            }
        fprintf(stdout, "%s\n", line);
    }

    /* the draw */
    if (d->prim == 6)
    {
        /* a fan: as a list */
        uint32_t tris = d->prims, n = tris * 3;
        void* p;
        Chunk* ic = ring(4 * n, 16, &off, &p);
        uint32_t* idx = (uint32_t*)p;
        for (uint32_t t = 0; t < tris; ++t)
        {
            uint32_t a = 0, b = t + 1, cc = t + 2;
            if (d->indices)
            {
                if (d->index_size == 2)
                    a = ((const uint16_t*)d->indices)[a], b = ((const uint16_t*)d->indices)[b], cc = ((const uint16_t*)d->indices)[cc];
                else
                    a = ((const uint32_t*)d->indices)[a], b = ((const uint32_t*)d->indices)[b], cc = ((const uint32_t*)d->indices)[cc];
                int32_t bv = d->base_vertex;
                a = (uint32_t)((int32_t)a + bv), b = (uint32_t)((int32_t)b + bv), cc = (uint32_t)((int32_t)cc + bv);
            }
            idx[3 * t] = a, idx[3 * t + 1] = b, idx[3 * t + 2] = cc;
        }
        wgpuRenderPassEncoderSetIndexBuffer(e, ic->gpu, WGPUIndexFormat_Uint32, off, 4ull * n);
        wgpuRenderPassEncoderDrawIndexed(e, n, inst, 0, 0, 0);
    }
    else if (d->indices && d->base_vertex == 0)
    {
        size_t bytes = (size_t)count * d->index_size;
        void* p;
        Chunk* ic = ring((bytes + 3) & ~(size_t)3, 16, &off, &p);
        memcpy(p, d->indices, bytes);
        wgpuRenderPassEncoderSetIndexBuffer(e, ic->gpu, d->index_size == 4 ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Uint16, off, bytes);
        wgpuRenderPassEncoderDrawIndexed(e, count, inst, 0, 0, 0);
    }
    else if (d->indices)
    {
        /* the base vertex applied to the indices as they are copied, as 32-bit indices */
        void* p;
        Chunk* ic = ring((size_t)count * 4, 16, &off, &p);
        uint32_t* ip = (uint32_t*)p;
        for (uint32_t i = 0; i < count; ++i)
        {
            int32_t v = (d->index_size == 4 ? (int32_t)((const uint32_t*)d->indices)[i] : (int32_t)((const uint16_t*)d->indices)[i]) + d->base_vertex;
            ip[i] = v < 0 ? 0u : (uint32_t)v;
        }
        wgpuRenderPassEncoderSetIndexBuffer(e, ic->gpu, WGPUIndexFormat_Uint32, off, 4ull * count);
        wgpuRenderPassEncoderDrawIndexed(e, count, inst, 0, 0, 0);
    }
    else
        wgpuRenderPassEncoderDraw(e, count, inst, 0, 0);
    g_draws++;
}

/* --- copies ------------------------------------------------------------------------------------------------- */

void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t* srect, G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t* drect, int linear)
{
    {
        char a[64], b[64];
        TRACE("[trace] stretch %s L%u -> %s L%u %s%s\n", tname(src, a, sizeof a), slevel, tname(dst, b, sizeof b), dlevel, srect ? "srect " : "",
            drect ? "drect" : "");
    }
    if (!g_dev || !src || !dst || !src->tex || !dst->tex || slevel >= src->levels || dlevel >= dst->levels)
        return;
    uint32_t sw, sh, sd, dw, dh, dd;
    level_size(src, slevel, &sw, &sh, &sd);
    level_size(dst, dlevel, &dw, &dh, &dd);
    int32_t s[4] = { 0, 0, (int32_t)sw, (int32_t)sh }, t[4] = { 0, 0, (int32_t)dw, (int32_t)dh };
    if (srect)
        memcpy(s, srect, sizeof s);
    if (drect)
        memcpy(t, drect, sizeof t);
    if (s[0] < 0 || s[1] < 0 || s[2] > (int32_t)sw || s[3] > (int32_t)sh || t[0] < 0 || t[1] < 0 || t[2] > (int32_t)dw || t[3] > (int32_t)dh)
        return;
    if (s[2] <= s[0] || s[3] <= s[1] || t[2] <= t[0] || t[3] <= t[1])
        return;
    flush_clear();
    end_pass();
    g_targets_dirty = 1;
    int same = s[2] - s[0] == t[2] - t[0] && s[3] - s[1] == t[3] - t[1] && src->format == dst->format;
    if (same || src->depth || dst->depth)
    {
        if (!same || src->block)
            return; /* a depth stretch: not done */
        if (src->depth && (s[0] || s[1] || t[0] || t[1] || (uint32_t)s[2] != sw || (uint32_t)s[3] != sh || sw != dw || sh != dh))
            return; /* depth copies are whole levels */
        WGPUTexelCopyTextureInfo a = { src->tex, slevel, { (uint32_t)s[0], (uint32_t)s[1], src->type == G9_TEX_CUBE ? sface : 0 }, WGPUTextureAspect_All };
        WGPUTexelCopyTextureInfo b = { dst->tex, dlevel, { (uint32_t)t[0], (uint32_t)t[1], dst->type == G9_TEX_CUBE ? dface : 0 }, WGPUTextureAspect_All };
        WGPUExtent3D size = { (uint32_t)(s[2] - s[0]), (uint32_t)(s[3] - s[1]), 1 };
        wgpuCommandEncoderCopyTextureToTexture(enc(), &a, &b, &size);
        src->used = dst->used = g_serial;
        dst->writes++;
        return;
    }
    WGPURenderPipeline pipe = copy_pipeline(dst->format);
    if (!pipe || src->unfilterable)
        return;
    WGPUTextureViewDescriptor vd = { 0 };
    vd.label = NOLABEL;
    vd.format = src->format;
    vd.dimension = WGPUTextureViewDimension_2D;
    vd.baseMipLevel = slevel, vd.mipLevelCount = 1;
    vd.baseArrayLayer = src->type == G9_TEX_CUBE ? sface : 0, vd.arrayLayerCount = 1;
    vd.aspect = WGPUTextureAspect_All;
    vd.usage = WGPUTextureUsage_TextureBinding;
    WGPUTextureView view = wgpuTextureCreateView(src->tex, &vd);
    uint64_t off;
    void* cpu;
    Chunk* c = ring(32, 256, &off, &cpu);
    /* uv across the viewport: the source rectangle (the triangle spans twice the viewport); X8
     * sources read alpha 1 (their views' swizzle is not on this one) */
    int x8 = src->fmt == D_X8R8G8B8 || src->fmt == D_X8B8G8R8;
    float u[8] = { (float)s[0] / sw, (float)s[1] / sh, (float)(s[2] - s[0]) / sw, (float)(s[3] - s[1]) / sh, x8 ? 1.0f : 0.0f, 0, 0, 0 };
    memcpy(cpu, u, sizeof u);
    WGPUBindGroup g = copy_group(c, off, view, linear ? g_linear : g_point);
    WGPURenderPassColorAttachment ca = { 0 };
    ca.view = target_view(dst, dface, dlevel);
    ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    ca.loadOp = WGPULoadOp_Load;
    ca.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor p = { 0 };
    p.label = NOLABEL;
    p.colorAttachmentCount = 1;
    p.colorAttachments = &ca;
    WGPURenderPassEncoder e = wgpuCommandEncoderBeginRenderPass(enc(), &p);
    wgpuRenderPassEncoderSetPipeline(e, pipe);
    wgpuRenderPassEncoderSetBindGroup(e, 0, g, 0, NULL);
    wgpuRenderPassEncoderSetViewport(e, (float)t[0], (float)t[1], (float)(t[2] - t[0]), (float)(t[3] - t[1]), 0, 1);
    wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(e);
    wgpuRenderPassEncoderRelease(e);
    wgpuBindGroupRelease(g);
    wgpuTextureViewRelease(view);
    src->used = dst->used = g_serial;
    dst->writes++;
}

/* --- present -------------------------------------------------------------------------------------------------- */

static char g_snap_path[1024];
static volatile int g_snap_pending;
void g9_snap_next(const char* path)
{
    snprintf(g_snap_path, sizeof g_snap_path, "%s", path);
    g_snap_pending = 1;
}

static uint32_t g_snaptex_id;
static char g_snaptex_path[1024];
void g9_snap_tex(uint32_t id, const char* path)
{
    snprintf(g_snaptex_path, sizeof g_snaptex_path, "%s", path);
    g_snaptex_id = id;
}

/* BGRA8 pixels to a PNG (alpha ignored: X8R8G8B8 back buffers carry garbage there) */
static void write_png(const uint8_t* px, uint32_t w, uint32_t h, const char* path)
{
    SDL_Surface* s = SDL_CreateSurfaceFrom((int)w, (int)h, SDL_PIXELFORMAT_BGRX32, (void*)px, (int)w * 4);
    if (!s)
        return;
    if (SDL_SavePNG(s, path))
        rt_log("[gfx9] frame %llu saved to %s\n", (unsigned long long)g_frame, path);
    else
        rt_log("[gfx9] cannot save %s: %s\n", path, SDL_GetError());
    SDL_DestroySurface(s);
}

static void save_png(G9Tex* bb, const char* path)
{
    if (bb->format != WGPUTextureFormat_BGRA8Unorm && bb->format != WGPUTextureFormat_RGBA8Unorm)
        return;
    uint8_t* px = (uint8_t*)malloc((size_t)bb->w * bb->h * 4);
    g9_tex_read(bb, 0, 0, px, bb->w * 4);
    if (bb->format == WGPUTextureFormat_RGBA8Unorm)
        for (size_t i = 0; i < (size_t)bb->w * bb->h; ++i)
        {
            uint8_t r = px[4 * i];
            px[4 * i] = px[4 * i + 2], px[4 * i + 2] = r;
        }
    write_png(px, bb->w, bb->h, path);
    free(px);
}

static float half_to_float(uint16_t h)
{
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 31, m = h & 1023, u;
    if (!e)
        return (s ? -1.0f : 1.0f) * ldexpf((float)m, -24);
    u = e == 31 ? s | 0x7F800000u | m << 13 : s | (e + 112) << 23 | m << 13;
    float f;
    memcpy(&f, &u, 4);
    return f;
}

/* debugging: a 2D texture level 0 as a PNG, float formats scaled per channel from their range */
static void save_png_any(G9Tex* t, const char* path)
{
    int ch = 0, f32 = 0;
    switch (t->format)
    {
    case WGPUTextureFormat_R32Float: ch = 1, f32 = 1; break;
    case WGPUTextureFormat_RG32Float: ch = 2, f32 = 1; break;
    case WGPUTextureFormat_RGBA32Float: ch = 4, f32 = 1; break;
    case WGPUTextureFormat_R16Float: ch = 1; break;
    case WGPUTextureFormat_RG16Float: ch = 2; break;
    case WGPUTextureFormat_RGBA16Float: ch = 4; break;
    case WGPUTextureFormat_BGRA8Unorm: case WGPUTextureFormat_RGBA8Unorm: save_png(t, path); return;
    default: rt_log("[gfx9] snaptex: format %u not saved\n", (unsigned)t->format); return;
    }
    uint32_t w = t->w, h = t->h;
    uint8_t* raw = (uint8_t*)malloc((size_t)w * h * t->bpp);
    g9_tex_read(t, 0, 0, raw, w * t->bpp);
    uint8_t* px = (uint8_t*)calloc((size_t)w * h, 4);
    float lo[4] = { 1e30f, 1e30f, 1e30f, 1e30f }, hi[4] = { -1e30f, -1e30f, -1e30f, -1e30f };
    size_t n = (size_t)w * h;
    for (int pass = 0; pass < 2; ++pass)
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < ch; ++c)
            {
                float v = f32 ? ((float*)raw)[i * ch + c] : half_to_float(((uint16_t*)raw)[i * ch + c]);
                if (pass == 0 && v == v)
                    lo[c] = v < lo[c] ? v : lo[c], hi[c] = v > hi[c] ? v : hi[c];
                if (pass == 1)
                {
                    uint8_t b = (uint8_t)((hi[c] > lo[c] ? (v - lo[c]) / (hi[c] - lo[c]) : 0) * 255.0f);
                    if (ch == 1)
                        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = b;
                    else if (c < 3)
                        px[i * 4 + (2 - c)] = b;
                }
            }
    rt_log("[gfx9] snaptex ranges r %g..%g g %g..%g b %g..%g a %g..%g\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], lo[3], hi[3]);
    write_png(px, w, h, path);
    free(raw);
    free(px);
}

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
    if (g_snap_pending)
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
    if (!dir || g_frame % every)
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%06llu.png", dir, (unsigned long long)g_frame);
    save_png(bb, path);
}

static void configure_surface(uint32_t w, uint32_t h)
{
    if (!g_surface || !w || !h)
        return;
    WGPUSurfaceConfiguration c = { 0 };
    c.device = g_dev;
    c.format = g_surface_fmt;
    c.usage = WGPUTextureUsage_RenderAttachment;
    c.width = w, c.height = h;
    c.alphaMode = WGPUCompositeAlphaMode_Auto;
    c.presentMode = g_present_mode;
    /* three frames in flight, as the Metal back end has (wgpu's default of two held the world at
     * exactly half the refresh rate whenever a frame ran past one refresh) */
    WGPUSurfaceConfigurationExtras ex = { 0 };
    ex.chain.sType = (WGPUSType)WGPUSType_SurfaceConfigurationExtras;
    ex.desiredMaximumFrameLatency = getenv("XIVLEGACY_WGPU_LATENCY") ? (uint32_t)atoi(getenv("XIVLEGACY_WGPU_LATENCY")) : 3;
    c.nextInChain = &ex.chain;
    wgpuSurfaceConfigure(g_surface, &c);
    g_surface_w = w, g_surface_h = h;
}

static void present_frame(G9Tex* bb);

/* XIVLEGACY_WGPU_STATS: how long the GPU takes from a frame's submit to its completion */
static _Atomic uint64_t g_gpu_ns, g_gpu_n;
static void on_work_done(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* u1, void* u2)
{
    (void)status, (void)message, (void)u2;
    uint64_t t0 = (uint64_t)(uintptr_t)u1;
    atomic_fetch_add(&g_gpu_ns, rt_monotonic_ns() - t0);
    atomic_fetch_add(&g_gpu_n, 1);
}

/* The frame goes to wgpu with the guest lock given up: wgpu records the frame's commands at the submit
 * (its encoding is a large part of a frame) and waits for a drawable, and none of it touches guest
 * memory, so the game's other threads run meanwhile. */
void g9_present(G9Tex* bb)
{
    static int unlock = -1;
    if (unlock < 0)
        unlock = !getenv("XIVLEGACY_WGPU_KEEPLOCK");
    int held = unlock && gt_holds();
    if (held)
        gt_unlock();
    present_frame(bb);
    if (held)
        gt_lock();
}

static void present_frame(G9Tex* bb)
{
    if (g_trace)
    {
        char a[64];
        TRACE("[trace] present %s, %u submits\n", tname(bb, a, sizeof a), g_submits);
        g_trace--;
    }
    if (!g_dev)
        return;
    g_frame++;
    g_submits = 0;
    flush_clear();
    end_pass();
    if (bb && bb->tex)
        snap(bb);
    if (g_surface && bb && bb->view)
    {
        /* the frame's commands go first, so the GPU works on them while the drawable is waited for */
        submit(0);
        int pw = 0, ph = 0;
        if (g_window)
            SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        if (pw > 0 && ph > 0 && ((uint32_t)pw != g_surface_w || (uint32_t)ph != g_surface_h))
            configure_surface((uint32_t)pw, (uint32_t)ph);
        WGPUSurfaceTexture st = { 0 };
        wgpuSurfaceGetCurrentTexture(g_surface, &st);
        if (st.texture && (st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal || st.status == WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal))
        {
            WGPUTextureView v = wgpuTextureCreateView(st.texture, NULL);
            uint64_t off;
            void* cpu;
            Chunk* c = ring(32, 256, &off, &cpu);
            float u[8] = { 0, 0, 1, 1, 1, 0, 0, 0 };
            memcpy(cpu, u, sizeof u);
            WGPUBindGroup g = copy_group(c, off, bb->view, g_linear);
            WGPURenderPassColorAttachment ca = { 0 };
            ca.view = v;
            ca.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            ca.loadOp = WGPULoadOp_Clear;
            ca.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor p = { 0 };
            p.label = NOLABEL;
            p.colorAttachmentCount = 1;
            p.colorAttachments = &ca;
            WGPURenderPassEncoder e = wgpuCommandEncoderBeginRenderPass(enc(), &p);
            wgpuRenderPassEncoderSetPipeline(e, g_present_pipe);
            wgpuRenderPassEncoderSetBindGroup(e, 0, g, 0, NULL);
            wgpuRenderPassEncoderDraw(e, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(e);
            wgpuRenderPassEncoderRelease(e);
            wgpuBindGroupRelease(g);
            bb->used = g_serial;
            submit(0);
            static int stats = -1;
            if (stats < 0)
                stats = getenv("XIVLEGACY_WGPU_STATS") != NULL;
            if (stats)
            {
                WGPUQueueWorkDoneCallbackInfo wd = { 0 };
                wd.mode = WGPUCallbackMode_AllowSpontaneous;
                wd.callback = on_work_done;
                wd.userdata1 = (void*)(uintptr_t)rt_monotonic_ns();
                wgpuQueueOnSubmittedWorkDone(g_queue, wd);
            }
            wgpuSurfacePresent(g_surface);
            wgpuTextureViewRelease(v);
        }
        else
        {
            submit(0);
            if (st.status == WGPUSurfaceGetCurrentTextureStatus_Outdated || st.status == WGPUSurfaceGetCurrentTextureStatus_Lost)
                configure_surface(g_surface_w, g_surface_h);
        }
        if (st.texture)
            wgpuTextureRelease(st.texture);
    }
    else
        submit(0);
    wgpuDevicePoll(g_dev, 0, NULL);
    static int stats = -1;
    if (stats < 0)
        stats = getenv("XIVLEGACY_WGPU_STATS") != NULL;
    if (stats && g_frame % 120 == 0)
    {
        uint64_t gn = atomic_exchange(&g_gpu_n, 0), gns = atomic_exchange(&g_gpu_ns, 0);
        rt_log("[gfx9] wgpu a frame: %.1f submits (%.1f for uploads), %.1f passes, %.2f target copies, %.0f KB ring, submit to done %.1f ms\n",
            g_st_submits / 120.0, g_st_uploads_submit / 120.0, g_st_passes / 120.0, g_st_shadows / 120.0, g_st_ring / 120.0 / 1024.0,
            gn ? gns / 1e6 / gn : 0.0);
        g_st_submits = g_st_passes = g_st_shadows = g_st_ring = g_st_uploads_submit = 0;
    }
    if (g_frame == 1 || g_frame % 1024 == 0)
        rt_log("[gfx9] frame %llu: %llu draws drawn, %llu skipped so far\n", (unsigned long long)g_frame, (unsigned long long)g_draws,
            (unsigned long long)g_skipped);
}

/* --- init ---------------------------------------------------------------------------------------------------- */

static void on_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void* u1, void* u2)
{
    (void)u2;
    if (status == WGPURequestAdapterStatus_Success)
        *(WGPUAdapter*)u1 = adapter;
    else
        rt_log("[gfx9] no wgpu adapter: %.*s\n", message.data ? (int)(message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0,
            message.data ? message.data : "");
}

static void on_device(WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void* u1, void* u2)
{
    (void)u2;
    if (status == WGPURequestDeviceStatus_Success)
        *(WGPUDevice*)u1 = device;
    else
        rt_log("[gfx9] no wgpu device: %.*s\n", message.data ? (int)(message.length == WGPU_STRLEN ? strlen(message.data) : message.length) : 0,
            message.data ? message.data : "");
}

#if defined(__APPLE__)
static void* g_layer;
static void make_metal_view(void* arg)
{
    SDL_MetalView v = SDL_Metal_CreateView((SDL_Window*)arg);
    g_layer = v ? SDL_Metal_GetLayer(v) : NULL;
}
#endif

/* the window's surface, from SDL's native handles: its CAMetalLayer on macOS, the HWND on Windows, the
 * X11 window or Wayland surface on Linux */
static WGPUSurface make_surface(SDL_Window* w)
{
    WGPUSurfaceDescriptor d = { 0 };
    d.label = NOLABEL;
#if defined(__APPLE__)
    /* the view is the main thread's to make: let it run (and pump messages) meanwhile */
    if (SDL_IsMainThread())
        make_metal_view(w);
    else
    {
        int held = gt_holds();
        if (held)
            gt_unlock();
        SDL_RunOnMainThread(make_metal_view, w, true);
        if (held)
            gt_lock();
    }
    if (!g_layer)
    {
        rt_log("[gfx9] no Metal layer for the window: %s\n", SDL_GetError());
        return NULL;
    }
    WGPUSurfaceSourceMetalLayer src = { 0 };
    src.chain.sType = WGPUSType_SurfaceSourceMetalLayer;
    src.layer = g_layer;
    d.nextInChain = &src.chain;
    return wgpuInstanceCreateSurface(g_inst, &d);
#elif defined(_WIN32)
    SDL_PropertiesID p = SDL_GetWindowProperties(w);
    WGPUSurfaceSourceWindowsHWND src = { 0 };
    src.chain.sType = WGPUSType_SurfaceSourceWindowsHWND;
    src.hwnd = SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    src.hinstance = SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WIN32_INSTANCE_POINTER, NULL);
    if (!src.hwnd)
    {
        rt_log("[gfx9] no HWND for the window: %s\n", SDL_GetError());
        return NULL;
    }
    d.nextInChain = &src.chain;
    return wgpuInstanceCreateSurface(g_inst, &d);
#else
    SDL_PropertiesID p = SDL_GetWindowProperties(w);
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && !strcmp(driver, "wayland"))
    {
        WGPUSurfaceSourceWaylandSurface src = { 0 };
        src.chain.sType = WGPUSType_SurfaceSourceWaylandSurface;
        src.display = SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        src.surface = SDL_GetPointerProperty(p, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
        if (!src.display || !src.surface)
            return NULL;
        d.nextInChain = &src.chain;
        return wgpuInstanceCreateSurface(g_inst, &d);
    }
    if (driver && !strcmp(driver, "x11"))
    {
        WGPUSurfaceSourceXlibWindow src = { 0 };
        src.chain.sType = WGPUSType_SurfaceSourceXlibWindow;
        src.display = SDL_GetPointerProperty(p, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        src.window = (uint64_t)SDL_GetNumberProperty(p, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (!src.display || !src.window)
            return NULL;
        d.nextInChain = &src.chain;
        return wgpuInstanceCreateSurface(g_inst, &d);
    }
    rt_log("[gfx9] wgpu: no surface for the video driver %s\n", driver ? driver : "(none)");
    return NULL;
#endif
}

static int want_feature(WGPUFeatureName f, WGPUFeatureName* list, size_t* n)
{
    if (!wgpuAdapterHasFeature(g_adapter, f))
        return 0;
    list[(*n)++] = f;
    return 1;
}

int g9_init(void* window, int vsync)
{
    g_vsync = vsync;
    if (g_dev)
        return 1;
    if (getenv("XIVLEGACY_NOGFX"))
        return 0;
    g_inst = wgpuCreateInstance(NULL);
    if (!g_inst)
    {
        rt_log("[gfx9] no wgpu instance: drawing nothing\n");
        return 0;
    }
    g_window = (SDL_Window*)window;
    if (g_window && !getenv("XIVLEGACY_WGPU_OFFSCREEN")) /* debugging: draw without presenting */
        g_surface = make_surface(g_window);
    WGPURequestAdapterOptions ao = { 0 };
    ao.powerPreference = WGPUPowerPreference_HighPerformance;
    /* XIVLEGACY_WGPU_BACKEND=dx12|vulkan|metal|gl: which API wgpu drives (default: its own pick) */
    {
        const char* b = getenv("XIVLEGACY_WGPU_BACKEND");
        if (b && !strcmp(b, "dx12"))
            ao.backendType = WGPUBackendType_D3D12;
        else if (b && !strcmp(b, "vulkan"))
            ao.backendType = WGPUBackendType_Vulkan;
        else if (b && !strcmp(b, "metal"))
            ao.backendType = WGPUBackendType_Metal;
        else if (b && !strcmp(b, "gl"))
            ao.backendType = WGPUBackendType_OpenGL;
    }
    ao.compatibleSurface = g_surface;
    WGPURequestAdapterCallbackInfo acb = { 0 };
    acb.mode = WGPUCallbackMode_AllowProcessEvents;
    acb.callback = on_adapter;
    acb.userdata1 = &g_adapter;
    wgpuInstanceRequestAdapter(g_inst, &ao, acb);
    for (int i = 0; i < 1000 && !g_adapter; ++i)
        wgpuInstanceProcessEvents(g_inst);
    if (!g_adapter)
        return 0;
    WGPUFeatureName feats[16];
    size_t nf = 0;
    g_has_bc = want_feature(WGPUFeatureName_TextureCompressionBC, feats, &nf);
    g_has_swizzle = want_feature(WGPUFeatureName_TextureComponentSwizzle, feats, &nf);
    g_has_d32s8 = want_feature(WGPUFeatureName_Depth32FloatStencil8, feats, &nf);
    g_has_f32filter = want_feature(WGPUFeatureName_Float32Filterable, feats, &nf);
    g_has_unorm16 = want_feature((WGPUFeatureName)WGPUNativeFeature_TextureFormat16bitNorm, feats, &nf);
    g_has_line = want_feature((WGPUFeatureName)WGPUNativeFeature_PolygonModeLine, feats, &nf);
    g_has_border = want_feature((WGPUFeatureName)WGPUNativeFeature_AddressModeClampToBorder, feats, &nf);
    WGPULimits limits = { 0 };
    wgpuAdapterGetLimits(g_adapter, &limits);
    WGPUDeviceDescriptor dd = { 0 };
    dd.label = NOLABEL;
    dd.requiredFeatureCount = nf;
    dd.requiredFeatures = feats;
    dd.requiredLimits = &limits;
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    WGPURequestDeviceCallbackInfo dcb = { 0 };
    dcb.mode = WGPUCallbackMode_AllowProcessEvents;
    dcb.callback = on_device;
    dcb.userdata1 = &g_dev;
    wgpuAdapterRequestDevice(g_adapter, &dd, dcb);
    for (int i = 0; i < 1000 && !g_dev; ++i)
        wgpuInstanceProcessEvents(g_inst);
    if (!g_dev)
        return 0;
    g_queue = wgpuDeviceGetQueue(g_dev);
    make_bgl0();
    g_util_module = shader_module(UTIL_WGSL);
    g_copy_module = shader_module(COPY_WGSL);
    WGPUPipelineLayoutDescriptor pl = { 0 };
    pl.label = NOLABEL;
    pl.bindGroupLayoutCount = 1;
    pl.bindGroupLayouts = &g_bgl_util;
    g_util_layout = wgpuDeviceCreatePipelineLayout(g_dev, &pl);
    {
        WGPUBindGroupLayoutEntry e[3];
        memset(e, 0, sizeof e);
        e[0].binding = 0, e[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        e[0].buffer.type = WGPUBufferBindingType_Uniform, e[0].buffer.minBindingSize = 32;
        e[1].binding = 1, e[1].visibility = WGPUShaderStage_Fragment;
        e[1].texture.sampleType = WGPUTextureSampleType_Float, e[1].texture.viewDimension = WGPUTextureViewDimension_2D;
        e[2].binding = 2, e[2].visibility = WGPUShaderStage_Fragment;
        e[2].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor d = { 0 };
        d.label = NOLABEL;
        d.entryCount = 3;
        d.entries = e;
        g_copy_bgl = wgpuDeviceCreateBindGroupLayout(g_dev, &d);
        pl.bindGroupLayouts = &g_copy_bgl;
        g_copy_layout = wgpuDeviceCreatePipelineLayout(g_dev, &pl);
    }
    WGPUSamplerDescriptor sd = { 0 };
    sd.label = NOLABEL;
    sd.addressModeU = sd.addressModeV = sd.addressModeW = WGPUAddressMode_ClampToEdge;
    sd.magFilter = sd.minFilter = WGPUFilterMode_Linear;
    sd.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sd.lodMaxClamp = 32;
    sd.maxAnisotropy = 1;
    g_linear = wgpuDeviceCreateSampler(g_dev, &sd);
    sd.magFilter = sd.minFilter = WGPUFilterMode_Nearest;
    g_point = wgpuDeviceCreateSampler(g_dev, &sd);
    if (g_surface)
    {
        WGPUSurfaceCapabilities caps = { 0 };
        if (wgpuSurfaceGetCapabilities(g_surface, g_adapter, &caps) == WGPUStatus_Success)
        {
            int bgra = 0, immediate = 0, mailbox = 0;
            for (size_t i = 0; i < caps.formatCount; ++i)
                bgra |= caps.formats[i] == WGPUTextureFormat_BGRA8Unorm;
            for (size_t i = 0; i < caps.presentModeCount; ++i)
                immediate |= caps.presentModes[i] == WGPUPresentMode_Immediate, mailbox |= caps.presentModes[i] == WGPUPresentMode_Mailbox;
            if (!bgra && caps.formatCount)
                g_surface_fmt = caps.formats[0];
            const char* pm = getenv("XIVLEGACY_WGPU_PRESENT"); /* fifo, mailbox, immediate: over the game's choice */
            if (pm)
                vsync = !strcmp(pm, "fifo") ? 1 : 0, immediate &= strcmp(pm, "mailbox") != 0;
            g_present_mode = vsync ? WGPUPresentMode_Fifo : immediate ? WGPUPresentMode_Immediate : mailbox ? WGPUPresentMode_Mailbox : WGPUPresentMode_Fifo;
            wgpuSurfaceCapabilitiesFreeMembers(caps);
        }
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(g_window, &pw, &ph);
        configure_surface(pw > 0 ? (uint32_t)pw : 1, ph > 0 ? (uint32_t)ph : 1);
    }
    g_present_pipe = copy_pipeline(g_surface_fmt);
    WGPUAdapterInfo info = { 0 };
    wgpuAdapterGetInfo(g_adapter, &info);
    static const char* const backends[] = { "?", "null", "WebGPU", "D3D11", "D3D12", "Metal", "Vulkan", "OpenGL", "OpenGL ES" };
    rt_log("[gfx9] wgpu (%s, present mode %d) on %.*s%s%s%s%s%s\n", (unsigned)info.backendType < 9 ? backends[info.backendType] : "?", (int)g_present_mode,
        info.device.data ? (int)(info.device.length == WGPU_STRLEN ? strlen(info.device.data) : info.device.length) : 0,
        info.device.data ? info.device.data : "", g_surface ? "" : ", offscreen", g_has_bc ? "" : ", no BC (decoded on upload)",
        g_has_swizzle ? "" : ", no swizzles", g_has_f32filter ? "" : ", float32 unfilterable", g_has_d32s8 ? "" : ", no D32S8");
    wgpuAdapterInfoFreeMembers(info);
    return 1;
}
