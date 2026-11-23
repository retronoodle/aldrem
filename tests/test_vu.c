/* test_vu.c — unit checks for the level-meter maths in src/vu.c. No device. */
#include <math.h>
#include <stdio.h>

#include "vu.h"

static int failures;

#define CHECK(cond, msg)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            fprintf(stderr, "FAIL: %s\n", msg);          \
            failures++;                                  \
        }                                                \
    } while (0)

static int near(float a, float b, float eps) { return fabsf(a - b) <= eps; }

int
main(void)
{
    VuBallistics b = { 0.0f, 0.0f, 0.0f };
    int i;

    /* Silence and the -40 dBFS floor both map to zero; full scale to 1. */
    CHECK(vu_norm(0.0f) == 0.0f, "silence maps to 0");
    CHECK(near(vu_norm(powf(10.0f, -40.0f / 20.0f)), 0.0f, 1e-4f),
          "-40 dBFS maps to 0");
    CHECK(near(vu_norm(1.0f), 40.0f / 43.0f, 1e-4f), "0 dBFS maps to 40/43");
    CHECK(vu_norm(10.0f) == 1.0f, "over-range clamps to 1");

    /* Attack should reach a loud level within a few frames at 60 fps. */
    for (i = 0; i < 6; i++)
        vu_ballistics_update(&b, 1.0f, 1.0f, 1.0f / 60.0f, 0);
    CHECK(b.needle > 0.8f, "needle rises quickly on attack");

    /* Release is slower: after the same time it should still be well up. */
    for (i = 0; i < 6; i++)
        vu_ballistics_update(&b, 0.0f, 0.0f, 1.0f / 60.0f, 0);
    CHECK(b.needle > 0.5f, "needle falls slowly on release");

    /* Peak marker holds for about a second, then falls. */
    b.peak = 0.0f;
    b.peak_age = 0.0f;
    vu_ballistics_update(&b, 1.0f, 1.0f, 1.0f / 60.0f, 0);
    for (i = 0; i < 30; i++)
        vu_ballistics_update(&b, 0.0f, 0.0f, 1.0f / 60.0f, 0);
    CHECK(near(b.peak, vu_norm(1.0f), 1e-3f), "peak holds during hold time");
    for (i = 0; i < 120; i++)
        vu_ballistics_update(&b, 0.0f, 0.0f, 1.0f / 60.0f, 0);
    CHECK(b.peak < vu_norm(1.0f), "peak falls after hold time");

    /* Snap jumps straight to target, used when reduce-motion is set. */
    vu_ballistics_update(&b, 0.5f, 0.5f, 1.0f / 60.0f, 1);
    CHECK(near(b.needle, vu_norm(0.5f), 1e-6f), "snap sets needle directly");

    if (failures) {
        fprintf(stderr, "test_vu: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_vu: ok\n");
    return 0;
}
