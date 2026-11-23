/* viz.h — pure visualizer maths: FFT, log-spaced spectrum bars, smoothing,
 * colour and fractal parameters. No SDL dependency, so it is unit-testable. */
#ifndef VIZ_H
#define VIZ_H

#define VIZ_FFT_N     1024 /* analysis window, power of 2 */
#define VIZ_BARS_MIN  8
#define VIZ_BARS_MAX  96

typedef enum { VIZ_SPECTRUM = 0, VIZ_WAVE = 1, VIZ_FRACTAL = 2, VIZ_MODE_COUNT } VizMode;

/* User-tunable visualizer settings. Percent fields are 0..100 unless noted. */
typedef struct {
    int mode;      /* VizMode */
    int gain_pct;  /* 25..400: input sensitivity, 100 = unity */
    int smooth_pct;/* 0..100: how slowly bars fall back */
    int bars;      /* VIZ_BARS_MIN..VIZ_BARS_MAX */
    int hue_pct;   /* 0..100: colour wheel rotation */
} VizSettings;

/* Clamps every field into its valid range. Out-of-range saved values end up
 * at the nearest limit rather than being rejected. */
void viz_settings_clamp(VizSettings *s);

/* In-place radix-2 FFT over n complex points. n must be a power of 2 and at
 * least 2. Returns 0 on success, -1 for an invalid n. */
int viz_fft(float *re, float *im, int n);

/* Fills bars[0..nbars) with 0..1 heights from the newest VIZ_FFT_N samples
 * (samples are mono, -1..1, oldest first, count n >= VIZ_FFT_N; shorter input
 * is zero-padded). Hann-windowed, log-spaced bands, dB scale over 60 dB.
 * gain is linear (1.0 = unity). */
void viz_spectrum(const float *samples, int n, float *bars, int nbars, float gain);

/* Moves each display value toward its target. Rises are quick; falls are
 * slower, more so as smooth (0..1) grows. Call once per frame. */
void viz_smooth(float *cur, const float *target, int n, float smooth);

/* Converts a hue in 0..1 (wrapping) with full saturation and value to RGB 0..255. */
void viz_hue_rgb(float hue, unsigned char *r, unsigned char *g, unsigned char *b);

/* Julia-set constant for the fractal mode, driven by bass and mid energy in
 * 0..1 and a running time in seconds. */
void viz_fractal_c(float bass, float mid, double t, float *cr, float *ci);

#endif
