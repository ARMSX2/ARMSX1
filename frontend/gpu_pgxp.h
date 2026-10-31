#ifndef ARMSX_GPU_PGXP_H
#define ARMSX_GPU_PGXP_H

#include <math.h>
#include <string.h>
#include "../psx/dev/gpu.h"

static inline int armsx_pgxp_finite(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static inline int armsx_pgxp_vertex_valid(const vertex_t* v) {
    return v->precise_valid && armsx_pgxp_finite(v->px) && armsx_pgxp_finite(v->py) &&
           armsx_pgxp_finite(v->pw) && v->pw >= 1.0f && v->pw <= 65535.0f &&
           fabsf(v->px - (float)v->x) <= 1.0f &&
           fabsf(v->py - (float)v->y) <= 1.0f;
}

static inline int armsx_pgxp_triangle_valid(const vertex_t* a, const vertex_t* b,
                                           const vertex_t* c) {
    return armsx_pgxp_vertex_valid(a) && armsx_pgxp_vertex_valid(b) &&
           armsx_pgxp_vertex_valid(c);
}

static inline int armsx_pgxp_depth_valid(const vertex_t* a, const vertex_t* b,
                                        const vertex_t* c) {
    const float lo = fminf(a->pw, fminf(b->pw, c->pw));
    const float hi = fmaxf(a->pw, fmaxf(b->pw, c->pw));
    return hi <= lo * 32.0f;
}

#endif
