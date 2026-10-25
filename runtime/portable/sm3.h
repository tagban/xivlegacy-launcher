/* Direct3D 9 shader bytecode (vs/ps 1.x-3.0 token streams): parsed into instructions, for the
 * disassembler (tools/sm3dis) and the translator to the back ends' shading languages.
 *
 * The token format is D3D9's (d3d9types.h, the D3DSIO_ and D3DSP_ values): a version token, then
 * instructions - an opcode token (opcode, control bits, length), a destination parameter token,
 * source parameter tokens (a relative-address token after a source that uses one) - with comment
 * blocks (the CTAB constant table among them) in between, and D3DSIO_END. */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* register types (D3DSHADER_PARAM_REGISTER_TYPE) */
enum
{
    SM3_TEMP = 0, SM3_INPUT = 1, SM3_CONST = 2, SM3_ADDR = 3, /* ps: TEXTURE */ SM3_RASTOUT = 4, SM3_ATTROUT = 5,
    SM3_OUTPUT = 6, /* vs < 3: TEXCRDOUT */ SM3_CONSTINT = 7, SM3_COLOROUT = 8, SM3_DEPTHOUT = 9, SM3_SAMPLER = 10,
    SM3_CONST2 = 11, SM3_CONST3 = 12, SM3_CONST4 = 13, SM3_CONSTBOOL = 14, SM3_LOOP = 15, SM3_TEMPFLOAT16 = 16,
    SM3_MISCTYPE = 17, SM3_LABEL = 18, SM3_PREDICATE = 19,
};

/* opcodes (D3DSHADER_INSTRUCTION_OPCODE_TYPE) */
enum
{
    SM3_NOP = 0, SM3_MOV, SM3_ADD, SM3_SUB, SM3_MAD, SM3_MUL, SM3_RCP, SM3_RSQ, SM3_DP3, SM3_DP4, SM3_MIN, SM3_MAX,
    SM3_SLT, SM3_SGE, SM3_EXP, SM3_LOG, SM3_LIT, SM3_DST, SM3_LRP, SM3_FRC, SM3_M4x4, SM3_M4x3, SM3_M3x4, SM3_M3x3,
    SM3_M3x2, SM3_CALL, SM3_CALLNZ, SM3_LOOP_OP, SM3_RET, SM3_ENDLOOP, SM3_LABEL_OP, SM3_DCL, SM3_POW, SM3_CRS,
    SM3_SGN, SM3_ABS, SM3_NRM, SM3_SINCOS, SM3_REP, SM3_ENDREP, SM3_IF, SM3_IFC, SM3_ELSE, SM3_ENDIF, SM3_BREAK,
    SM3_BREAKC, SM3_MOVA, SM3_DEFB, SM3_DEFI,
    SM3_TEXCOORD = 64, SM3_TEXKILL, SM3_TEX, SM3_TEXBEM, SM3_TEXBEML, SM3_TEXREG2AR, SM3_TEXREG2GB, SM3_TEXM3x2PAD,
    SM3_TEXM3x2TEX, SM3_TEXM3x3PAD, SM3_TEXM3x3TEX, SM3_RESERVED0, SM3_TEXM3x3SPEC, SM3_TEXM3x3VSPEC, SM3_EXPP,
    SM3_LOGP, SM3_CND, SM3_DEF, SM3_TEXREG2RGB, SM3_TEXDP3TEX, SM3_TEXM3x2DEPTH, SM3_TEXDP3, SM3_TEXM3x3,
    SM3_TEXDEPTH, SM3_CMP, SM3_BEM, SM3_DP2ADD, SM3_DSX, SM3_DSY, SM3_TEXLDD, SM3_SETP, SM3_TEXLDL, SM3_BREAKP,
    SM3_PHASE = 0xFFFD, SM3_COMMENT = 0xFFFE, SM3_END = 0xFFFF,
};

/* source modifiers (D3DSHADER_PARAM_SRCMOD_TYPE >> 24) */
enum
{
    SM3_SRC_NONE, SM3_SRC_NEG, SM3_SRC_BIAS, SM3_SRC_BIASNEG, SM3_SRC_SIGN, SM3_SRC_SIGNNEG, SM3_SRC_COMP,
    SM3_SRC_X2, SM3_SRC_X2NEG, SM3_SRC_DZ, SM3_SRC_DW, SM3_SRC_ABS, SM3_SRC_ABSNEG, SM3_SRC_NOT,
};

/* destination result modifiers (D3DSPDM_*) */
#define SM3_DST_SATURATE 1u
#define SM3_DST_PP 2u
#define SM3_DST_CENTROID 4u

/* sampler texture types (D3DSAMPLER_TEXTURE_TYPE >> 27) */
enum { SM3_TEX_UNKNOWN = 0, SM3_TEX_2D = 2, SM3_TEX_CUBE = 3, SM3_TEX_VOLUME = 4 };

/* declaration usages (D3DDECLUSAGE) */
enum
{
    SM3_U_POSITION, SM3_U_BLENDWEIGHT, SM3_U_BLENDINDICES, SM3_U_NORMAL, SM3_U_PSIZE, SM3_U_TEXCOORD,
    SM3_U_TANGENT, SM3_U_BINORMAL, SM3_U_TESSFACTOR, SM3_U_POSITIONT, SM3_U_COLOR, SM3_U_FOG, SM3_U_DEPTH,
    SM3_U_SAMPLE,
};

typedef struct Sm3Reg
{
    uint8_t type;      /* SM3_TEMP ... */
    uint16_t num;
    uint8_t mask;      /* destination write mask (bit 0 = x) */
    uint8_t swizzle;   /* source: 2 bits per component, x first */
    uint8_t mod;       /* source modifier, or destination result modifiers */
    int8_t shift;      /* destination shift (ps 1.x) */
    uint8_t rel;       /* relative addressing */
    uint8_t rel_type;  /* the address register: SM3_ADDR (a0) or SM3_LOOP (aL) */
    uint8_t rel_comp;  /* which of its components */
} Sm3Reg;

typedef struct Sm3Ins
{
    uint16_t op;
    uint8_t control;   /* comparison for ifc/breakc/setp; texld: project/bias */
    uint8_t has_dst;
    uint8_t nsrc;
    uint8_t predicated;
    Sm3Reg pred;       /* the predicate source, when predicated */
    Sm3Reg dst;
    Sm3Reg src[4];
    /* dcl */
    uint8_t usage, usage_index, tex_type;
    /* def / defi / defb */
    uint32_t value[4];
} Sm3Ins;

typedef struct Sm3Shader
{
    int pixel;                  /* 0: vertex shader */
    uint8_t major, minor;
    Sm3Ins* ins;
    size_t nins;
    const uint32_t* ctab;       /* the CTAB comment's data (after its FOURCC), or NULL */
    uint32_t ctab_size;         /* bytes */
} Sm3Shader;

/* Parses `n` tokens. Returns 0 and fills `err` (if given) on a malformed stream. */
int sm3_parse(const uint32_t* tokens, size_t n, Sm3Shader* out, char* err, size_t errn);
void sm3_free(Sm3Shader* s);

/* The opcode's assembler name ("mad", "texld" ...), or NULL. */
const char* sm3_opname(unsigned op);

/* One instruction as D3D assembly text (Microsoft's spelling: "mad r0.xyz, r1, c4.x, -v0"). */
void sm3_format(const Sm3Shader* s, const Sm3Ins* i, char* out, size_t n);
