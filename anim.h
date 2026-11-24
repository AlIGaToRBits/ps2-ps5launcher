#pragma once
#include <math.h>
/* Значение плавно идёт к цели. k — скорость (чем больше, тем быстрее). */
typedef struct { float v, t, k; } Anim;
static inline void anim_init(Anim *a, float v, float k) { a->v = a->t = v; a->k = k; }
static inline void anim_to(Anim *a, float t) { a->t = t; }
static inline void anim_step(Anim *a, float dt) {
    a->v += (a->t - a->v) * (1.0f - expf(-a->k * dt));
    if (fabsf(a->t - a->v) < 0.001f) a->v = a->t;
}
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
