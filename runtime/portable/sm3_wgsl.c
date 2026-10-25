/* Direct3D 9 shader pairs to WGSL. See sm3_wgsl.h; the structure is sm3_msl.c's, kept apart so the
 * Windows and Linux builds (wgpu only) carry no Metal code. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3_wgsl.h"

typedef struct Buf
{
    char* s;
    size_t len, cap;
} Buf;

static void put(Buf* b, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Buf* b, const char* fmt, ...)
{
    va_list ap;
    for (;;)
    {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = b->cap ? vsnprintf(b->s + b->len, room, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < room)
        {
            b->len += (size_t)n;
            return;
        }
        size_t want = b->cap ? b->cap * 2 : 16384;
        while (n >= 0 && want < b->len + (size_t)n + 1)
            want *= 2;
        b->s = (char*)realloc(b->s, want);
        b->cap = want;
    }
}

/* --- semantics ------------------------------------------------------------------------------------- */

typedef struct Sem
{
    uint8_t usage, index;
    uint8_t reg;     /* the register that carries it (output o<n> / input v<n>, or t<n> in ps 2) */
    uint8_t regtype; /* SM3_OUTPUT, SM3_INPUT, SM3_ADDR ... */
    uint8_t centroid;
} Sem;

static const char* const USAGE[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
    "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample" };

static void sem_name(const Sem* s, char* out, size_t n) { snprintf(out, n, "%s%u", s->usage < 14 ? USAGE[s->usage] : "u", s->index); }

static unsigned vs_outputs(const Sm3Shader* vs, Sem* out, unsigned max)
{
    unsigned n = 0;
    if (vs->major >= 3)
    {
        for (size_t i = 0; i < vs->nins && n < max; ++i)
        {
            const Sm3Ins* in = &vs->ins[i];
            if (in->op == SM3_DCL && in->dst.type == SM3_OUTPUT)
                out[n++] = (Sem){ in->usage, in->usage_index, (uint8_t)in->dst.num, SM3_OUTPUT, 0 };
        }
        return n;
    }
    uint32_t seen = 0;
    for (size_t i = 0; i < vs->nins; ++i)
    {
        const Sm3Ins* in = &vs->ins[i];
        if (!in->has_dst || in->op == SM3_DCL || in->op == SM3_DEF)
            continue;
        Sem s = { 0 };
        unsigned bit;
        if (in->dst.type == SM3_RASTOUT && in->dst.num == 0)
            s = (Sem){ SM3_U_POSITION, 0, 0, SM3_RASTOUT, 0 }, bit = 0;
        else if (in->dst.type == SM3_RASTOUT && in->dst.num == 1)
            s = (Sem){ SM3_U_FOG, 0, 1, SM3_RASTOUT, 0 }, bit = 1;
        else if (in->dst.type == SM3_RASTOUT && in->dst.num == 2)
            s = (Sem){ SM3_U_PSIZE, 0, 2, SM3_RASTOUT, 0 }, bit = 2;
        else if (in->dst.type == SM3_ATTROUT && in->dst.num < 2)
            s = (Sem){ SM3_U_COLOR, (uint8_t)in->dst.num, (uint8_t)in->dst.num, SM3_ATTROUT, 0 }, bit = 3 + in->dst.num;
        else if (in->dst.type == SM3_OUTPUT && in->dst.num < 8)
            s = (Sem){ SM3_U_TEXCOORD, (uint8_t)in->dst.num, (uint8_t)in->dst.num, SM3_OUTPUT, 0 }, bit = 5 + in->dst.num;
        else
            continue;
        if (!(seen & (1u << bit)) && n < max)
        {
            seen |= 1u << bit;
            out[n++] = s;
        }
    }
    return n;
}

static unsigned ps_inputs(const Sm3Shader* ps, Sem* out, unsigned max)
{
    unsigned n = 0;
    for (size_t i = 0; i < ps->nins && n < max; ++i)
    {
        const Sm3Ins* in = &ps->ins[i];
        if (in->op != SM3_DCL)
            continue;
        uint8_t centroid = (in->dst.mod & SM3_DST_CENTROID) != 0;
        if (ps->major >= 3 && in->dst.type == SM3_INPUT)
            out[n++] = (Sem){ in->usage, in->usage_index, (uint8_t)in->dst.num, SM3_INPUT, centroid };
        else if (ps->major < 3 && in->dst.type == SM3_INPUT)
            out[n++] = (Sem){ SM3_U_COLOR, (uint8_t)in->dst.num, (uint8_t)in->dst.num, SM3_INPUT, centroid };
        else if (ps->major < 3 && in->dst.type == SM3_ADDR)
            out[n++] = (Sem){ SM3_U_TEXCOORD, (uint8_t)in->dst.num, (uint8_t)in->dst.num, SM3_ADDR, centroid };
    }
    return n;
}

static int is_position(const Sem* s) { return s->usage == SM3_U_POSITION && s->index == 0; }

/* --- one shader's translation ------------------------------------------------------------------------ */

enum { BLK_IF, BLK_REP, BLK_LOOP };

typedef struct Ctx
{
    const Sm3Shader* sh;
    Buf* b;
    int pixel, failed;
    char* err;
    size_t errn;
    const char* u;          /* the stage's uniform block: "vu" or "pu" */
    uint8_t has_def[256];
    float def[256][4];
    uint8_t has_defi[16];
    int32_t defi[16][4];
    uint8_t has_defb[16];
    uint32_t defb[16];
    uint8_t tex_type[20];   /* sampler dcl types */
    uint32_t depth_mask;    /* bit n: sampler n is a depth texture */
    int depth;              /* block nesting (indentation) */
    int loops;
    int nblk;
    uint8_t blk[64];        /* open blocks: BLK_* */
    int blk_id[64];         /* their loop numbers */
} Ctx;

static void fail(Ctx* c, const char* fmt, ...)
{
    if (c->failed)
        return;
    c->failed = 1;
    if (c->err && c->errn)
    {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errn, fmt, ap);
        va_end(ap);
    }
}

static void line(Ctx* c, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void line(Ctx* c, const char* fmt, ...)
{
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    put(c->b, "%*s%s\n", 4 + 4 * c->depth, "", tmp);
}

/* an exact f32 literal; infinities and NaNs (shader-creation errors as constants) made at run time */
static void flt(Ctx* c, float f, char* out, size_t n)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    if (!isfinite(f))
    {
        snprintf(out, n, "bitcast<f32>(0x%08Xu ^ %s.fx.zero)", u, c->u);
        return;
    }
    snprintf(out, n, "%.9g", f);
    if (strtof(out, NULL) != f)
    {
        snprintf(out, n, "bitcast<f32>(0x%08Xu)", u);
        return;
    }
    if (!strpbrk(out, ".en"))
        strncat(out, ".0", n - strlen(out) - 1);
}

static const char COMP[] = "xyzw";

static void prepass(Ctx* c)
{
    for (size_t i = 0; i < c->sh->nins; ++i)
    {
        const Sm3Ins* in = &c->sh->ins[i];
        if (in->op == SM3_DEF && in->dst.num < 256)
        {
            c->has_def[in->dst.num] = 1;
            memcpy(c->def[in->dst.num], in->value, 16);
        }
        else if (in->op == SM3_DEFI && in->dst.num < 16)
        {
            c->has_defi[in->dst.num] = 1;
            memcpy(c->defi[in->dst.num], in->value, 16);
        }
        else if (in->op == SM3_DEFB && in->dst.num < 16)
            c->has_defb[in->dst.num] = 1, c->defb[in->dst.num] = in->value[0];
        else if (in->op == SM3_DCL && in->dst.type == SM3_SAMPLER && in->dst.num < 20)
            c->tex_type[in->dst.num] = in->tex_type ? in->tex_type : SM3_TEX_2D;
    }
    for (size_t i = 0; i < c->sh->nins; ++i)
    {
        const Sm3Ins* in = &c->sh->ins[i];
        if ((in->op == SM3_TEX || in->op == SM3_TEXLDL || in->op == SM3_TEXLDD) && in->nsrc >= 2 && in->src[1].type == SM3_SAMPLER &&
            in->src[1].num < 20 && !c->tex_type[in->src[1].num])
            c->tex_type[in->src[1].num] = SM3_TEX_2D;
    }
}

static int max_temp(const Sm3Shader* s)
{
    int m = -1;
    for (size_t i = 0; i < s->nins; ++i)
    {
        const Sm3Ins* in = &s->ins[i];
        if (in->has_dst && in->dst.type == SM3_TEMP && (int)in->dst.num > m)
            m = in->dst.num;
        for (unsigned k = 0; k < in->nsrc; ++k)
            if (in->src[k].type == SM3_TEMP && (int)in->src[k].num > m)
                m = in->src[k].num;
    }
    return m;
}

static void rel_index(const Sm3Reg* r, char* out, size_t n)
{
    if (r->rel_type == SM3_LOOP)
        snprintf(out, n, "aL + %u", r->num);
    else
        snprintf(out, n, "a0.%c + %u", COMP[r->rel_comp & 3], r->num);
}

static void reg_value(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    char idx[48], f[4][64];
    switch (r->type)
    {
    case SM3_TEMP: snprintf(out, n, "r%u", r->num); return;
    case SM3_INPUT:
        if (r->rel)
        {
            fail(c, "relative input register");
            snprintf(out, n, "vec4<f32>(0.0)");
            return;
        }
        snprintf(out, n, "v%u", r->num);
        return;
    case SM3_CONST:
        if (r->rel)
        {
            rel_index(r, idx, sizeof idx);
            snprintf(out, n, "%s(%s)", c->pixel ? "crel_ps" : "crel_vs", idx);
            return;
        }
        if (r->num < 256 && c->has_def[r->num])
        {
            for (int k = 0; k < 4; ++k)
                flt(c, c->def[r->num][k], f[k], sizeof f[k]);
            snprintf(out, n, "vec4<f32>(%s, %s, %s, %s)", f[0], f[1], f[2], f[3]);
            return;
        }
        if (r->num >= (c->pixel ? 224u : 256u))
        {
            snprintf(out, n, "vec4<f32>(0.0)");
            return;
        }
        snprintf(out, n, "%s.c[%u]", c->u, r->num);
        return;
    case SM3_CONSTINT:
        if (r->num < 16 && c->has_defi[r->num])
            snprintf(out, n, "vec4<f32>(%d.0, %d.0, %d.0, %d.0)", c->defi[r->num][0], c->defi[r->num][1], c->defi[r->num][2], c->defi[r->num][3]);
        else
            snprintf(out, n, "vec4<f32>(%s.i[%u])", c->u, r->num & 15);
        return;
    case SM3_CONSTBOOL:
        if (c->has_defb[r->num & 15])
            snprintf(out, n, "vec4<f32>(%s)", c->defb[r->num & 15] ? "1.0" : "0.0");
        else
            snprintf(out, n, "vec4<f32>(select(0.0, 1.0, %s.b[%u][%u] != 0u))", c->u, (r->num & 15) / 4, (r->num & 15) % 4);
        return;
    case SM3_ADDR:
        if (c->pixel)
            snprintf(out, n, "t%u", r->num);
        else
            snprintf(out, n, "vec4<f32>(a0)");
        return;
    case SM3_LOOP: snprintf(out, n, "vec4<f32>(f32(aL))"); return;
    case SM3_MISCTYPE: snprintf(out, n, "%s", r->num == 0 ? "vpos" : "vface"); return;
    case SM3_PREDICATE: snprintf(out, n, "vec4<f32>(p0)"); return;
    default:
        fail(c, "source register type %u", r->type);
        snprintf(out, n, "vec4<f32>(0.0)");
        return;
    }
}

static int bool_value(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    if (r->type == SM3_CONSTBOOL)
    {
        if (r->num < 16 && c->has_defb[r->num])
            snprintf(out, n, "%s", c->defb[r->num] ? "true" : "false");
        else
            snprintf(out, n, "(%s.b[%u][%u] != 0u)", c->u, (r->num & 15) / 4, (r->num & 15) % 4);
        return 1;
    }
    if (r->type == SM3_PREDICATE)
    {
        snprintf(out, n, "%sp0.%c", r->mod == SM3_SRC_NOT ? "!" : "", COMP[r->swizzle & 3]);
        return 1;
    }
    fail(c, "boolean from register type %u", r->type);
    snprintf(out, n, "false");
    return 0;
}

static void src(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    char v[300], sw[8] = "";
    reg_value(c, r, v, sizeof v);
    if (r->swizzle != 0xE4)
    {
        sw[0] = '.';
        for (int k = 0; k < 4; ++k)
            sw[1 + k] = COMP[(r->swizzle >> (2 * k)) & 3];
        sw[5] = 0;
    }
    char x[340];
    if (sw[0])
        snprintf(x, sizeof x, "(%s)%s", v, sw);
    else
        snprintf(x, sizeof x, "%s", v);
    switch (r->mod)
    {
    case SM3_SRC_NONE: snprintf(out, n, "%s", x); break;
    case SM3_SRC_NEG: snprintf(out, n, "(-%s)", x); break;
    case SM3_SRC_ABS: snprintf(out, n, "abs(%s)", x); break;
    case SM3_SRC_ABSNEG: snprintf(out, n, "(-abs(%s))", x); break;
    case SM3_SRC_BIAS: snprintf(out, n, "(%s - 0.5)", x); break;
    case SM3_SRC_BIASNEG: snprintf(out, n, "(0.5 - %s)", x); break;
    case SM3_SRC_SIGN: snprintf(out, n, "(2.0 * %s - 1.0)", x); break;
    case SM3_SRC_SIGNNEG: snprintf(out, n, "(1.0 - 2.0 * %s)", x); break;
    case SM3_SRC_COMP: snprintf(out, n, "(1.0 - %s)", x); break;
    case SM3_SRC_X2: snprintf(out, n, "(2.0 * %s)", x); break;
    case SM3_SRC_X2NEG: snprintf(out, n, "(-2.0 * %s)", x); break;
    default:
        fail(c, "source modifier %u", r->mod);
        snprintf(out, n, "%s", x);
        break;
    }
}

static void scalar(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    char s[380];
    src(c, r, s, sizeof s);
    snprintf(out, n, "(%s).%c", s, r->swizzle == 0xE4 ? 'w' : 'x');
}

/* `target` = `val` under the write mask: one component directly, several through select (WGSL
 * cannot assign through a swizzle of more than one component) */
static void masked(Ctx* c, const char* target, const char* val, uint8_t mask, const char* type)
{
    if (mask == 0xF || mask == 0)
    {
        line(c, "%s = %s;", target, val);
        return;
    }
    int bits = 0, one = 0;
    for (int i = 0; i < 4; ++i)
        if (mask & (1 << i))
            bits++, one = i;
    if (bits == 1)
    {
        line(c, "%s.%c = (%s).%c;", target, COMP[one], val, COMP[one]);
        return;
    }
    line(c, "%s = select(%s, %s(%s), vec4<bool>(%s, %s, %s, %s));", target, target, type, val, mask & 1 ? "true" : "false",
        mask & 2 ? "true" : "false", mask & 4 ? "true" : "false", mask & 8 ? "true" : "false");
}

static void assign(Ctx* c, const Sm3Ins* in, const char* e)
{
    const Sm3Reg* d = &in->dst;
    char target[64], val[2600];
    if (d->shift)
        fail(c, "destination shift");
    if (d->mod & SM3_DST_SATURATE)
        snprintf(val, sizeof val, "saturate(%s)", e);
    else
        snprintf(val, sizeof val, "%s", e);
    switch (d->type)
    {
    case SM3_TEMP: snprintf(target, sizeof target, "r%u", d->num); break;
    case SM3_OUTPUT:
        if (c->sh->major >= 3)
        {
            if (d->rel)
            {
                char idx[48];
                rel_index(d, idx, sizeof idx);
                snprintf(target, sizeof target, "o[%s]", idx);
            }
            else
                snprintf(target, sizeof target, "o[%u]", d->num);
        }
        else
            snprintf(target, sizeof target, "o_t%u", d->num);
        break;
    case SM3_RASTOUT:
        if (d->num == 0)
            snprintf(target, sizeof target, "o_pos");
        else
        {
            line(c, "%s = (%s).x;", d->num == 1 ? "o_fog" : "o_pts", val);
            return;
        }
        break;
    case SM3_ATTROUT: snprintf(target, sizeof target, "o_d%u", d->num); break;
    case SM3_COLOROUT: snprintf(target, sizeof target, "oc%u", d->num); break;
    case SM3_DEPTHOUT: line(c, "odepth = (%s).x;", val); return;
    case SM3_ADDR:
        if (!c->pixel)
        {
            /* mova and vs 1.1 mov a0: round to nearest (vs 2+), as D3D does */
            char r[2700];
            snprintf(r, sizeof r, "vec4<i32>(floor(%s + 0.5))", val);
            masked(c, "a0", r, d->mask, "vec4<i32>");
            return;
        }
        snprintf(target, sizeof target, "t%u", d->num);
        break;
    default:
        fail(c, "destination register type %u", d->type);
        return;
    }
    masked(c, target, val, d->mask, "vec4<f32>");
}

static const char* cmp_op(unsigned control)
{
    switch (control & 7)
    {
    case 1: return ">";
    case 2: return "==";
    case 3: return ">=";
    case 4: return "<";
    case 5: return "!=";
    case 6: return "<=";
    default: return NULL;
    }
}

enum { SMP_PLAIN, SMP_PROJ, SMP_BIAS, SMP_LEVEL, SMP_GRAD };

/* a texture sample of sampler src[1] at `coord` (a vec4 expression); gx/gy: gradients (SMP_GRAD) */
static void sample(Ctx* c, const Sm3Ins* in, const char* coord, int how, const char* gx, const char* gy)
{
    const Sm3Reg* sr = &in->src[1];
    unsigned n = sr->num;
    if (sr->type != SM3_SAMPLER || n >= 20)
    {
        fail(c, "texture sample without a sampler");
        return;
    }
    unsigned type = c->tex_type[n] ? c->tex_type[n] : SM3_TEX_2D;
    int depth = type == SM3_TEX_2D && SM3W_DEPTH_TEX(c->depth_mask, n);
    const char* comps = type == SM3_TEX_2D ? "xy" : "xyz";
    unsigned bind = c->pixel ? 2 * n : 32 + 2 * n;
    char tc[64], e[1200];
    line(c, "{");
    c->depth++;
    line(c, "let tc = %s;", coord);
    if (how == SMP_PROJ)
        snprintf(tc, sizeof tc, "tc.%s / tc.w", comps);
    else
        snprintf(tc, sizeof tc, "tc.%s", comps);
    switch (how)
    {
    case SMP_BIAS: snprintf(e, sizeof e, "textureSampleBias(tex%u, smp%u, %s, tc.w)", bind, bind, tc); break;
    case SMP_LEVEL:
        snprintf(e, sizeof e, "textureSampleLevel(tex%u, smp%u, %s, %s)", bind, bind, tc, depth ? "i32(tc.w)" : "tc.w");
        break;
    case SMP_GRAD:
        if (depth)
            snprintf(e, sizeof e, "textureSampleLevel(tex%u, smp%u, %s, 0)", bind, bind, tc);
        else
            snprintf(e, sizeof e, "textureSampleGrad(tex%u, smp%u, %s, (%s).%s, (%s).%s)", bind, bind, tc, gx, comps, gy, comps);
        break;
    default: snprintf(e, sizeof e, "textureSample(tex%u, smp%u, %s)", bind, bind, tc); break;
    }
    if (depth)
        line(c, "var tv = vec4<f32>(%s);", e);
    else
        line(c, "var tv = %s;", e);
    line(c, "if ((%s.fx.srgb_mask & %uu) != 0u) { tv = vec4<f32>(srgb_to_linear(tv.rgb), tv.a); }", c->u, 1u << n);
    line(c, "if ((%s.fx.one_mask & %uu) != 0u) { tv.a = 1.0; }", c->u, 1u << n);
    if (sr->swizzle != 0xE4)
    {
        char sw[6];
        for (int k = 0; k < 4; ++k)
            sw[k] = COMP[(sr->swizzle >> (2 * k)) & 3];
        sw[4] = 0;
        line(c, "tv = tv.%s;", sw);
    }
    assign(c, in, "tv");
    c->depth--;
    line(c, "}");
}

static void matrix(Ctx* c, const Sm3Ins* in, int width, int rows)
{
    char s0[380], r[4][380], e[2400];
    src(c, &in->src[0], s0, sizeof s0);
    for (int k = 0; k < rows; ++k)
    {
        Sm3Reg row = in->src[1];
        row.num = (uint16_t)(row.num + k);
        src(c, &row, r[k], sizeof r[k]);
    }
    const char* w = width == 4 ? "" : ".xyz";
    int len = snprintf(e, sizeof e, "vec4<f32>(");
    for (int k = 0; k < 4; ++k)
    {
        if (k < rows)
            len += snprintf(e + len, sizeof e - (size_t)len, "%sdot((%s)%s, (%s)%s)", k ? ", " : "", s0, w, r[k], w);
        else
            len += snprintf(e + len, sizeof e - (size_t)len, ", 0.0");
    }
    snprintf(e + len, sizeof e - (size_t)len, ")");
    assign(c, in, e);
}

static void open_block(Ctx* c, int kind, int id)
{
    if (c->nblk >= 64)
    {
        fail(c, "blocks nested too deep");
        return;
    }
    c->blk[c->nblk] = (uint8_t)kind, c->blk_id[c->nblk] = id;
    c->nblk++;
    c->depth++;
}

static void emit(Ctx* c, const Sm3Ins* in)
{
    char a[400], b[400], d[400], e[2400];
    if (in->predicated)
    {
        fail(c, "predicated %s", sm3_opname(in->op));
        return;
    }
    switch (in->op)
    {
    case SM3_NOP: case SM3_DCL: case SM3_DEF: case SM3_DEFI: case SM3_DEFB: return;
    case SM3_MOV: case SM3_MOVA: src(c, &in->src[0], a, sizeof a); assign(c, in, a); return;
    case SM3_ADD: case SM3_SUB: case SM3_MUL: case SM3_MIN: case SM3_MAX: case SM3_SLT: case SM3_SGE:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        switch (in->op)
        {
        case SM3_ADD: snprintf(e, sizeof e, "(%s + %s)", a, b); break;
        case SM3_SUB: snprintf(e, sizeof e, "(%s - %s)", a, b); break;
        case SM3_MUL: snprintf(e, sizeof e, "(%s * %s)", a, b); break;
        case SM3_MIN: snprintf(e, sizeof e, "min(%s, %s)", a, b); break;
        case SM3_MAX: snprintf(e, sizeof e, "max(%s, %s)", a, b); break;
        case SM3_SLT: snprintf(e, sizeof e, "select(vec4<f32>(0.0), vec4<f32>(1.0), %s < %s)", a, b); break;
        default: snprintf(e, sizeof e, "select(vec4<f32>(0.0), vec4<f32>(1.0), %s >= %s)", a, b); break;
        }
        assign(c, in, e);
        return;
    case SM3_MAD:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        src(c, &in->src[2], d, sizeof d);
        snprintf(e, sizeof e, "(%s * %s + %s)", a, b, d);
        assign(c, in, e);
        return;
    case SM3_LRP:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        src(c, &in->src[2], d, sizeof d);
        snprintf(e, sizeof e, "(%s * (%s - %s) + %s)", a, b, d, d);
        assign(c, in, e);
        return;
    case SM3_CMP: case SM3_CND:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        src(c, &in->src[2], d, sizeof d);
        snprintf(e, sizeof e, "select(%s, %s, %s %s)", d, b, a, in->op == SM3_CMP ? ">= vec4<f32>(0.0)" : "> vec4<f32>(0.5)");
        assign(c, in, e);
        return;
    case SM3_RCP: scalar(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "vec4<f32>(1.0 / %s)", a); assign(c, in, e); return;
    case SM3_RSQ: scalar(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "vec4<f32>(inverseSqrt(abs(%s)))", a); assign(c, in, e); return;
    case SM3_EXP: case SM3_EXPP:
        scalar(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "vec4<f32>(exp2(%s))", a);
        assign(c, in, e);
        return;
    case SM3_LOG: case SM3_LOGP:
        scalar(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "vec4<f32>(log2(abs(%s)))", a);
        assign(c, in, e);
        return;
    case SM3_POW:
        scalar(c, &in->src[0], a, sizeof a);
        scalar(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "vec4<f32>(pow(abs(%s), %s))", a, b);
        assign(c, in, e);
        return;
    case SM3_DP3: case SM3_DP4:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        if (in->op == SM3_DP3)
            snprintf(e, sizeof e, "vec4<f32>(dot((%s).xyz, (%s).xyz))", a, b);
        else
            snprintf(e, sizeof e, "vec4<f32>(dot(%s, %s))", a, b);
        assign(c, in, e);
        return;
    case SM3_DP2ADD:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        scalar(c, &in->src[2], d, sizeof d);
        snprintf(e, sizeof e, "vec4<f32>(dot((%s).xy, (%s).xy) + %s)", a, b, d);
        assign(c, in, e);
        return;
    case SM3_FRC: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "fract(%s)", a); assign(c, in, e); return;
    case SM3_ABS: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "abs(%s)", a); assign(c, in, e); return;
    case SM3_SGN: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "sign(%s)", a); assign(c, in, e); return;
    case SM3_NRM:
        src(c, &in->src[0], a, sizeof a);
        line(c, "{");
        c->depth++;
        line(c, "let nv = %s;", a);
        line(c, "let nf = dot(nv.xyz, nv.xyz);");
        assign(c, in, "(nv * select(3.4028234e38, inverseSqrt(nf), nf != 0.0))");
        c->depth--;
        line(c, "}");
        return;
    case SM3_SINCOS:
        scalar(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "vec4<f32>(cos(%s), sin(%s), 0.0, 0.0)", a, a);
        assign(c, in, e);
        return;
    case SM3_CRS:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "vec4<f32>(cross((%s).xyz, (%s).xyz), 0.0)", a, b);
        assign(c, in, e);
        return;
    case SM3_LIT: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "lit4(%s)", a); assign(c, in, e); return;
    case SM3_DST:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "vec4<f32>(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b);
        assign(c, in, e);
        return;
    case SM3_M4x4: matrix(c, in, 4, 4); return;
    case SM3_M4x3: matrix(c, in, 4, 3); return;
    case SM3_M3x4: matrix(c, in, 3, 4); return;
    case SM3_M3x3: matrix(c, in, 3, 3); return;
    case SM3_M3x2: matrix(c, in, 3, 2); return;
    case SM3_DSX: case SM3_DSY:
        if (!c->pixel)
            fail(c, "derivative in a vertex shader");
        src(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "%s(%s)", in->op == SM3_DSX ? "dpdx" : "dpdy", a);
        assign(c, in, e);
        return;
    case SM3_TEX:
        if (c->sh->major < 2)
        {
            fail(c, "ps 1.x texture instruction");
            return;
        }
        if (!c->pixel)
        {
            fail(c, "texld in a vertex shader");
            return;
        }
        src(c, &in->src[0], a, sizeof a);
        sample(c, in, a, in->control == 2 ? SMP_BIAS : in->control == 1 ? SMP_PROJ : SMP_PLAIN, NULL, NULL);
        return;
    case SM3_TEXLDL:
        src(c, &in->src[0], a, sizeof a);
        sample(c, in, a, SMP_LEVEL, NULL, NULL);
        return;
    case SM3_TEXLDD:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[2], b, sizeof b);
        src(c, &in->src[3], d, sizeof d);
        sample(c, in, a, SMP_GRAD, b, d);
        return;
    case SM3_TEXKILL:
    {
        Sm3Reg r = in->dst;
        r.swizzle = 0xE4;
        r.mod = 0;
        reg_value(c, &r, a, sizeof a);
        char m[6] = "";
        int k = 0;
        for (int i = 0; i < 4; ++i)
            if ((in->dst.mask ? in->dst.mask : 0x7) & (1 << i))
                m[k++] = COMP[i];
        m[k] = 0;
        if (k == 1)
            line(c, "if ((%s).%s < 0.0) { discard; }", a, m);
        else
            line(c, "if (any((%s).%s < vec%d<f32>(0.0))) { discard; }", a, m, k);
        return;
    }
    case SM3_IF:
        bool_value(c, &in->src[0], a, sizeof a);
        line(c, "if (%s) {", a);
        open_block(c, BLK_IF, 0);
        return;
    case SM3_IFC: case SM3_BREAKC:
    {
        const char* op = cmp_op(in->control);
        if (!op)
        {
            fail(c, "comparison %u", in->control);
            return;
        }
        scalar(c, &in->src[0], a, sizeof a);
        scalar(c, &in->src[1], b, sizeof b);
        if (in->op == SM3_IFC)
        {
            line(c, "if (%s %s %s) {", a, op, b);
            open_block(c, BLK_IF, 0);
        }
        else
            line(c, "if (%s %s %s) { break; }", a, op, b);
        return;
    }
    case SM3_ELSE:
        c->depth--;
        line(c, "} else {");
        c->depth++;
        return;
    case SM3_ENDIF: case SM3_ENDREP: case SM3_ENDLOOP:
    {
        if (!c->nblk)
        {
            fail(c, "unbalanced blocks");
            return;
        }
        int kind = c->blk[--c->nblk], id = c->blk_id[c->nblk];
        if (kind == BLK_LOOP)
            line(c, "aL = aL + aLstep%d;", id);
        c->depth--;
        line(c, "}");
        if (kind == BLK_LOOP)
            line(c, "aL = aLsave%d;", id);
        if (kind != BLK_IF)
        {
            c->depth--;
            line(c, "}");
        }
        return;
    }
    case SM3_BREAK: line(c, "break;"); return;
    case SM3_BREAKP:
        bool_value(c, &in->src[0], a, sizeof a);
        line(c, "if (%s) { break; }", a);
        return;
    case SM3_REP: case SM3_LOOP_OP:
    {
        const Sm3Reg* ir = &in->src[in->op == SM3_REP ? 0 : 1];
        char ix[64], iy[64], iz[64];
        if (ir->type != SM3_CONSTINT || ir->num >= 16)
        {
            fail(c, "loop count from register type %u", ir->type);
            return;
        }
        if (c->has_defi[ir->num])
        {
            snprintf(ix, sizeof ix, "%d", c->defi[ir->num][0]);
            snprintf(iy, sizeof iy, "%d", c->defi[ir->num][1]);
            snprintf(iz, sizeof iz, "%d", c->defi[ir->num][2]);
        }
        else
        {
            snprintf(ix, sizeof ix, "%s.i[%u].x", c->u, ir->num);
            snprintf(iy, sizeof iy, "%s.i[%u].y", c->u, ir->num);
            snprintf(iz, sizeof iz, "%s.i[%u].z", c->u, ir->num);
        }
        int k = ++c->loops;
        line(c, "{");
        c->depth++;
        if (in->op == SM3_LOOP_OP)
        {
            line(c, "let aLsave%d = aL;", k);
            line(c, "let aLstep%d: i32 = %s;", k, iz);
            line(c, "aL = %s;", iy);
        }
        line(c, "for (var lk%d: i32 = 0; lk%d < %s; lk%d = lk%d + 1) {", k, k, ix, k, k);
        open_block(c, in->op == SM3_REP ? BLK_REP : BLK_LOOP, k);
        return;
    }
    case SM3_SETP:
    {
        const char* op = cmp_op(in->control);
        if (!op)
        {
            fail(c, "comparison %u", in->control);
            return;
        }
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "(%s %s %s)", a, op, b);
        masked(c, "p0", e, in->dst.mask, "vec4<bool>");
        return;
    }
    case SM3_RET: return;
    default:
        fail(c, "instruction %s", sm3_opname(in->op) ? sm3_opname(in->op) : "?");
        return;
    }
}

static void body(Ctx* c)
{
    for (size_t i = 0; i < c->sh->nins && !c->failed; ++i)
        emit(c, &c->sh->ins[i]);
    if (c->nblk && !c->failed)
        fail(c, "unbalanced blocks");
}

static const char PRELUDE[] =
    "diagnostic(off, derivative_uniformity);\n"
    "\n"
    "struct Fix { posfix: vec4<f32>, alpha_ref: f32, alpha_func: u32, srgb_write: u32, srgb_mask: u32, one_mask: u32, zero: u32, pad0: u32, pad1: u32 };\n"
    "struct VsU { fx: Fix, i: array<vec4<i32>, 16>, b: array<vec4<u32>, 4>, c: array<vec4<f32>, 256> };\n"
    "struct PsU { fx: Fix, i: array<vec4<i32>, 16>, b: array<vec4<u32>, 4>, c: array<vec4<f32>, 224> };\n"
    "@group(0) @binding(0) var<uniform> vu: VsU;\n"
    "@group(0) @binding(1) var<uniform> pu: PsU;\n"
    "\n"
    "fn srgb_to_linear(c: vec3<f32>) -> vec3<f32> { return select(pow((c + 0.055) / 1.055, vec3<f32>(2.4)), c / 12.92, c <= vec3<f32>(0.04045)); }\n"
    "fn linear_to_srgb(x: vec3<f32>) -> vec3<f32> {\n"
    "    let c = saturate(x);\n"
    "    return select(1.055 * pow(c, vec3<f32>(1.0 / 2.4)) - 0.055, c * 12.92, c <= vec3<f32>(0.0031308));\n"
    "}\n"
    "fn lit4(s: vec4<f32>) -> vec4<f32> {\n"
    "    var d = vec4<f32>(1.0, 0.0, 0.0, 1.0);\n"
    "    if (s.x > 0.0) { d.y = s.x; if (s.y > 0.0) { d.z = pow(s.y, clamp(s.w, -127.9961, 127.9961)); } }\n"
    "    return d;\n"
    "}\n"
    "fn alpha_pass(f: u32, a: f32, r: f32) -> bool {\n"
    "    switch f {\n"
    "        case 1u: { return false; }\n"
    "        case 2u: { return a < r; }\n"
    "        case 3u: { return a == r; }\n"
    "        case 4u: { return a <= r; }\n"
    "        case 5u: { return a > r; }\n"
    "        case 6u: { return a != r; }\n"
    "        case 7u: { return a >= r; }\n"
    "        default: { return true; }\n"
    "    }\n"
    "}\n";

/* relative constant reads: the shader's def registers win over the bound constants */
static void crel_function(Ctx* c, const char* name, unsigned count)
{
    put(c->b, "fn %s(i: i32) -> vec4<f32> {\n", name);
    char f[4][64];
    for (unsigned k = 0; k < 256; ++k)
        if (c->has_def[k])
        {
            for (int j = 0; j < 4; ++j)
                flt(c, c->def[k][j], f[j], sizeof f[j]);
            put(c->b, "    if (i == %u) { return vec4<f32>(%s, %s, %s, %s); }\n", k, f[0], f[1], f[2], f[3]);
        }
    put(c->b, "    if (u32(i) < %uu) { return %s.c[i]; }\n    return vec4<f32>(0.0);\n}\n", count, c->u);
}

static int uses_rel_const(const Sm3Shader* s)
{
    for (size_t i = 0; i < s->nins; ++i)
        for (unsigned k = 0; k < s->ins[i].nsrc; ++k)
            if (s->ins[i].src[k].type == SM3_CONST && s->ins[i].src[k].rel)
                return 1;
    return 0;
}

static void texture_decls(Ctx* c, Buf* b, unsigned count, unsigned base)
{
    for (unsigned n = 0; n < count; ++n)
    {
        if (!c->tex_type[n])
            continue;
        const char* t = c->tex_type[n] == SM3_TEX_CUBE ? "texture_cube<f32>"
            : c->tex_type[n] == SM3_TEX_VOLUME          ? "texture_3d<f32>"
            : SM3W_DEPTH_TEX(c->depth_mask, n)          ? "texture_depth_2d"
                                                        : "texture_2d<f32>";
        put(b, "@group(1) @binding(%u) var tex%u: %s;\n@group(1) @binding(%u) var smp%u: sampler;\n", base + 2 * n, base + 2 * n, t,
            base + 2 * n + 1, base + 2 * n);
    }
}

char* sm3_wgsl_pair(const Sm3Shader* vs, const Sm3Shader* ps, const uint8_t in_kind[16], uint32_t depth_mask, char* err, size_t errn)
{
    if (err && errn)
        err[0] = 0;
    if (vs->pixel || !ps->pixel)
    {
        snprintf(err, errn, "not a vertex and pixel shader pair");
        return NULL;
    }
    if (vs->major < 2 || ps->major < 2)
    {
        snprintf(err, errn, "shader model 1 (vs_%u_%u, ps_%u_%u) is not translated here", vs->major, vs->minor, ps->major, ps->minor);
        return NULL;
    }
    Buf b = { 0 };
    put(&b, "%s", PRELUDE);
    Sem outs[16], ins[16];
    unsigned nout = vs_outputs(vs, outs, 16), nin = ps_inputs(ps, ins, 16);
    char name[32];

    /* the varyings: a location per vertex output the pixel shader reads (others are not written) */
    int loc_of[16], nloc = 0;
    uint8_t centroid[16] = { 0 }, linked[16] = { 0 };
    for (unsigned j = 0; j < nout; ++j)
    {
        loc_of[j] = -1;
        if (is_position(&outs[j]) || outs[j].usage == SM3_U_PSIZE)
            continue;
        for (unsigned k = 0; k < nin; ++k)
            if (outs[j].usage == ins[k].usage && outs[j].index == ins[k].index && !linked[k])
            {
                linked[k] = (uint8_t)(j + 1);
                centroid[j] = ins[k].centroid;
                if (loc_of[j] < 0)
                    loc_of[j] = nloc++;
            }
    }

    /* --- vertex function --- */
    Ctx v = { .sh = vs, .b = &b, .pixel = 0, .err = err, .errn = errn, .u = "vu", .depth_mask = depth_mask >> 16 };
    prepass(&v);
    texture_decls(&v, &b, 4, 32);
    if (uses_rel_const(vs))
        crel_function(&v, "crel_vs", 256);
    int vin = 0;
    for (size_t i = 0; i < vs->nins; ++i)
        if (vs->ins[i].op == SM3_DCL && vs->ins[i].dst.type == SM3_INPUT && vs->ins[i].dst.num < 16)
        {
            unsigned r = vs->ins[i].dst.num;
            unsigned k = in_kind ? in_kind[r] : SM3W_IN_FLOAT;
            if (k == SM3W_IN_ZERO)
                continue;
            if (!vin++)
                put(&b, "\nstruct VIn {\n");
            put(&b, "    @location(%u) v%u: %s,\n", r, r, k == SM3W_IN_INT ? "vec4<i32>" : k == SM3W_IN_UINT ? "vec4<u32>" : "vec4<f32>");
        }
    if (vin)
        put(&b, "};\n");
    put(&b, "\nstruct VOut {\n    @builtin(position) pos: vec4<f32>,\n");
    for (unsigned k = 0; k < nout; ++k)
        if (loc_of[k] >= 0)
        {
            sem_name(&outs[k], name, sizeof name);
            put(&b, "    @location(%d)%s %s: vec4<f32>,\n", loc_of[k], centroid[k] ? " @interpolate(perspective, centroid)" : "", name);
        }
    put(&b, "};\n\n@vertex\nfn vs_main(%s) -> VOut {\n", vin ? "in: VIn" : "");
    int nt = max_temp(vs);
    for (int k = 0; k <= nt; ++k)
        put(&b, "    var r%d = vec4<f32>(0.0);\n", k);
    put(&b, "    var a0 = vec4<i32>(0); var aL: i32 = 0; var p0 = vec4<bool>(false);\n");
    for (size_t i = 0; i < vs->nins; ++i)
        if (vs->ins[i].op == SM3_DCL && vs->ins[i].dst.type == SM3_INPUT && vs->ins[i].dst.num < 16)
        {
            unsigned r = vs->ins[i].dst.num;
            unsigned k = in_kind ? in_kind[r] : SM3W_IN_FLOAT;
            if (k == SM3W_IN_ZERO)
                put(&b, "    var v%u = vec4<f32>(0.0);\n", r);
            else
                put(&b, "    var v%u = vec4<f32>(in.v%u);\n", r, r);
        }
    if (vs->major >= 3)
        put(&b, "    var o: array<vec4<f32>, 12>;\n");
    else
        put(&b, "    var o_pos = vec4<f32>(0.0); var o_d0 = vec4<f32>(0.0); var o_d1 = vec4<f32>(0.0);\n"
                "    var o_t0 = vec4<f32>(0.0); var o_t1 = vec4<f32>(0.0); var o_t2 = vec4<f32>(0.0); var o_t3 = vec4<f32>(0.0);\n"
                "    var o_t4 = vec4<f32>(0.0); var o_t5 = vec4<f32>(0.0); var o_t6 = vec4<f32>(0.0); var o_t7 = vec4<f32>(0.0);\n"
                "    var o_fog: f32 = 0.0; var o_pts: f32 = 1.0;\n");
    body(&v);
    if (v.failed)
        goto failed;
    put(&b, "    var out: VOut;\n");
    for (unsigned k = 0; k < nout; ++k)
    {
        char from[64];
        if (vs->major >= 3)
            snprintf(from, sizeof from, "o[%u]", outs[k].reg);
        else if (outs[k].regtype == SM3_RASTOUT)
            snprintf(from, sizeof from, "%s", outs[k].reg == 0 ? "o_pos" : outs[k].reg == 1 ? "vec4<f32>(o_fog, 0.0, 0.0, 0.0)" : "vec4<f32>(o_pts)");
        else if (outs[k].regtype == SM3_ATTROUT)
            snprintf(from, sizeof from, "saturate(o_d%u)", outs[k].reg);
        else
            snprintf(from, sizeof from, "o_t%u", outs[k].reg);
        if (is_position(&outs[k]))
            put(&b, "    out.pos = %s;\n", from);
        else if (loc_of[k] >= 0)
        {
            sem_name(&outs[k], name, sizeof name);
            put(&b, "    out.%s = %s;\n", name, from);
        }
    }
    put(&b, "    out.pos = vec4<f32>(out.pos.xy + vu.fx.posfix.xy * out.pos.w, out.pos.zw);\n    return out;\n}\n");

    /* --- fragment function --- */
    {
        Ctx p = { .sh = ps, .b = &b, .pixel = 1, .err = err, .errn = errn, .u = "pu", .depth_mask = depth_mask & 0xFFFF };
        prepass(&p);
        put(&b, "\n");
        texture_decls(&p, &b, 16, 0);
        if (uses_rel_const(ps))
            crel_function(&p, "crel_ps", 224);
        int pin = 0;
        for (unsigned k = 0; k < nin; ++k)
            if (linked[k])
            {
                if (!pin++)
                    put(&b, "\nstruct PIn {\n");
                unsigned j = linked[k] - 1u;
                sem_name(&ins[k], name, sizeof name);
                put(&b, "    @location(%d)%s %s: vec4<f32>,\n", loc_of[j], centroid[j] ? " @interpolate(perspective, centroid)" : "", name);
            }
        if (pin)
            put(&b, "};\n");
        int depth_out = 0, colors = 1;
        for (size_t i = 0; i < ps->nins; ++i)
        {
            const Sm3Ins* in = &ps->ins[i];
            if (in->has_dst && in->dst.type == SM3_DEPTHOUT)
                depth_out = 1;
            if (in->has_dst && in->dst.type == SM3_COLOROUT && in->dst.num + 1 > (unsigned)colors)
                colors = in->dst.num + 1 > 4 ? 4 : in->dst.num + 1;
        }
        put(&b, "\nstruct POut {\n");
        for (int k = 0; k < colors; ++k)
            put(&b, "    @location(%d) c%d: vec4<f32>,\n", k, k);
        if (depth_out)
            put(&b, "    @builtin(frag_depth) depth: f32,\n");
        put(&b, "};\n\n@fragment\nfn fs_main(%s@builtin(position) fragpos: vec4<f32>, @builtin(front_facing) front: bool) -> POut {\n",
            pin ? "in: PIn, " : "");
        int pt = max_temp(ps);
        for (int k = 0; k <= pt; ++k)
            put(&b, "    var r%d = vec4<f32>(0.0);\n", k);
        put(&b, "    var a0 = vec4<i32>(0); var aL: i32 = 0; var p0 = vec4<bool>(false);\n");
        put(&b, "    let vpos = vec4<f32>(floor(fragpos.xy), 0.0, 0.0);\n    let vface = vec4<f32>(select(-1.0, 1.0, front));\n");
        for (unsigned k = 0; k < nin; ++k)
        {
            const char* reg = ins[k].regtype == SM3_ADDR ? "t" : "v";
            sem_name(&ins[k], name, sizeof name);
            if (linked[k])
                put(&b, "    var %s%u = in.%s;\n", reg, ins[k].reg, name);
            else
                put(&b, "    var %s%u = vec4<f32>(0.0); // %s: not written by the vertex shader\n", reg, ins[k].reg, name);
        }
        for (int k = 0; k < colors; ++k)
            put(&b, "    var oc%d = vec4<f32>(0.0);\n", k);
        put(&b, "    var odepth = fragpos.z;\n");
        body(&p);
        if (p.failed)
            goto failed;
        put(&b, "    if (!alpha_pass(pu.fx.alpha_func, oc0.a, pu.fx.alpha_ref)) { discard; }\n");
        put(&b, "    if (pu.fx.srgb_write != 0u) { oc0 = vec4<f32>(linear_to_srgb(oc0.rgb), oc0.a); }\n");
        put(&b, "    var out: POut;\n");
        for (int k = 0; k < colors; ++k)
            put(&b, "    out.c%d = oc%d;\n", k, k);
        if (depth_out)
            put(&b, "    out.depth = odepth;\n");
        put(&b, "    return out;\n}\n");
    }
    return b.s;

failed:
    free(b.s);
    return NULL;
}
