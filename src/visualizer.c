/* visualizer.c — the Visualizer tab and fullscreen view. See visualizer.h. */
#include "visualizer.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "playback.h"

/* The fractal is computed at a low resolution and stretched by the renderer,
 * which keeps the per-frame CPU work small and the upscale on the GPU. */
#define FRACTAL_W    192
#define FRACTAL_H    108
#define FRACTAL_ITER 40

static const char *const viz_mode_names[VIZ_MODE_COUNT] = {
    "Spectrum", "Wave", "Fractal"
};

struct VizView {
    float target[VIZ_BARS_MAX]; /* this frame's raw bar heights, 0..1 */
    float bars[VIZ_BARS_MAX];   /* smoothed bar heights shown on screen */
    float samples[VIZ_FFT_N];   /* newest mono samples, -1..1 */
    float pts[2 * VIZ_FFT_N];   /* waveform polyline, x/y pairs */
    unsigned int pixels[FRACTAL_W * FRACTAL_H];
    SDL_Texture *fractal;       /* streaming texture, NULL if unavailable */
    double t;                   /* animation clock, seconds */
    float bass, mid;            /* band energy driving the fractal, 0..1 */
    float treble;               /* top-quarter band energy, 0..1 */
    float bass_avg;             /* slow bass average the beat tracker uses */
    float kick;                 /* beat envelope, 0..1, jumps on bass onsets */
    float mid_avg;              /* slow mid average: what counts as a swell */
    float mid_env;              /* mid swell envelope: the entity's presence */
    float trail_decay;          /* per-frame keep factor for fractal trails */
    float phx[3];               /* Phoenix shape now: c real, c imag, p */
    int phx_preset;             /* PHX_PRESETS entry the shape is pulled to */
    float phx_wf[3], phx_wp[3]; /* random wander rates and phases */
    float phx_cool;             /* seconds before a kick may switch preset */
    unsigned int phx_rng;       /* xorshift state: random start and jumps */
    struct MpAnim mode_anim[VIZ_MODE_COUNT];
    struct MpAnim fs_anim;
};

/* Phoenix parameter starting points (c real, c imag, p). Each one gives a
 * different silhouette; the shape eases between them on kicks. */
#define PHX_PRESET_N 6
static const float PHX_PRESETS[PHX_PRESET_N][3] = {
    {0.56667f,  0.00f, -0.50f}, /* classic Phoenix */
    {0.40f,     0.10f, -0.60f},
    {0.60f,    -0.12f, -0.45f},
    {0.25f,     0.20f, -0.80f},
    {0.50f,    -0.20f, -0.30f},
    {0.35f,     0.00f, -0.85f},
};
/* Safe box the shape stays in, so it never collapses to blank or solid. */
static const float PHX_LO[3] = {0.20f, -0.25f, -0.90f};
static const float PHX_HI[3] = {0.65f,  0.25f,  0.10f};
/* How far the random wander swings each parameter. */
static const float PHX_WANDER[3] = {0.08f, 0.08f, 0.12f};

static float
phx_rand01(unsigned int *s)
{
    *s ^= *s << 13;
    *s ^= *s >> 17;
    *s ^= *s << 5;
    return (float)(*s & 0xFFFFFFu) / 16777216.0f;
}

static int
phx_pick(unsigned int *s, int avoid)
{
    int n = (int)(phx_rand01(s) * PHX_PRESET_N);

    if (n >= PHX_PRESET_N)
        n = PHX_PRESET_N - 1;
    return (n == avoid) ? (n + 1) % PHX_PRESET_N : n;
}

VizView *
viz_view_create(SDL_Renderer *ren)
{
    VizView *v = calloc(1, sizeof *v);
    int i;

    if (!v)
        return NULL;
    if (ren) {
        v->fractal = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                       SDL_TEXTUREACCESS_STREAMING,
                                       FRACTAL_W, FRACTAL_H);
        if (v->fractal)
            SDL_SetTextureScaleMode(v->fractal, SDL_ScaleModeLinear);
    }

    /* Every launch starts with a different creature: random seed, random
     * preset, random wander rates. The state is set here, not eased from
     * zero, so the first frame already shows a finished shape. */
    v->phx_rng = ((unsigned int)SDL_GetTicks() * 2654435761u) ^ (unsigned int)(uintptr_t)v;
    if (!v->phx_rng)
        v->phx_rng = 0x9E3779B9u;
    v->phx_preset = phx_pick(&v->phx_rng, -1);
    for (i = 0; i < 3; ++i) {
        v->phx_wf[i] = 0.10f + 0.20f * phx_rand01(&v->phx_rng);
        v->phx_wp[i] = 6.2831853f * phx_rand01(&v->phx_rng);
        v->phx[i] = PHX_PRESETS[v->phx_preset][i];
    }
    return v;
}

void
viz_view_destroy(VizView *v)
{
    if (!v)
        return;
    if (v->fractal)
        SDL_DestroyTexture(v->fractal);
    free(v);
}

void
viz_view_update(VizView *v, const VizSettings *s, float dt, int active,
                int reduce_motion)
{
    int nb = s->bars, q, h, i;
    float sum_bass = 0.0f, sum_mid = 0.0f, sum_treble = 0.0f;

    /* Hidden views do no work: no sample copy, no FFT, no clock. */
    if (!active)
        return;

    if (!reduce_motion)
        v->t += dt;
    playback_get_samples(v->samples, VIZ_FFT_N);
    viz_spectrum(v->samples, VIZ_FFT_N, v->target, nb,
                 (float)s->gain_pct / 100.0f);
    viz_smooth(v->bars, v->target, nb, (float)s->smooth_pct / 100.0f);

    /* Bass and mid are the mean of the lower quarter and the next quarter of
     * the bars, which are log-spaced, so they follow low and mid frequencies. */
    q = nb / 4;
    h = nb / 2;
    for (i = 0; i < q; ++i)
        sum_bass += v->bars[i];
    for (i = q; i < h; ++i)
        sum_mid += v->bars[i];
    v->bass = q ? sum_bass / (float)q : 0.0f;
    v->mid = (h > q) ? sum_mid / (float)(h - q) : 0.0f;

    /* Treble is the top quarter of the bars. */
    for (i = nb - q; i < nb; ++i)
        sum_treble += v->bars[i];
    v->treble = q ? sum_treble / (float)q : 0.0f;
    viz_beat(&v->bass_avg, &v->kick, v->bass, dt);

    /* The entity follows swells: mid energy rising above its own slow average,
     * which is what a vocal or a solo does. A held note settles back to
     * nothing, so the creature pulses with the phrasing instead of sitting on. */
    {
        const float ka = 1.0f - expf(-dt * 0.5f);
        float rise = (v->mid - v->mid_avg) * 4.0f;
        float k;

        rise = rise < 0.0f ? 0.0f : (rise > 1.0f ? 1.0f : rise);
        v->mid_avg += (v->mid - v->mid_avg) * ka;
        k = rise > v->mid_env ? 0.6f : 1.0f - expf(-dt * 1.5f);
        v->mid_env += (rise - v->mid_env) * k;
    }
    v->trail_decay = expf(-dt * 5.0f);

    if (reduce_motion) {
        /* Fractal holds still without audio drive; the creature sits at its
         * resting shape, since it has no swell to follow. */
        v->bass = v->mid = 0.0f;
        v->treble = v->kick = v->mid_env = 0.0f;
    }

    /* Phoenix shape: a slow random wander around the current preset, nudged
     * by the swell (mid_env) and the mids, with a kick jumping to a new
     * preset. Everything eases, so changes morph rather than snap. Under
     * reduce_motion the clock is frozen and the audio terms are zero, so the
     * shape holds still. */
    v->phx_cool = v->phx_cool > dt ? v->phx_cool - dt : 0.0f;
    if (v->kick > 0.7f && v->phx_cool <= 0.0f) {
        v->phx_preset = phx_pick(&v->phx_rng, v->phx_preset);
        v->phx_cool = 1.5f;
    }
    {
        const float ease = 1.0f - expf(-dt * 1.2f);
        const float nudge[3] = {
            0.06f * v->mid_env, 0.08f * v->mid, -0.15f * v->mid_env
        };
        int i;

        for (i = 0; i < 3; ++i) {
            const float base = PHX_PRESETS[v->phx_preset][i];
            const float wander = PHX_WANDER[i] * sinf((float)v->t * v->phx_wf[i]
                                                      + v->phx_wp[i]);
            float tgt = base + wander + nudge[i];

            tgt = tgt < PHX_LO[i] ? PHX_LO[i] : (tgt > PHX_HI[i] ? PHX_HI[i] : tgt);
            v->phx[i] += (tgt - v->phx[i]) * ease;
        }
    }
}

/* Fractal look. The Julia layer is rippled by the treble and folded into a
 * kaleidoscope; a Phoenix creature sits on top, always present and reshaped by
 * the clock and the mids; each frame keeps a fading copy of the last one. */
#define KALEIDO_SEGS 6
#define PHX_HALF     1.6f     /* half-height of the Phoenix view */
#define HUE_DRIFT     0.02f /* palette turns per second */
#define RIPPLE_AMP    0.04f

/* Colour for an escape count. mu < 0 is inside the set, drawn black. */
static void
mu_rgb(float mu, float hue, float *r, float *g, float *b)
{
    unsigned char hr, hg, hb;
    float lift;

    if (mu < 0.0f) {
        *r = *g = *b = 0.0f;
        return;
    }
    lift = sqrtf(mu / FRACTAL_ITER);
    viz_hue_rgb(hue + mu / FRACTAL_ITER * 1.2f, &hr, &hg, &hb);
    *r = hr * lift;
    *g = hg * lift;
    *b = hb * lift;
}

/* Fills the fractal pixel buffer, then uploads it. */
static void
fractal_render(VizView *v, const VizSettings *s)
{
    const float aspect = (float)FRACTAL_W / (float)FRACTAL_H;
    const float t = (float)v->t;
    const float hue = (float)s->hue_pct / 100.0f + t * HUE_DRIFT;
    /* The creature's shape (c, p) is eased in viz_view_update, so it keeps
     * morphing with no audio and reacts to swells and kicks. */
    const float breathe = 0.5f + 0.5f * sinf(t * 0.35f);
    /* The view wanders across the plane, so the shape keeps changing. */
    const float phx_cx = 0.3f * sinf(t * 0.2f);
    const float phx_cy = 0.25f * cosf(t * 0.17f);
    const float phx_half = PHX_HALF * (1.0f - 0.3f * breathe - 0.3f * v->mid_env);
    float mask = 0.35f + 0.65f * v->mid_env;
    float cr, ci, cx, cy, zoom;
    int x, y;

    /* Smoothstep: the entity fades in and out rather than popping. */
    mask = mask < 0.0f ? 0.0f : (mask > 1.0f ? 1.0f : mask);
    mask = mask * mask * (3.0f - 2.0f * mask);

    viz_fractal_c(v->bass, v->mid, v->t, &cr, &ci);
    viz_drift(v->t, &cx, &cy, &zoom);
    zoom *= (1.0f - 0.25f * v->bass) * (1.0f - 0.35f * v->kick);

    for (y = 0; y < FRACTAL_H; ++y) {
        const float w = ((float)y / FRACTAL_H - 0.5f) * 2.0f;

        for (x = 0; x < FRACTAL_W; ++x) {
            const float u = ((float)x / FRACTAL_W - 0.5f) * 2.0f;
            const float re = cx + u * zoom * aspect;
            const float im = cy + w * zoom;
            const float amp = RIPPLE_AMP * v->treble * zoom;
            const float rre = re + amp * sinf(im * 9.0f + t * 1.7f);
            const float rim = im + amp * sinf(re * 7.0f - t * 1.3f);
            const size_t idx = (size_t)y * FRACTAL_W + (size_t)x;
            float fre, fim, r, g, b, sr, sg, sb, pr, pg, pb;
            unsigned int prev;

            viz_kaleido(rre, rim, KALEIDO_SEGS, t * 0.05f, &fre, &fim);
            mu_rgb(viz_julia_mu(fre, fim, cr, ci, FRACTAL_ITER), hue,
                   &r, &g, &b);

            if (mask > 0.01f) {
                mu_rgb(viz_phoenix_mu(phx_cx + u * phx_half * aspect,
                                      phx_cy + w * phx_half,
                                      v->phx[0], v->phx[1], v->phx[2],
                                      FRACTAL_ITER),
                       hue + 0.5f, &sr, &sg, &sb);
                r += mask * sr;
                g += mask * sg;
                b += mask * sb;
            }
            r = fminf(r, 255.0f);
            g = fminf(g, 255.0f);
            b = fminf(b, 255.0f);

            /* Trails: keep the brighter of this frame and a faded last frame. */
            prev = v->pixels[idx];
            pr = (float)((prev >> 16) & 0xFFu) * v->trail_decay;
            pg = (float)((prev >> 8) & 0xFFu) * v->trail_decay;
            pb = (float)(prev & 0xFFu) * v->trail_decay;
            r = fmaxf(r, pr);
            g = fmaxf(g, pg);
            b = fmaxf(b, pb);

            v->pixels[idx] = 0xFF000000u
                | ((unsigned)r << 16)
                | ((unsigned)g << 8)
                | (unsigned)b;
        }
    }
    SDL_UpdateTexture(v->fractal, NULL, v->pixels, FRACTAL_W * 4);
}

static void
draw_spectrum(struct nk_command_buffer *c, const struct MpTheme *th,
              const VizView *v, const VizSettings *s, struct nk_rect r)
{
    const int nb = s->bars;
    const float pad = r.h * 0.06f;
    const float x0 = r.x + pad, bottom = r.y + r.h - pad;
    const float w = r.w - 2.0f * pad, hmax = r.h - 2.0f * pad;
    const float slot = w / (float)nb;
    const float bw = slot * 0.72f;
    int i;

    for (i = 0; i < nb; ++i) {
        float h = v->bars[i] * hmax, tt, tri;

        if (h < 1.0f)
            h = 1.0f;
        /* A triangle wave over the gradient keeps the colours continuous
         * across the bar that wraps around. */
        tt = (float)i / (float)nb + (float)s->hue_pct / 100.0f;
        tt -= floorf(tt);
        tri = tt < 0.5f ? 2.0f * tt : 2.0f - 2.0f * tt;
        nk_fill_rect(c, nk_rect(x0 + i * slot + (slot - bw) * 0.5f,
                                bottom - h, bw, h),
                     0.0f, mp_col_lerp(th->accent_a, th->accent_b, tri));
    }
}

static void
draw_wave(struct nk_command_buffer *c, const struct MpTheme *th,
          VizView *v, const VizSettings *s, struct nk_rect r)
{
    int n = (int)r.w, i;
    const float mid = r.y + r.h * 0.5f;
    const float amp = r.h * 0.42f;
    const float gain = (float)s->gain_pct / 100.0f;

    if (n > VIZ_FFT_N)
        n = VIZ_FFT_N;
    if (n < 2)
        return;

    nk_stroke_line(c, r.x, mid, r.x + r.w, mid, 1.0f, th->border);
    for (i = 0; i < n; ++i) {
        const int idx = i * (VIZ_FFT_N - 1) / (n - 1);
        float s_val = v->samples[idx] * gain;

        if (s_val > 1.0f)
            s_val = 1.0f;
        else if (s_val < -1.0f)
            s_val = -1.0f;
        v->pts[2 * i] = r.x + (float)i * (r.w - 1.0f) / (float)(n - 1);
        v->pts[2 * i + 1] = mid - s_val * amp;
    }
    nk_stroke_polyline(c, v->pts, n, 2.0f, th->accent_a);
}

static void
draw_fractal(struct nk_command_buffer *c, VizView *v, const VizSettings *s,
             struct nk_rect r)
{
    struct nk_image img;

    if (!v->fractal)
        return;
    fractal_render(v, s);
    img = nk_image_ptr(v->fractal);
    nk_draw_image(c, r, &img, nk_rgb(255, 255, 255));
}

void
viz_view_draw(struct nk_context *ctx, const struct MpTheme *th, VizView *v,
              const VizSettings *s, const char *title)
{
    struct nk_rect r;
    struct nk_command_buffer *c;

    if (nk_widget(&r, ctx) == NK_WIDGET_INVALID)
        return;
    c = nk_window_get_canvas(ctx);

    nk_fill_rect(c, r, 0.0f, th->surface);
    switch (s->mode) {
    case VIZ_WAVE:
        draw_wave(c, th, v, s, r);
        break;
    case VIZ_FRACTAL:
        draw_fractal(c, v, s, r);
        break;
    default:
        draw_spectrum(c, th, v, s, r);
        break;
    }
    nk_stroke_rect(c, r, 0.0f, 1.0f, th->border);

    /* The track name sits on a translucent strip so it reads over any visual. */
    if (title && title[0] && ctx->style.font) {
        const int len = (int)strlen(title);
        const float fh = ctx->style.font->height;
        const float tw = ctx->style.font->width(ctx->style.font->userdata,
                                                fh, title, len);
        struct nk_rect strip = nk_rect(r.x, r.y, tw + 2.0f * fh * 0.6f,
                                       fh * 1.6f);
        nk_fill_rect(c, strip, 0.0f, mp_col_alpha(th->shadow, 170));
        nk_draw_text(c, nk_rect(strip.x + fh * 0.6f, strip.y + fh * 0.3f,
                                tw, fh),
                     title, len, ctx->style.font,
                     nk_rgba(0, 0, 0, 0), th->text_primary);
    }
}

/* One label and one int slider in the current row. */
static int
ctrl_slider(struct nk_context *ctx, const char *label, int *val, int min,
            int max)
{
    const int before = *val;

    nk_label(ctx, label, NK_TEXT_LEFT);
    nk_slider_int(ctx, min, val, max, 1);
    return *val != before;
}

int
viz_view_controls(struct nk_context *ctx, float scale,
                  const struct MpTheme *th, VizView *v, VizSettings *s,
                  int tab_fullscreen, int enabled,
                  const struct MpButtonOpts *opt)
{
    const float h = mp_s(mp_button_height(MP_BTN_NEUTRAL), scale);
    const float label_w = mp_s(76.0f, scale);
    int flags = 0, i, changed = 0;

    /* Row 1: the three visual modes, then Fullscreen. */
    nk_layout_row_dynamic(ctx, h, VIZ_MODE_COUNT + 1);
    for (i = 0; i < VIZ_MODE_COUNT; ++i) {
        if (mp_button(ctx, scale, th, viz_mode_names[i],
                      s->mode == i ? MP_BTN_PRIMARY : MP_BTN_NEUTRAL,
                      MP_ICON_PLAY, 0, enabled, &v->mode_anim[i], opt)) {
            s->mode = i;
            changed = 1;
        }
    }
    if (mp_button(ctx, scale, th, "Fullscreen", MP_BTN_NEUTRAL, MP_ICON_PLAY,
                  tab_fullscreen, enabled, &v->fs_anim, opt))
        flags |= VIZ_CTRL_FULLSCREEN;

    /* Rows 2 and 3: two label + slider pairs each. */
    if (!enabled)
        nk_widget_disable_begin(ctx);
    nk_layout_row_template_begin(ctx, h);
    nk_layout_row_template_push_static(ctx, label_w);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, label_w);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    changed |= ctrl_slider(ctx, "Gain", &s->gain_pct, 25, 400);
    changed |= ctrl_slider(ctx, "Smooth", &s->smooth_pct, 0, 100);

    nk_layout_row_template_begin(ctx, h);
    nk_layout_row_template_push_static(ctx, label_w);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_push_static(ctx, label_w);
    nk_layout_row_template_push_dynamic(ctx);
    nk_layout_row_template_end(ctx);
    changed |= ctrl_slider(ctx, "Bars", &s->bars, VIZ_BARS_MIN, VIZ_BARS_MAX);
    changed |= ctrl_slider(ctx, "Hue", &s->hue_pct, 0, 100);
    if (!enabled)
        nk_widget_disable_end(ctx);

    if (changed)
        flags |= VIZ_CTRL_CHANGED;
    return flags;
}

float
viz_view_controls_height(struct nk_context *ctx, float scale)
{
    /* Three rows (modes, two slider rows) plus the gaps Nuklear adds between
     * them. */
    return 3.0f * mp_s(mp_button_height(MP_BTN_NEUTRAL), scale)
        + 3.0f * ctx->style.window.spacing.y;
}
