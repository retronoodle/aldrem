/* visualizer.h — the Visualizer tab and its fullscreen view. Drawing goes
 * through Nuklear's canvas and image commands, so the visual stays in order
 * with the rest of the frame and runs on the renderer's GPU path. The maths
 * lives in viz.h; this module only owns the drawing and per-view state. */
#ifndef VISUALIZER_H
#define VISUALIZER_H

#include <SDL.h>

#include "nuklear_config.h"
#include "theme.h"
#include "viz.h"

/* Result bits from viz_view_controls. */
#define VIZ_CTRL_CHANGED    1 /* a setting moved: caller should persist it */
#define VIZ_CTRL_FULLSCREEN 2 /* the Fullscreen button was activated */

/* Opaque per-app visualizer state: animation clock, smoothed bars, sample
 * buffer and the fractal texture. Created once; never a static. */
typedef struct VizView VizView;

/* Returns NULL on allocation failure. Needs the live renderer for the fractal
 * texture; fractal mode is skipped if that texture cannot be made. */
VizView *viz_view_create(SDL_Renderer *ren);
void viz_view_destroy(VizView *v);

/* Advances one frame. Samples are only read when active is 1, so the
 * visualizer costs nothing while its tab is hidden. reduce_motion freezes the
 * fractal clock and ignores audio input for it, so it sits still. */
void viz_view_update(VizView *v, const VizSettings *s, float dt, int active,
                     int reduce_motion);

/* Draws the current visual into the next row of the current layout. A
 * non-NULL title is printed in the top-left corner of the visual. */
void viz_view_draw(struct nk_context *ctx, const struct MpTheme *th,
                   VizView *v, const VizSettings *s, const char *title);

/* Mode buttons, the Fullscreen button and four sliders, laid out in the
 * current frame. tab_fullscreen is the Tab focus slot for the Fullscreen
 * button (0 for none). Returns VIZ_CTRL_* bits. */
int viz_view_controls(struct nk_context *ctx, float scale,
                      const struct MpTheme *th, VizView *v, VizSettings *s,
                      int tab_fullscreen, int enabled,
                      const struct MpButtonOpts *opt);

/* Height of the controls block in renderer pixels, including row spacing, for
 * sizing the visual above it. */
float viz_view_controls_height(struct nk_context *ctx, float scale);

#endif
