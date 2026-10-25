/* Direct3D 9 shaders (sm3.h) to Metal Shading Language, a vertex/pixel shader pair at a time.
 *
 * Pairs, because D3D9 links a vertex shader's outputs to a pixel shader's inputs by semantic
 * (texcoord3, color0 ...) while Metal requires every fragment input to be a vertex output: the
 * pixel function's inputs are cut down to what the vertex shader writes, the rest read as zero.
 *
 * The functions are vs_main and fs_main. Bindings:
 *   vertex   stage_in attributes: attribute(n) = input register v<n> (the vertex descriptor maps the
 *            declaration's elements onto them), streams in buffers 0..15;
 *            buffer(16) float4 c[256], buffer(17) int4 i[16], buffer(18) uint b[16], buffer(19) Sm3Fix;
 *            texture/sampler(0..3) = the vertex samplers (D3DVERTEXTEXTURESAMPLER0..3).
 *   fragment buffer(0) float4 c[224], buffer(1) int4 i[16], buffer(2) uint b[16], buffer(3) Sm3Fix;
 *            texture/sampler(n) = s<n>.
 *
 * D3D9 conventions the code keeps: pixel centres on integer coordinates (the vertex position moves
 * half a pixel, Sm3Fix.posfix; vPos is the integer coordinate), alpha test (fixed function, still
 * applied with shaders), sRGB reads and writes as flags, vs 1-2 color outputs clamped to [0, 1]. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sm3.h"

/* the uniform block both stages read (std140-like layout; MSL struct Sm3Fix matches) */
typedef struct Sm3Fix
{
    float posfix[4];     /* xy: added to position.xy times w (1/width, -1/height) */
    float alpha_ref;     /* 0..1 */
    uint32_t alpha_func; /* D3DCMPFUNC; 0 or 8 (ALWAYS): no alpha test */
    uint32_t srgb_write; /* convert color output 0 to sRGB */
    uint32_t srgb_mask;  /* samplers whose reads convert from sRGB (bit n = s<n>; vertex: bits 0..3) */
} Sm3Fix;

/* vertex input kinds: how the vertex descriptor delivers input register v<n> (Metal hands non-normalized
 * integer formats - UBYTE4, SHORT2/4 - only to integer inputs) */
enum { SM3_IN_FLOAT, SM3_IN_INT, SM3_IN_UINT };

/* MSL source for the pair (malloc'd), or NULL with a reason in err. in_kind: per input register (NULL:
 * all float). */
/* debugging: nonzero makes the vertex function also store each vertex's output position (before the
 * half-pixel fixup) at buffer(29)[instance_id * buffer(28) + vertex_id] */
extern int sm3_msl_vtx_debug;

char* sm3_msl_pair(const Sm3Shader* vs, const Sm3Shader* ps, const uint8_t in_kind[16], char* err, size_t errn);
