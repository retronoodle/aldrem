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

    if (failures) {
        fprintf(stderr, "test_viz: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_viz: ok\n");
    return 0;
}
