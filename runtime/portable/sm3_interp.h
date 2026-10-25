/* A CPU interpreter for Direct3D 9 vertex shaders (vs 1.1-3.0, parsed by sm3.h): the reference the
 * Metal translation (sm3_msl.h) is checked against, one vertex at a time (debugging: gfx9 `cpuvs`).
 *
 * D3D9 semantics: the shader's own def/defi/defb override the supplied constants; relative constant
 * reads outside the file read zero; rcp/rsq of zero give infinity; mova rounds to nearest (vs 2+). */
#pragma once

#include <stdint.h>

#include "sm3.h"

typedef struct Sm3VsConsts
{
    float c[256][4];
    int32_t i[16][4];
    uint32_t b[16];
} Sm3VsConsts;

typedef struct Sm3VsResult
{
    float out[12][4]; /* vs 3.0 o0..o11 */
    float pos[4];     /* the position output (o<n> declared POSITION, or vs 1-2's oPos) */
    int unsupported;  /* an instruction the interpreter skipped (texldl, call ...): its op, or 0 */
} Sm3VsResult;

/* Runs the vertex shader on one vertex: in = input registers v0..v15 as the declaration delivers them. */
void sm3_run_vs(const Sm3Shader* s, const float in[16][4], const Sm3VsConsts* k, Sm3VsResult* r);
