#ifndef PVZ2NATIVE_CONFIG_H
#define PVZ2NATIVE_CONFIG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runtime configuration, read once from config.ini next to the executable.
 *
 * Every debug switch defaults OFF, so a plain build (no config.ini at all) is
 * quiet and fast. The .ini is the ONLY source of truth -- the old PVZ2_*
 * environment variables are no longer consulted. See config.cpp for the parser
 * and pvz2_config_load() for how defaults and path resolution are filled in.
 *
 * C-callable on purpose: main.c (the SDL host loop) is C, and the dependency /
 * runtime shims that read these flags are C++. Both include this one header. */
typedef struct pvz2_config {
    /* [log] -- what the harness prints. */
    int verbose;        /* blow-by-blow boot log: per .init_array entry, per lifecycle call */
    int trace;          /* per-import-call trace (~57MB/run, very loud) */
    int pc_sample;      /* [pc-sample]/[HOT] instruction sampling + hot-frame slicing */
    int input;          /* per-event touch/key logging + a frame heartbeat */

    /* [runtime] -- how the emulator behaves. */
    int no_page_table;        /* route guest memory through the slow callback path */
    unsigned heap_quarantine; /* hold the N most-recently-freed heap blocks out of reuse */

    /* [gl] -- rendering diagnostics. */
    int gl_debug_clear;     /* force a loud clear colour instead of the engine's black */
    int gl_no_viewport_fix; /* disable the zero-size viewport substitution */
    int gl_flat_fragment;   /* replace every fragment shader body with a constant colour */
    int gl_strict;          /* query GL state on every suspicious call */

    /* [game] -- what the engine is told about the "device".
     *
     * The Java locale, e.g. "en_US" or "es_AR". Info_SysGetUserLocale returns it
     * verbatim, and Info_SysGetCountryCodeString derives the country from the
     * part after '_'. It steers currency, store region and localized text; a
     * value the .obb has no translation for just falls back to English, so any
     * <lang>_<REGION> string is safe. Empty -> "en_US". */
    char user_locale[32];

    /* Emulated in-app purchases. When on, the store reports itself available and
     * every purchase the engine requests completes immediately (the item is
     * granted). When off, the store reports "not supported" and nothing is
     * purchasable. Defaults ON -- see pvz2_config_load. */
    int emulate_iap;

    /* [video] -- how the window and the guest surface are sized.
     *
     * mode picks the launch resolution when width/height are left at 0:
     *   "auto"   -- match the primary display's ASPECT at a moderate base height
     *               (kAutoBaseHeight), so proportions are right without the GPU
     *               cost of a full-native surface. The default.
     *   "native" -- match the display's full resolution.
     * An explicit width AND height (both > 0) override mode and are used verbatim.
     * The window is always resizable and the engine re-fits on resize, so this is
     * only the STARTING size. fullscreen starts the window borderless-fullscreen
     * (F11 toggles it at runtime either way). */
    char video_mode[16];
    int video_width;
    int video_height;
    int video_fullscreen;

    /* Frame-rate cap, in presented frames per second.
     *
     * The engine advances its simulation from the wall clock (gettimeofday), not
     * from a frame count, so presenting fewer frames does NOT slow the game
     * down -- it only stops the loop from re-running onDrawFrame as fast as the
     * CPU allows and pinning a core at 100%.
     *
     *   > 0 -- pace the loop to at most this many frames per second. Vsync stays
     *          on, so the rate that actually reaches the screen is the LOWER of
     *          this and the display's refresh rate.
     *   0   -- no cap at all: vsync is switched off too and frames are presented
     *          as fast as the machine manages (tearing, one core saturated).
     * Anything negative reads as 0. Default 120 -- see pvz2_config_load. */
    int fps_limit;

    /* What AndroidHttpProxy.GetNetworkStatus() reports to the engine:
     *   0 -- no network        1 -- mobile        2 -- Wi-Fi
     *
     * Defaults to 0 and should stay there: this port has no network stack, and a
     * connected answer makes the boot hang on the loading screen waiting for
     * downloads that can never arrive (measured on 4.5.2, not theorised).
     *
     * It is configurable only because the value is also what the purchase broker
     * consults before allowing a purchase -- which is why buying used to report
     * "Service Unavailable". That is now solved without lying to the whole
     * engine: the broker's own connectivity check is rewritten in the loaded
     * image (game/patches.h), so the store works with this at 0. */
    int network_status;

    /* Persist the engine's key/value store (its SharedPreferences equivalent) to
     * save_dir, so settings that must survive a restart -- the age gate above
     * all -- actually do. Off makes every launch a clean first run. Default ON. */
    int persist_saves;

    /* [paths] -- always concrete after load: a value left unset in the .ini is
     * filled with <exe_dir>/lib/<default filename> (so/obb) or <exe_dir>/save
     * (save_dir). */
    char so_path[512];
    char obb_path[512];
    char save_dir[512];
} pvz2_config_t;

/* Base height the "auto" video mode scales the display aspect to. 540 keeps the
 * fragment load close to the original fixed 960x540 while giving 4:3/16:10/16:9
 * displays each their correct proportions. */
#define PVZ2_AUTO_BASE_HEIGHT 540

/* Loads config.ini from `ini_path` (may be NULL or missing -> all defaults).
 * Any path left unset in the file is resolved against `base_dir` (the
 * executable's directory, normally with a trailing separator) as
 * base_dir + "lib/" + default name. Call once, early, before
 * pvz2_session_start. Safe to call again to reload. */
void pvz2_config_load(const char *ini_path, const char *base_dir);

/* The loaded config. Never NULL: before pvz2_config_load runs it returns an
 * all-defaults (all-off, empty paths) instance, so shim code can read it
 * unconditionally without an init check. */
const pvz2_config_t *pvz2_config(void);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif
