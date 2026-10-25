/* The graphics back end under the Direct3D 9 front end (d3d9.c): Metal on macOS (gfx9_metal.m).
 *
 * d3d9.c keeps every D3D9 semantic - objects, state, state blocks, what a lock returns - and hands
 * this layer resolved work: textures to create and fill from guest memory, the render targets,
 * clears, copies, and draws. A draw carries the shader pair, the vertex declaration, the vertex and
 * index bytes it touches (host pointers into guest memory, already offset to the first vertex
 * used), the constants, the bound textures with their sampler states, and the render states; the
 * back end turns those into pipelines, depth-stencil and sampler objects and caches them.
 *
 * Formats are D3DFORMAT values, primitive types D3DPRIMITIVETYPE, render and sampler states the
 * D3DRS_ / D3DSAMP_ arrays as d3d9.c keeps them. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct G9Tex G9Tex;
typedef struct G9Shader G9Shader;

enum { G9_TEX_2D, G9_TEX_CUBE, G9_TEX_3D };
enum { G9_USE_SAMPLE, G9_USE_RT, G9_USE_DEPTH };

/* 1 when the back end draws (a Metal device and the window's layer); 0 headless: the front end
 * carries on with every call accepted and nothing drawn. */
int g9_init(void* sdl_window, int vsync);
int g9_active(void);

G9Tex* g9_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int use);
void g9_tex_destroy(G9Tex* t);
/* A level (of a cube's face) from memory in D3D's layout: pitch bytes per row (of 4x4 blocks when
 * compressed), slice bytes per depth slice. */
void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch, uint32_t slice);
/* A level back into memory (GetRenderTargetData, a lock of a render target): waits for the GPU. */
void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch);
/* StretchRect: rectangles {left, top, right, bottom}; NULL is the whole level. */
void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t* srect, G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t* drect, int linear);

/* A shader from its bytecode; NULL when it cannot be parsed. Translation happens per pair, at the
 * first draw that uses it. */
G9Shader* g9_shader_create(const uint32_t* tokens, size_t n);
void g9_shader_destroy(G9Shader* s);

/* the render targets draws and clears go to; rt[0] NULL (or a D3DFMT_NULL surface) draws depth only */
typedef struct G9Target
{
    G9Tex* tex;
    uint32_t face, level;
} G9Target;
void g9_set_targets(const G9Target rt[4], G9Tex* depth);

/* D3DCLEAR_TARGET 1, ZBUFFER 2, STENCIL 4; rects (left, top, right, bottom) or the viewport, cut to the
 * scissor rectangle when scissor test is on (scissor NULL: off) */
void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t viewport[6], const int32_t* scissor);

/* a vertex declaration element (D3DVERTEXELEMENT9 without Method) */
typedef struct G9Elem
{
    uint8_t stream, type, usage, index;
    uint16_t offset;
} G9Elem;

typedef struct G9Stream
{
    const uint8_t* data; /* the first vertex the draw uses (or instance 0) */
    uint32_t size, stride;
    uint32_t instanced;  /* steps per instance (D3DSTREAMSOURCE_INSTANCEDATA) */
} G9Stream;

typedef struct G9Draw
{
    G9Shader *vs, *ps;
    G9Elem elems[16];
    uint32_t nelems;
    G9Stream streams[16];
    uint32_t prim, prims;        /* D3DPRIMITIVETYPE and the primitive count */
    const void* indices;         /* NULL: a non-indexed draw from the streams' first vertex */
    uint32_t index_size;         /* 2 or 4 */
    int32_t base_vertex;         /* added to each index, relative to the streams' first vertex */
    uint32_t instances;          /* 1 unless instanced */
    const float* vsc;            /* float4 c[256] */
    const int32_t* vsi;          /* int4 i[16] */
    const uint32_t* vsb;         /* b[16] */
    const float* psc;            /* float4 c[224] */
    const int32_t* psi;
    const uint32_t* psb;
    G9Tex* tex[20];              /* s0..s15, then the vertex samplers */
    const uint32_t (*ss)[14];    /* sampler states, same indexing */
    const uint32_t* rs;          /* render states [256] */
    uint32_t viewport[6];        /* x, y, w, h, minz, maxz (floats) */
    int32_t scissor[4];
} G9Draw;
void g9_draw(const G9Draw* d);

/* the costs since the last call (the hitch log): shader pairs compiled, pipelines built, texture levels
 * uploaded, and the time each took */
void g9_frame_costs(uint32_t* compiles, uint64_t* compile_ns, uint32_t* pipes, uint64_t* pipe_ns, uint32_t* uploads, uint64_t* upload_bytes,
    uint64_t* upload_ns);

/* the frame: the back buffer to the window */
void g9_present(G9Tex* backbuffer);
/* debugging: 0 normal, 1 wireframe, 2 mid-gray in place of sampled 2D textures */
void g9_debug(int mode);
/* debugging: the next whole frame's targets, clears, copies and draws to stdout */
void g9_trace_next(void);
/* debugging: skip the draws that use this vertex shader id (0: none) */
void g9_skip_vs(uint32_t id);
/* debugging: describe the next `count` draws that use this vertex shader id (inputs, streams, bools) */
void g9_dump_vs(uint32_t id, uint32_t count);
/* debugging: check the next indexed draw with this vertex shader id against the CPU interpreter */
void g9_cpu_check_vs(uint32_t id);
/* debugging: in full-size color passes, draw only this vertex shader id (0: all) */
void g9_only_vs(uint32_t id);
/* debugging: at the next present, a texture (by the id a trace names) saved as a PNG */
void g9_snap_tex(uint32_t id, const char* path);
/* the next presented frame saved as a PNG at path (debugging) */
void g9_snap_next(const char* path);
