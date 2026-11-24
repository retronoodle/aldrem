/* visualizer.c — the Visualizer tab and fullscreen view. See visualizer.h. */
#include "visualizer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "playback.h"

/* The fractal is computed at a low resolution and stretched by the renderer,
 * which keeps the per-frame CPU work small and the upscale on the GPU. */
#define FRACTAL_W    192
#define FRACTAL_H    108
#define FRACTAL_ITER 40
#define TAU_F        6.28318530718f

static const char *const viz_mode_names[VIZ_MODE_COUNT] = {
    "Spectrum", "Wave", "Fractal"
};

struct VizView {
    float target[VIZ_BARS_MAX]; /* this frame's raw bar heights, 0..1 */
    float bars[VIZ_BARS_MAX];   /* smoothed bar heights shown on screen */
    float cbars[VIZ_BARS_MAX];  /* bars lifted for quiet music; the fractal reads these */
    float samples[VIZ_FFT_N];   /* newest mono samples, -1..1 */
    float pts[2 * VIZ_FFT_N];   /* waveform polyline, x/y pairs */
    unsigned int pixels[FRACTAL_W * FRACTAL_H];
    SDL_Texture *fractal;       /* streaming texture, NULL if unavailable */
    int dark;                   /* last fractal frame was fully black */
    float bass, mid;            /* band energy driving the fractal, 0..1 */
    float treble;               /* top-quarter band energy, 0..1 */
    float level;                /* mean of all bars, 0..1: overall loudness */
    float master;               /* silence-gated level, 0 in silence */
    float mid_gate;             /* gated mid energy, 0..1: the Phoenix's presence */
    float centroid;             /* smoothed spectral centroid, 0..1 */
    float prev_bars[VIZ_BARS_MAX]; /* last frame's bars, for spectral flux */
    int primed;                 /* prev_bars holds a frame from this visible run */
    float flux;                 /* spectral flux envelope, 0..1 */
    float ripple;               /* treble driving the ripple, 0 under reduce_motion */
    float rot;                 /* kaleidoscope rotation, driven by flux */
    float bass_avg;             /* slow bass average the beat tracker uses */
    float kick;                 /* beat envelope, 0..1, jumps on bass onsets */
    float trail_decay;          /* per-frame keep factor for fractal trails */
    float phx[3];               /* Phoenix shape now: c real, c imag, p */
    float phx_off[3];           /* Phoenix offset, set at each section change */
    float wander[2];            /* Julia constant offset, flung by kick onsets */
    float ctr[2];               /* Julia centre, eases toward the current anchor */
    int anchor;                 /* curated Julia anchor index, -1 = classic */
    int segs;                   /* kaleidoscope wedge count, changes per section */
    float spec_avg[VIZ_BARS_MAX]; /* slow average spectrum, for section changes */
    int avg_ready;              /* spec_avg holds a real frame from this run */
    float novelty_cool;         /* seconds until another section change may fire */
    float prev_kick;            /* last frame's kick, to find onset rises */
    float tick_avg;             /* slow average of block energy, for the tick detector */
    float tick;                 /* transient envelope, 0..1, jumps on clicks */
    float prev_tick;            /* last frame's tick, to find click rises */
    float outline[VIZ_OUTLINE_N]; /* spectrum outline the background shape follows */
    float shape_depth;          /* outline depth, follows the level and kicks */
    VizActor actors[VIZ_ACTORS];                /* the cast on the fractal stage */
    float actor_cool[VIZ_ONSET_GROUPS + 1];     /* spawn cooldowns per band; last is clicks */
    unsigned int rng;           /* xorshift state for the music-driven choices */
    struct MpAnim mode_anim[VIZ_MODE_COUNT];
    struct MpAnim fs_anim;
};

/* Exponent for the fractal's soft compression: below 1 lifts quiet bands so
 * soft passages show; 0.6 is stronger, 0.8 gentler. */
#define VIZ_COMPRESS 0.7f
/* Clicks: how much a tick lifts the fractal's brightness, and the floor a
 * spectrum bar is lifted to by a tick. */
#define TICK_LUM 0.6f
#define TICK_BAR 0.35f
/* Kaleidoscope wedges: the classic six, and the set a section change picks from. */
#define KALEIDO_SEGS 6

/* Phoenix resting parameters (c real, c imag, p): the classic silhouette. The
 * music pulls the shape away from it; with no music it is not drawn at all. */
static const float PHX_REST[3] = {0.56667f, 0.00f, -0.50f};
/* Safe box the shape stays in, so it never collapses to blank or solid. */
static const float PHX_LO[3] = {0.20f, -0.25f, -0.90f};
static const float PHX_HI[3] = {0.65f,  0.25f,  0.10f};

/* Music memory tuning. A section change is a spectrum that has drifted from its
 * slow average by more than NOVELTY_THRESHOLD; sections are at least
 * SECTION_COOLDOWN seconds apart. PHX_OFF bounds the Phoenix offset per section. */
#define NOVELTY_THRESHOLD 0.15f
#define SECTION_COOLDOWN  4.0f
static const float PHX_OFF[3] = {0.12f, 0.10f, 0.08f};
/* Background shape: the spectrum outline's depth cap. */
#define SHAPE_DEPTH_MAX 0.9f

/* Puts the music memory back to the classic look. Used at creation and on
 * reopen, so each visible run starts from the same picture. */
static void
motion_reset(VizView *v)
{
    float base[2];
    int k;

    viz_fractal_c(0.0f, 0.0f, 0.0f, &base[0], &base[1]);
    v->ctr[0] = base[0];
    v->ctr[1] = base[1];
    v->anchor = -1;
    v->segs = KALEIDO_SEGS;
    v->wander[0] = v->wander[1] = 0.0f;
    v->novelty_cool = 0.0f;
    v->prev_kick = 0.0f;
    v->tick_avg = 0.0f;
    v->tick = 0.0f;
    v->prev_tick = 0.0f;
    memset(v->outline, 0, sizeof v->outline);
    v->shape_depth = 0.0f;
    memset(v->actors, 0, sizeof v->actors);
    memset(v->actor_cool, 0, sizeof v->actor_cool);
    for (k = 0; k < 3; ++k)
        v->phx_off[k] = 0.0f;
}

/* Actor cooldowns: a band may spawn again after ACTOR_COOL seconds, clicks
 * after ACTOR_COOL_TICK. A rise must clear the trigger to count as a new sound. */
#define ACTOR_COOL      1.5f
#define ACTOR_COOL_TICK 0.6f
#define ONSET_TRIGGER   0.3f
#define TICK_TRIGGER    0.3f

/* The stage cast. A section change retires the cast and brings a new lead; a
 * new sound in a band brings an extra (bass) or a spark (anything higher); a
 * click brings a spark. Every actor then ages with the music and releases when
 * the sound that drew it goes quiet. Nothing spawns under reduce_motion. */
static void
actors_update(VizView *v, float dt, int nb, int section_fired,
              float tick_rise, int reduce_motion)
{
    float rise[VIZ_ONSET_GROUPS], lvl[VIZ_ONSET_GROUPS];
    float rate, presence;
    int g, a, slot;

    for (g = 0; g <= VIZ_ONSET_GROUPS; ++g)
        if (v->actor_cool[g] > 0.0f)
            v->actor_cool[g] -= dt;

    if (section_fired) {
        for (a = 0; a < VIZ_ACTORS; ++a)
            if (v->actors[a].live)
                v->actors[a].releasing = 1;
        slot = viz_actor_slot(v->actors, VIZ_ACTORS);
        if (slot >= 0)
            viz_actor_spawn(&v->actors[slot], &v->rng, VIZ_ACTOR_LEAD,
                            (int)(viz_rand01(&v->rng) * VIZ_ONSET_GROUPS)
                                % VIZ_ONSET_GROUPS,
                            VIZ_ONSET_GROUPS, 1.0f);
    }

    if (!reduce_motion && v->master > 0.0f) {
        viz_onset(v->cbars, v->spec_avg, nb, rise, VIZ_ONSET_GROUPS);
        for (g = 0; g < VIZ_ONSET_GROUPS; ++g) {
            if (rise[g] < ONSET_TRIGGER || v->actor_cool[g] > 0.0f)
                continue;
            slot = viz_actor_slot(v->actors, VIZ_ACTORS);
            if (slot < 0)
                break;
            viz_actor_spawn(&v->actors[slot], &v->rng,
                            g == 0 ? VIZ_ACTOR_EXTRA : VIZ_ACTOR_SPARK, g,
                            VIZ_ONSET_GROUPS, rise[g]);
            v->actor_cool[g] = ACTOR_COOL;
        }
        if (tick_rise > TICK_TRIGGER && v->actor_cool[VIZ_ONSET_GROUPS] <= 0.0f) {
            slot = viz_actor_slot(v->actors, VIZ_ACTORS);
            if (slot >= 0) {
                viz_actor_spawn(&v->actors[slot], &v->rng, VIZ_ACTOR_SPARK,
                                VIZ_ONSET_GROUPS - 1, VIZ_ONSET_GROUPS, v->tick);
                v->actor_cool[VIZ_ONSET_GROUPS] = ACTOR_COOL_TICK;
            }
        }
    }

    /* Presence: a band's raw level, or the mids for a lead. Rate is music time:
     * flux and kick push it, a steady tone or silence leaves it at zero. */
    for (g = 0; g < VIZ_ONSET_GROUPS; ++g) {
        const int from = g * nb / VIZ_ONSET_GROUPS;
        const int to = (g + 1) * nb / VIZ_ONSET_GROUPS;

        lvl[g] = viz_band_mean(v->bars, from, to);
    }
    rate = reduce_motion ? 0.0f : (v->flux + 0.5f * v->kick) * v->master;
    for (a = 0; a < VIZ_ACTORS; ++a) {
        VizActor *ac = &v->actors[a];

        if (!ac->live)
            continue;
        presence = ac->kind == VIZ_ACTOR_LEAD ? v->mid_gate : lvl[ac->band];
        if (reduce_motion)
            presence = 0.0f;
        viz_actor_step(ac, dt, rate, presence * v->master);
    }
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

    for (i = 0; i < 3; ++i)
        v->phx[i] = PHX_REST[i];
    v->trail_decay = 1.0f;
    /* Seeded from the clock and the allocation, so each launch differs. */
    v->rng = ((unsigned int)SDL_GetTicks() * 2654435761u) ^ (unsigned int)(size_t)v;
    if (!v->rng)
        v->rng = 1u;
    motion_reset(v);
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
    float sum_bass = 0.0f, sum_mid = 0.0f, sum_treble = 0.0f, sum_all = 0.0f;
    float centre, flux_now, rise_k;

    /* Hidden views do no work: no sample copy, no FFT. */
    if (!active) {
        /* Forget the spectrum and flux so reopening the tab starts clean. */
        v->primed = 0;
        v->flux = 0.0f;
        v->avg_ready = 0;
        motion_reset(v);
        return;
    }

    playback_get_samples(v->samples, VIZ_FFT_N);
    viz_spectrum(v->samples, VIZ_FFT_N, v->target, nb,
                 (float)s->gain_pct / 100.0f);
    viz_smooth(v->bars, v->target, nb, (float)s->smooth_pct / 100.0f);
    /* The fractal works from the compressed copy, so quiet bands register. */
    viz_compress(v->cbars, v->bars, nb, VIZ_COMPRESS);

    /* Bass and mid are the mean of the lower quarter and the next quarter of
     * the bars, which are log-spaced, so they follow low and mid frequencies. */
    q = nb / 4;
    h = nb / 2;
    for (i = 0; i < q; ++i)
        sum_bass += v->cbars[i];
    for (i = q; i < h; ++i)
        sum_mid += v->cbars[i];
    v->bass = q ? sum_bass / (float)q : 0.0f;
    v->mid = (h > q) ? sum_mid / (float)(h - q) : 0.0f;

    /* Treble is the top quarter of the bars. */
    for (i = nb - q; i < nb; ++i)
        sum_treble += v->cbars[i];
    v->treble = q ? sum_treble / (float)q : 0.0f;
    /* The beat tracker and the silence gates read the raw bars: their
     * thresholds were tuned on raw values, and compression would lift hiss
     * over the floor. */
    viz_beat(&v->bass_avg, &v->kick, viz_band_mean(v->bars, 0, q), dt);
    /* Clicks are too brief for the bars, so they get their own detector on
     * the raw samples. */
    viz_tick(v->samples, VIZ_FFT_N, (float)s->gain_pct / 100.0f,
             &v->tick_avg, &v->tick, dt);

    /* Overall loudness, gated: below the noise floor the picture is exactly
     * black, so silence and pause leave no shape and no movement. */
    for (i = 0; i < nb; ++i)
        sum_all += v->cbars[i];
    v->level = nb ? sum_all / (float)nb : 0.0f;
    v->master = viz_band_gate(v->bars, 0, nb, 0.03f);
    v->mid_gate = viz_band_gate(v->bars, q, h, 0.03f);

    /* Spectral flux: how much the spectrum is rising right now. A steady tone
     * gives zero, so the kaleidoscope stops; each new note turns it. The
     * envelope lets a rise ring out briefly rather than cutting off. Gated by
     * master so silence holds the picture still. */
    flux_now = v->primed ? viz_flux(v->prev_bars, v->cbars, nb) : 0.0f;
    flux_now *= v->master;
    memcpy(v->prev_bars, v->cbars, (size_t)nb * sizeof v->cbars[0]);
    v->primed = 1;
    v->flux = flux_now > v->flux ? flux_now : v->flux * expf(-dt * 6.0f);

    /* Centroid is smoothed so the hue glides instead of flickering. */
    centre = viz_centroid(v->cbars, nb);
    rise_k = 1.0f - expf(-dt * 2.0f);
    v->centroid += (centre - v->centroid) * rise_k;

    /* Trails fade faster as the music drops, so silence clears the screen. */
    v->trail_decay = expf(-dt * (5.0f + 25.0f * (1.0f - v->master)));

    /* Under reduce_motion nothing moves on its own: the shape and brightness
     * still follow the bands, but the kick, flux rotation and ripple are off. */
    if (reduce_motion) {
        v->kick = 0.0f;
        v->tick = 0.0f;
        v->flux = 0.0f;
        v->ripple = 0.0f;
    } else {
        v->ripple = v->treble;
        /* Rotation only ever advances on flux; wrapped to one turn so float
         * precision holds over long sessions (2*pi is a symmetry of the fold). */
        v->rot += v->flux * dt * 1.5f;
        if (v->rot > TAU_F)
            v->rot -= TAU_F;
    }

    /* Music memory. Kick onsets fling the Julia constant (wander). A section
     * change, where the spectrum drifts from its slow average, picks a new
     * curated anchor, wedge count and Phoenix offset. All of it is gated by the
     * master level and off under reduce_motion, so silence and reduced motion
     * hold the picture still. */
    {
        static const int segs_choice[4] = {4, 5, 6, 8};
        const float ka = 1.0f - expf(-dt * 0.25f);  /* about a 4 s average */
        const float glide = 1.0f - expf(-dt * 0.5f); /* about a 2 s centre glide */
        const float kick_rise = (!reduce_motion && v->kick > v->prev_kick)
                                    ? v->kick - v->prev_kick : 0.0f;
        /* A click flings the constant too, but not through the master gate:
         * a click over near-silence still moves the picture. */
        const float tick_rise = (!reduce_motion && v->tick > v->prev_tick)
                                    ? v->tick - v->prev_tick : 0.0f;
        float novelty, base[2], tgt[2];
        int k, section_fired = 0;

        if (!v->avg_ready) {
            memcpy(v->spec_avg, v->cbars, (size_t)nb * sizeof v->cbars[0]);
            v->avg_ready = 1;
        }
        novelty = viz_novelty(v->spec_avg, v->cbars, nb) * v->master;
        for (i = 0; i < nb; ++i)
            v->spec_avg[i] += (v->cbars[i] - v->spec_avg[i]) * ka;

        v->novelty_cool -= dt;
        if (!reduce_motion && novelty > NOVELTY_THRESHOLD
            && v->novelty_cool <= 0.0f) {
            /* Always move: a random step of 1..N-1 from the current anchor. */
            if (v->anchor < 0)
                v->anchor = (int)(viz_rand01(&v->rng) * VIZ_JULIA_ANCHORS)
                            % VIZ_JULIA_ANCHORS;
            else
                v->anchor = (v->anchor + 1 + (int)(viz_rand01(&v->rng)
                                * (VIZ_JULIA_ANCHORS - 1))) % VIZ_JULIA_ANCHORS;
            v->segs = segs_choice[(int)(viz_rand01(&v->rng) * 4.0f) % 4];
            for (k = 0; k < 3; ++k)
                v->phx_off[k] = (viz_rand01(&v->rng) * 2.0f - 1.0f) * PHX_OFF[k];
            v->novelty_cool = SECTION_COOLDOWN;
            section_fired = 1;
        }

        viz_wander_step(v->wander, &v->rng,
                        fminf(1.0f, kick_rise * v->master + tick_rise), dt);

        /* The centre glides to the anchor, or to the classic centre when there
         * is none. The music still moves the shape around that centre. */
        viz_fractal_c(0.0f, 0.0f, 0.0f, &base[0], &base[1]);
        if (v->anchor >= 0)
            viz_julia_anchor(v->anchor, &tgt[0], &tgt[1]);
        else {
            tgt[0] = base[0];
            tgt[1] = base[1];
        }
        for (k = 0; k < 2; ++k)
            v->ctr[k] += (tgt[k] - v->ctr[k]) * glide;

        /* The background shape is the spectrum wrapped around it. Its depth
         * follows the gated level, with a kick punch, and eases so it morphs.
         * Under reduce_motion it goes back to the plain circle. */
        {
            const float depth_tgt = reduce_motion ? 0.0f
                : fminf(SHAPE_DEPTH_MAX,
                        v->master * (0.7f + 0.2f * v->kick));
            const float d_ease = 1.0f - expf(-dt * 4.0f);

            viz_outline(v->cbars, nb, v->outline, VIZ_OUTLINE_N);
            v->shape_depth += (depth_tgt - v->shape_depth) * d_ease;
        }
        actors_update(v, dt, nb, section_fired, tick_rise, reduce_motion);
        v->prev_kick = v->kick;
        v->prev_tick = v->tick;
    }

    /* Phoenix shape: pulled from its resting silhouette by the mids and the
     * treble, and eased so changes morph rather than snap. */
    {
        const float ease = 1.0f - expf(-dt * 1.2f);
        const float nudge[3] = {
            0.10f * v->mid, 0.08f * v->mid, -0.15f * v->treble
        };
        int k;

        for (k = 0; k < 3; ++k) {
            float tgt = PHX_REST[k] + nudge[k] + v->phx_off[k];

            tgt = tgt < PHX_LO[k] ? PHX_LO[k] : (tgt > PHX_HI[k] ? PHX_HI[k] : tgt);
            v->phx[k] += (tgt - v->phx[k]) * ease;
        }
    }
}

/* Fractal look. Every visible element is a response to the music, with no
 * clock: the Julia layer is rippled by the treble and folded into a
 * kaleidoscope whose rotation is driven by spectral flux; its rings take their
 * brightness from the spectrum, bass at the centre and treble at the rim. The
 * Phoenix creature sits on top, present only while the mids are. Brightness
 * follows the gated level, and each frame keeps a fading copy of the last. */
#define PHX_HALF     1.6f     /* half-height of the Phoenix view */
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
    /* Hue follows the centroid (dark sound one end of the wheel, bright the
     * other) on top of the user's offset. No time term, so no drift. */
    const float hue = (float)s->hue_pct / 100.0f + 0.6f * v->centroid;
    /* Brightness: the gated level, with a short punch on each bass onset. */
    const float lum = v->master * (1.0f + 0.5f * v->kick) + TICK_LUM * v->tick;
    /* Louder pushes in; a bass onset punches in briefly. */
    const float zoom = (1.5f - 0.6f * v->level) * (1.0f - 0.2f * v->kick);
    const float phx_half = PHX_HALF * (1.0f - 0.3f * v->level);
    /* The creature's presence is the mids, so it appears with vocals and solos
     * and is absent in a held silence. */
    const float phx_on = lum * v->mid_gate;
    const float rot = v->rot;
    unsigned int any = 0;
    float cr, ci;
    int x, y, a;

    /* Silence: the last frame was black and the level is gated to zero, so
     * there is nothing to draw and the texture already shows black. */
    if (v->master <= 0.0f && v->tick < 0.01f && v->dark)
        return;

    /* The Julia constant: the music's offset from the classic centre, added to
     * the eased centre (which tracks the anchor) plus the kick wander. */
    {
        float bcr, bci;

        viz_fractal_c(v->bass, v->mid, v->treble, &cr, &ci);
        viz_fractal_c(0.0f, 0.0f, 0.0f, &bcr, &bci);
        cr = v->ctr[0] + (cr - bcr) + v->wander[0];
        ci = v->ctr[1] + (ci - bci) + v->wander[1];
    }

    for (y = 0; y < FRACTAL_H; ++y) {
        const float w = ((float)y / FRACTAL_H - 0.5f) * 2.0f;

        for (x = 0; x < FRACTAL_W; ++x) {
            const float u = ((float)x / FRACTAL_W - 0.5f) * 2.0f;
            const float re = u * zoom * aspect;
            const float im = w * zoom;
            const float amp = RIPPLE_AMP * v->ripple * zoom;
            const float rre = re + amp * sinf(im * 9.0f + rot * 2.0f);
            const float rim = im + amp * sinf(re * 7.0f - rot * 1.5f);
            /* Ring: centre is bass, rim is treble. Each ring glows with the bar
             * its radius maps to, so the picture is the spectrum laid out. */
            /* The distance is a morphing superellipse with star points, drifted
             * by the kick wander; at p=2 and no depth it is the old circle. */
            const float ring = v->cbars[viz_ring_bin(
                viz_shape_metric(u - v->wander[0], w - v->wander[1],
                                 v->outline, VIZ_OUTLINE_N, v->shape_depth),
                s->bars)];
            const size_t idx = (size_t)y * FRACTAL_W + (size_t)x;
            float fre, fim, r, g, b, sr, sg, sb, pr, pg, pb, k;
            unsigned int prev;

            viz_kaleido(rre, rim, v->segs, rot, &fre, &fim);
            mu_rgb(viz_julia_mu(fre, fim, cr, ci, FRACTAL_ITER), hue,
                   &r, &g, &b);
            k = lum * ring;
            r *= k;
            g *= k;
            b *= k;

            if (phx_on > 0.01f) {
                mu_rgb(viz_phoenix_mu(u * phx_half * aspect, w * phx_half,
                                      v->phx[0], v->phx[1], v->phx[2],
                                      FRACTAL_ITER),
                       hue + 0.5f, &sr, &sg, &sb);
                k = phx_on * ring;
                r += k * sr;
                g += k * sg;
                b += k * sb;
            }
            /* Actors: each live one is a soft disc of its own Julia set, lit by
             * its envelope and the spectrum ring under the pixel. */
            for (a = 0; a < VIZ_ACTORS; ++a) {
                const VizActor *ac = &v->actors[a];
                float dx, dy, d2, mask, ar, ag, ab;

                if (!ac->live)
                    continue;
                dx = (u - ac->x) / ac->size;
                dy = (w - ac->y) / ac->size;
                d2 = dx * dx + dy * dy;
                if (d2 >= 1.0f)
                    continue;
                mask = (1.0f - d2) * (1.0f - d2);
                mu_rgb(viz_julia_mu(dx * 1.2f, dy * 1.2f, ac->cr, ac->ci,
                                    FRACTAL_ITER),
                       hue + ac->hue, &ar, &ag, &ab);
                k = lum * ac->env * mask * (0.5f + 0.5f * ring);
                r += k * ar;
                g += k * ag;
                b += k * ab;
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
            any |= v->pixels[idx] & 0x00FFFFFFu;
        }
    }
    v->dark = (any == 0);
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
        /* A click lifts every bar to a low floor, drawn only here, so the
         * bars the gates and fractal read are unchanged. */
        float h = fmaxf(v->bars[i], TICK_BAR * v->tick) * hmax, tt, tri;

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
