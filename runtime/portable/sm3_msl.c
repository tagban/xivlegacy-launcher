/* Direct3D 9 shader pairs to Metal Shading Language. See sm3_msl.h. */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3_msl.h"

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

/* --- semantics: a varying's name, from its usage and index ---------------------------------------- */

typedef struct Sem
{
    uint8_t usage, index;
    uint8_t reg;      /* the register that carries it (output o<n> / input v<n>, or t<n> in ps 2) */
    uint8_t regtype;  /* SM3_OUTPUT, SM3_INPUT, SM3_ADDR ... */
    uint8_t centroid;
} Sem;

static const char* const USAGE[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
    "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample" };

static void sem_name(const Sem* s, char* out, size_t n)
{
    snprintf(out, n, "%s%u", s->usage < 14 ? USAGE[s->usage] : "u", s->index);
}

/* the vertex shader's outputs (position included, as usage POSITION index 0) */
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
    /* vs 1-2: the registers written */
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

/* the pixel shader's varying inputs */
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

typedef struct Ctx
{
    const Sm3Shader* sh;
    Buf* b;
    int pixel, failed;
    char* err;
    size_t errn;
    uint8_t has_def[256];
    float def[256][4];
    uint8_t has_defi[16];
    int32_t defi[16][4];
    uint8_t has_defb[16];
    uint32_t defb[16];
    uint8_t tex_type[20];  /* sampler dcl types */
    int depth;             /* block nesting, for indentation and loop counters */
    int loops;
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
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    put(c->b, "%*s%s\n", 4 + 4 * c->depth, "", tmp);
}

/* an exact float literal */
static void flt(float f, char* out, size_t n)
{
    if (!isfinite(f))
    {
        uint32_t u;
        memcpy(&u, &f, 4);
        snprintf(out, n, "as_type<float>(0x%08Xu)", u);
        return;
    }
    snprintf(out, n, "%.9g", f);
    if (strtof(out, NULL) != f)
    {
        uint32_t u;
        memcpy(&u, &f, 4);
        snprintf(out, n, "as_type<float>(0x%08Xu)", u);
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
    /* ps 1.x and samplers used without a dcl (ps 2.0 may skip it): 2D */
    for (size_t i = 0; i < c->sh->nins; ++i)
    {
        const Sm3Ins* in = &c->sh->ins[i];
        if ((in->op == SM3_TEX || in->op == SM3_TEXLDL || in->op == SM3_TEXLDD) && in->nsrc >= 2 &&
            in->src[1].type == SM3_SAMPLER && in->src[1].num < 20 && !c->tex_type[in->src[1].num])
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

/* the address of a relative access: a0.<c> or aL, plus the register number */
static void rel_index(const Sm3Reg* r, char* out, size_t n)
{
    if (r->rel_type == SM3_LOOP)
        snprintf(out, n, "aL + %u", r->num);
    else
        snprintf(out, n, "a0.%c + %u", COMP[r->rel_comp & 3], r->num);
}

/* a register as a float4 expression, before swizzle and modifier */
static void reg_value(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    char idx[48], f[4][40];
    switch (r->type)
    {
    case SM3_TEMP: snprintf(out, n, "r%u", r->num); return;
    case SM3_INPUT:
        if (r->rel)
        {
            fail(c, "relative input register");
            snprintf(out, n, "float4(0)");
            return;
        }
        snprintf(out, n, "v%u", r->num);
        return;
    case SM3_CONST:
        if (r->rel)
        {
            rel_index(r, idx, sizeof idx);
            snprintf(out, n, "crel(c, %s)", idx);
            return;
        }
        if (r->num < 256 && c->has_def[r->num])
        {
            for (int k = 0; k < 4; ++k)
                flt(c->def[r->num][k], f[k], sizeof f[k]);
            snprintf(out, n, "float4(%s, %s, %s, %s)", f[0], f[1], f[2], f[3]);
            return;
        }
        snprintf(out, n, "c[%u]", r->num);
        return;
    case SM3_CONSTINT:
        if (r->num < 16 && c->has_defi[r->num])
            snprintf(out, n, "float4(%d, %d, %d, %d)", c->defi[r->num][0], c->defi[r->num][1], c->defi[r->num][2],
                c->defi[r->num][3]);
        else
            snprintf(out, n, "float4(ci[%u])", r->num);
        return;
    case SM3_CONSTBOOL:
        if (c->has_defb[r->num & 15])
            snprintf(out, n, "float4(%s)", c->defb[r->num & 15] ? "1" : "0");
        else
            snprintf(out, n, "float4(cb[%u] != 0u ? 1.0 : 0.0)", r->num & 15);
        return;
    case SM3_ADDR:
        if (c->pixel)
            snprintf(out, n, "t%u", r->num); /* ps 1-2: a texture coordinate input */
        else
            snprintf(out, n, "float4(a0)");
        return;
    case SM3_LOOP: snprintf(out, n, "float4(aL)"); return;
    case SM3_MISCTYPE: snprintf(out, n, "%s", r->num == 0 ? "vpos" : "vface"); return;
    case SM3_PREDICATE: snprintf(out, n, "float4(p0)"); return;
    default:
        fail(c, "source register type %u", r->type);
        snprintf(out, n, "float4(0)");
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
            snprintf(out, n, "(cb[%u] != 0u)", r->num);
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

/* a source operand: the register, swizzled, modified */
static void src(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    char v[160], sw[8] = "";
    reg_value(c, r, v, sizeof v);
    if (r->swizzle != 0xE4)
    {
        sw[0] = '.';
        for (int k = 0; k < 4; ++k)
            sw[1 + k] = COMP[(r->swizzle >> (2 * k)) & 3];
        sw[5] = 0;
    }
    char x[200];
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

/* a scalar source: D3D wants a replicate swizzle; without one, w (as vs 1.x reads it) */
static void scalar(Ctx* c, const Sm3Reg* r, char* out, size_t n)
{
    char s[220];
    src(c, r, s, sizeof s);
    snprintf(out, n, "(%s).%c", s, r->swizzle == 0xE4 ? 'w' : 'x');
}

/* assigns the float4 expression `e` to the instruction's destination (mask, saturate) */
static void assign(Ctx* c, const Sm3Ins* in, const char* e)
{
    const Sm3Reg* d = &in->dst;
    char target[64], val[1400], mask[8] = "";
    if (d->shift)
        fail(c, "destination shift");
    int full = d->mask == 0xF || d->mask == 0;
    if (!full)
    {
        int k = 0;
        mask[k++] = '.';
        for (int i = 0; i < 4; ++i)
            if (d->mask & (1 << i))
                mask[k++] = COMP[i];
        mask[k] = 0;
    }
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
            /* oFog / oPts: scalars */
            line(c, "%s = (%s).x;", d->num == 1 ? "o_fog" : "o_pts", val);
            return;
        }
        break;
    case SM3_ATTROUT: snprintf(target, sizeof target, "o_d%u", d->num); break;
    case SM3_COLOROUT: snprintf(target, sizeof target, "oc%u", d->num); break;
    case SM3_DEPTHOUT:
        line(c, "odepth = (%s).x;", val);
        return;
    case SM3_ADDR:
        if (!c->pixel)
        {
            /* mova and vs 1.1 mov a0: round to nearest (vs 2+), as D3D does */
            line(c, "a0%s = int4(floor(%s + 0.5))%s;", mask, val, mask);
            return;
        }
        snprintf(target, sizeof target, "t%u", d->num);
        break;
    default:
        fail(c, "destination register type %u", d->type);
        return;
    }
    if (full)
        line(c, "%s = %s;", target, val);
    else
        line(c, "%s%s = (%s)%s;", target, mask, val, mask);
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

/* a texture sample: sampler register `s`, coordinates `coord` (float4 expression) */
static void sample(Ctx* c, const Sm3Ins* in, const char* coord, const char* extra, int project)
{
    const Sm3Reg* sr = &in->src[1];
    unsigned n = sr->num;
    if (sr->type != SM3_SAMPLER || n >= 20)
    {
        fail(c, "texture sample without a sampler");
        return;
    }
    unsigned type = c->tex_type[n] ? c->tex_type[n] : SM3_TEX_2D;
    const char* comps = type == SM3_TEX_2D ? "xy" : "xyz";
    char e[1100], tc[700];
    line(c, "{");
    c->depth++;
    line(c, "float4 tc = %s;", coord);
    if (project)
        snprintf(tc, sizeof tc, "tc.%s / tc.w", comps);
    else
        snprintf(tc, sizeof tc, "tc.%s", comps);
    snprintf(e, sizeof e, "tex%u.sample(smp%u, %s%s%s)", n, n, tc, extra ? ", " : "", extra ? extra : "");
    line(c, "float4 tv = %s;", e);
    line(c, "if (fx.srgb_mask & %uu) tv.rgb = srgb_to_linear(tv.rgb);", 1u << n);
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

/* a matrix multiply (m4x4 ...): `rows` dot products of `width` components against src1, src1+1 ... */
static void matrix(Ctx* c, const Sm3Ins* in, int width, int rows)
{
    char s0[220], r[4][220], e[1100];
    src(c, &in->src[0], s0, sizeof s0);
    for (int k = 0; k < rows; ++k)
    {
        Sm3Reg row = in->src[1];
        row.num = (uint16_t)(row.num + k);
        src(c, &row, r[k], sizeof r[k]);
    }
    const char* w = width == 4 ? "" : ".xyz";
    int len = snprintf(e, sizeof e, "float4(");
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

static void emit(Ctx* c, const Sm3Ins* in)
{
    char a[260], b[260], d[260], e[1200], f[260];
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
        case SM3_SLT: snprintf(e, sizeof e, "select(float4(0.0), float4(1.0), %s < %s)", a, b); break;
        default: snprintf(e, sizeof e, "select(float4(0.0), float4(1.0), %s >= %s)", a, b); break;
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
        snprintf(e, sizeof e, "select(%s, %s, %s %s)", d, b, a, in->op == SM3_CMP ? ">= 0.0" : "> 0.5");
        assign(c, in, e);
        return;
    case SM3_RCP: scalar(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "float4(1.0 / %s)", a); assign(c, in, e); return;
    case SM3_RSQ: scalar(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "float4(rsqrt(abs(%s)))", a); assign(c, in, e); return;
    case SM3_EXP: case SM3_EXPP:
        scalar(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "float4(exp2(%s))", a);
        assign(c, in, e);
        return;
    case SM3_LOG: case SM3_LOGP:
        scalar(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "float4(log2(abs(%s)))", a);
        assign(c, in, e);
        return;
    case SM3_POW:
        scalar(c, &in->src[0], a, sizeof a);
        scalar(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "float4(pow(abs(%s), %s))", a, b);
        assign(c, in, e);
        return;
    case SM3_DP3: case SM3_DP4:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        if (in->op == SM3_DP3)
            snprintf(e, sizeof e, "float4(dot((%s).xyz, (%s).xyz))", a, b);
        else
            snprintf(e, sizeof e, "float4(dot(%s, %s))", a, b);
        assign(c, in, e);
        return;
    case SM3_DP2ADD:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        scalar(c, &in->src[2], d, sizeof d);
        snprintf(e, sizeof e, "float4(dot((%s).xy, (%s).xy) + %s)", a, b, d);
        assign(c, in, e);
        return;
    case SM3_FRC: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "(%s - floor(%s))", a, a); assign(c, in, e); return;
    case SM3_ABS: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "abs(%s)", a); assign(c, in, e); return;
    case SM3_SGN: src(c, &in->src[0], a, sizeof a); snprintf(e, sizeof e, "sign(%s)", a); assign(c, in, e); return;
    case SM3_NRM:
        src(c, &in->src[0], a, sizeof a);
        line(c, "{");
        c->depth++;
        line(c, "float4 nv = %s;", a);
        line(c, "float nf = dot(nv.xyz, nv.xyz);");
        assign(c, in, "(nv * (nf != 0.0 ? rsqrt(nf) : FLT_MAX))");
        c->depth--;
        line(c, "}");
        return;
    case SM3_SINCOS:
        scalar(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "float4(cos(%s), sin(%s), 0.0, 0.0)", a, a);
        assign(c, in, e);
        return;
    case SM3_CRS:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "float4(cross((%s).xyz, (%s).xyz), 0.0)", a, b);
        assign(c, in, e);
        return;
    case SM3_LIT:
        src(c, &in->src[0], a, sizeof a);
        snprintf(e, sizeof e, "lit4(%s)", a);
        assign(c, in, e);
        return;
    case SM3_DST:
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[1], b, sizeof b);
        snprintf(e, sizeof e, "float4(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, b, a, b);
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
        snprintf(e, sizeof e, "%s(%s)", in->op == SM3_DSX ? "dfdx" : "dfdy", a);
        assign(c, in, e);
        return;
    case SM3_TEX:
        if (c->sh->major < 2)
        {
            fail(c, "ps 1.x texture instruction");
            return;
        }
        src(c, &in->src[0], a, sizeof a);
        if (!c->pixel)
        {
            fail(c, "texld in a vertex shader");
            return;
        }
        if (in->control == 2)
        {
            snprintf(f, sizeof f, "bias(tc.w)");
            sample(c, in, a, f, 0);
        }
        else
            sample(c, in, a, NULL, in->control == 1);
        return;
    case SM3_TEXLDL:
        src(c, &in->src[0], a, sizeof a);
        sample(c, in, a, "level(tc.w)", 0);
        return;
    case SM3_TEXLDD:
    {
        src(c, &in->src[0], a, sizeof a);
        src(c, &in->src[2], b, sizeof b);
        src(c, &in->src[3], d, sizeof d);
        unsigned n = in->src[1].num < 20 ? in->src[1].num : 0;
        unsigned type = c->tex_type[n];
        if (type == SM3_TEX_CUBE)
            snprintf(f, sizeof f, "gradientcube((%s).xyz, (%s).xyz)", b, d);
        else if (type == SM3_TEX_VOLUME)
            snprintf(f, sizeof f, "gradient3d((%s).xyz, (%s).xyz)", b, d);
        else
            snprintf(f, sizeof f, "gradient2d((%s).xy, (%s).xy)", b, d);
        sample(c, in, a, f, 0);
        return;
    }
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
            line(c, "if ((%s).%s < 0.0) discard_fragment();", a, m);
        else
            line(c, "if (any((%s).%s < 0.0)) discard_fragment();", a, m);
        return;
    }
    case SM3_IF:
        bool_value(c, &in->src[0], a, sizeof a);
        line(c, "if (%s) {", a);
        c->depth++;
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
            c->depth++;
        }
        else
            line(c, "if (%s %s %s) break;", a, op, b);
        return;
    }
    case SM3_ELSE:
        c->depth--;
        line(c, "} else {");
        c->depth++;
        return;
    case SM3_ENDIF: case SM3_ENDREP: case SM3_ENDLOOP:
        c->depth--;
        line(c, "}");
        return;
    case SM3_BREAK: line(c, "break;"); return;
    case SM3_BREAKP:
        bool_value(c, &in->src[0], a, sizeof a);
        line(c, "if (%s) break;", a);
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
            snprintf(ix, sizeof ix, "ci[%u].x", ir->num);
            snprintf(iy, sizeof iy, "ci[%u].y", ir->num);
            snprintf(iz, sizeof iz, "ci[%u].z", ir->num);
        }
        int k = ++c->loops;
        if (in->op == SM3_REP)
            line(c, "for (int lk%d = 0; lk%d < %s; ++lk%d) {", k, k, ix, k);
        else
            line(c, "for (int lk%d = 0, aL = %s; lk%d < %s; ++lk%d, aL += %s) {", k, iy, k, ix, k, iz);
        c->depth++;
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
        char m[6] = "";
        int k = 0;
        for (int i = 0; i < 4; ++i)
            if (in->dst.mask & (1 << i))
                m[k++] = COMP[i];
        m[k] = 0;
        line(c, "p0.%s = (%s %s %s).%s;", m, a, op, b, m);
        return;
    }
    case SM3_RET:
        return; /* the end of main; subroutines are refused at CALL */
    default:
        fail(c, "instruction %s", sm3_opname(in->op) ? sm3_opname(in->op) : "?");
        return;
    }
}

static void body(Ctx* c)
{
    for (size_t i = 0; i < c->sh->nins && !c->failed; ++i)
        emit(c, &c->sh->ins[i]);
    if (c->depth)
        fail(c, "unbalanced blocks");
}

/* the helpers both functions share */
static const char PRELUDE[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "struct Sm3Fix { float4 posfix; float alpha_ref; uint alpha_func; uint srgb_write; uint srgb_mask; };\n"
    "\n"
    "static float3 srgb_to_linear(float3 c) { return select(pow((c + 0.055) / 1.055, 2.4), c / 12.92, c <= 0.04045); }\n"
    "static float3 linear_to_srgb(float3 c) { c = saturate(c); return select(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, c <= 0.0031308); }\n"
    "static float4 lit4(float4 s)\n"
    "{\n"
    "    float4 d = float4(1.0, 0.0, 0.0, 1.0);\n"
    "    if (s.x > 0.0) { d.y = s.x; if (s.y > 0.0) d.z = pow(s.y, clamp(s.w, -127.9961, 127.9961)); }\n"
    "    return d;\n"
    "}\n"
    "static bool alpha_pass(uint f, float a, float ref)\n"
    "{\n"
    "    switch (f) { case 1: return false; case 2: return a < ref; case 3: return a == ref; case 4: return a <= ref;\n"
    "                 case 5: return a > ref; case 6: return a != ref; case 7: return a >= ref; default: return true; }\n"
    "}\n";

/* relative constant reads: the shader's def registers win over the bound constants */
static void crel_function(Ctx* c, const char* name, unsigned count)
{
    put(c->b, "static float4 %s(constant float4* c, int i)\n{\n", name);
    char f[4][40];
    for (unsigned k = 0; k < 256; ++k)
        if (c->has_def[k])
        {
            for (int j = 0; j < 4; ++j)
                flt(c->def[k][j], f[j], sizeof f[j]);
            put(c->b, "    if (i == %u) return float4(%s, %s, %s, %s);\n", k, f[0], f[1], f[2], f[3]);
        }
    put(c->b, "    return (uint)i < %uu ? c[i] : float4(0.0);\n}\n", count);
}

static int uses_rel_const(const Sm3Shader* s)
{
    for (size_t i = 0; i < s->nins; ++i)
        for (unsigned k = 0; k < s->ins[i].nsrc; ++k)
            if (s->ins[i].src[k].type == SM3_CONST && s->ins[i].src[k].rel)
                return 1;
    return 0;
}

static void texture_params(Ctx* c, Buf* b, unsigned first, unsigned count, unsigned base)
{
    for (unsigned n = first; n < first + count; ++n)
    {
        if (!c->tex_type[n])
            continue;
        const char* t = c->tex_type[n] == SM3_TEX_CUBE ? "texturecube" : c->tex_type[n] == SM3_TEX_VOLUME ? "texture3d" : "texture2d";
        put(b, ",\n    %s<float> tex%u [[texture(%u)]], sampler smp%u [[sampler(%u)]]", t, n - first + base, n - first,
            n - first + base, n - first);
    }
}

int sm3_msl_vtx_debug;

char* sm3_msl_pair(const Sm3Shader* vs, const Sm3Shader* ps, const uint8_t in_kind[16], char* err, size_t errn)
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
        snprintf(err, errn, "shader model 1 (vs_%u_%u, ps_%u_%u) is not translated here", vs->major, vs->minor, ps->major,
            ps->minor);
        return NULL;
    }
    Buf b = { 0 };
    put(&b, "%s", PRELUDE);
    Sem outs[16], ins[16];
    unsigned nout = vs_outputs(vs, outs, 16), nin = ps_inputs(ps, ins, 16);
    char name[32];

    /* --- vertex function --- */
    Ctx v = { .sh = vs, .b = &b, .pixel = 0, .err = err, .errn = errn };
    prepass(&v);
    if (uses_rel_const(vs))
        crel_function(&v, "crel", 256);
    int vin = 0;
    for (size_t i = 0; i < vs->nins; ++i)
        if (vs->ins[i].op == SM3_DCL && vs->ins[i].dst.type == SM3_INPUT)
        {
            if (!vin++)
                put(&b, "\nstruct VIn\n{\n");
        {
            unsigned r = vs->ins[i].dst.num;
            unsigned k = in_kind && r < 16 ? in_kind[r] : SM3_IN_FLOAT;
            put(&b, "    %s v%u [[attribute(%u)]];\n", k == SM3_IN_INT ? "int4" : k == SM3_IN_UINT ? "uint4" : "float4", r, r);
        }
        }
    if (vin)
        put(&b, "};\n");
    put(&b, "\nstruct VOut\n{\n    float4 pos [[position]];\n");
    int psize = 0;
    for (unsigned k = 0; k < nout; ++k)
    {
        if (is_position(&outs[k]))
            continue;
        if (outs[k].usage == SM3_U_PSIZE)
        {
            psize = 1;
            continue;
        }
        sem_name(&outs[k], name, sizeof name);
        put(&b, "    float4 %s [[user(%s)]];\n", name, name);
    }
    if (psize)
        put(&b, "    float psize [[point_size]];\n");
    put(&b, "};\n\nvertex VOut vs_main(");
    if (vin)
        put(&b, "VIn in [[stage_in]],\n    ");
    put(&b, "constant float4* c [[buffer(16)]], constant int4* ci [[buffer(17)]], constant uint* cb [[buffer(18)]],\n"
            "    constant Sm3Fix& fx [[buffer(19)]]");
    /* vertex samplers are s0..s3 in a vs_3_0 (D3DVERTEXTEXTURESAMPLER0..3) */
    texture_params(&v, &b, 0, 4, 0);
    if (sm3_msl_vtx_debug)
        put(&b, ",\n    device float4* dbg_pos [[buffer(29)]], uint dbg_vid [[vertex_id]], uint dbg_iid [[instance_id]],\n"
                "    constant uint& dbg_n [[buffer(28)]]");
    put(&b, ")\n{\n");
    int nt = max_temp(vs);
    for (int k = 0; k <= nt; ++k)
        put(&b, "    float4 r%d = float4(0.0);\n", k);
    put(&b, "    int4 a0 = int4(0); int aL = 0; bool4 p0 = bool4(false);\n");
    for (size_t i = 0; i < vs->nins; ++i)
        if (vs->ins[i].op == SM3_DCL && vs->ins[i].dst.type == SM3_INPUT)
            put(&b, "    float4 v%u = float4(in.v%u);\n", vs->ins[i].dst.num, vs->ins[i].dst.num);
    if (vs->major >= 3)
        put(&b, "    float4 o[12] = { float4(0.0) };\n");
    else
        put(&b, "    float4 o_pos = float4(0.0), o_d0 = float4(0.0), o_d1 = float4(0.0);\n"
                "    float4 o_t0 = float4(0.0), o_t1 = float4(0.0), o_t2 = float4(0.0), o_t3 = float4(0.0);\n"
                "    float4 o_t4 = float4(0.0), o_t5 = float4(0.0), o_t6 = float4(0.0), o_t7 = float4(0.0);\n"
                "    float o_fog = 0.0, o_pts = 1.0;\n");
    put(&b, "    (void)a0; (void)aL; (void)p0; (void)ci; (void)cb;\n");
    body(&v);
    if (v.failed)
        goto failed;
    put(&b, "    VOut out;\n");
    for (unsigned k = 0; k < nout; ++k)
    {
        char from[32];
        if (vs->major >= 3)
            snprintf(from, sizeof from, "o[%u]", outs[k].reg);
        else if (outs[k].regtype == SM3_RASTOUT)
            snprintf(from, sizeof from, "%s", outs[k].reg == 0 ? "o_pos" : outs[k].reg == 1 ? "float4(o_fog, 0.0, 0.0, 0.0)" : "float4(o_pts)");
        else if (outs[k].regtype == SM3_ATTROUT)
            snprintf(from, sizeof from, "saturate(o_d%u)", outs[k].reg); /* vs 1-2 clamp colors */
        else
            snprintf(from, sizeof from, "o_t%u", outs[k].reg);
        if (is_position(&outs[k]))
            put(&b, "    out.pos = %s;\n", from);
        else if (outs[k].usage == SM3_U_PSIZE)
            put(&b, "    out.psize = (%s).x;\n", from);
        else
        {
            sem_name(&outs[k], name, sizeof name);
            put(&b, "    out.%s = %s;\n", name, from);
        }
    }
    if (sm3_msl_vtx_debug)
        put(&b, "    dbg_pos[dbg_iid * dbg_n + dbg_vid] = out.pos;\n");
    put(&b, "    out.pos.xy += fx.posfix.xy * out.pos.w;\n    return out;\n}\n");

    /* --- fragment function --- */
    {
        Ctx p = { .sh = ps, .b = &b, .pixel = 1, .err = err, .errn = errn };
        prepass(&p);
        if (uses_rel_const(ps))
            crel_function(&p, "crel_ps", 224);
        int pin = 0;
        uint8_t linked[16] = { 0 };
        for (unsigned k = 0; k < nin; ++k)
            for (unsigned j = 0; j < nout; ++j)
                if (!is_position(&outs[j]) && outs[j].usage != SM3_U_PSIZE && outs[j].usage == ins[k].usage &&
                    outs[j].index == ins[k].index)
                {
                    if (!pin++)
                        put(&b, "\nstruct PIn\n{\n");
                    linked[k] = 1;
                    sem_name(&ins[k], name, sizeof name);
                    put(&b, "    float4 %s [[user(%s)%s]];\n", name, name, ins[k].centroid ? ", centroid_perspective" : "");
                    break;
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
                colors = in->dst.num + 1;
        }
        put(&b, "\nstruct POut\n{\n");
        for (int k = 0; k < colors; ++k)
            put(&b, "    float4 c%d [[color(%d)]];\n", k, k);
        if (depth_out)
            put(&b, "    float depth [[depth(any)]];\n");
        put(&b, "};\n\nfragment POut fs_main(");
        if (pin)
            put(&b, "PIn in [[stage_in]], ");
        put(&b, "float4 fragpos [[position]], bool front [[front_facing]],\n"
                "    constant float4* c [[buffer(0)]], constant int4* ci [[buffer(1)]], constant uint* cb [[buffer(2)]],\n"
                "    constant Sm3Fix& fx [[buffer(3)]]");
        texture_params(&p, &b, 0, 16, 0);
        put(&b, ")\n{\n");
        int pt = max_temp(ps);
        for (int k = 0; k <= pt; ++k)
            put(&b, "    float4 r%d = float4(0.0);\n", k);
        put(&b, "    int4 a0 = int4(0); int aL = 0; bool4 p0 = bool4(false);\n");
        put(&b, "    float4 vpos = float4(floor(fragpos.xy), 0.0, 0.0), vface = float4(front ? 1.0 : -1.0);\n");
        for (unsigned k = 0; k < nin; ++k)
        {
            const char* reg = ins[k].regtype == SM3_ADDR ? "t" : "v";
            sem_name(&ins[k], name, sizeof name);
            if (linked[k])
                put(&b, "    float4 %s%u = in.%s;\n", reg, ins[k].reg, name);
            else
                put(&b, "    float4 %s%u = float4(0.0); /* %s: not written by the vertex shader */\n", reg, ins[k].reg, name);
        }
        for (int k = 0; k < colors; ++k)
            put(&b, "    float4 oc%d = float4(0.0);\n", k);
        put(&b, "    float odepth = fragpos.z;\n");
        put(&b, "    (void)a0; (void)aL; (void)p0; (void)ci; (void)cb; (void)vpos; (void)vface; (void)odepth;\n");
        /* relative constants in a pixel shader read the pixel table */
        if (uses_rel_const(ps))
            put(&b, "#define crel crel_ps\n");
        body(&p);
        if (uses_rel_const(ps))
            put(&b, "#undef crel\n");
        if (p.failed)
            goto failed;
        put(&b, "    if (!alpha_pass(fx.alpha_func, oc0.a, fx.alpha_ref)) discard_fragment();\n");
        put(&b, "    if (fx.srgb_write) oc0.rgb = linear_to_srgb(oc0.rgb);\n");
        put(&b, "    POut out;\n");
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
