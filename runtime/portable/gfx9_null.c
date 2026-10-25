/* A back end that draws nothing (gfx9.h): the game runs headless, as with XIVLEGACY_NOGFX on the
 * Metal back end. For hosts whose graphics back end isn't built yet and for tests: the game still
 * signs in, enters the world and runs its logic. */
#include <stdlib.h>
#include <string.h>

#include "gfx9.h"

struct G9Tex
{
    int unused;
};
struct G9Shader
{
    int unused;
};

int g9_init(void* sdl_window, int vsync)
{
    (void)sdl_window, (void)vsync;
    return 0;
}
int g9_active(void) { return 0; }

G9Tex* g9_tex_create(int type, uint32_t fmt, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, int use)
{
    (void)type, (void)fmt, (void)w, (void)h, (void)d, (void)levels, (void)use;
    return (G9Tex*)calloc(1, sizeof(G9Tex));
}
void g9_tex_destroy(G9Tex* t) { free(t); }
void g9_tex_upload(G9Tex* t, uint32_t face, uint32_t level, const void* src, uint32_t pitch, uint32_t slice)
{
    (void)t, (void)face, (void)level, (void)src, (void)pitch, (void)slice;
}
void g9_tex_read(G9Tex* t, uint32_t face, uint32_t level, void* dst, uint32_t pitch)
{
    (void)t, (void)face, (void)level, (void)dst, (void)pitch;
}
void g9_stretch(G9Tex* src, uint32_t sface, uint32_t slevel, const int32_t* srect, G9Tex* dst, uint32_t dface,
    uint32_t dlevel, const int32_t* drect, int linear)
{
    (void)src, (void)sface, (void)slevel, (void)srect, (void)dst, (void)dface, (void)dlevel, (void)drect, (void)linear;
}

G9Shader* g9_shader_create(const uint32_t* tokens, size_t n)
{
    (void)tokens, (void)n;
    return (G9Shader*)calloc(1, sizeof(G9Shader));
}
void g9_shader_destroy(G9Shader* s) { free(s); }

void g9_set_targets(const G9Target rt[4], G9Tex* depth) { (void)rt, (void)depth; }
void g9_clear(uint32_t nrects, const int32_t* rects, uint32_t flags, uint32_t color, float z, uint32_t stencil,
    const uint32_t viewport[6], const int32_t* scissor)
{
    (void)nrects, (void)rects, (void)flags, (void)color, (void)z, (void)stencil, (void)viewport, (void)scissor;
}
void g9_draw(const G9Draw* d) { (void)d; }
void g9_frame_costs(uint32_t* compiles, uint64_t* compile_ns, uint32_t* pipes, uint64_t* pipe_ns, uint32_t* uploads,
    uint64_t* upload_bytes, uint64_t* upload_ns)
{
    *compiles = 0, *compile_ns = 0, *pipes = 0, *pipe_ns = 0, *uploads = 0, *upload_bytes = 0, *upload_ns = 0;
}
void g9_present(G9Tex* backbuffer) { (void)backbuffer; }

void g9_debug(int mode) { (void)mode; }
void g9_trace_next(void) {}
void g9_skip_vs(uint32_t id) { (void)id; }
void g9_dump_vs(uint32_t id, uint32_t count) { (void)id, (void)count; }
void g9_cpu_check_vs(uint32_t id) { (void)id; }
void g9_only_vs(uint32_t id) { (void)id; }
void g9_snap_tex(uint32_t id, const char* path) { (void)id, (void)path; }
void g9_snap_next(const char* path) { (void)path; }
