#ifndef PVZ2NATIVE_GFX_VIDEO_MODE_H
#define PVZ2NATIVE_GFX_VIDEO_MODE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves the launch resolution from the [video] config and the primary
 * display, filling *width/*height with the window (and guest surface) size and
 * *fullscreen with whether to start borderless-fullscreen.
 *
 * Precedence, mirroring config.h:
 *   1. an explicit width AND height in the config are used verbatim;
 *   2. mode = native  -> the display's full resolution;
 *   3. mode = auto     -> the display's ASPECT at PVZ2_AUTO_BASE_HEIGHT, so a
 *      4:3/16:10/16:9 monitor each get correct proportions at a light GPU cost.
 * Falls back to 960x540 if the display cannot be queried. Requires SDL video to
 * be initialised. None of the outputs is ever left <= 0. */
void pvz2_choose_window_size(int *width, int *height, int *fullscreen);

#ifdef __cplusplus
}
#endif

#endif
