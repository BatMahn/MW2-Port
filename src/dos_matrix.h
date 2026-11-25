#ifndef DOS_MATRIX_H
#define DOS_MATRIX_H

#include "dos_trig.h"
#include <string.h>

typedef struct {
    int32_t r[9];   /* row-major 3.29 rotation */
    int32_t p[3];   /* centimetres */
} dos_matrix;

static inline int32_t dos_round29(int64_t n)
{
    return (int32_t)((n + (INT64_C(1) << 28)) >> 29);
}

static inline int32_t dos_mul29(int32_t a, int32_t b)
{
    return dos_round29((int64_t)a * b);
}

/* Mode 0 of 0x3b522 takes pitch, yaw, roll in 16.16 degrees from the caller.
 * In the mixed-axis path it reconstructs the middle column by cross product. */
static inline dos_matrix dos_matrix_rotation(int32_t pitch, int32_t yaw, int32_t roll)
{
    dos_matrix m = {.r = {1 << 29, 0, 0, 0, 1 << 29, 0, 0, 0, 1 << 29}};
    if (!pitch && !yaw) {
        m.r[0] = m.r[4] = dos_cos(roll);
        m.r[3] = dos_sin(roll);
        m.r[1] = -m.r[3];
    } else if (!pitch && !roll) {
        m.r[0] = m.r[8] = dos_cos(yaw);
        m.r[2] = dos_sin(yaw);
        m.r[6] = -m.r[2];
    } else if (!yaw && !roll) {
        m.r[4] = m.r[8] = dos_cos(pitch);
        m.r[7] = dos_sin(pitch);
        m.r[5] = -m.r[7];
    } else {
        const int32_t ca = dos_cos(pitch), sa = dos_sin(pitch);
        const int32_t cb = dos_cos(yaw), sb = dos_sin(yaw);
        const int32_t cc = dos_cos(roll), sc = dos_sin(roll);
        m.r[0] = dos_mul29(cc, cb) + dos_mul29(dos_mul29(sc, sa), sb);
        m.r[3] = dos_mul29(ca, sc);
        m.r[6] = dos_mul29(dos_mul29(cb, sc), sa) - dos_mul29(cc, sb);
        m.r[2] = dos_mul29(ca, sb);
        m.r[5] = -sa;
        m.r[8] = dos_mul29(ca, cb);
        m.r[1] = dos_round29((int64_t)m.r[5] * m.r[6] - (int64_t)m.r[3] * m.r[8]);
        m.r[4] = dos_round29((int64_t)m.r[8] * m.r[0] - (int64_t)m.r[6] * m.r[2]);
        m.r[7] = dos_round29((int64_t)m.r[2] * m.r[3] - (int64_t)m.r[0] * m.r[5]);
    }
    return m;
}

/* 0x3aefc and 0x3b20c round once after summing the three products. */
static inline void dos_matrix_point(const dos_matrix *m, const int32_t v[3], int32_t out[3])
{
    int32_t p[3];
    for (int r = 0; r < 3; r++)
        p[r] = m->p[r] + dos_round29((int64_t)m->r[r * 3] * v[0] +
                                     (int64_t)m->r[r * 3 + 1] * v[1] +
                                     (int64_t)m->r[r * 3 + 2] * v[2]);
    memcpy(out, p, sizeof p);
}

static inline dos_matrix dos_matrix_compose(const dos_matrix *parent, const dos_matrix *local)
{
    dos_matrix world;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            world.r[r * 3 + c] = dos_round29((int64_t)parent->r[r * 3] * local->r[c] +
                                            (int64_t)parent->r[r * 3 + 1] * local->r[3 + c] +
                                            (int64_t)parent->r[r * 3 + 2] * local->r[6 + c]);
    dos_matrix_point(parent, local->p, world.p);
    return world;
}

#endif
