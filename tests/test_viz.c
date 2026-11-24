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

    /* Compression: 0 and 1 are fixed points, quiet values are lifted,
     * the curve is monotonic, and exponent 1 is the identity. */
    {
        float in[5] = { 0.0f, 0.2f, 0.5f, 0.8f, 1.0f };
        float out[5], id[5];
        int i;

        viz_compress(out, in, 5, 0.7f);
        CHECK(out[0] == 0.0f && fabsf(out[4] - 1.0f) < 1e-6f, "compress keeps 0 and 1");
        CHECK(out[1] > in[1] + 0.05f, "compress lifts quiet values");
        for (i = 1; i < 5; ++i)
            CHECK(out[i] > out[i - 1], "compress is monotonic");
        viz_compress(id, in, 5, 1.0f);
        CHECK(fabsf(id[2] - 0.5f) < 1e-6f, "exponent 1 leaves values unchanged");
        {
            float over[1] = { 3.0f }, res[1];
            viz_compress(res, over, 1, 0.7f);
            CHECK(res[0] == 1.0f, "compress clamps input above 1");
        }
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

    /* Gate on raw bars: a 0.01 floor compresses to about 0.04, which would open
     * a gate at 0.03, but the raw gate stays shut. Loud raw bars open it. */
    {
        float floor_bars[32], loud[32], comp[32];
        int i;
        for (i = 0; i < 32; ++i)
            floor_bars[i] = 0.01f;
        viz_compress(comp, floor_bars, 32, 0.7f);
        CHECK(viz_band_mean(comp, 0, 32) > 0.03f, "compressed hiss clears the floor");
        CHECK(viz_band_gate(floor_bars, 0, 32, 0.03f) == 0.0f, "raw hiss keeps the gate shut");
        for (i = 0; i < 32; ++i)
            loud[i] = 0.5f;
        CHECK(viz_band_gate(loud, 0, 32, 0.03f) == 1.0f, "raw loud bars open the gate");
        CHECK(fabsf(viz_band_mean(loud, 8, 16) - 0.5f) < 1e-6f, "band mean reads its range");
        CHECK(viz_band_mean(loud, 4, 4) == 0.0f, "empty band mean is zero");
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

    /* Tick: a steady tone and quiet hiss leave no tick; a 1 ms click over a
     * quiet bed registers, at the level whose bars only reach about 0.19; the
     * tick decays; and an empty window is safe. */
    {
        float buf[N];
        float avg = 0.0f, tick = 0.0f, tone_tick = 0.0f, hiss_tick = 0.0f;
        unsigned int seed = 7u;
        int f, j;

        for (f = 0; f < 120; ++f) {
            for (j = 0; j < N; ++j)
                buf[j] = 0.3f * sinf(2.0f * 3.14159265f * 1000.0f * j / 44100.0f);
            viz_tick(buf, N, 1.0f, &avg, &tone_tick, 1.0f / 60.0f);
        }
        CHECK(tone_tick < 0.05f, "a steady tone gives no tick");

        avg = 0.0f;
        for (f = 0; f < 120; ++f) {
            for (j = 0; j < N; ++j)
                buf[j] = 0.001f * (viz_rand01(&seed) * 2.0f - 1.0f);
            viz_tick(buf, N, 1.0f, &avg, &hiss_tick, 1.0f / 60.0f);
        }
        CHECK(hiss_tick < 0.05f, "quiet hiss gives no tick");

        avg = 0.0f;
        for (f = 0; f < 120; ++f) {
            for (j = 0; j < N; ++j)
                buf[j] = 0.001f * (viz_rand01(&seed) * 2.0f - 1.0f);
            viz_tick(buf, N, 1.0f, &avg, &tick, 1.0f / 60.0f);
        }
        for (j = 0; j < N; ++j)
            buf[j] = (j >= 500 && j < 544) ? 0.05f * (j % 2 ? -1.0f : 1.0f)
                                           : 0.001f * (viz_rand01(&seed) * 2.0f - 1.0f);
        viz_tick(buf, N, 1.0f, &avg, &tick, 1.0f / 60.0f);
        CHECK(tick > 0.5f, "a 1 ms click over a quiet bed gives a tick");
        CHECK(tick <= 1.0f, "tick stays in 0..1");

        for (f = 0; f < 60; ++f) {
            for (j = 0; j < N; ++j)
                buf[j] = 0.001f * (viz_rand01(&seed) * 2.0f - 1.0f);
            viz_tick(buf, N, 1.0f, &avg, &tick, 1.0f / 60.0f);
        }
        CHECK(tick < 0.05f, "tick decays after a second");

        viz_tick(buf, 0, 1.0f, &avg, &tick, 1.0f / 60.0f);
        CHECK(tick >= 0.0f && tick <= 1.0f, "an empty window is safe");

        avg = 0.0f;
        tick = 0.0f;
        for (j = 0; j < N; ++j)
            buf[j] = (j >= 500 && j < 544) ? 1e-6f * (j % 2 ? -1.0f : 1.0f) : 0.0f;
        viz_tick(buf, N, 1.0f, &avg, &tick, 1.0f / 60.0f);
        CHECK(tick == 0.0f, "a click below the floor is ignored");
    }

    /* Random state: never stuck at zero, always in 0..1, same seed same run. */
    {
        unsigned int a = 12345u, b = 12345u, z = 0u;
        float r01;
        int ok = 1, same = 1;

        for (i = 0; i < 1000; ++i) {
            r01 = viz_rand01(&a);
            if (r01 < 0.0f || r01 >= 1.0f)
                ok = 0;
            if (r01 != viz_rand01(&b))
                same = 0;
        }
        CHECK(ok, "rand01 stays in 0..1");
        CHECK(same, "same seed gives the same sequence");
        viz_rand_next(&z);
        CHECK(z != 0u, "zero seed is repaired");
    }

    /* Wander: impulses never push it past the clamp, and it decays to rest. */
    {
        float w[2] = {0.0f, 0.0f};
        unsigned int rng = 99u;
        float mag, worst = 0.0f;

        for (i = 0; i < 10000; ++i) {
            viz_wander_step(w, &rng, 1.0f, 1.0f / 60.0f);
            mag = sqrtf(w[0] * w[0] + w[1] * w[1]);
            if (mag > worst)
                worst = mag;
        }
        CHECK(worst <= VIZ_WANDER_MAX + 1e-5f, "wander stays within its clamp");
        CHECK(worst > 0.0f, "impulses move the wander");
        for (i = 0; i < 600; ++i)
            viz_wander_step(w, &rng, 0.0f, 1.0f / 60.0f);
        CHECK(sqrtf(w[0] * w[0] + w[1] * w[1]) < 0.01f, "wander decays to rest");
    }

    /* Novelty: zero for the same spectrum, and rising with the difference. */
    {
        float avg[16], near[16], far[16];

        for (i = 0; i < 16; ++i) {
            avg[i] = 0.4f;
            near[i] = 0.4f;
            far[i] = 0.9f;
        }
        CHECK(viz_novelty(avg, near, 16) == 0.0f, "novelty is zero for the same spectrum");
        CHECK(viz_novelty(avg, far, 16) > 0.4f, "novelty rises with a change");
        CHECK(viz_novelty(avg, far, 0) == 0.0f, "novelty of no bars is zero");
    }

    /* Shape metric: at p=2 with no depth it is the old circle distance. */
    {
        int j;
        int same = 1, diamond_ok = 1, rot_ok = 1, finite = 1, mono = 1;
        float prev = -1.0f;

        for (j = 0; j < 40; ++j) {
            const float a = (float)j * 0.157f;
            const float u = 0.9f * cosf(a), w = 0.6f * sinf(a);
            const float old = sqrtf(u * u + w * w) * 0.70710678f;
            if (fabsf(viz_shape_metric(u, w, 6, 0.0f, 0.0f, 2.0f) - old) > 1e-5f)
                same = 0;
        }
        CHECK(same, "shape metric matches the circle at p=2 with no depth");

        /* p=1 is a diamond: the corner maps to 1, an axis tip to 0.5. */
        diamond_ok = fabsf(viz_shape_metric(1.0f, 1.0f, 6, 0.0f, 0.0f, 1.0f) - 1.0f) < 1e-5f
                  && fabsf(viz_shape_metric(1.0f, 0.0f, 6, 0.0f, 0.0f, 1.0f) - 0.5f) < 1e-5f;
        CHECK(diamond_ok, "diamond maps the corner to 1 and the axis tip to 0.5");

        /* p=4 is squarer: an off-axis point sits closer to the edge than at p=2. */
        CHECK(viz_shape_metric(1.0f, 0.7f, 6, 0.0f, 0.0f, 4.0f)
              > viz_shape_metric(1.0f, 0.7f, 6, 0.0f, 0.0f, 2.0f),
              "squircle is squarer than the circle");

        /* n-fold symmetry at p=2: rotating by 2*pi/n leaves the metric alone. */
        for (j = 0; j < 20; ++j) {
            const float a = (float)j * 0.31f;
            const float phi = 2.0f * 3.14159265f / 5.0f;
            const float u = 0.7f * cosf(a), w = 0.7f * sinf(a);
            const float ru = 0.7f * cosf(a + phi), rw = 0.7f * sinf(a + phi);
            if (fabsf(viz_shape_metric(u, w, 5, 0.4f, 0.0f, 2.0f)
                      - viz_shape_metric(ru, rw, 5, 0.4f, 0.0f, 2.0f)) > 1e-4f)
                rot_ok = 0;
        }
        CHECK(rot_ok, "petals repeat every 2*pi/n");

        /* Finite at the origin and at maximum depth, and along a ray at depth 0 it grows. */
        finite = isfinite(viz_shape_metric(0.0f, 0.0f, 6, 0.5f, 0.0f, 2.0f))
              && isfinite(viz_shape_metric(0.3f, -0.2f, 6, 0.5f, 1.0f, 3.0f));
        CHECK(finite, "shape metric is finite at the origin and at full depth");
        for (j = 1; j <= 20; ++j) {
            const float m = viz_shape_metric((float)j * 0.05f, 0.0f, 6, 0.0f, 0.0f, 2.0f);
            if (m <= prev)
                mono = 0;
            prev = m;
        }
        CHECK(mono, "shape metric grows outward along a ray at zero depth");
        CHECK(diamond_ok && same && rot_ok && finite && mono, "shape metric checks pass");
    }

    /* Anchors: every one sits inside |c| <= 1.2, and indices wrap. */
    {
        int idx;
        int bounded = 1;

        for (idx = 0; idx < VIZ_JULIA_ANCHORS; ++idx) {
            float ar, ai;

            viz_julia_anchor(idx, &ar, &ai);
            if (sqrtf(ar * ar + ai * ai) > 1.2f)
                bounded = 0;
        }
        CHECK(bounded, "julia anchors are bounded");
        viz_julia_anchor(VIZ_JULIA_ANCHORS + 2, &cr, &ci);
        {
            float r2, i2;
            viz_julia_anchor(2, &r2, &i2);
            CHECK(cr == r2 && ci == i2, "anchor index wraps forward");
            viz_julia_anchor(-1, &cr, &ci);
            viz_julia_anchor(VIZ_JULIA_ANCHORS - 1, &r2, &i2);
            CHECK(cr == r2 && ci == i2, "anchor index wraps backward");
        }
    }

    if (failures) {
        fprintf(stderr, "test_viz: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_viz: ok\n");
    return 0;
}
