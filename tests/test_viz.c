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
    viz_fractal_c(0.0f, 0.0f, 0.0f, &cr, &ci);
    CHECK(fabsf(cr) < 1.0f && fabsf(ci) < 1.0f, "fractal c is bounded");
    viz_fractal_c(1.0f, 1.0f, 1.0f, &cr, &ci);
    CHECK(fabsf(cr) < 1.0f && fabsf(ci) < 1.0f, "fractal c stays bounded under load");
    viz_fractal_c(0.4f, 0.3f, 0.2f, &cr, &ci);
    {
        float cr2, ci2;
        viz_fractal_c(0.4f, 0.3f, 0.2f, &cr2, &ci2);
        CHECK(cr == cr2 && ci == ci2, "fractal c is a pure function of the bands");
    }

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

    /* Silence gate: closed at the floor, open well above it, never falls as the
     * level rises. */
    {
        float prev = -1.0f;
        int mono = 1, i;
        CHECK(viz_gate(0.0f, 0.02f) == 0.0f, "gate is shut in silence");
        CHECK(viz_gate(0.02f, 0.02f) == 0.0f, "gate is shut at the floor");
        CHECK(viz_gate(0.5f, 0.02f) == 1.0f, "gate is open well above the floor");
        for (i = 0; i <= 100; ++i) {
            const float g = viz_gate((float)i / 100.0f, 0.02f);
            if (g < prev)
                mono = 0;
            prev = g;
        }
        CHECK(mono, "gate is monotonic");
    }

    /* Spectral flux: steady is 0, a rise is positive, a pure fall is 0. */
    {
        float a[VIZ_BARS_MAX], bb[VIZ_BARS_MAX];
        int i;
        for (i = 0; i < 16; ++i) {
            a[i] = 0.3f;
            bb[i] = 0.3f;
        }
        CHECK(viz_flux(a, bb, 16) == 0.0f, "flux is zero for a steady spectrum");
        for (i = 0; i < 16; ++i)
            bb[i] = 0.6f;
        CHECK(viz_flux(a, bb, 16) > 0.0f, "flux is positive on a rise");
        CHECK(viz_flux(bb, a, 16) == 0.0f, "flux is zero on a pure fall");
        CHECK(viz_flux(a, bb, 16) <= 1.0f, "flux stays in 0..1");
    }

    /* Centroid: bass-only is low, treble-only is high, silence is centred. */
    {
        float lo[VIZ_BARS_MAX] = {0}, hi[VIZ_BARS_MAX] = {0}, none[VIZ_BARS_MAX] = {0};
        lo[0] = 1.0f;
        hi[31] = 1.0f;
        CHECK(viz_centroid(lo, 32) < 0.05f, "centroid is low for bass");
        CHECK(viz_centroid(hi, 32) > 0.95f, "centroid is high for treble");
        CHECK(fabsf(viz_centroid(none, 32) - 0.5f) < 1e-6f, "centroid is centred in silence");
    }

    /* Ring bin: always a valid bar, rises with radius, bass at the centre. */
    {
        int prev = -1, mono = 1, i;
        CHECK(viz_ring_bin(0.0f, 24) == 0, "centre ring reads the lowest bar");
        CHECK(viz_ring_bin(1.0f, 24) == 23, "rim ring reads the highest bar");
        CHECK(viz_ring_bin(5.0f, 24) == 23 && viz_ring_bin(-1.0f, 24) == 0,
              "ring bin clamps out-of-range radius");
        for (i = 0; i <= 100; ++i) {
            const int b = viz_ring_bin((float)i / 100.0f, 24);
            if (b < prev || b < 0 || b > 23)
                mono = 0;
            prev = b;
        }
        CHECK(mono, "ring bin is monotonic and in range");
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
