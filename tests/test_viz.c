/* test_viz.c — unit checks for the visualizer maths in src/viz.c. No device. */
#include <math.h>
#include <stdio.h>
#include <string.h>

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

    /* Shape metric and spectrum outline for the background shape. */
    {
        float flat[VIZ_OUTLINE_N], bars_flat[16], bars_bass[16], out[VIZ_OUTLINE_N];
        float zero_out[VIZ_OUTLINE_N];
        int j, k;
        int same = 1, mirror = 1, finite = 1, mono = 1, lift = 1;
        int flat_ok = 1, zero_ok = 1, mean_ok = 1;
        float prev = -1.0f, mean = 0.0f;

        /* Zero depth is the plain circle, whatever the outline holds. */
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            flat[k] = (k % 2) ? 0.4f : -0.4f;
        for (j = 0; j < 40; ++j) {
            const float a = (float)j * 0.157f;
            const float u = 0.9f * cosf(a), w = 0.6f * sinf(a);
            const float old = sqrtf(u * u + w * w) * 0.70710678f;
            if (fabsf(viz_shape_metric(u, w, flat, VIZ_OUTLINE_N, 0.0f) - old) > 1e-5f)
                same = 0;
        }
        CHECK(same, "shape metric is the circle with no depth, whatever the outline");

        /* Left and right of the centre line look up the same outline. */
        for (j = 0; j < 20; ++j) {
            const float a = (float)j * 0.31f;
            const float u = 0.7f * cosf(a), w = 0.7f * sinf(a);
            if (fabsf(viz_shape_metric(u, w, flat, VIZ_OUTLINE_N, 0.6f)
                      - viz_shape_metric(-u, w, flat, VIZ_OUTLINE_N, 0.6f)) > 1e-5f)
                mirror = 0;
        }
        CHECK(mirror, "shape is mirrored left to right");

        /* Finite at the origin and at full depth. */
        finite = isfinite(viz_shape_metric(0.0f, 0.0f, flat, VIZ_OUTLINE_N, 0.9f))
              && isfinite(viz_shape_metric(0.3f, -0.2f, flat, VIZ_OUTLINE_N, 0.9f));
        CHECK(finite, "shape metric is finite at the origin and at full depth");

        /* Along a ray at zero depth it still grows outward. */
        for (j = 1; j <= 20; ++j) {
            const float m = viz_shape_metric((float)j * 0.05f, 0.0f, flat, VIZ_OUTLINE_N, 0.0f);
            if (m <= prev)
                mono = 0;
            prev = m;
        }
        CHECK(mono, "shape metric grows outward along a ray at zero depth");

        /* A louder band at the bottom (outline index 0) bulges the bottom: the
         * same point at the bottom reads closer in than at the side. */
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            flat[k] = 0.0f;
        flat[0] = 0.5f;
        flat[VIZ_OUTLINE_N - 1] = 0.0f;
        lift = viz_shape_metric(0.0f, 0.6f, flat, VIZ_OUTLINE_N, 0.5f)
             < viz_shape_metric(0.6f, 0.0f, flat, VIZ_OUTLINE_N, 0.5f);
        CHECK(lift, "a loud bass band pulls the bottom of the shape outward");

        /* Outline: flat spectrum gives zeros, a mean of zero, and no bars gives zeros. */
        for (k = 0; k < 16; ++k) {
            bars_flat[k] = 0.3f;
            bars_bass[k] = (k < 4) ? 0.9f : 0.1f;
        }
        viz_outline(bars_flat, 16, out, VIZ_OUTLINE_N);
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            if (fabsf(out[k]) > 1e-6f)
                flat_ok = 0;
        CHECK(flat_ok, "a flat spectrum gives a flat outline");

        viz_outline(bars_bass, 16, out, VIZ_OUTLINE_N);
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            mean += out[k];
        mean /= (float)VIZ_OUTLINE_N;
        mean_ok = fabsf(mean) < 0.05f && out[0] > 0.0f && out[VIZ_OUTLINE_N - 1] < 0.0f;
        CHECK(mean_ok, "outline is zero-mean, bass high at index 0, treble low at the end");

        viz_outline(bars_bass, 0, zero_out, VIZ_OUTLINE_N);
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            if (zero_out[k] != 0.0f)
                zero_ok = 0;
        CHECK(zero_ok, "outline of no bars is zero");

        CHECK(same && mirror && finite && mono && lift && flat_ok && mean_ok && zero_ok,
              "shape outline checks pass");
    }

    /* Onset: a band rising over its slow average fires, steady sound does not. */
    {
        float bars[16], avg[16], rise[VIZ_ONSET_GROUPS];
        int k, steady = 1, spike_ok = 1, small_ok = 1;

        for (k = 0; k < 16; ++k)
            bars[k] = avg[k] = 0.3f;
        viz_onset(bars, avg, 16, rise, VIZ_ONSET_GROUPS);
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (rise[k] != 0.0f)
                steady = 0;
        CHECK(steady, "steady spectrum gives no onset");

        /* Six groups over 16 bars: group 2 is bars 5..7. */
        for (k = 0; k < 16; ++k)
            bars[k] = 0.3f;
        bars[5] = bars[6] = bars[7] = 0.8f;
        viz_onset(bars, avg, 16, rise, VIZ_ONSET_GROUPS);
        spike_ok = rise[2] > 0.9f;
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (k != 2 && rise[k] != 0.0f)
                spike_ok = 0;
        CHECK(spike_ok, "a rise in one band fires only that band");

        /* A rise under the margin is ignored. */
        for (k = 0; k < 16; ++k)
            bars[k] = 0.36f;
        viz_onset(bars, avg, 16, rise, VIZ_ONSET_GROUPS);
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (rise[k] != 0.0f)
                small_ok = 0;
        CHECK(small_ok, "a rise below the margin is ignored");
    }

    /* Stereo balance: a band's side decides its pan, silence centres. */
    {
        float left[16], right[16], pan[VIZ_ONSET_GROUPS];
        int k, ok = 1, bounded = 1, mono_ok = 1, quiet_ok = 1;
        float x_prev = -2.0f;

        for (k = 0; k < 16; ++k)
            left[k] = right[k] = 0.0f;
        viz_pan(left, right, 16, pan, VIZ_ONSET_GROUPS);
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (pan[k] != 0.0f)
                quiet_ok = 0;
        CHECK(quiet_ok, "silence centres every band");

        for (k = 0; k < 16; ++k) {
            left[k] = 0.6f;
            right[k] = 0.0f;
        }
        viz_pan(left, right, 16, pan, VIZ_ONSET_GROUPS);
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (pan[k] > -0.99f)
                ok = 0;
        CHECK(ok, "left-only sound pans fully left");

        for (k = 0; k < 16; ++k) {
            left[k] = 0.0f;
            right[k] = 0.6f;
        }
        viz_pan(left, right, 16, pan, VIZ_ONSET_GROUPS);
        ok = 1;
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (pan[k] < 0.99f)
                ok = 0;
        CHECK(ok, "right-only sound pans fully right");

        for (k = 0; k < 16; ++k)
            left[k] = right[k] = 0.4f;
        viz_pan(left, right, 16, pan, VIZ_ONSET_GROUPS);
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k)
            if (pan[k] != 0.0f)
                mono_ok = 0;
        CHECK(mono_ok, "equal sides centre");

        /* Stage x follows the balance and stays on the stage. */
        {
            float p;
            for (p = -1.0f; p <= 1.0f; p += 0.25f) {
                float x = viz_pan_to_x(p, 0.0f, 0.0f);
                if (x < x_prev)
                    bounded = 0;
                if (x < -0.71f || x > 0.71f)
                    bounded = 0;
                x_prev = x;
            }
            CHECK(bounded, "pan maps monotonically within the stage");
            CHECK(viz_pan_to_x(1.0f, 1.0f, 1.0f) <= 0.71f, "pan with jitter stays on stage");
        }
    }

    /* Stereo width, actor drift, and the left/right background shape. */
    {
        float l[8], r[8], ol[VIZ_OUTLINE_N], orr[VIZ_OUTLINE_N], bl[16], br[16];
        float oe[VIZ_OUTLINE_N], pair_l[VIZ_OUTLINE_N], pair_r[VIZ_OUTLINE_N];
        float w_mono, w_out, w_silent, w_one;
        VizActor a;
        int k, width_ok = 1, same_ok = 1, shape_same = 1, seam_ok = 1;
        float x_prev = 0.0f;

        for (k = 0; k < 8; ++k)
            l[k] = r[k] = 0.3f * (float)(k % 3) - 0.2f;
        w_mono = viz_width(l, r, 8);
        CHECK(w_mono == 0.0f, "mono has zero width");

        for (k = 0; k < 8; ++k)
            r[k] = -l[k];
        w_out = viz_width(l, r, 8);
        CHECK(w_out == 1.0f, "out of phase clamps to full width");

        for (k = 0; k < 8; ++k)
            l[k] = r[k] = 0.0f;
        w_silent = viz_width(l, r, 8);
        CHECK(w_silent == 0.0f, "silence has zero width");

        for (k = 0; k < 8; ++k) {
            l[k] = 0.5f;
            r[k] = 0.0f;
        }
        w_one = viz_width(l, r, 8);
        width_ok = w_one >= 0.0f && w_one <= 1.0f;
        CHECK(width_ok, "width stays within 0..1");

        /* Drift: eases toward the target, never past it, and does nothing if
         * the actor is releasing or not live. */
        memset(&a, 0, sizeof a);
        a.live = 1;
        a.x = 0.0f;
        for (k = 0; k < 200; ++k) {
            viz_actor_drift(&a, 0.05f, 0.5f);
            if (a.x > 0.5f + 1e-6f)
                seam_ok = 0;
        }
        CHECK(seam_ok && fabsf(a.x - 0.5f) < 1e-3f, "drift converges without overshoot");
        a.releasing = 1;
        x_prev = a.x;
        viz_actor_drift(&a, 0.05f, -0.5f);
        CHECK(a.x == x_prev, "releasing actor does not drift");
        a.releasing = 0;
        a.live = 0;
        viz_actor_drift(&a, 0.05f, -0.5f);
        CHECK(a.x == x_prev, "dead actor does not drift");

        /* Outlines: identical sides match the mono outline; a lopsided mix
         * bulges one way on the left and the other on the right. */
        for (k = 0; k < 16; ++k)
            bl[k] = br[k] = 0.1f + 0.05f * (float)(k % 4);
        viz_outline(bl, 16, oe, VIZ_OUTLINE_N);
        viz_outline_pair(bl, br, 16, pair_l, pair_r, VIZ_OUTLINE_N);
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            if (fabsf(pair_l[k] - oe[k]) > 1e-6f || fabsf(pair_r[k] - oe[k]) > 1e-6f)
                same_ok = 0;
        CHECK(same_ok, "identical sides give the mono outline");

        for (k = 0; k < 16; ++k) {
            bl[k] = 0.8f;
            br[k] = 0.2f;
        }
        viz_outline_pair(bl, br, 16, pair_l, pair_r, VIZ_OUTLINE_N);
        CHECK(pair_l[0] > 0.0f && pair_r[0] < 0.0f,
              "a lopsided mix bulges to its own side");

        /* Shape metric: identical halves match the plain metric; a lopsided
         * pair differs at mirrored points; the seam is finite. */
        for (k = 0; k < VIZ_OUTLINE_N; ++k) {
            ol[k] = 0.3f;
            orr[k] = -0.3f;
        }
        for (k = 0; k < VIZ_OUTLINE_N; ++k)
            pair_l[k] = pair_r[k] = 0.25f;
        {
            const float plain = viz_shape_metric(0.4f, 0.5f, pair_l, VIZ_OUTLINE_N, 0.5f);
            const float lr = viz_shape_metric_lr(0.4f, 0.5f, pair_l, pair_r, VIZ_OUTLINE_N, 0.5f);
            if (fabsf(plain - lr) > 1e-6f)
                shape_same = 0;
        }
        CHECK(shape_same, "identical halves match the plain shape metric");
        {
            const float m_left = viz_shape_metric_lr(-0.5f, 0.5f, ol, orr, VIZ_OUTLINE_N, 0.5f);
            const float m_right = viz_shape_metric_lr(0.5f, 0.5f, ol, orr, VIZ_OUTLINE_N, 0.5f);
            CHECK(fabsf(m_left - m_right) > 1e-3f,
                  "a lopsided shape differs left and right");
        }
        for (k = -10; k <= 10; ++k) {
            const float m = viz_shape_metric_lr((float)k * 0.02f, 0.4f, ol, orr,
                                                VIZ_OUTLINE_N, 0.5f);
            if (!isfinite(m))
                seam_ok = 0;
        }
        CHECK(seam_ok, "the shape is finite across the seam");
    }

    /* Actors: slots, spawn placement and ageing. */
    {
        VizActor pool[VIZ_ACTORS];
        VizActor one;
        unsigned int rng = 12345u, rng2 = 12345u;
        int k, slot_ok = 1, bound_ok = 1, same_ok = 1, size_ok = 1, hue_ok = 1;
        int finite_ok = 1;

        memset(pool, 0, sizeof pool);
        CHECK(viz_actor_slot(pool, 0) == -1, "no slots means -1");
        CHECK(viz_actor_slot(pool, VIZ_ACTORS) == 0, "a free slot is taken first");

        /* All live: a releasing dim one is taken before a bright live one. */
        for (k = 0; k < VIZ_ACTORS; ++k) {
            pool[k].live = 1;
            pool[k].env = 0.9f;
            pool[k].releasing = 0;
        }
        pool[3].env = 0.2f;
        pool[3].releasing = 1;
        pool[1].env = 0.05f;   /* dimmer but not releasing */
        slot_ok = viz_actor_slot(pool, VIZ_ACTORS) == 3;
        CHECK(slot_ok, "a releasing actor is replaced before a live one");

        /* Spawn across every band and kind: constant bounded, position on stage. */
        for (k = 0; k < VIZ_ONSET_GROUPS; ++k) {
            int kind;
            for (kind = 0; kind < 3; ++kind) {
                VizActor a;
                viz_actor_spawn(&a, &rng, kind, k, VIZ_ONSET_GROUPS, 0.5f);
                if (sqrtf(a.cr * a.cr + a.ci * a.ci) > 1.2f)
                    bound_ok = 0;
                if (fabsf(a.x) > 0.71f || fabsf(a.y) > 0.56f)
                    bound_ok = 0;
                if (!a.live || a.releasing || a.env != 0.0f)
                    bound_ok = 0;
            }
        }
        CHECK(bound_ok, "spawned actors have bounded constants and sit on the stage");

        /* Same seed, same actor. */
        rng = rng2 = 12345u;
        viz_actor_spawn(&one, &rng, 0, 1, VIZ_ONSET_GROUPS, 0.7f);
        {
            VizActor two;
            viz_actor_spawn(&two, &rng2, 0, 1, VIZ_ONSET_GROUPS, 0.7f);
            same_ok = one.cr == two.cr && one.ci == two.ci && one.x == two.x
                   && one.y == two.y && one.size == two.size;
        }
        CHECK(same_ok, "the same seed spawns the same actor");

        /* Size: a lead is bigger than an extra, which is bigger than a spark. */
        {
            VizActor lead, extra, spark;
            unsigned int r3 = 999u;
            viz_actor_spawn(&lead, &r3, VIZ_ACTOR_LEAD, 0, VIZ_ONSET_GROUPS, 0.5f);
            viz_actor_spawn(&extra, &r3, VIZ_ACTOR_EXTRA, 0, VIZ_ONSET_GROUPS, 0.5f);
            viz_actor_spawn(&spark, &r3, VIZ_ACTOR_SPARK, 0, VIZ_ONSET_GROUPS, 0.5f);
            size_ok = lead.size > extra.size && extra.size > spark.size;
        }
        CHECK(size_ok, "leads are bigger than extras, which are bigger than sparks");

        /* Hue: bass band is lower than treble band. */
        {
            VizActor lo, hi;
            unsigned int r4 = 4242u;
            viz_actor_spawn(&lo, &r4, VIZ_ACTOR_EXTRA, 0, VIZ_ONSET_GROUPS, 0.5f);
            viz_actor_spawn(&hi, &r4, VIZ_ACTOR_SPARK, VIZ_ONSET_GROUPS - 1,
                            VIZ_ONSET_GROUPS, 0.5f);
            hue_ok = lo.hue < hi.hue;
        }
        CHECK(hue_ok, "bass actors take a lower hue than treble actors");

        /* Pitch look: treble is smaller, brighter, shimmers more and lives
         * shorter than bass, all from the same kind and strength. */
        {
            VizActor lo, hi;
            unsigned int r6 = 31337u;
            int look_ok;

            viz_actor_spawn(&lo, &r6, VIZ_ACTOR_SPARK, 0, VIZ_ONSET_GROUPS, 0.5f);
            viz_actor_spawn(&hi, &r6, VIZ_ACTOR_SPARK, VIZ_ONSET_GROUPS - 1,
                            VIZ_ONSET_GROUPS, 0.5f);
            look_ok = hi.size < lo.size && hi.bright > lo.bright
                   && hi.shimmer > lo.shimmer && hi.life < lo.life
                   && lo.shimmer == 0.0f;
            CHECK(look_ok, "treble actors are smaller, brighter, shimmer more and live shorter");
        }

        /* Quantity and cadence: more per onset and sooner again as the band rises. */
        {
            int g, count_ok = 1, cool_ok = 1;
            int prev_count = 0;
            float prev_cool = 1e9f;

            for (g = 0; g < VIZ_ONSET_GROUPS; ++g) {
                const int c = viz_actor_count(g, VIZ_ONSET_GROUPS);
                const float cd = viz_actor_cooldown(g, VIZ_ONSET_GROUPS);

                if (c < prev_count || c < 1)
                    count_ok = 0;
                if (cd > prev_cool || cd <= 0.0f)
                    cool_ok = 0;
                prev_count = c;
                prev_cool = cd;
            }
            CHECK(viz_actor_count(0, VIZ_ONSET_GROUPS) == 1 && count_ok,
                  "bass brings one actor and higher bands bring more");
            CHECK(cool_ok, "higher bands may spawn again sooner");
        }

        /* Shimmer: no shimmer means no flicker; otherwise it stays in 0..1. */
        {
            VizActor still, flick;
            unsigned int r7 = 555u;
            int tw_ok = 1, t;

            viz_actor_spawn(&still, &r7, VIZ_ACTOR_EXTRA, 0, VIZ_ONSET_GROUPS, 0.5f);
            viz_actor_spawn(&flick, &r7, VIZ_ACTOR_SPARK, VIZ_ONSET_GROUPS - 1,
                            VIZ_ONSET_GROUPS, 0.5f);
            for (t = 0; t < 400; ++t) {
                const float m = viz_actor_twinkle(&flick, t * 0.05f);

                if (!isfinite(m) || m < 0.0f || m > 1.0f)
                    tw_ok = 0;
                if (viz_actor_twinkle(&still, t * 0.05f) != 1.0f)
                    tw_ok = 0;
            }
            CHECK(tw_ok, "twinkle stays in 0..1 and only shimmering actors flicker");
        }

        /* Eviction: with every slot live and equally bright, a spark goes first. */
        {
            VizActor full[VIZ_ACTORS];
            unsigned int r8 = 8u;
            int i, evict_ok;

            for (i = 0; i < VIZ_ACTORS; ++i) {
                const int kind = i % 3;

                viz_actor_spawn(&full[i], &r8, kind, 2, VIZ_ONSET_GROUPS, 0.5f);
                full[i].env = 0.5f;
            }
            /* Slot 2 is a spark; slot 0 is a lead, slot 1 an extra. */
            full[0].kind = VIZ_ACTOR_LEAD;
            full[1].kind = VIZ_ACTOR_EXTRA;
            full[2].kind = VIZ_ACTOR_SPARK;
            evict_ok = viz_actor_slot(full, VIZ_ACTORS) == 2;
            CHECK(evict_ok, "with all slots live, a spark is replaced before an extra or lead");
        }

        /* Ageing: holds on rate 0, releases when presence drops, dies after the
         * fade, and never goes non-finite. */
        {
            VizActor a;
            unsigned int r5 = 77u;
            float age_before;
            int t;

            viz_actor_spawn(&a, &r5, VIZ_ACTOR_EXTRA, 2, VIZ_ONSET_GROUPS, 0.5f);
            for (t = 0; t < 40; ++t) {       /* 2 s, music running */
                viz_actor_step(&a, 0.05f, 1.0f, 1.0f);
                if (!isfinite(a.env) || a.env < 0.0f || a.env > 1.0f)
                    finite_ok = 0;
            }
            CHECK(a.live && a.env > 0.9f, "an actor fades in while its sound holds");

            age_before = a.age;
            for (t = 0; t < 200; ++t)        /* 10 s of a held tone */
                viz_actor_step(&a, 0.05f, 0.0f, 1.0f);
            CHECK(a.live && a.age == age_before && !a.releasing,
                  "a held tone with no rate holds the actor still");

            for (t = 0; t < 40; ++t)         /* 2 s with the sound gone */
                viz_actor_step(&a, 0.05f, 1.0f, 0.0f);
            for (t = 0; t < 400; ++t)        /* 20 s more to finish fading */
                viz_actor_step(&a, 0.05f, 1.0f, 0.0f);
            CHECK(!a.live && a.env == 0.0f, "a released actor fades out and frees its slot");
            CHECK(finite_ok, "actor envelope stays finite and within 0..1");
        }
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
