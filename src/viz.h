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

/* Julia-set constant for the fractal mode, a pure function of bass, mid and
 * treble energy in 0..1. No clock: a steady sound gives a steady shape. */
void viz_fractal_c(float bass, float mid, float treble, float *cr, float *ci);

/* Soft compression for the fractal: dst[i] = src[i]^exponent, with src clamped
 * to 0..1. Monotonic, 0 -> 0 and 1 -> 1; exponent < 1 lifts quiet values more
 * than loud ones. exponent 1.0 copies unchanged. */
void viz_compress(float *dst, const float *src, int n, float exponent);

/* Silence gate: 0 at or below floor, rising smoothly to 1 a quarter above it. */
float viz_gate(float level, float floor);

/* Mean of bars[from..to), or 0 for an empty range. */
float viz_band_mean(const float *bars, int from, int to);

/* viz_gate applied to the mean of bars[from..to). */
float viz_band_gate(const float *bars, int from, int to, float floor);

/* Spectral flux: mean positive rise per bar between two frames, scaled and
 * clamped to 0..1. Zero when the spectrum is steady or only falling. */
float viz_flux(const float *prev, const float *cur, int n);

/* Spectral centroid: energy-weighted mean bar position in 0..1 (0 = bass,
 * 1 = treble). Returns 0.5 when there is no energy. */
float viz_centroid(const float *bars, int n);

/* Maps a normalised radius 0..1 (centre to rim) to a bar index in 0..nbars-1,
 * log-spaced so bass sits at the centre. */
int viz_ring_bin(float r, int nbars);

/* Spectrum outline for the fractal's background shape: n values from bass
 * (index 0) to treble (index n-1), resampled from nbars bars, with the mean
 * removed so a flat spectrum gives a flat outline. */
#define VIZ_OUTLINE_N 64
void viz_outline(const float *bars, int nbars, float *out, int n);

/* Shape metric for the fractal rings. u, w are the screen-normalised offsets
 * from the centre. The distance is the circle (sqrt(u*u+w*w)/sqrt(2)) divided
 * by 1 + depth * outline, where the outline is read at the point's angle,
 * folded left-right with bass at the bottom and treble at the top. depth 0
 * leaves the plain circle. The result feeds viz_ring_bin. */
float viz_shape_metric(float u, float w, const float *outline, int n, float depth);

/* Smooth escape count for colouring: n iterations and |z| at escape give a
 * fractional value, so colour bands blend instead of stepping. */
float viz_smooth_mu(int n, float zx, float zy);

/* Julia iteration from z0 with constant c. Returns the smooth escape count, or
 * -1 if the point stays bounded for iter steps (inside the set). */
float viz_julia_mu(float zx, float zy, float cr, float ci, int iter);

/* Phoenix iteration from z0 with constants c and p (the memory term). Same
 * return convention as viz_julia_mu. */
float viz_phoenix_mu(float zx, float zy, float cr, float ci, float p, int iter);

/* Folds the plane into segs mirrored wedges around the origin (segs >= 1),
 * with the wedges rotated by rot radians. Radius is preserved. */
void viz_kaleido(float re, float im, int segs, float rot, float *ore, float *oim);

/* Beat tracker. avg is a slow bass average and kick a 0..1 envelope that jumps
 * on a bass onset and decays. Both are updated in place. Call once per frame. */
void viz_beat(float *avg, float *kick, float bass, float dt);

/* Transient (tick) detector for short clicks the bars average away. samples is
 * the newest window of mono samples, gain the Gain slider as a fraction, avg a
 * slow average of block energy and tick a 0..1 envelope. Both are updated in
 * place. Call once per frame. */
void viz_tick(const float *samples, int n, float gain, float *avg, float *tick, float dt);

/* Xorshift32 random state. A zero state is replaced with 1, so any seed works.
 * viz_rand_next returns the raw 32-bit value; viz_rand01 returns 0..1. */
unsigned int viz_rand_next(unsigned int *s);
float viz_rand01(unsigned int *s);

/* Julia-constant wander: a 2D offset that jumps by a random direction scaled
 * by impulse (0..1, e.g. a kick rise) and springs back to zero over time. The
 * magnitude never exceeds VIZ_WANDER_MAX. Call once per frame. */
#define VIZ_WANDER_MAX 0.20f
void viz_wander_step(float w[2], unsigned int *rng, float impulse, float dt);

/* Mean absolute difference between a slow average and the current spectrum,
 * over n bars. Zero for identical spectra; grows as the sound changes. */
float viz_novelty(const float *avg, const float *cur, int n);

/* Number of curated Julia constants, and the one at idx (wrapped into range,
 * so any int is valid). Every anchor stays inside |c| <= 1.2. */
#define VIZ_JULIA_ANCHORS 6
void viz_julia_anchor(int idx, float *cr, float *ci);

/* Actors: small Julia blobs on the fractal stage. Each is born from a sound
 * that rises above its band's slow average (viz_onset), then ages with the
 * music (viz_actor_step), so a held tone holds the cast and silence clears it. */
#define VIZ_ACTORS        6
#define VIZ_ONSET_GROUPS  6
typedef enum { VIZ_ACTOR_LEAD = 0, VIZ_ACTOR_EXTRA = 1, VIZ_ACTOR_SPARK = 2 } VizActorKind;

typedef struct {
    int live;          /* slot in use */
    int kind;          /* VizActorKind */
    int band;          /* onset group that spawned it, 0 bass .. groups-1 treble */
    float cr, ci;      /* Julia constant, a curated anchor */
    float x, y;        /* centre on the stage, in the fractal's -1..1 units */
    float size;        /* radius on the stage, same units */
    float hue;         /* hue offset from the view's hue */
    float age;         /* music time lived, advanced by rate */
    float life;        /* music time to live before it releases */
    float env;         /* brightness envelope, 0..1 */
    int releasing;     /* fading out: set by life, presence loss or a section change */
} VizActor;

/* Rise of each of groups equal bands of bars over its slow average, 0..1. A
 * rise must clear a margin above the average, so steady sound gives zero. */
void viz_onset(const float *bars, const float *avg, int nbars, float *rise, int groups);

/* Slot to use for a new actor among n: a free one, else the dimmest releasing
 * one, else the dimmest overall. Returns -1 when n is zero. */
int viz_actor_slot(const VizActor *a, int n);

/* Starts actor a of the given kind (VizActorKind) from onset band of bands,
 * with strength 0..1 setting its size. Position, size, hue and constant follow
 * the band, with seeded jitter from rng. */
void viz_actor_spawn(VizActor *a, unsigned int *rng, int kind, int band,
                     int bands, float strength);

/* Advances a live actor by dt seconds. Its music-time age advances by dt*rate,
 * so rate 0 holds it still. presence is its band energy 0..1: when it drops,
 * or the age reaches its life, the actor releases and fades out. */
void viz_actor_step(VizActor *a, float dt, float rate, float presence);

#endif
