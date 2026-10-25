/* Translates Direct3D 9 shader pairs to WGSL and has wgpu (naga, then the platform's compiler) build
 * a render pipeline from each, as tools/msl_check.m does for Metal.
 *
 *   wgsl_check <dump folder> [-v]
 *
 * The folder is what XIVLEGACY_DUMP_SHADERS writes: vs_*.dxbc9 / ps_*.dxbc9 and pairs.txt (the pairs
 * the game drew with). Every pair in pairs.txt is checked, then every vertex shader once with the
 * first pixel shader. -v prints the WGSL of failures.
 *
 *   cc -std=c11 -I runtime/portable -I /opt/homebrew/opt/wgpu-native/include tools/wgsl_check.c \
 *      runtime/portable/sm3.c runtime/portable/sm3_wgsl.c -L /opt/homebrew/opt/wgpu-native/lib -lwgpu_native \
 *      -Wl,-rpath,/opt/homebrew/opt/wgpu-native/lib -o build/wgsl_check */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webgpu.h>
#include <wgpu.h>

#include "sm3.h"
#include "sm3_wgsl.h"

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
static WGPUDevice g_dev;
static int g_errors;
static char g_last_error[8192];

static Shader* find(const char* name)
{
    for (int i = 0; i < g_n; ++i)
        if (!strcmp(g_sh[i].name, name))
            return &g_sh[i];
    return NULL;
}

static void on_error(WGPUDevice const* d, WGPUErrorType type, WGPUStringView m, void* u1, void* u2)
{
    (void)d, (void)type, (void)u1, (void)u2;
    g_errors++;
    snprintf(g_last_error, sizeof g_last_error, "%.*s", (int)(m.length == WGPU_STRLEN ? strlen(m.data) : m.length), m.data ? m.data : "");
}

static void on_adapter(WGPURequestAdapterStatus s, WGPUAdapter a, WGPUStringView m, void* u1, void* u2)
{
    (void)m, (void)u2;
    if (s == WGPURequestAdapterStatus_Success)
        *(WGPUAdapter*)u1 = a;
}

static void on_device(WGPURequestDeviceStatus s, WGPUDevice d, WGPUStringView m, void* u1, void* u2)
{
    (void)m, (void)u2;
    if (s == WGPURequestDeviceStatus_Success)
        *(WGPUDevice*)u1 = d;
}

static int check(Shader* vs, Shader* ps, int verbose)
{
    char err[256];
    char* src = sm3_wgsl_pair(&vs->s, &ps->s, NULL, 0, err, sizeof err);
    if (!src)
    {
        printf("TRANSLATE %s + %s: %s\n", vs->name, ps->name, err);
        return 0;
    }
    int before = g_errors;
    WGPUShaderSourceWGSL w = { 0 };
    w.chain.sType = WGPUSType_ShaderSourceWGSL;
    w.code = (WGPUStringView){ src, WGPU_STRLEN };
    WGPUShaderModuleDescriptor md = { 0 };
    md.nextInChain = &w.chain;
    WGPUShaderModule m = wgpuDeviceCreateShaderModule(g_dev, &md);
    WGPUVertexAttribute at[16];
    int na = 0;
    for (size_t i = 0; i < vs->s.nins; ++i)
        if (vs->s.ins[i].op == SM3_DCL && vs->s.ins[i].dst.type == SM3_INPUT && na < 16)
            at[na] = (WGPUVertexAttribute){ NULL, WGPUVertexFormat_Float32x4, 0, vs->s.ins[i].dst.num }, na++;
    WGPUVertexBufferLayout vb = { NULL, WGPUVertexStepMode_Vertex, 16, (size_t)na, at };
    WGPUColorTargetState ct[4];
    memset(ct, 0, sizeof ct);
    int colors = 1;
    for (size_t i = 0; i < ps->s.nins; ++i)
        if (ps->s.ins[i].has_dst && ps->s.ins[i].dst.type == SM3_COLOROUT && ps->s.ins[i].dst.num + 1 > (unsigned)colors)
            colors = ps->s.ins[i].dst.num + 1;
    for (int k = 0; k < colors; ++k)
        ct[k].format = WGPUTextureFormat_BGRA8Unorm, ct[k].writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fs = { 0 };
    fs.module = m;
    fs.entryPoint = (WGPUStringView){ "fs_main", WGPU_STRLEN };
    fs.targetCount = (size_t)colors;
    fs.targets = ct;
    WGPUDepthStencilState ds = { 0 };
    ds.format = WGPUTextureFormat_Depth32Float;
    ds.depthCompare = WGPUCompareFunction_LessEqual;
    ds.depthWriteEnabled = WGPUOptionalBool_True;
    ds.stencilFront = ds.stencilBack = (WGPUStencilFaceState){ WGPUCompareFunction_Always, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep, WGPUStencilOperation_Keep };
    WGPURenderPipelineDescriptor pd = { 0 };
    pd.vertex.module = m;
    pd.vertex.entryPoint = (WGPUStringView){ "vs_main", WGPU_STRLEN };
    pd.vertex.bufferCount = na ? 1 : 0;
    pd.vertex.buffers = &vb;
    pd.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    pd.primitive.frontFace = WGPUFrontFace_CW;
    pd.depthStencil = &ds;
    pd.multisample.count = 1;
    pd.multisample.mask = ~0u;
    pd.fragment = &fs;
    WGPURenderPipeline p = wgpuDeviceCreateRenderPipeline(g_dev, &pd);
    int ok = g_errors == before;
    if (!ok)
    {
        printf("WGPU %s + %s: %s\n", vs->name, ps->name, g_last_error);
        if (verbose)
            printf("%s\n", src);
    }
    if (p)
        wgpuRenderPipelineRelease(p);
    wgpuShaderModuleRelease(m);
    free(src);
    return ok;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: wgsl_check <dump folder> [-v]\n");
        return 2;
    }
    int verbose = argc > 2 && !strcmp(argv[2], "-v");
    WGPUInstance inst = wgpuCreateInstance(NULL);
    WGPUAdapter ad = NULL;
    WGPURequestAdapterOptions ao = { 0 };
    WGPURequestAdapterCallbackInfo acb = { 0 };
    acb.mode = WGPUCallbackMode_AllowProcessEvents;
    acb.callback = on_adapter;
    acb.userdata1 = &ad;
    wgpuInstanceRequestAdapter(inst, &ao, acb);
    for (int i = 0; i < 1000 && !ad; ++i)
        wgpuInstanceProcessEvents(inst);
    if (!ad)
        return fprintf(stderr, "no adapter\n"), 1;
    WGPUDeviceDescriptor dd = { 0 };
    dd.uncapturedErrorCallbackInfo.callback = on_error;
    WGPURequestDeviceCallbackInfo dcb = { 0 };
    dcb.mode = WGPUCallbackMode_AllowProcessEvents;
    dcb.callback = on_device;
    dcb.userdata1 = &g_dev;
    wgpuAdapterRequestDevice(ad, &dd, dcb);
    for (int i = 0; i < 1000 && !g_dev; ++i)
        wgpuInstanceProcessEvents(inst);
    if (!g_dev)
        return fprintf(stderr, "no device\n"), 1;

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
        if (!f)
            continue;
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
        if (!vs || !ps || !vs->ok || !ps->ok)
            continue;
        pairs++;
        good += check(vs, ps, verbose);
    }
    if (pf)
        fclose(pf);
    printf("game pairs: %d, %d good\n", pairs, good);
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
        vok += check(&g_sh[j], anyps, verbose);
    }
    int pt = 0, pok = 0;
    Shader* anyvs = NULL;
    for (int i = 0; i < g_n && !anyvs; ++i)
        if (g_sh[i].ok && !g_sh[i].s.pixel && g_sh[i].s.major >= 3)
            anyvs = &g_sh[i];
    for (int j = 0; j < g_n && anyvs; ++j)
    {
        if (!g_sh[j].ok || !g_sh[j].s.pixel)
            continue;
        pt++;
        pok += check(anyvs, &g_sh[j], verbose);
    }
    printf("vertex shaders: %d, %d good; pixel shaders: %d, %d good\n", vt, vok, pt, pok);
    return (good == pairs && vok == vt && pok == pt) ? 0 : 1;
}
