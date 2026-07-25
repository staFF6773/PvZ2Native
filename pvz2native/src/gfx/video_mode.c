/* Launch-resolution selection -- see include/pvz2native/gfx/video_mode.h.
 *
 * Kept apart from main.c because "which resolution do we start at" is its own
 * decision with three inputs (config, display, aspect) and a clear precedence,
 * and main.c only wants the answer.
 */

#include <pvz2native/gfx/video_mode.h>

#include <pvz2native/config.h>

#include <SDL.h>

#include <string.h>

/* 960x540 -- the historical fixed size, used when the display cannot be read. */
#define PVZ2_FALLBACK_W 960
#define PVZ2_FALLBACK_H 540

/* Textures and the composite tend to assume even dimensions; round to the
 * nearest even so an odd aspect (e.g. 1366x768) never yields an odd width. */
static int round_even(double v) {
    int n = (int)(v + 0.5);
    return n & 1 ? n + 1 : n;
}

void pvz2_choose_window_size(int *width, int *height, int *fullscreen) {
    const pvz2_config_t *cfg = pvz2_config();

    if (fullscreen != NULL) *fullscreen = cfg->video_fullscreen ? 1 : 0;

    /* 1. Explicit override -- both dimensions set -> use as-is. */
    if (cfg->video_width > 0 && cfg->video_height > 0) {
        *width = cfg->video_width;
        *height = cfg->video_height;
        return;
    }

    /* The primary display's mode drives both auto and native; without it there
     * is nothing to derive an aspect from, so fall back to the fixed size. */
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) != 0 || dm.w <= 0 || dm.h <= 0) {
        *width = PVZ2_FALLBACK_W;
        *height = PVZ2_FALLBACK_H;
        return;
    }

    /* 2. Native -- the display's full resolution. */
    if (strcmp(cfg->video_mode, "native") == 0) {
        *width = dm.w;
        *height = dm.h;
        return;
    }

    /* 3. Auto (the default) -- the display's aspect at a moderate base height,
     * so proportions are right without paying for a full-native surface. */
    const double aspect = (double)dm.w / (double)dm.h;
    int h = PVZ2_AUTO_BASE_HEIGHT;
    int w = round_even((double)h * aspect);
    if (w < 320) w = 320; /* an implausibly narrow aspect still yields a usable window */
    *width = w;
    *height = h;
}
