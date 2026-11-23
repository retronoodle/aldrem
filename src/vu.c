/* vu.c — pure level-meter maths. See vu.h. */
#include "vu.h"

#include <math.h>

#define VU_FLOOR_DB    (-40.0f)
#define VU_CEIL_DB     (3.0f)
#define VU_ATTACK_S    (0.010f) /* needle rise time constant */
#define VU_RELEASE_S   (0.300f) /* needle fall time constant */
#define VU_PEAK_HOLD_S (1.0f)   /* peak marker freezes this long */
#define VU_PEAK_FALL   (0.6f)   /* peak marker fall rate, units/second */

float
vu_norm(float linear)
{
    float db;

    if (linear <= 1e-5f)
        return 0.0f;
    db = 20.0f * log10f(linear);
    db = (db - VU_FLOOR_DB) / (VU_CEIL_DB - VU_FLOOR_DB);
    if (db < 0.0f)
        return 0.0f;
    if (db > 1.0f)
        return 1.0f;
    return db;
}

/* One-pole smoothing toward target with time constant tau. */
static float
ease(float current, float target, float dt, float tau)
{
    float k;

    if (tau <= 0.0f || dt <= 0.0f)
        return target;
    k = 1.0f - expf(-dt / tau);
    return current + (target - current) * k;
}

void
vu_ballistics_update(VuBallistics *b, float level, float peak,
                     float dt, int snap)
{
    const float target = vu_norm(level);
    const float peak_target = vu_norm(peak);

    if (snap) {
        b->needle = target;
        b->peak = peak_target;
        b->peak_age = 0.0f;
        return;
    }

    /* Rise fast, fall slow: pick the time constant by direction. */
    b->needle = ease(b->needle, target, dt,
                     target > b->needle ? VU_ATTACK_S : VU_RELEASE_S);

    if (peak_target >= b->peak) {
        b->peak = peak_target;
        b->peak_age = 0.0f;
    } else {
        b->peak_age += dt;
        if (b->peak_age > VU_PEAK_HOLD_S) {
            b->peak -= VU_PEAK_FALL * dt;
            if (b->peak < peak_target)
                b->peak = peak_target;
            if (b->peak < 0.0f)
                b->peak = 0.0f;
        }
    }
}
