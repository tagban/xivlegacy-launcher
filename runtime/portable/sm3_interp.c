/* A CPU interpreter for Direct3D 9 vertex shaders. See sm3_interp.h. */
#include "sm3_interp.h"

#include <math.h>
#include <string.h>

typedef struct
{
    const Sm3Shader* s;
    float c[256][4];
    int32_t ci[16][4];
    uint32_t cb[16];
    const float (*v)[4];
    float r[32][4];
    float o[12][4];
    float opos[4];
    int32_t a0[4];
    int32_t aL;
    int p0[4];
    int pos_out; /* the o register declared POSITION (vs 3), or -1 */
    int unsupported;
} Vm;

static float sat(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* the register a source or destination names, as a float4 pointer (NULL: not addressable) */
static float* reg_ptr(Vm* m, const Sm3Reg* r, float tmp[4])
{
    int n = r->num;
    if (r->rel)
        n += r->rel_type == SM3_LOOP ? m->aL : m->a0[r->rel_comp & 3];
    switch (r->type)
    {
    case SM3_TEMP: return n >= 0 && n < 32 ? m->r[n] : NULL;
    case SM3_INPUT:
        if (n >= 0 && n < 16)
        {
            memcpy(tmp, m->v[n], 16);
            return tmp;
        }
        return NULL;
    case SM3_CONST:
        if (n >= 0 && n < 256)
        {
            memcpy(tmp, m->c[n], 16);
            return tmp;
        }
        memset(tmp, 0, 16);
        return tmp;
    case SM3_CONSTINT:
        for (int k = 0; k < 4; ++k)
            tmp[k] = n >= 0 && n < 16 ? (float)m->ci[n][k] : 0;
        return tmp;
    case SM3_CONSTBOOL:
        for (int k = 0; k < 4; ++k)
            tmp[k] = n >= 0 && n < 16 && m->cb[n] ? 1.0f : 0.0f;
        return tmp;
    case SM3_ADDR:
        for (int k = 0; k < 4; ++k)
            tmp[k] = (float)m->a0[k];
        return tmp;
    case SM3_LOOP:
        tmp[0] = tmp[1] = tmp[2] = tmp[3] = (float)m->aL;
        return tmp;
    case SM3_OUTPUT: return n >= 0 && n < 12 ? m->o[n] : NULL;
    case SM3_RASTOUT: return n == 0 ? m->opos : NULL;
    case SM3_ATTROUT: return n >= 0 && n < 2 ? m->o[10 + n] : NULL;
    case SM3_PREDICATE:
        for (int k = 0; k < 4; ++k)
            tmp[k] = (float)m->p0[k];
        return tmp;
    default: return NULL;
    }
}

static void src(Vm* m, const Sm3Reg* r, float out[4])
{
    float tmp[4] = { 0, 0, 0, 0 }, v[4];
    const float* p = reg_ptr(m, r, tmp);
    if (!p)
        p = tmp;
    for (int k = 0; k < 4; ++k)
        v[k] = p[(r->swizzle >> (2 * k)) & 3];
    for (int k = 0; k < 4; ++k)
    {
        float x = v[k];
        switch (r->mod)
        {
        case SM3_SRC_NEG: x = -x; break;
        case SM3_SRC_BIAS: x = x - 0.5f; break;
        case SM3_SRC_BIASNEG: x = -(x - 0.5f); break;
        case SM3_SRC_SIGN: x = 2 * x - 1; break;
        case SM3_SRC_SIGNNEG: x = -(2 * x - 1); break;
        case SM3_SRC_COMP: x = 1 - x; break;
        case SM3_SRC_X2: x = 2 * x; break;
        case SM3_SRC_X2NEG: x = -2 * x; break;
        case SM3_SRC_ABS: x = fabsf(x); break;
        case SM3_SRC_ABSNEG: x = -fabsf(x); break;
        default: break;
        }
        out[k] = x;
    }
}

static void store(Vm* m, const Sm3Ins* in, const float v[4])
{
    const Sm3Reg* d = &in->dst;
    if (d->type == SM3_ADDR)
    {
        for (int k = 0; k < 4; ++k)
            if (d->mask & (1 << k))
                m->a0[k] = m->s->major < 2 ? (int32_t)floorf(v[k]) : (int32_t)floorf(v[k] + 0.5f);
        return;
    }
    if (d->type == SM3_PREDICATE)
    {
        for (int k = 0; k < 4; ++k)
            if (d->mask & (1 << k))
                m->p0[k] = v[k] != 0;
        return;
    }
    float tmp[4];
    float* p = reg_ptr(m, d, tmp);
    if (!p || p == tmp)
        return;
    for (int k = 0; k < 4; ++k)
        if (d->mask & (1 << k))
        {
            if (in->predicated)
            {
                int comp = (in->pred.swizzle >> (2 * k)) & 3, pv = m->p0[comp];
                if (in->pred.mod == SM3_SRC_NOT)
                    pv = !pv;
                if (!pv)
                    continue;
            }
            p[k] = (d->mod & SM3_DST_SATURATE) ? sat(v[k]) : v[k];
        }
}

static int compare(unsigned control, float a, float b)
{
    switch (control)
    {
    case 1: return a > b;
    case 2: return a == b;
    case 3: return a >= b;
    case 4: return a < b;
    case 5: return a != b;
    case 6: return a <= b;
    default: return 0;
    }
}

static float dp(const float* a, const float* b, int n)
{
    float s = 0;
    for (int k = 0; k < n; ++k)
        s += a[k] * b[k];
    return s;
}

/* the instruction after the matching end of the block opened at `i` (else/endif, endrep/endloop) */
static size_t block_end(const Sm3Shader* s, size_t i, int want_else, size_t* else_at)
{
    int depth = 0;
    for (size_t j = i + 1; j < s->nins; ++j)
    {
        unsigned op = s->ins[j].op;
        if (op == SM3_IF || op == SM3_IFC || op == SM3_REP || op == SM3_LOOP_OP)
            depth++;
        else if (op == SM3_ENDIF || op == SM3_ENDREP || op == SM3_ENDLOOP)
        {
            if (depth == 0)
                return j;
            depth--;
        }
        else if (op == SM3_ELSE && depth == 0 && want_else && else_at)
            *else_at = j;
    }
    return s->nins;
}

typedef struct
{
    size_t start, end;
    int32_t left, step, saved_aL;
    int is_loop;
} Frame;

void sm3_run_vs(const Sm3Shader* s, const float in[16][4], const Sm3VsConsts* k, Sm3VsResult* res)
{
    static Vm m;
    memset(&m, 0, sizeof m);
    m.s = s;
    m.v = in;
    memcpy(m.c, k->c, sizeof m.c);
    memcpy(m.ci, k->i, sizeof m.ci);
    memcpy(m.cb, k->b, sizeof m.cb);
    m.pos_out = -1;
    for (size_t i = 0; i < s->nins; ++i)
    {
        const Sm3Ins* x = &s->ins[i];
        if (x->op == SM3_DEF && x->dst.num < 256)
            memcpy(m.c[x->dst.num], x->value, 16);
        else if (x->op == SM3_DEFI && x->dst.num < 16)
            memcpy(m.ci[x->dst.num], x->value, 16);
        else if (x->op == SM3_DEFB && x->dst.num < 16)
            m.cb[x->dst.num] = x->value[0];
        else if (x->op == SM3_DCL && x->dst.type == SM3_OUTPUT && x->usage == SM3_U_POSITION && x->usage_index == 0)
            m.pos_out = x->dst.num;
    }

    Frame stack[16];
    int sp = 0;
    float a[4], b[4], c[4], v[4];
    for (size_t pc = 0; pc < s->nins;)
    {
        const Sm3Ins* x = &s->ins[pc];
        size_t next = pc + 1;
        switch (x->op)
        {
        case SM3_NOP: case SM3_DCL: case SM3_DEF: case SM3_DEFI: case SM3_DEFB: case SM3_COMMENT: case SM3_END: break;
        case SM3_MOV: src(&m, &x->src[0], v); store(&m, x, v); break;
        case SM3_MOVA: src(&m, &x->src[0], v); store(&m, x, v); break;
        case SM3_ADD: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] + b[j]; store(&m, x, v); break;
        case SM3_SUB: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] - b[j]; store(&m, x, v); break;
        case SM3_MUL: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] * b[j]; store(&m, x, v); break;
        case SM3_MAD:
            src(&m, &x->src[0], a); src(&m, &x->src[1], b); src(&m, &x->src[2], c);
            for (int j = 0; j < 4; ++j) v[j] = a[j] * b[j] + c[j];
            store(&m, x, v);
            break;
        case SM3_MIN: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] < b[j] ? a[j] : b[j]; store(&m, x, v); break;
        case SM3_MAX: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] >= b[j] ? a[j] : b[j]; store(&m, x, v); break;
        case SM3_SLT: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] < b[j] ? 1.0f : 0.0f; store(&m, x, v); break;
        case SM3_SGE: src(&m, &x->src[0], a); src(&m, &x->src[1], b); for (int j = 0; j < 4; ++j) v[j] = a[j] >= b[j] ? 1.0f : 0.0f; store(&m, x, v); break;
        case SM3_DP3: src(&m, &x->src[0], a); src(&m, &x->src[1], b); v[0] = v[1] = v[2] = v[3] = dp(a, b, 3); store(&m, x, v); break;
        case SM3_DP4: src(&m, &x->src[0], a); src(&m, &x->src[1], b); v[0] = v[1] = v[2] = v[3] = dp(a, b, 4); store(&m, x, v); break;
        case SM3_RCP: src(&m, &x->src[0], a); v[0] = v[1] = v[2] = v[3] = a[0] == 0 ? INFINITY : 1.0f / a[0]; store(&m, x, v); break;
        case SM3_RSQ: src(&m, &x->src[0], a); v[0] = v[1] = v[2] = v[3] = a[0] == 0 ? INFINITY : 1.0f / sqrtf(fabsf(a[0])); store(&m, x, v); break;
        case SM3_EXP: case SM3_EXPP: src(&m, &x->src[0], a); v[0] = v[1] = v[2] = v[3] = exp2f(a[0]); store(&m, x, v); break;
        case SM3_LOG: case SM3_LOGP: src(&m, &x->src[0], a); v[0] = v[1] = v[2] = v[3] = a[0] == 0 ? -INFINITY : log2f(fabsf(a[0])); store(&m, x, v); break;
        case SM3_POW: src(&m, &x->src[0], a); src(&m, &x->src[1], b); v[0] = v[1] = v[2] = v[3] = powf(fabsf(a[0]), b[0]); store(&m, x, v); break;
        case SM3_FRC: src(&m, &x->src[0], a); for (int j = 0; j < 4; ++j) v[j] = a[j] - floorf(a[j]); store(&m, x, v); break;
        case SM3_ABS: src(&m, &x->src[0], a); for (int j = 0; j < 4; ++j) v[j] = fabsf(a[j]); store(&m, x, v); break;
        case SM3_SGN: src(&m, &x->src[0], a); for (int j = 0; j < 4; ++j) v[j] = a[j] > 0 ? 1.0f : a[j] < 0 ? -1.0f : 0.0f; store(&m, x, v); break;
        case SM3_NRM:
        {
            src(&m, &x->src[0], a);
            float d = dp(a, a, 3), f = d == 0 ? INFINITY : 1.0f / sqrtf(d);
            for (int j = 0; j < 4; ++j) v[j] = a[j] * f;
            store(&m, x, v);
            break;
        }
        case SM3_CRS:
            src(&m, &x->src[0], a); src(&m, &x->src[1], b);
            v[0] = a[1] * b[2] - a[2] * b[1], v[1] = a[2] * b[0] - a[0] * b[2], v[2] = a[0] * b[1] - a[1] * b[0], v[3] = 0;
            store(&m, x, v);
            break;
        case SM3_LRP:
            src(&m, &x->src[0], a); src(&m, &x->src[1], b); src(&m, &x->src[2], c);
            for (int j = 0; j < 4; ++j) v[j] = a[j] * (b[j] - c[j]) + c[j];
            store(&m, x, v);
            break;
        case SM3_LIT:
            src(&m, &x->src[0], a);
            v[0] = 1, v[3] = 1, v[1] = a[0] > 0 ? a[0] : 0;
            {
                float pw = a[3] < -128 ? -128 : a[3] > 128 ? 128 : a[3];
                v[2] = a[0] > 0 && a[1] > 0 ? powf(a[1], pw) : 0;
            }
            store(&m, x, v);
            break;
        case SM3_DST: src(&m, &x->src[0], a); src(&m, &x->src[1], b); v[0] = 1, v[1] = a[1] * b[1], v[2] = a[2], v[3] = b[3]; store(&m, x, v); break;
        case SM3_SINCOS: src(&m, &x->src[0], a); v[0] = cosf(a[0]), v[1] = sinf(a[0]), v[2] = v[3] = 0; store(&m, x, v); break;
        case SM3_M4x4: case SM3_M4x3: case SM3_M3x4: case SM3_M3x3: case SM3_M3x2:
        {
            int n = x->op == SM3_M4x4 || x->op == SM3_M4x3 ? 4 : 3;
            int rows = x->op == SM3_M4x4 || x->op == SM3_M3x4 ? 4 : x->op == SM3_M3x2 ? 2 : 3;
            src(&m, &x->src[0], a);
            v[0] = v[1] = v[2] = v[3] = 0;
            for (int rr = 0; rr < rows; ++rr)
            {
                Sm3Reg r2 = x->src[1];
                r2.num = (uint16_t)(r2.num + rr);
                src(&m, &r2, b);
                v[rr] = dp(a, b, n);
            }
            store(&m, x, v);
            break;
        }
        case SM3_SETP:
            src(&m, &x->src[0], a); src(&m, &x->src[1], b);
            for (int j = 0; j < 4; ++j) v[j] = compare(x->control, a[j], b[j]) ? 1.0f : 0.0f;
            store(&m, x, v);
            break;
        case SM3_IF: case SM3_IFC:
        {
            int cond;
            if (x->op == SM3_IFC)
            {
                src(&m, &x->src[0], a); src(&m, &x->src[1], b);
                cond = compare(x->control, a[0], b[0]);
            }
            else if (x->src[0].type == SM3_PREDICATE)
            {
                cond = m.p0[x->src[0].swizzle & 3];
                if (x->src[0].mod == SM3_SRC_NOT)
                    cond = !cond;
            }
            else
            {
                src(&m, &x->src[0], a);
                cond = a[0] != 0;
            }
            size_t else_at = 0, end = block_end(s, pc, 1, &else_at);
            if (!cond)
                next = else_at ? else_at + 1 : end + 1;
            break;
        }
        case SM3_ELSE:
        {
            /* reached from the taken branch: skip to the endif */
            next = block_end(s, pc, 0, NULL) + 1;
            break;
        }
        case SM3_ENDIF: break;
        case SM3_REP: case SM3_LOOP_OP:
        {
            const Sm3Reg* ir = &x->src[x->op == SM3_REP ? 0 : 1];
            int32_t cnt = 0, init = 0, step = 0;
            if (ir->num < 16)
                cnt = m.ci[ir->num][0], init = m.ci[ir->num][1], step = m.ci[ir->num][2];
            size_t end = block_end(s, pc, 0, NULL);
            if (cnt <= 0 || sp >= 16)
            {
                next = end + 1;
                break;
            }
            Frame* f = &stack[sp++];
            f->start = pc + 1, f->end = end, f->left = cnt, f->step = step, f->saved_aL = m.aL;
            f->is_loop = x->op == SM3_LOOP_OP;
            if (f->is_loop)
                m.aL = init;
            break;
        }
        case SM3_ENDREP: case SM3_ENDLOOP:
            if (sp > 0)
            {
                Frame* f = &stack[sp - 1];
                if (--f->left > 0)
                {
                    if (f->is_loop)
                        m.aL += f->step;
                    next = f->start;
                }
                else
                {
                    if (f->is_loop)
                        m.aL = f->saved_aL;
                    sp--;
                }
            }
            break;
        case SM3_BREAK: case SM3_BREAKC: case SM3_BREAKP:
        {
            int go = 1;
            if (x->op == SM3_BREAKC)
            {
                src(&m, &x->src[0], a); src(&m, &x->src[1], b);
                go = compare(x->control, a[0], b[0]);
            }
            else if (x->op == SM3_BREAKP)
            {
                go = m.p0[x->src[0].swizzle & 3];
                if (x->src[0].mod == SM3_SRC_NOT)
                    go = !go;
            }
            if (go && sp > 0)
            {
                Frame* f = &stack[--sp];
                if (f->is_loop)
                    m.aL = f->saved_aL;
                next = f->end + 1;
            }
            break;
        }
        case SM3_RET: next = s->nins; break;
        default:
            if (!m.unsupported)
                m.unsupported = x->op ? x->op : -1;
            break;
        }
        pc = next;
    }
    memcpy(res->out, m.o, sizeof res->out);
    if (m.pos_out >= 0)
        memcpy(res->pos, m.o[m.pos_out], 16);
    else
        memcpy(res->pos, m.opos, 16);
    res->unsupported = m.unsupported;
}
