/* viz.c — pure visualizer maths. See viz.h. */
#include "viz.h"

#include <math.h>

static const float PI_F = 3.14159265358979f;

void
viz_settings_clamp(VizSettings *s)
{
    if (s->mode < 0)
        s->mode = 0;
    if (s->mode >= VIZ_MODE_COUNT)
        s->mode = VIZ_MODE_COUNT - 1;
    if (s->gain_pct < 25)
        s->gain_pct = 25;
    if (s->gain_pct > 400)
        s->gain_pct = 400;
    if (s->smooth_pct < 0)
        s->smooth_pct = 0;
    if (s->smooth_pct > 100)
        s->smooth_pct = 100;
    if (s->bars < VIZ_BARS_MIN)
        s->bars = VIZ_BARS_MIN;
    if (s->bars > VIZ_BARS_MAX)
        s->bars = VIZ_BARS_MAX;
    if (s->hue_pct < 0)
        s->hue_pct = 0;
    if (s->hue_pct > 100)
        s->hue_pct = 100;
}

/* Bit-reverses the low `bits` bits of x. */
static unsigned
reverse_bits(unsigned x, int bits)
{
    unsigned r = 0;
    int i;
    for (i = 0; i < bits; ++i) {
        r = (r << 1) | (x & 1u);
        x >>= 1;
    }
    return r;
}

int
viz_fft(float *re, float *im, int n)
{
    int bits = 0, i, len;

    if (n < 2 || (n & (n - 1)) != 0)
        return -1;
    while ((1 << bits) < n)
        ++bits;

    for (i = 0; i < n; ++i) {
        unsigned j = reverse_bits((unsigned)i, bits);
        if ((int)j > i) {
            float t = re[i];
            re[i] = re[j];
            re[j] = t;
            t = im[i];
            im[i] = im[j];
            im[j] = t;
        }
    }

    for (len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * PI_F / (float)len;
        const int half = len >> 1;
        int k;
        for (i = 0; i < n; i += len) {
            for (k = 0; k < half; ++k) {
                const float wr = cosf(ang * (float)k);
                const float wi = sinf(ang * (float)k);
                const int a = i + k, b = i + k + half;
                const float xr = re[b] * wr - im[b] * wi;
                const float xi = re[b] * wi + im[b] * wr;
                re[b] = re[a] - xr;
                im[b] = im[a] - xi;
                re[a] += xr;
                im[a] += xi;
            }
        }
    }
    return 0;
}

void
viz_spectrum(const float *samples, int n, float *bars, int nbars, float gain)
{
    static float re[VIZ_FFT_N], im[VIZ_FFT_N];
    const int half = VIZ_FFT_N / 2;
    float win_sum = 0.0f;
    int i, b;

    if (nbars <= 0)
        return;

    /* Newest VIZ_FFT_N samples; zero-pad when the input is short. */
    for (i = 0; i < VIZ_FFT_N; ++i) {
        const int src = n - VIZ_FFT_N + i;
        const float s = (src >= 0 && src < n) ? samples[src] : 0.0f;
        const float w = 0.5f - 0.5f * cosf(2.0f * PI_F * (float)i / (float)(VIZ_FFT_N - 1));
        re[i] = s * w;
        im[i] = 0.0f;
        win_sum += w;
    }
    viz_fft(re, im, VIZ_FFT_N);

    /* Log-spaced bands over bins 1..half-1. Each bar gets at least one bin. */
    for (b = 0; b < nbars; ++b) {
        const float f0 = powf((float)half, (float)b / (float)nbars);
        const float f1 = powf((float)half, (float)(b + 1) / (float)nbars);
        int lo = (int)f0, hi = (int)f1, k;
        double acc = 0.0;
        int count = 0;
        float v;

        if (lo < 1)
            lo = 1;
        if (hi <= lo)
            hi = lo + 1;
        if (hi > half)
            hi = half;
        for (k = lo; k < hi; ++k) {
            const float mag = sqrtf(re[k] * re[k] + im[k] * im[k]);
            acc += mag;
            ++count;
        }
        /* Amplitude-normalised: a full-scale sine reads about 1.0. */
        v = count ? (float)(acc / count) * 2.0f / win_sum : 0.0f;
        v *= gain;
        /* 60 dB range: -60 dBFS maps to 0, 0 dBFS to 1. */
        v = (20.0f * log10f(v + 1e-9f) + 60.0f) / 60.0f;
        if (v < 0.0f)
            v = 0.0f;
        if (v > 1.0f)
            v = 1.0f;
        bars[b] = v;
    }
}

void
viz_smooth(float *cur, const float *target, int n, float smooth)
{
    const float fall = 0.08f + 0.5f * (1.0f - smooth);
    int i;
    for (i = 0; i < n; ++i) {
        const float k = target[i] > cur[i] ? 0.7f : fall;
        cur[i] += (target[i] - cur[i]) * k;
    }
}

void
viz_hue_rgb(float hue, unsigned char *r, unsigned char *g, unsigned char *b)
{
    const float h6 = (hue - floorf(hue)) * 6.0f;
    const int sector = (int)h6 % 6;
    const float f = h6 - floorf(h6);
    const float q = 1.0f - f;
    float rr = 0, gg = 0, bb = 0;

    switch (sector) {
    case 0: rr = 1;  gg = f;  bb = 0;  break;
    case 1: rr = q;  gg = 1;  bb = 0;  break;
    case 2: rr = 0;  gg = 1;  bb = f;  break;
    case 3: rr = 0;  gg = q;  bb = 1;  break;
    case 4: rr = f;  gg = 0;  bb = 1;  break;
    default: rr = 1; gg = 0;  bb = q;  break;
    }
    *r = (unsigned char)(rr * 255.0f + 0.5f);
    *g = (unsigned char)(gg * 255.0f + 0.5f);
    *b = (unsigned char)(bb * 255.0f + 0.5f);
}

void
viz_fractal_c(float bass, float mid, float treble, float *cr, float *ci)
{
    /* No clock: the constant is a pure function of the band energies, so a
     * steady sound holds a steady shape. Inputs are 0..1, output stays inside
     * the bounded Julia range around -0.7 + 0.27i. */
    *cr = -0.7f + 0.15f * bass + 0.05f * treble;
    *ci = 0.27f + 0.10f * mid - 0.05f * treble;
}

void
viz_compress(float *dst, const float *src, int n, float exponent)
{
    int i;

    for (i = 0; i < n; ++i) {
        float x = src[i];

        if (x <= 0.0f) {
            dst[i] = 0.0f;
            continue;
        }
        if (x > 1.0f)
            x = 1.0f;
        dst[i] = powf(x, exponent);
    }
}

float
viz_gate(float level, float floor)
{
    float t = (level - floor) * 4.0f;

    if (t <= 0.0f)
        return 0.0f;
    if (t >= 1.0f)
        return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

float
viz_band_mean(const float *bars, int from, int to)
{
    float sum = 0.0f;
    int i;

    if (to <= from)
        return 0.0f;
    for (i = from; i < to; ++i)
        sum += bars[i];
    return sum / (float)(to - from);
}

float
viz_band_gate(const float *bars, int from, int to, float floor)
{
    return viz_gate(viz_band_mean(bars, from, to), floor);
}

float
viz_flux(const float *prev, const float *cur, int n)
{
    float sum = 0.0f;
    int i;

    for (i = 0; i < n; ++i) {
        const float d = cur[i] - prev[i];
        if (d > 0.0f)
            sum += d;
    }
    /* Mean positive rise per bar, scaled so a strong onset reaches 1. */
    if (n <= 0)
        return 0.0f;
    sum = sum / (float)n * 4.0f;
    return sum > 1.0f ? 1.0f : sum;
}

float
viz_centroid(const float *bars, int n)
{
    float wsum = 0.0f, total = 0.0f;
    int i;

    if (n < 2)
        return 0.5f;
    for (i = 0; i < n; ++i) {
        wsum += (float)i * bars[i];
        total += bars[i];
    }
    if (total < 1e-6f)
        return 0.5f;
    return wsum / total / (float)(n - 1);
}

int
viz_ring_bin(float r, int nbars)
{
    /* Log-spaced: the inner rings cover the bass bars and the outer rings the
     * treble, with the same number of bars spread across the radius. */
    float f;
    int b;

    if (nbars < 1)
        return 0;
    if (r < 0.0f)
        r = 0.0f;
    if (r > 1.0f)
        r = 1.0f;
    f = log2f(1.0f + 15.0f * r) / 4.0f; /* 0..1 */
    b = (int)(f * (float)nbars);
    if (b >= nbars)
        b = nbars - 1;
    return b < 0 ? 0 : b;
}

float
viz_smooth_mu(int n, float zx, float zy)
{
    float mod = sqrtf(zx * zx + zy * zy);

    if (mod < 2.0f)
        mod = 2.0f; /* only called on escape, so |z| >= 2 already */
    float mu = (float)n + 1.0f - log2f(log2f(mod));

    /* Far escapes can dip below zero; keep 0 as the floor so a negative value
     * can only mean "inside the set". */
    return mu < 0.0f ? 0.0f : mu;
}

float
viz_julia_mu(float zx, float zy, float cr, float ci, int iter)
{
    int n = 0;
    float nz;

    while (n < iter && zx * zx + zy * zy < 4.0f) {
        nz = zx * zx - zy * zy + cr;
        zy = 2.0f * zx * zy + ci;
        zx = nz;
        ++n;
    }
    if (zx * zx + zy * zy < 4.0f)
        return -1.0f;
    return viz_smooth_mu(n, zx, zy);
}

float
viz_phoenix_mu(float zx, float zy, float cr, float ci, float p, int iter)
{
    /* Phoenix: z_{n+1} = z_n^2 + c + p * z_{n-1}, with z_{-1} = 0. The p term
     * is what gives the feathered, wing-like shapes. */
    float px = 0.0f, py = 0.0f;
    int n = 0;

    while (n < iter && zx * zx + zy * zy < 4.0f) {
        const float nx = zx * zx - zy * zy + cr + p * px;
        const float ny = 2.0f * zx * zy + ci + p * py;
        px = zx;
        py = zy;
        zx = nx;
        zy = ny;
        ++n;
    }
    if (zx * zx + zy * zy < 4.0f)
        return -1.0f;
    return viz_smooth_mu(n, zx, zy);
}

void
viz_kaleido(float re, float im, int segs, float rot, float *ore, float *oim)
{
    const float seg = 2.0f * PI_F / (float)(segs < 1 ? 1 : segs);
    const float r = sqrtf(re * re + im * im);
    float a = atan2f(im, re) - rot;

    a -= floorf(a / seg) * seg; /* 0..seg */
    if (a > 0.5f * seg)
        a = seg - a; /* mirror the upper half of the wedge */
    a += rot;
    *ore = r * cosf(a);
    *oim = r * sinf(a);
}

void
viz_beat(float *avg, float *kick, float bass, float dt)
{
    const float ka = 1.0f - expf(-dt * 2.0f); /* roughly half a second */
    /* Only a rise well above the running average counts as an onset. */
    float excess = (bass - *avg - 0.12f) * 4.0f;

    if (excess < 0.0f)
        excess = 0.0f;
    if (excess > 1.0f)
        excess = 1.0f;
    *avg += (bass - *avg) * ka;
    if (excess > *kick)
        *kick = excess;
    else
        *kick *= expf(-dt * 6.0f);
}

/* Transient (tick) detector, on the raw samples. The FFT bars average a click
 * away over a 23 ms window, so this looks at short blocks instead: the first
 * difference removes bass, each block's mean energy is taken, and the loudest
 * block is compared with the slow average of block energy. */
#define TICK_BLOCK     32     /* samples per energy block, about 0.7 ms */
#define TICK_FLOOR_DB  -70.0f /* block energy below this is hiss, ignored */
#define TICK_ABOVE_DB  12.0f  /* how far over the average a block must be */
#define TICK_RANGE_DB  12.0f  /* excess that maps to a full 1.0 */

void
viz_tick(const float *samples, int n, float gain, float *avg, float *tick, float dt)
{
    const float ka = 1.0f - expf(-dt * 2.0f);  /* about a 0.5 s average */
    const float tick_ka = expf(-dt * 8.0f);
    double block_acc = 0.0, sum = 0.0;
    float peak = 0.0f, mean, peak_db, avg_db, excess;
    int i, blocks = 0, count = 0;

    for (i = 1; i < n; ++i) {
        const float d = (samples[i] - samples[i - 1]) * gain;

        block_acc += (double)d * (double)d;
        if (++count == TICK_BLOCK) {
            const float e = (float)(block_acc / TICK_BLOCK);

            if (e > peak)
                peak = e;
            sum += e;
            ++blocks;
            block_acc = 0.0;
            count = 0;
        }
    }
    if (blocks == 0) {
        *tick *= tick_ka;
        return;
    }
    mean = (float)(sum / blocks);
    if (*avg <= 0.0f)
        *avg = mean;

    peak_db = 10.0f * log10f(peak + 1e-12f);
    avg_db = 10.0f * log10f(*avg + 1e-12f);
    excess = peak_db - avg_db - TICK_ABOVE_DB;
    if (peak_db < TICK_FLOOR_DB || excess < 0.0f)
        excess = 0.0f;
    if (excess > TICK_RANGE_DB)
        excess = TICK_RANGE_DB;
    excess /= TICK_RANGE_DB;

    /* The average is updated after the test, so a click cannot raise its own
     * threshold. */
    *avg += (mean - *avg) * ka;
    if (excess > *tick)
        *tick = excess;
    else
        *tick *= tick_ka;
}

/* Wander tuning: each onset adds up to WANDER_IMPULSE of offset, and the
 * offset loses about 1 - exp(-WANDER_SPRING * dt) of itself per frame. */
#define WANDER_IMPULSE 0.25f
#define WANDER_SPRING  0.5f

unsigned int
viz_rand_next(unsigned int *s)
{
    unsigned int x = *s ? *s : 1u;

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

float
viz_rand01(unsigned int *s)
{
    return (float)(viz_rand_next(s) >> 8) / 16777216.0f;
}

void
viz_wander_step(float w[2], unsigned int *rng, float impulse, float dt)
{
    const float decay = expf(-WANDER_SPRING * dt);
    float mag;

    if (impulse > 0.0f) {
        const float a = viz_rand01(rng) * 2.0f * PI_F;
        const float k = impulse * WANDER_IMPULSE;

        w[0] += k * cosf(a);
        w[1] += k * sinf(a);
    }
    w[0] *= decay;
    w[1] *= decay;

    mag = sqrtf(w[0] * w[0] + w[1] * w[1]);
    if (mag > VIZ_WANDER_MAX) {
        const float s = VIZ_WANDER_MAX / mag;

        w[0] *= s;
        w[1] *= s;
    }
}

float
viz_novelty(const float *avg, const float *cur, int n)
{
    float sum = 0.0f;
    int i;

    if (n <= 0)
        return 0.0f;
    for (i = 0; i < n; ++i)
        sum += fabsf(cur[i] - avg[i]);
    return sum / (float)n;
}

/* Resamples bars linearly onto n outline points, bass to treble, with mean
 * subtracted. */
static void
outline_fill(const float *bars, int nbars, float mean, float *out, int n)
{
    float pos;
    int i, b0, b1;

    for (i = 0; i < n; ++i) {
        float f, v;

        if (nbars < 1) {
            out[i] = 0.0f;
            continue;
        }
        pos = n > 1 ? (float)i / (float)(n - 1) * (float)(nbars - 1) : 0.0f;
        b0 = (int)pos;
        if (b0 >= nbars)
            b0 = nbars - 1;
        b1 = b0 + 1 < nbars ? b0 + 1 : nbars - 1;
        f = pos - (float)b0;
        v = bars[b0] * (1.0f - f) + bars[b1] * f;
        out[i] = v - mean;
    }
}

void
viz_outline(const float *bars, int nbars, float *out, int n)
{
    /* Mean removed, so a flat spectrum gives a flat outline (a circle). The
     * table runs bass (index 0) to treble (index n-1), resampled linearly. */
    float mean = 0.0f;
    int i;

    if (n <= 0)
        return;
    for (i = 0; i < nbars; ++i)
        mean += bars[i];
    mean = nbars > 0 ? mean / (float)nbars : 0.0f;
    outline_fill(bars, nbars, mean, out, n);
}

void
viz_outline_pair(const float *bars_l, const float *bars_r, int nbars,
                 float *out_l, float *out_r, int n)
{
    /* One mean across both sides: a lopsided mix stays lopsided rather than
     * each side being centred on its own level. */
    float mean = 0.0f;
    int i;

    if (n <= 0)
        return;
    for (i = 0; i < nbars; ++i)
        mean += bars_l[i] + bars_r[i];
    mean = nbars > 0 ? mean / (float)(2 * nbars) : 0.0f;
    outline_fill(bars_l, nbars, mean, out_l, n);
    outline_fill(bars_r, nbars, mean, out_r, n);
}

/* Half-width of the crossfade between the left and right outlines, in u units,
 * centred on the seam at u = 0. */
#define SHAPE_SEAM 0.12f

float
viz_shape_metric_lr(float u, float w, const float *out_l, const float *out_r,
                    int n, float depth)
{
    /* Circle distance on the p=2 scale, so zero depth is the plain circle. The
     * angle is measured from the bottom of the screen up, and looks up the
     * outline: bass at the bottom, treble at the top. Left of centre reads the
     * left outline, right the right; they blend across the seam so the shape
     * has no hard edge. */
    const float r = sqrtf(u * u + w * w) / sqrtf(2.0f);
    float div, t, pos, f, ol, or_, o, wgt;
    int i0, i1;

    if (depth <= 0.0f || !out_l || !out_r || n < 1)
        return r;
    t = atan2f(fabsf(u), w) / 3.14159265f; /* 0 at the bottom, 1 at the top */
    pos = t * (float)(n - 1);
    i0 = (int)pos;
    if (i0 < 0)
        i0 = 0;
    if (i0 > n - 1)
        i0 = n - 1;
    i1 = i0 + 1 < n ? i0 + 1 : n - 1;
    f = pos - (float)i0;
    if (f < 0.0f)
        f = 0.0f;
    if (f > 1.0f)
        f = 1.0f;
    ol = out_l[i0] * (1.0f - f) + out_l[i1] * f;
    or_ = out_r[i0] * (1.0f - f) + out_r[i1] * f;
    /* wgt is 0 well left of the seam, 1 well right, linear across it. Equal
     * outlines give o = ol exactly, since or_ - ol is then zero. */
    wgt = u / (2.0f * SHAPE_SEAM) + 0.5f;
    if (wgt < 0.0f)
        wgt = 0.0f;
    else if (wgt > 1.0f)
        wgt = 1.0f;
    o = ol + (or_ - ol) * wgt;
    div = 1.0f + depth * o;
    if (div < 0.4f)
        div = 0.4f;
    return r / div;
}

float
viz_shape_metric(float u, float w, const float *outline, int n, float depth)
{
    return viz_shape_metric_lr(u, w, outline, outline, n, depth);
}

/* Curated Julia constants: dendrite, rabbit, spiral, Siegel disc, seahorse
 * and basilica. All have |c| <= 1.2 so they stay well inside the bounded set
 * family the fractal's wander is tuned for. */
static const float JULIA_ANCHOR[VIZ_JULIA_ANCHORS][2] = {
    {0.00f, 1.00f},
    {-0.123f, 0.745f},
    {-0.80f, 0.156f},
    {-0.391f, -0.587f},
    {-0.75f, 0.11f},
    {-1.00f, 0.00f},
};

void
viz_julia_anchor(int idx, float *cr, float *ci)
{
    int k = idx % VIZ_JULIA_ANCHORS;

    if (k < 0)
        k += VIZ_JULIA_ANCHORS;
    *cr = JULIA_ANCHOR[k][0];
    *ci = JULIA_ANCHOR[k][1];
}

/* Onset: a band's mean rise above its slow average, past a margin, scaled to
 * 0..1. The margin keeps steady sound at zero. */
#define ONSET_MARGIN 0.10f
#define ONSET_GAIN   4.0f

void
viz_onset(const float *bars, const float *avg, int nbars, float *rise, int groups)
{
    int g;

    for (g = 0; g < groups; ++g) {
        const int from = g * nbars / groups, to = (g + 1) * nbars / groups;
        const float cur = viz_band_mean(bars, from, to);
        const float old = viz_band_mean(avg, from, to);
        float e = (cur - old - ONSET_MARGIN) * ONSET_GAIN;

        if (e < 0.0f)
            e = 0.0f;
        if (e > 1.0f)
            e = 1.0f;
        rise[g] = e;
    }
}

/* Below this combined band level a band counts as silent and centres. */
#define PAN_FLOOR 0.02f

float
viz_balance(float l, float r)
{
    const float sum = l + r;
    float p = 0.0f;

    if (sum > PAN_FLOOR)
        p = (r - l) / sum;
    if (p < -1.0f)
        p = -1.0f;
    else if (p > 1.0f)
        p = 1.0f;
    return p;
}

void
viz_pan(const float *bars_l, const float *bars_r, int nbars, float *pan, int groups)
{
    int g;

    for (g = 0; g < groups; ++g) {
        const int from = g * nbars / groups, to = (g + 1) * nbars / groups;
        pan[g] = viz_balance(viz_band_mean(bars_l, from, to),
                             viz_band_mean(bars_r, from, to));
    }
}

float
viz_side_lerp(float left, float right, float u)
{
    /* u is -1 at the left edge, +1 at the right. Equal inputs return exactly
     * that value, so a mono mix is unchanged. */
    float t = (u + 1.0f) * 0.5f;

    if (t < 0.0f)
        t = 0.0f;
    else if (t > 1.0f)
        t = 1.0f;
    return left + (right - left) * t;
}

/* Brightness on the stage: a right-heavy mix brightens the right edge, a
 * left-heavy one the left, clamped so nothing blows out or vanishes. */
float
viz_side_mult(float u, float pan, float k)
{
    float m = 1.0f + k * pan * u;

    if (m < 0.5f)
        m = 0.5f;
    else if (m > 1.5f)
        m = 1.5f;
    return m;
}

float
viz_pan_to_x(float pan, float jitter, float width)
{
    float wd = width, spread, x;

    if (wd < 0.0f)
        wd = 0.0f;
    else if (wd > 1.0f)
        wd = 1.0f;
    /* A wide mix fans its burst out; a mono mix keeps it tight. */
    spread = jitter * (0.05f + 0.35f * wd);
    x = pan * 0.85f + spread;
    if (x < -0.9f)
        x = -0.9f;
    else if (x > 0.9f)
        x = 0.9f;
    return x;
}

float
viz_width(const float *l, const float *r, int n)
{
    double em = 0.0, es = 0.0, sum;
    int i;

    for (i = 0; i < n; ++i) {
        const double m = 0.5 * ((double)l[i] + (double)r[i]);
        const double s = 0.5 * ((double)l[i] - (double)r[i]);
        em += m * m;
        es += s * s;
    }
    sum = em + es;
    if (sum < 1e-12)
        return 0.0f;
    /* Side over total: 0 for mono, rising toward 1 as the sides decorrelate;
     * an out-of-phase mix clamps to 1. */
    {
        const double w = 2.0 * es / sum;
        return (float)(w > 1.0 ? 1.0 : w);
    }
}

/* How fast an actor's x eases toward its pan target, per second. */
#define ACTOR_DRIFT 1.5f

void
viz_actor_drift(VizActor *a, float dt, float target_x)
{
    if (!a->live || a->releasing)
        return;
    /* The factor is in 0..1, so the ease approaches the target without overshoot. */
    a->x += (target_x - a->x) * (1.0f - expf(-dt * ACTOR_DRIFT));
}

int
viz_actor_slot(const VizActor *a, int n)
{
    int i, best = -1;
    float best_score = 0.0f;

    if (n <= 0)
        return -1;
    for (i = 0; i < n; ++i)
        if (!a[i].live)
            return i;
    /* Releasing actors score below 1, live ones above, so a fading actor goes
     * first; within each group the dimmest is taken. A spark scores lowest of
     * its kind, so the leads and extras are the last to be taken. */
    for (i = 0; i < n; ++i) {
        const float score = a[i].env + (a[i].releasing ? 0.0f : 1.0f)
                          + 0.25f * (float)(VIZ_ACTOR_SPARK - a[i].kind);

        if (best < 0 || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

void
viz_actor_spawn(VizActor *a, unsigned int *rng, int kind, int band,
                int bands, float strength)
{
    static const float size_base[3] = {0.55f, 0.30f, 0.12f};
    static const float life_base[3] = {40.0f, 12.0f, 4.0f};
    float f, s, jx, jy;
    int idx;

    if (kind < 0 || kind > 2)
        kind = VIZ_ACTOR_EXTRA;
    if (bands < 1)
        bands = 1;
    if (band < 0)
        band = 0;
    if (band >= bands)
        band = bands - 1;
    s = strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength);
    /* Bass sits low on the stage with a low hue, treble high with a high hue. */
    f = bands > 1 ? (float)band / (float)(bands - 1) : 0.0f;
    jx = viz_rand01(rng) * 2.0f - 1.0f;
    jy = viz_rand01(rng) * 2.0f - 1.0f;
    idx = (band + (int)(viz_rand01(rng) * VIZ_JULIA_ANCHORS)) % VIZ_JULIA_ANCHORS;
    viz_julia_anchor(idx, &a->cr, &a->ci);

    a->kind = kind;
    a->band = band;
    a->x = jx * 0.7f;
    a->y = (0.5f - f) * 0.8f + jy * 0.15f;
    /* Pitch sets the look: treble is a third the size, brighter, shimmers
     * more, and lives a little shorter than bass. */
    a->size = size_base[kind] * (1.0f - 0.65f * f) * (0.8f + 0.4f * s);
    a->hue = f * 0.6f;
    a->bright = 0.45f + 1.15f * f;
    a->shimmer = f * f;
    a->tw = 6.0f + 10.0f * viz_rand01(rng);
    a->tph = viz_rand01(rng) * 2.0f * PI_F;
    a->life = life_base[kind] * (1.0f - 0.3f * f) * (0.8f + 0.4f * viz_rand01(rng));
    a->age = 0.0f;
    a->env = 0.0f;
    a->releasing = 0;
    a->live = 1;
}

int
viz_actor_count(int band, int bands)
{
    const float f = bands > 1 ? (float)band / (float)(bands - 1) : 0.0f;

    return 1 + (int)(3.0f * f + 0.5f);
}

float
viz_actor_cooldown(int band, int bands)
{
    const float f = bands > 1 ? (float)band / (float)(bands - 1) : 0.0f;

    return 1.5f + (0.4f - 1.5f) * f;
}

float
viz_actor_twinkle(const VizActor *a, float t)
{
    float m;

    if (a->shimmer <= 0.0f)
        return 1.0f;
    m = 1.0f - a->shimmer * (0.5f - 0.5f * sinf(t * a->tw + a->tph));
    return m < 0.0f ? 0.0f : (m > 1.0f ? 1.0f : m);
}

/* Attack and release times, and the presence below which a band has gone quiet. */
#define ACTOR_ATTACK_S   0.4f
#define ACTOR_RELEASE    2.0f  /* envelope decay per second while releasing */
#define ACTOR_PRESENCE   0.05f

void
viz_actor_step(VizActor *a, float dt, float rate, float presence)
{
    if (!a->live)
        return;
    if (rate > 0.0f)
        a->age += dt * rate;
    if (presence < ACTOR_PRESENCE || a->age >= a->life)
        a->releasing = 1;
    if (a->releasing) {
        a->env *= expf(-dt * ACTOR_RELEASE);
        if (a->env < 0.01f) {
            a->env = 0.0f;
            a->live = 0;
        }
    } else {
        a->env += (1.0f - a->env) * (1.0f - expf(-dt / ACTOR_ATTACK_S));
    }
}
