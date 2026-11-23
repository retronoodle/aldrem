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
viz_fractal_c(float bass, float mid, double t, float *cr, float *ci)
{
    *cr = -0.7f + 0.25f * (float)sin(t * 0.31) + 0.12f * bass;
    *ci = 0.27f + 0.20f * (float)cos(t * 0.23) + 0.10f * mid;
}

/* Seahorse valley, a boundary point where the Mandelbrot detail is rich. */
#define SEAHORSE_RE -0.7436438870f
#define SEAHORSE_IM  0.1318259043f

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
viz_drift(double t, float *cx, float *cy, float *zoom)
{
    const double phase = fmod(t * 0.05, 1.0);
    const float tri = 1.0f - fabsf(2.0f * (float)phase - 1.0f); /* 0..1..0 */
    const float ease = tri * tri * (3.0f - 2.0f * tri);

    *zoom = exp2f(-6.0f * tri);
    *cx = SEAHORSE_RE * ease;
    *cy = SEAHORSE_IM * ease;
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
