/* Direct3D 9 shader bytecode parser and disassembler. See sm3.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm3.h"

/* has a destination, and how many sources (shader model 1.x, which has no instruction length) */
static const struct
{
    const char* name;
    uint8_t dst, nsrc;
} OPS[] = {
    [SM3_NOP] = { "nop", 0, 0 }, [SM3_MOV] = { "mov", 1, 1 }, [SM3_ADD] = { "add", 1, 2 },
    [SM3_SUB] = { "sub", 1, 2 }, [SM3_MAD] = { "mad", 1, 3 }, [SM3_MUL] = { "mul", 1, 2 },
    [SM3_RCP] = { "rcp", 1, 1 }, [SM3_RSQ] = { "rsq", 1, 1 }, [SM3_DP3] = { "dp3", 1, 2 },
    [SM3_DP4] = { "dp4", 1, 2 }, [SM3_MIN] = { "min", 1, 2 }, [SM3_MAX] = { "max", 1, 2 },
    [SM3_SLT] = { "slt", 1, 2 }, [SM3_SGE] = { "sge", 1, 2 }, [SM3_EXP] = { "exp", 1, 1 },
    [SM3_LOG] = { "log", 1, 1 }, [SM3_LIT] = { "lit", 1, 1 }, [SM3_DST] = { "dst", 1, 2 },
    [SM3_LRP] = { "lrp", 1, 3 }, [SM3_FRC] = { "frc", 1, 1 }, [SM3_M4x4] = { "m4x4", 1, 2 },
    [SM3_M4x3] = { "m4x3", 1, 2 }, [SM3_M3x4] = { "m3x4", 1, 2 }, [SM3_M3x3] = { "m3x3", 1, 2 },
    [SM3_M3x2] = { "m3x2", 1, 2 }, [SM3_CALL] = { "call", 0, 1 }, [SM3_CALLNZ] = { "callnz", 0, 2 },
    [SM3_LOOP_OP] = { "loop", 0, 2 }, [SM3_RET] = { "ret", 0, 0 }, [SM3_ENDLOOP] = { "endloop", 0, 0 },
    [SM3_LABEL_OP] = { "label", 0, 1 }, [SM3_DCL] = { "dcl", 1, 0 }, [SM3_POW] = { "pow", 1, 2 },
    [SM3_CRS] = { "crs", 1, 2 }, [SM3_SGN] = { "sgn", 1, 3 }, [SM3_ABS] = { "abs", 1, 1 },
    [SM3_NRM] = { "nrm", 1, 1 }, [SM3_SINCOS] = { "sincos", 1, 3 }, [SM3_REP] = { "rep", 0, 1 },
    [SM3_ENDREP] = { "endrep", 0, 0 }, [SM3_IF] = { "if", 0, 1 }, [SM3_IFC] = { "if", 0, 2 },
    [SM3_ELSE] = { "else", 0, 0 }, [SM3_ENDIF] = { "endif", 0, 0 }, [SM3_BREAK] = { "break", 0, 0 },
    [SM3_BREAKC] = { "break", 0, 2 }, [SM3_MOVA] = { "mova", 1, 1 }, [SM3_DEFB] = { "defb", 1, 0 },
    [SM3_DEFI] = { "defi", 1, 0 }, [SM3_TEXCOORD] = { "texcoord", 1, 0 }, [SM3_TEXKILL] = { "texkill", 1, 0 },
    [SM3_TEX] = { "texld", 1, 2 }, [SM3_TEXBEM] = { "texbem", 1, 1 }, [SM3_TEXBEML] = { "texbeml", 1, 1 },
    [SM3_TEXREG2AR] = { "texreg2ar", 1, 1 }, [SM3_TEXREG2GB] = { "texreg2gb", 1, 1 },
    [SM3_TEXM3x2PAD] = { "texm3x2pad", 1, 1 }, [SM3_TEXM3x2TEX] = { "texm3x2tex", 1, 1 },
    [SM3_TEXM3x3PAD] = { "texm3x3pad", 1, 1 }, [SM3_TEXM3x3TEX] = { "texm3x3tex", 1, 1 },
    [SM3_TEXM3x3SPEC] = { "texm3x3spec", 1, 2 }, [SM3_TEXM3x3VSPEC] = { "texm3x3vspec", 1, 1 },
    [SM3_EXPP] = { "expp", 1, 1 }, [SM3_LOGP] = { "logp", 1, 1 }, [SM3_CND] = { "cnd", 1, 3 },
    [SM3_DEF] = { "def", 1, 0 }, [SM3_TEXREG2RGB] = { "texreg2rgb", 1, 1 }, [SM3_TEXDP3TEX] = { "texdp3tex", 1, 1 },
    [SM3_TEXM3x2DEPTH] = { "texm3x2depth", 1, 1 }, [SM3_TEXDP3] = { "texdp3", 1, 1 },
    [SM3_TEXM3x3] = { "texm3x3", 1, 1 }, [SM3_TEXDEPTH] = { "texdepth", 1, 0 }, [SM3_CMP] = { "cmp", 1, 3 },
    [SM3_BEM] = { "bem", 1, 2 }, [SM3_DP2ADD] = { "dp2add", 1, 3 }, [SM3_DSX] = { "dsx", 1, 1 },
    [SM3_DSY] = { "dsy", 1, 1 }, [SM3_TEXLDD] = { "texldd", 1, 4 }, [SM3_SETP] = { "setp", 1, 2 },
    [SM3_TEXLDL] = { "texldl", 1, 2 }, [SM3_BREAKP] = { "breakp", 0, 1 },
};
#define NOPS (sizeof OPS / sizeof OPS[0])

const char* sm3_opname(unsigned op) { return op < NOPS ? OPS[op].name : NULL; }

static unsigned reg_type(uint32_t t) { return ((t >> 28) & 7u) | ((t >> 8) & 0x18u); }

static void parse_dst(uint32_t t, Sm3Reg* r)
{
    memset(r, 0, sizeof *r);
    r->type = (uint8_t)reg_type(t);
    r->num = (uint16_t)(t & 0x7FF);
    r->mask = (uint8_t)((t >> 16) & 0xF);
    r->mod = (uint8_t)((t >> 20) & 0xF);
    r->shift = (int8_t)((int32_t)(t << 4) >> 28); /* bits 24..27, signed */
    r->rel = (t >> 13) & 1u;
    r->swizzle = 0xE4;
}

static void parse_src(uint32_t t, Sm3Reg* r)
{
    memset(r, 0, sizeof *r);
    r->type = (uint8_t)reg_type(t);
    r->num = (uint16_t)(t & 0x7FF);
    r->swizzle = (uint8_t)((t >> 16) & 0xFF);
    r->mod = (uint8_t)((t >> 24) & 0xF);
    r->rel = (t >> 13) & 1u;
    r->mask = 0xF;
}

/* a relative-address token: the register and the component it selects */
static void parse_rel(uint32_t t, Sm3Reg* r)
{
    r->rel_type = (uint8_t)reg_type(t);
    r->rel_comp = (uint8_t)((t >> 16) & 3u);
}

static int fail(char* err, size_t n, const char* what, size_t at)
{
    if (err && n)
        snprintf(err, n, "%s at token %zu", what, at);
    return 0;
}

int sm3_parse(const uint32_t* tk, size_t n, Sm3Shader* out, char* err, size_t errn)
{
    memset(out, 0, sizeof *out);
    if (n < 2)
        return fail(err, errn, "too short", 0);
    uint32_t v = tk[0];
    if ((v >> 16) != 0xFFFE && (v >> 16) != 0xFFFF)
        return fail(err, errn, "not a shader version token", 0);
    out->pixel = (v >> 16) == 0xFFFF;
    out->major = (uint8_t)((v >> 8) & 0xFF);
    out->minor = (uint8_t)(v & 0xFF);
    int sm2 = out->major >= 2;
    size_t cap = 64;
    out->ins = (Sm3Ins*)calloc(cap, sizeof *out->ins);
    for (size_t p = 1; p < n;)
    {
        uint32_t t = tk[p];
        unsigned op = t & 0xFFFF;
        if (op == SM3_END)
            return 1;
        if (op == SM3_COMMENT)
        {
            uint32_t len = (t >> 16) & 0x7FFF;
            if (p + 1 + len > n)
                return fail(err, errn, "comment past the end", p);
            if (len >= 1 && tk[p + 1] == 0x42415443u && !out->ctab) /* 'CTAB' */
            {
                out->ctab = &tk[p + 2];
                out->ctab_size = 4u * (len - 1);
            }
            p += 1 + len;
            continue;
        }
        if (op == SM3_PHASE)
        {
            p++;
            continue;
        }
        if (op >= NOPS || !OPS[op].name)
            return fail(err, errn, "unknown opcode", p);
        if (out->nins == cap)
        {
            cap *= 2;
            out->ins = (Sm3Ins*)realloc(out->ins, cap * sizeof *out->ins);
        }
        Sm3Ins* i = &out->ins[out->nins++];
        memset(i, 0, sizeof *i);
        i->op = (uint16_t)op;
        i->control = (uint8_t)((t >> 16) & 0xFF);
        i->predicated = (t >> 28) & 1u;
        size_t len = sm2 ? (t >> 24) & 0xF : 0, q = p + 1, end;
        if (sm2)
            end = q + len;
        else
        {
            /* 1.x: walk the parameter tokens (bit 31 set) */
            end = q;
            while (end < n && (tk[end] & 0x80000000u))
                end++;
        }
        if (end > n)
            return fail(err, errn, "instruction past the end", p);
        if (op == SM3_DCL)
        {
            if (q + 2 > end)
                return fail(err, errn, "short dcl", p);
            uint32_t u = tk[q++];
            i->usage = (uint8_t)(u & 0x1F);
            i->usage_index = (uint8_t)((u >> 16) & 0xF);
            i->tex_type = (uint8_t)((u >> 27) & 0xF);
            i->has_dst = 1;
            parse_dst(tk[q++], &i->dst);
        }
        else if (op == SM3_DEF || op == SM3_DEFI || op == SM3_DEFB)
        {
            unsigned k = op == SM3_DEFB ? 1 : 4;
            if (q + 1 + k > end)
                return fail(err, errn, "short def", p);
            i->has_dst = 1;
            parse_dst(tk[q++], &i->dst);
            for (unsigned c = 0; c < k; ++c)
                i->value[c] = tk[q++];
        }
        else
        {
            if (OPS[op].dst && q < end)
            {
                i->has_dst = 1;
                parse_dst(tk[q++], &i->dst);
                if (i->dst.rel && sm2 && q < end)
                    parse_rel(tk[q++], &i->dst);
            }
            if (i->predicated && q < end)
            {
                parse_src(tk[q++], &i->pred);
            }
            while (q < end && i->nsrc < 4)
            {
                Sm3Reg* r = &i->src[i->nsrc++];
                parse_src(tk[q++], r);
                if (r->rel)
                {
                    if (sm2 && q < end)
                        parse_rel(tk[q++], r);
                    else
                        r->rel_type = SM3_ADDR, r->rel_comp = 0; /* vs 1.1: a0.x, implied */
                }
            }
        }
        p = end;
    }
    return fail(err, errn, "no end token", n);
}

void sm3_free(Sm3Shader* s)
{
    free(s->ins);
    memset(s, 0, sizeof *s);
}

/* --- text ------------------------------------------------------------------------------------------ */

static int reg_name(const Sm3Shader* s, const Sm3Reg* r, char* o, size_t n)
{
    unsigned num = r->num;
    switch (r->type)
    {
    case SM3_TEMP: return snprintf(o, n, "r%u", num);
    case SM3_INPUT: return snprintf(o, n, "v%u", num);
    case SM3_CONST: return snprintf(o, n, "c%u", num);
    case SM3_CONST2: return snprintf(o, n, "c%u", num + 2048);
    case SM3_CONST3: return snprintf(o, n, "c%u", num + 4096);
    case SM3_CONST4: return snprintf(o, n, "c%u", num + 6144);
    case SM3_ADDR: return snprintf(o, n, s->pixel ? "t%u" : "a%u", num);
    case SM3_RASTOUT: return snprintf(o, n, "%s", num == 0 ? "oPos" : num == 1 ? "oFog" : "oPts");
    case SM3_ATTROUT: return snprintf(o, n, "oD%u", num);
    case SM3_OUTPUT: return snprintf(o, n, s->major >= 3 ? "o%u" : "oT%u", num);
    case SM3_CONSTINT: return snprintf(o, n, "i%u", num);
    case SM3_COLOROUT: return snprintf(o, n, "oC%u", num);
    case SM3_DEPTHOUT: return snprintf(o, n, "oDepth");
    case SM3_SAMPLER: return snprintf(o, n, "s%u", num);
    case SM3_CONSTBOOL: return snprintf(o, n, "b%u", num);
    case SM3_LOOP: return snprintf(o, n, "aL");
    case SM3_TEMPFLOAT16: return snprintf(o, n, "half%u", num);
    case SM3_MISCTYPE: return snprintf(o, n, "%s", num == 0 ? "vPos" : "vFace");
    case SM3_LABEL: return snprintf(o, n, "l%u", num);
    case SM3_PREDICATE: return snprintf(o, n, "p%u", num);
    default: return snprintf(o, n, "?%u_%u", r->type, num);
    }
}

static const char COMP[] = "xyzw";

static int rel_text(const Sm3Reg* r, char* o, size_t n)
{
    if (!r->rel)
        return 0;
    if (r->rel_type == SM3_LOOP)
        return snprintf(o, n, "[aL]");
    return snprintf(o, n, "[a0.%c]", COMP[r->rel_comp & 3]);
}

/* a constant register with relative addressing prints as c[a0.x + 4] */
static int src_body(const Sm3Shader* s, const Sm3Reg* r, char* o, size_t n)
{
    char name[32];
    reg_name(s, r, name, sizeof name);
    if (r->rel && (r->type == SM3_CONST || r->type == SM3_INPUT || r->type == SM3_OUTPUT))
    {
        char rel[16];
        rel_text(r, rel, sizeof rel);
        rel[strlen(rel) - 1] = 0; /* drop ']' */
        return snprintf(o, n, "%c%s + %u]", name[0], rel, r->num);
    }
    return snprintf(o, n, "%s", name);
}

static void dst_text(const Sm3Shader* s, const Sm3Ins* i, char* o, size_t n)
{
    const Sm3Reg* r = &i->dst;
    int k = src_body(s, r, o, n);
    if (r->mask != 0xF && r->mask && i->op != SM3_DCL)
    {
        o[k++] = '.';
        for (int c = 0; c < 4; ++c)
            if (r->mask & (1 << c))
                o[k++] = COMP[c];
        o[k] = 0;
    }
}

static void src_text(const Sm3Shader* s, const Sm3Reg* r, char* o, size_t n)
{
    char body[48], sw[8] = "";
    src_body(s, r, body, sizeof body);
    if (r->swizzle != 0xE4)
    {
        char c[4];
        for (int k = 0; k < 4; ++k)
            c[k] = COMP[(r->swizzle >> (2 * k)) & 3];
        if (c[0] == c[1] && c[1] == c[2] && c[2] == c[3])
            snprintf(sw, sizeof sw, ".%c", c[0]);
        else
            snprintf(sw, sizeof sw, ".%c%c%c%c", c[0], c[1], c[2], c[3]);
    }
    switch (r->mod)
    {
    case SM3_SRC_NEG: snprintf(o, n, "-%s%s", body, sw); break;
    case SM3_SRC_BIAS: snprintf(o, n, "%s_bias%s", body, sw); break;
    case SM3_SRC_BIASNEG: snprintf(o, n, "-%s_bias%s", body, sw); break;
    case SM3_SRC_SIGN: snprintf(o, n, "%s_bx2%s", body, sw); break;
    case SM3_SRC_SIGNNEG: snprintf(o, n, "-%s_bx2%s", body, sw); break;
    case SM3_SRC_COMP: snprintf(o, n, "1-%s%s", body, sw); break;
    case SM3_SRC_X2: snprintf(o, n, "%s_x2%s", body, sw); break;
    case SM3_SRC_X2NEG: snprintf(o, n, "-%s_x2%s", body, sw); break;
    case SM3_SRC_DZ: snprintf(o, n, "%s_dz%s", body, sw); break;
    case SM3_SRC_DW: snprintf(o, n, "%s_dw%s", body, sw); break;
    case SM3_SRC_ABS: snprintf(o, n, "%s_abs%s", body, sw); break;
    case SM3_SRC_ABSNEG: snprintf(o, n, "-%s_abs%s", body, sw); break;
    case SM3_SRC_NOT: snprintf(o, n, "!%s%s", body, sw); break;
    default: snprintf(o, n, "%s%s", body, sw); break;
    }
}

static const char* const USAGE[] = { "position", "blendweight", "blendindices", "normal", "psize", "texcoord",
    "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth", "sample" };
static const char* const CMPS[] = { "", "_gt", "_eq", "_ge", "_lt", "_ne", "_le", "" };

void sm3_format(const Sm3Shader* s, const Sm3Ins* i, char* out, size_t n)
{
    char buf[96];
    size_t k = 0;
#define PUT(...) (k += (size_t)snprintf(out + k, k < n ? n - k : 0, __VA_ARGS__))
    if (i->predicated)
    {
        src_text(s, &i->pred, buf, sizeof buf);
        PUT("(%s) ", buf);
    }
    const char* name = sm3_opname(i->op);
    if (i->op == SM3_DCL)
    {
        if (i->dst.type == SM3_SAMPLER)
            PUT("dcl_%s", i->tex_type == SM3_TEX_CUBE ? "cube" : i->tex_type == SM3_TEX_VOLUME ? "volume" : "2d");
        else if (s->pixel && s->major < 3 && i->dst.type != SM3_MISCTYPE)
            PUT("dcl");
        else if (i->dst.type == SM3_MISCTYPE)
            PUT("dcl");
        else
        {
            PUT("dcl_%s", i->usage < sizeof USAGE / sizeof USAGE[0] ? USAGE[i->usage] : "?");
            if (i->usage_index)
                PUT("%u", i->usage_index);
        }
        if (i->dst.mod & SM3_DST_CENTROID)
            PUT("_centroid");
        dst_text(s, i, buf, sizeof buf);
        int masked = i->dst.mask && i->dst.mask != 0xF && i->dst.type != SM3_SAMPLER;
        PUT(" %s", buf);
        if (masked)
        {
            PUT(".");
            for (int c = 0; c < 4; ++c)
                if (i->dst.mask & (1 << c))
                    PUT("%c", COMP[c]);
        }
        return;
    }
    if (i->op == SM3_DEF || i->op == SM3_DEFI || i->op == SM3_DEFB)
    {
        dst_text(s, i, buf, sizeof buf);
        PUT("%s %s", name, buf);
        if (i->op == SM3_DEFB)
            PUT(", %s", i->value[0] ? "true" : "false");
        else if (i->op == SM3_DEFI)
            PUT(", %d, %d, %d, %d", (int)i->value[0], (int)i->value[1], (int)i->value[2], (int)i->value[3]);
        else
        {
            float f[4];
            memcpy(f, i->value, 16);
            PUT(", %g, %g, %g, %g", f[0], f[1], f[2], f[3]);
        }
        return;
    }
    PUT("%s", name ? name : "?");
    if (i->op == SM3_IFC || i->op == SM3_BREAKC || i->op == SM3_SETP)
        PUT("%s", CMPS[i->control & 7]);
    if (i->op == SM3_TEX && s->major >= 2)
    {
        if (i->control == 1)
            PUT("p");
        else if (i->control == 2)
            PUT("b");
    }
    if (i->has_dst)
    {
        if (i->dst.mod & SM3_DST_SATURATE)
            PUT("_sat");
        if (i->dst.mod & SM3_DST_PP)
            PUT("_pp");
        if (i->dst.mod & SM3_DST_CENTROID)
            PUT("_centroid");
        dst_text(s, i, buf, sizeof buf);
        PUT(" %s", buf);
    }
    for (unsigned j = 0; j < i->nsrc; ++j)
    {
        src_text(s, &i->src[j], buf, sizeof buf);
        PUT("%s%s", (i->has_dst || j) ? ", " : " ", buf);
    }
#undef PUT
}
