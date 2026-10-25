/* Translates Direct3D 9 shader pairs to MSL and has Metal compile them and build a pipeline
 * (which also checks that the fragment inputs link to the vertex outputs).
 *
 *   msl_check <dump folder> [-v]
 *
 * The folder is what XIVLEGACY_DUMP_SHADERS writes: vs_*.dxbc9 / ps_*.dxbc9 and pairs.txt (the
 * pairs the game drew with). Every pair in pairs.txt is checked, and every pixel shader with the
 * vertex shader that leaves the fewest of its inputs unwritten. -v prints the MSL of failures. */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>

#include "sm3.h"
#include "sm3_msl.h"

typedef struct
{
    char name[64];
    uint32_t* tk;
    size_t n;
    Sm3Shader s;
    int ok;
} Shader;

static Shader g_sh[4096];
static int g_n;

static Shader* find(const char* name)
{
    for (int i = 0; i < g_n; ++i)
        if (!strcmp(g_sh[i].name, name))
            return &g_sh[i];
    return NULL;
}

static int unlinked(const char* msl)
{
    int n = 0;
    for (const char* p = msl; (p = strstr(p, "not written by the vertex shader")); ++p)
        n++;
    return n;
}

static int check(id<MTLDevice> dev, Shader* vs, Shader* ps, int verbose)
{
    char err[256];
    char* msl = sm3_msl_pair(&vs->s, &ps->s, NULL, err, sizeof err);
    if (!msl)
    {
        printf("TRANSLATE %s + %s: %s\n", vs->name, ps->name, err);
        return 0;
    }
    NSError* e = nil;
    MTLCompileOptions* o = [MTLCompileOptions new];
    id<MTLLibrary> lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:msl] options:o error:&e];
    if (!lib)
    {
        printf("COMPILE %s + %s:\n%s\n", vs->name, ps->name, [[e localizedDescription] UTF8String]);
        if (verbose)
            printf("%s\n", msl);
        free(msl);
        return 0;
    }
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"vs_main"];
    d.fragmentFunction = [lib newFunctionWithName:@"fs_main"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    for (int k = 1; k < 4; ++k)
        d.colorAttachments[k].pixelFormat = MTLPixelFormatBGRA8Unorm;
    MTLVertexDescriptor* vd = [MTLVertexDescriptor vertexDescriptor];
    for (size_t i = 0; i < vs->s.nins; ++i)
        if (vs->s.ins[i].op == SM3_DCL && vs->s.ins[i].dst.type == SM3_INPUT)
        {
            unsigned r = vs->s.ins[i].dst.num;
            vd.attributes[r].format = MTLVertexFormatFloat4;
            vd.attributes[r].offset = 0;
            vd.attributes[r].bufferIndex = 0;
        }
    vd.layouts[0].stride = 16;
    d.vertexDescriptor = vd;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p)
    {
        printf("PIPELINE %s + %s: %s\n", vs->name, ps->name, [[e localizedDescription] UTF8String]);
        if (verbose)
            printf("%s\n", msl);
        free(msl);
        return 0;
    }
    free(msl);
    return 1;
}

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        if (argc < 2)
        {
            fprintf(stderr, "usage: msl_check <dump folder> [-v]\n");
            return 2;
        }
        int verbose = argc > 2 && !strcmp(argv[2], "-v");
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        DIR* dir = opendir(argv[1]);
        struct dirent* de;
        while (dir && (de = readdir(dir)) && g_n < 4096)
        {
            size_t l = strlen(de->d_name);
            if (l < 7 || strcmp(de->d_name + l - 6, ".dxbc9"))
                continue;
            Shader* s = &g_sh[g_n++];
            snprintf(s->name, sizeof s->name, "%.*s", (int)(l - 6), de->d_name);
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", argv[1], de->d_name);
            FILE* f = fopen(path, "rb");
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            fseek(f, 0, SEEK_SET);
            s->tk = malloc((size_t)size + 4);
            s->n = fread(s->tk, 4, (size_t)size / 4, f);
            fclose(f);
            char err[128];
            s->ok = sm3_parse(s->tk, s->n, &s->s, err, sizeof err);
        }
        if (dir)
            closedir(dir);
        int pairs = 0, good = 0;
        char path[2048];
        snprintf(path, sizeof path, "%s/pairs.txt", argv[1]);
        FILE* pf = fopen(path, "r");
        char a[64], b[64];
        while (pf && fscanf(pf, "%63s %63s", a, b) == 2)
        {
            Shader* vs = find(a);
            Shader* ps = find(b);
            if (!vs || !ps)
                continue;
            pairs++;
            good += check(dev, vs, ps, verbose);
        }
        if (pf)
            fclose(pf);
        printf("game pairs: %d, %d good\n", pairs, good);
        /* every pixel shader with its best vertex shader */
        int tried = 0, ok = 0;
        for (int i = 0; i < g_n; ++i)
        {
            if (!g_sh[i].ok || !g_sh[i].s.pixel)
                continue;
            Shader* best = NULL;
            int best_n = 1 << 30;
            for (int j = 0; j < g_n; ++j)
            {
                if (!g_sh[j].ok || g_sh[j].s.pixel)
                    continue;
                char err[64];
                char* msl = sm3_msl_pair(&g_sh[j].s, &g_sh[i].s, NULL, err, sizeof err);
                int u = msl ? unlinked(msl) : (1 << 29);
                free(msl);
                if (u < best_n)
                    best_n = u, best = &g_sh[j];
            }
            if (!best)
                continue;
            tried++;
            ok += check(dev, best, &g_sh[i], verbose);
        }
        printf("pixel shaders with their best vertex shader: %d, %d good\n", tried, ok);
        /* every vertex shader once, with the first pixel shader */
        int vt = 0, vok = 0;
        Shader* anyps = NULL;
        for (int i = 0; i < g_n && !anyps; ++i)
            if (g_sh[i].ok && g_sh[i].s.pixel && g_sh[i].s.major >= 3)
                anyps = &g_sh[i];
        for (int j = 0; j < g_n && anyps; ++j)
        {
            if (!g_sh[j].ok || g_sh[j].s.pixel)
                continue;
            vt++;
            vok += check(dev, &g_sh[j], anyps, verbose);
        }
        printf("vertex shaders: %d, %d good\n", vt, vok);
        return (good == pairs && ok == tried && vok == vt) ? 0 : 1;
    }
}
