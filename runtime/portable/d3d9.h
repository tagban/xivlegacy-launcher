#pragma once

#include <stdint.h>

/* What d3dx9.c needs from d3d9.c's objects. */
typedef struct D3d9Mem
{
    uint32_t fmt, w, h, d;
    uint32_t pitch, slice; /* bytes per row (of blocks, when compressed) and per depth slice */
    uint32_t mem;          /* guest address, made on first use */
} D3d9Mem;

enum { D3D9_TEXTURE, D3D9_CUBE, D3D9_VOLUME_TEXTURE };

/* A new texture (guest COM pointer, one reference), or 0. */
uint32_t d3d9_create_texture(int type, uint32_t w, uint32_t h, uint32_t d, uint32_t levels, uint32_t usage, uint32_t fmt,
    uint32_t pool);
/* The memory of a surface or volume; 0 if `p` is neither. */
int d3d9_resource_mem(uint32_t p, D3d9Mem* out);
uint32_t d3d9_fmt_block(uint32_t fmt); /* bytes per 4x4 block, 0 if uncompressed */
uint32_t d3d9_fmt_bytes(uint32_t fmt); /* bytes per pixel */
