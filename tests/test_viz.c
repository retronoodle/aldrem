/* test_viz.c — unit checks for the visualizer maths in src/viz.c. No device. */
#include <math.h>
#include <stdio.h>

#include "viz.h"

static int failures;

#define CHECK(cond, msg)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            fprintf(stderr, "FAIL: %s\n", msg);          \
            failures++;                                  \
        }                                                \
    } while (0)

#define N VIZ_FFT_N

int
main(void)
{
    static float re[N], im[N], sine[N], bars[VIZ_BARS_MAX], cur[VIZ_BARS_MAX];
    static float zero[N];
    VizSettings s;
    unsigned char r, g, b;
    float cr, ci;
    int i, peak = 0;

    /* FFT rejects non-power-of-2 sizes and accepts powers of 2. */
    CHECK(viz_fft(re, im, 1000) == -1, "fft rejects non power of 2");
    CHECK(viz_fft(re, im, 0) == -1, "fft rejects zero");

    /* A pure sine at bin 32 concentrates its energy in that bin. */
    for (i = 0; i < N; ++i) {
        re[i] = sinf(2.0f * 3.14159265f * 32.0f * (float)i / (float)N);
        im[i] = 0.0f;
    }
    viz_fft(re, im, N);
    for (i = 1; i < N / 2; ++i) {
        float m = sqrtf(re[i] * re[i] + im[i] * im[i]);
        if (m > sqrtf(re[peak] * re[peak] + im[peak] * im[peak]))
            peak = i;
    }
    CHECK(peak == 32, "sine peak lands in bin 32");

    /* Silence reads as the floor, a loud sine reads high in its band. */
    viz_spectrum(zero, N, bars, 16, 1.0f);
    CHECK(bars[0] == 0.0f && bars[15] == 0.0f, "silence gives zero bars");

    /* 40 cycles per window: a bin-aligned tone. */
    for (i = 0; i < N; ++i)
        sine[i] = 0.9f * sinf(2.0f * 3.14159265f * 40.0f * (float)i / (float)N);
    viz_spectrum(sine, N, bars, 32, 1.0f);
    {
        int hot = 0;
        for (i = 0; i < 32; ++i)
            if (bars[i] > bars[hot])
                hot = i;
        CHECK(bars[hot] > 0.5f, "tone produces a strong bar");
        CHECK(hot > 0 && hot < 31, "tone bar is inside the range, not an edge");
    }
    for (i = 0; i < 32; ++i)
        CHECK(bars[i] >= 0.0f && bars[i] <= 1.0f, "bars stay in 0..1");

    /* Short input is zero-padded, not read out of bounds. */
    viz_spectrum(sine, 16, bars, 8, 1.0f);
    CHECK(bars[0] >= 0.0f && bars[0] <= 1.0f, "short input is safe");

    /* Gain raises the bars. */
    viz_spectrum(sine, N, bars, 32, 0.25f);
    {
        float quiet = bars[0];
        viz_spectrum(sine, N, bars, 32, 4.0f);
        CHECK(bars[0] >= quiet, "more gain never lowers a bar");
    }

    /* Smoothing: rises snap quickly, falls take time and slow down as the
     * smoothing amount rises. */
    cur[0] = 0.0f;
    {
        float target[1] = { 1.0f };
        viz_smooth(cur, target, 1, 0.5f);
        CHECK(cur[0] > 0.5f, "attack is fast");
    }
    cur[0] = 1.0f;
    {
        float target[1] = { 0.0f };
        float lo_s = 1.0f, hi_s = 1.0f;
        viz_smooth(&lo_s, target, 1, 0.0f);
        viz_smooth(&hi_s, target, 1, 1.0f);
        CHECK(lo_s < 1.0f, "release moves down");
        CHECK(hi_s > lo_s, "higher smoothing releases more slowly");
    }

    /* Settings clamp into range. */
    s.mode = 99;
    s.gain_pct = 1;
    s.smooth_pct = 500;
    s.bars = 1000;
    s.hue_pct = -4;
    viz_settings_clamp(&s);
    CHECK(s.mode == VIZ_MODE_COUNT - 1, "mode clamps high");
    CHECK(s.gain_pct == 25, "gain clamps low");
    CHECK(s.smooth_pct == 100, "smoothing clamps high");
    CHECK(s.bars == VIZ_BARS_MAX, "bars clamp high");
    CHECK(s.hue_pct == 0, "hue clamps low");

    /* Hue: red at 0, green at 1/3, blue at 2/3, wraps at 1. */
    viz_hue_rgb(0.0f, &r, &g, &b);
    CHECK(r == 255 && g == 0 && b == 0, "hue 0 is red");
    viz_hue_rgb(1.0f / 3.0f, &r, &g, &b);
    CHECK(g == 255 && r == 0, "hue 1/3 is green");
    viz_hue_rgb(2.0f / 3.0f, &r, &g, &b);
    CHECK(b == 255 && g == 0, "hue 2/3 is blue");
    viz_hue_rgb(1.0f, &r, &g, &b);
    CHECK(r == 255 && g == 0 && b == 0, "hue wraps at 1");

    /* Fractal constant stays inside a range where the set is interesting. */
    viz_fractal_c(0.0f, 0.0f, 0.0, &cr, &ci);
    CHECK(fabsf(cr) < 1.0f && fabsf(ci) < 1.0f, "fractal c is bounded");
    viz_fractal_c(1.0f, 1.0f, 12.3, &cr, &ci);
    CHECK(fabsf(cr) < 1.0f && fabsf(ci) < 1.0f, "fractal c stays bounded under load");

    /* Smooth escape count: |z| = 2 gives n + 1, |z| = 4 gives n. */
    CHECK(fabsf(viz_smooth_mu(5, 2.0f, 0.0f) - 6.0f) < 1e-4f, "smooth mu at |z|=2");
    CHECK(fabsf(viz_smooth_mu(5, 4.0f, 0.0f) - 5.0f) < 1e-4f, "smooth mu at |z|=4");
    CHECK(viz_smooth_mu(0, 1000.0f, 0.0f) >= 0.0f, "smooth mu never negative");

    /* Julia and Phoenix: the origin with c = 0 stays bounded (inside);
     * a far point escapes with a non-negative count. */
    CHECK(viz_julia_mu(0.0f, 0.0f, 0.0f, 0.0f, 40) < 0.0f, "julia origin is inside");
    CHECK(viz_julia_mu(10.0f, 0.0f, 0.0f, 0.0f, 40) >= 0.0f, "julia far point escapes");
    CHECK(viz_phoenix_mu(0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 40) < 0.0f, "phoenix origin is inside");
    CHECK(viz_phoenix_mu(3.0f, 0.0f, 0.0f, 0.0f, -0.5f, 40) >= 0.0f, "phoenix far point escapes");

    /* Kaleidoscope: periodic in the wedge angle, mirror-symmetric, radius kept. */
    {
        const float seg = 2.0f * 3.14159265f / 6.0f;
        float a1x, a1y, a2x, a2y, a3x, a3y;
        viz_kaleido(2.0f * cosf(0.3f), 2.0f * sinf(0.3f), 6, 0.0f, &a1x, &a1y);
        viz_kaleido(2.0f * cosf(0.3f + seg), 2.0f * sinf(0.3f + seg), 6, 0.0f, &a2x, &a2y);
        viz_kaleido(2.0f * cosf(-0.3f), 2.0f * sinf(-0.3f), 6, 0.0f, &a3x, &a3y);
        CHECK(fabsf(a1x - a2x) < 1e-4f && fabsf(a1y - a2y) < 1e-4f, "kaleido is periodic per wedge");
        CHECK(fabsf(a1x - a3x) < 1e-4f && fabsf(a1y - a3y) < 1e-4f, "kaleido mirrors the wedge");
        CHECK(fabsf(sqrtf(a1x * a1x + a1y * a1y) - 2.0f) < 1e-4f, "kaleido keeps radius");
    }

    /* Drift: zoom stays in 1/64..1, starts at 1, reaches the deep end, and moves
     * smoothly with no jumps. */
    {
        float cx, cy, z, prev_z = 0.0f, max_step = 0.0f;
        double tt;
        int bad = 0;
        viz_drift(0.0, &cx, &cy, &z);
        CHECK(fabsf(z - 1.0f) < 1e-5f && fabsf(cx) < 1e-6f, "drift starts at full view");
        viz_drift(10.0, &cx, &cy, &z);
        CHECK(fabsf(z - 1.0f / 64.0f) < 1e-5f, "drift reaches deep zoom at half cycle");
        for (tt = 0.0; tt < 40.0; tt += 0.01) {
            viz_drift(tt, &cx, &cy, &z);
            if (z < 1.0f / 64.0f - 1e-5f || z > 1.0f + 1e-5f)
                bad++;
            if (tt > 0.0 && fabsf(z - prev_z) > max_step)
                max_step = fabsf(z - prev_z);
            prev_z = z;
        }
        CHECK(bad == 0, "drift zoom stays in range");
        CHECK(max_step < 0.01f, "drift zoom is continuous");
    }

    /* Beat: a steady bass leaves no kick; an onset jumps it and it decays. */
    {
        float avg = 0.2f, kick = 0.0f;
        int i;
        for (i = 0; i < 120; ++i)
            viz_beat(&avg, &kick, 0.2f, 1.0f / 60.0f);
        CHECK(kick < 0.01f, "steady bass gives no kick");
        viz_beat(&avg, &kick, 0.9f, 1.0f / 60.0f);
        CHECK(kick > 0.5f, "bass onset jumps the kick");
        CHECK(kick <= 1.0f, "kick stays in 0..1");
        for (i = 0; i < 60; ++i)
            viz_beat(&avg, &kick, 0.9f, 1.0f / 60.0f);
        CHECK(kick < 0.05f, "kick decays after a second");
    }

    if (failures) {
        fprintf(stderr, "test_viz: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_viz: ok\n");
    return 0;
}
