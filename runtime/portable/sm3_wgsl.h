/* Direct3D 9 shaders (sm3.h) to WGSL, a vertex/pixel shader pair at a time, for the wgpu back end
 * (gfx9_wgpu.c). The structure follows sm3_msl.c: pairs, because D3D9 links a vertex shader's outputs
 * to a pixel shader's inputs by semantic while WebGPU links them by location; the pixel inputs the
 * vertex shader does not write read as zero.
 *
 * The entry points are vs_main and fs_main. Bindings:
 *   vertex inputs   @location(n) = input register v<n> (the pipeline's vertex buffer layouts map the
 *                   declaration's elements onto them);
 *   group 0         binding 0: the vertex stage's uniform block (Sm3wVsU), binding 1: the pixel
 *                   stage's (Sm3wPsU), both from the ring with dynamic offsets;
 *   group 1         pixel sampler s<n>: texture at binding 2n, sampler at 2n + 1; vertex sampler n
 *                   (D3DVERTEXTEXTURESAMPLER0..3): texture 32 + 2n, sampler 33 + 2n.
 *
 * What WGSL asks for that MSL did not: no assignment through multi-component swizzles (masked writes
 * become select()), no implicit conversions, a uniform array stride of 16 bytes (bools travel as
 * vec4<u32>), no constant-folded infinities or NaNs (those literals are made at run time from a zero
 * in the block), and implicit-derivative sampling outside uniform control flow, which D3D allows: the
 * module turns that diagnostic off. D3D conventions kept as in sm3_msl.c: the half-pixel position
 * offset (posfix), vPos on integer coordinates, alpha test, sRGB reads and writes as flags, vs 1-2
 * colors clamped to [0, 1]; and X8 textures' alpha read as 1 by flag (one_mask) where the adapter has
 * no view swizzles. Point size has no WGSL output: points draw one pixel. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sm3.h"

/* the head of both uniform blocks (48 bytes) */
typedef struct Sm3wFix
{
    float posfix[4];     /* xy: added to position.xy times w (1/width, -1/height) */
    float alpha_ref;     /* 0..1 */
    uint32_t alpha_func; /* D3DCMPFUNC; 0 or 8 (ALWAYS): no alpha test */
    uint32_t srgb_write; /* convert color output 0 to sRGB */
    uint32_t srgb_mask;  /* samplers whose reads convert from sRGB (bit n = s<n>; vertex: bits 0..3) */
    uint32_t one_mask;   /* samplers whose alpha reads 1 (X8 formats where views have no swizzles) */
    uint32_t zero;       /* 0: what run-time infinities and NaNs are made from */
    uint32_t pad[2];
} Sm3wFix;

/* a stage's uniform block: the fixups, i0..i15, b0..b15 (one per u32, four to a vec4), c0..c<n> */
#define SM3W_OFF_I 48u
#define SM3W_OFF_B (SM3W_OFF_I + 256u)
#define SM3W_OFF_C (SM3W_OFF_B + 64u)
#define SM3W_VS_SIZE (SM3W_OFF_C + 256u * 16u)
#define SM3W_PS_SIZE (SM3W_OFF_C + 224u * 16u)

/* vertex input kinds: what type input register v<n> arrives as (integer vertex formats reach only
 * integer inputs), or ZERO: no element feeds it, it reads (0, 0, 0, 0) */
enum { SM3W_IN_FLOAT, SM3W_IN_INT, SM3W_IN_UINT, SM3W_IN_ZERO };

/* what a sampler is bound to, where the shader text depends on it: a depth texture samples as
 * texture_depth_2d (its value in all four channels) */
#define SM3W_DEPTH_TEX(mask, n) (((mask) >> (n)) & 1u)

/* WGSL source for the pair (malloc'd), or NULL with a reason in err. in_kind: per input register (NULL:
 * all float). depth_mask: bit n = pixel sampler s<n> is a depth texture, bit 16 + n vertex sampler n. */
char* sm3_wgsl_pair(const Sm3Shader* vs, const Sm3Shader* ps, const uint8_t in_kind[16], uint32_t depth_mask, char* err, size_t errn);
