/* vu.h — pure level-meter maths: dB mapping and needle/LED ballistics.
 * No SDL dependency, so it is unit-testable on its own. */
#ifndef VU_H
#define VU_H

/* Per-channel meter state, owned by the UI and advanced once per frame. */
typedef struct {
    float needle;     /* smoothed display position, 0..1 */
    float peak;       /* held peak position, 0..1 */
    float peak_age;   /* seconds since the held peak was last raised */
} VuBallistics;

/* Maps a linear amplitude to a 0..1 display position: -40 dBFS at 0, +3 dBFS
 * at 1, linear in dB. Zero or negligible input maps to 0. */
float vu_norm(float linear);

/* Advances one channel by dt seconds toward level (linear, post-volume).
 * Attack is quick and release slow, like a real meter. snap skips easing and
 * is used when motion is reduced. */
void vu_ballistics_update(VuBallistics *b, float level, float peak,
                          float dt, int snap);

#endif
