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
