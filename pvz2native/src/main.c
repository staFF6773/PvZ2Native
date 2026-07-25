#include <stdio.h>
#include <stdlib.h>
#include <SDL.h>
#include <glad/gl.h>
#include <pvz2native/config.h>
#include <pvz2native/game_parameters.h>
#include <pvz2native/gfx/frame_limiter.h>
#include <pvz2native/gfx/gl_requirements.h>
#include <pvz2native/gfx/video_mode.h>
#include <pvz2native/input/input_queue.h>
#include <pvz2native/pvz2_session.h>

/* The engine renders at a fixed resolution (pvz2_render_size); the window can be
 * any size and the compositor scales to it. So input arrives in window pixels
 * and has to be mapped back into that fixed render space, or a click lands
 * somewhere else once the window is not exactly the render size. Filled from
 * pvz2_render_size() at startup; the window size is tracked on every resize. */
static int g_render_w = 960, g_render_h = 540;
static int g_win_w = 960, g_win_h = 540;

static int to_render_x(int x) { return g_win_w > 0 ? x * g_render_w / g_win_w : x; }
static int to_render_y(int y) { return g_win_h > 0 ? y * g_render_h / g_win_h : y; }

/* Turns one SDL event into a guest input event.
 *
 * The engine has exactly one input channel -- UI_ProcessEvents -- so
 * everything funnels through the queue and is serialised there once per frame.
 *
 * Only ENTER and BACKSPACE are sent as key events, because the engine's
 * CharToKeyCode maps every other keycode to 0 and drops it; printable
 * characters have to arrive as SDL_TEXTINPUT text instead. ESCAPE is
 * deliberately NOT mapped to Android's BACK key: it already means "quit" here,
 * and having one key do both would make the window impossible to close once
 * the game starts consuming input. */
static void feed_input_event(const SDL_Event *e) {
    switch (e->type) {
        case SDL_MOUSEBUTTONDOWN:
            if (e->button.button == SDL_BUTTON_LEFT) {
                pvz2_input_push_touch(PVZ2_TOUCH_DOWN, to_render_x(e->button.x), to_render_y(e->button.y));
            }
            break;
        case SDL_MOUSEBUTTONUP:
            if (e->button.button == SDL_BUTTON_LEFT) {
                pvz2_input_push_touch(PVZ2_TOUCH_UP, to_render_x(e->button.x), to_render_y(e->button.y));
            }
            break;
        case SDL_MOUSEMOTION:
            /* A mouse with no button held has no touch equivalent; sending it
             * would look to the engine like a finger dragging across the
             * screen at all times. */
            if (e->motion.state & SDL_BUTTON_LMASK) {
                pvz2_input_push_touch(PVZ2_TOUCH_MOVE, to_render_x(e->motion.x), to_render_y(e->motion.y));
            }
            break;
        case SDL_TEXTINPUT:
            pvz2_input_push_text(e->text.text);
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            int code = 0;
            if (e->key.keysym.sym == SDLK_RETURN || e->key.keysym.sym == SDLK_KP_ENTER) {
                code = PVZ2_KEY_ENTER;
            } else if (e->key.keysym.sym == SDLK_BACKSPACE) {
                code = PVZ2_KEY_DEL;
            }
            if (code != 0) pvz2_input_push_key(code, e->type == SDL_KEYDOWN ? 1 : 0);
            break;
        }
        default:
            break;
    }
}

/* SDL only delivers SDL_TEXTINPUT while text input is active, so this is what
 * makes typing reach the game -- driven by the engine's own
 * Device_ShowKeyboard/Device_HideKeyboard calls. */
static void sync_text_input(void) {
    int wanted = pvz2_input_keyboard_wanted();
    SDL_bool active = SDL_IsTextInputActive();
    if (wanted && !active) {
        SDL_StartTextInput();
    } else if (!wanted && active) {
        SDL_StopTextInput();
    }
}

/* Set once the window exists, so the boot-time pump can present to it. */
static SDL_Window *g_window = NULL;
static int g_quit_requested = 0;

/* Current borderless-fullscreen state; F11 toggles it. */
static int g_fullscreen = 0;

/* Flipped once pvz2_session_start returns. While 0 the window title shows the
 * boot progress (the boot is minutes of dark window and this is the only sign
 * it is alive); once 1 the title shows the live FPS instead. */
static int g_booted = 0;

/* Re-reads the window's drawable size and re-renders the game at it: the
 * compositor is told the new size immediately (so THIS frame already fills the
 * window), and the engine re-runs onSurfaceChanged so it re-fits its projection.
 *
 * Note what is deliberately NOT updated here: g_render_w/h. The engine's TOUCH
 * coordinate space is frozen at the launch resolution (pvz2_render_size) --
 * onSurfaceChanged updates the projection but the touch scaler keys off
 * mOrigScreenWidth, which SetWidthHeight sets once at startup and the resize
 * path never revisits. So input must always map window pixels back into that
 * fixed space; making g_render track the window (identity) put every click in
 * the wrong place. g_render stays at the launch size and to_render_* rescales. */
static void update_window_size(void) {
    if (!g_window) return;
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    if (dw > 0 && dh > 0) {
        g_win_w = dw;
        g_win_h = dh;
        pvz2_set_drawable_size(dw, dh);
        pvz2_session_request_resize(dw, dh);
    }
}

/* Closing the window has to take effect immediately. The boot runs for many
 * seconds inside a single guest call that cannot be unwound, so setting a flag
 * would leave the window on screen -- ignoring the close -- until the boot
 * finishes. Exiting straight from the event handler is the only way to make the
 * X button respond during startup; the OS reclaims everything. */
static void handle_quit(void) {
    fflush(stdout);
    exit(0);
}

/* F11 toggles borderless fullscreen. FULLSCREEN_DESKTOP (not real fullscreen)
 * keeps the desktop resolution and just borderlessly fills the screen, which
 * avoids a mode switch and plays well with the compositor scaling. The size
 * change it causes arrives as SDL_WINDOWEVENT_SIZE_CHANGED, so update_window_size
 * re-fits the engine the same way an ordinary resize does -- nothing extra here. */
static void toggle_fullscreen(void) {
    if (!g_window) return;
    g_fullscreen = !g_fullscreen;
    SDL_SetWindowFullscreen(g_window, g_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}

/* The single place an SDL event turns into an action. Both drain sites -- the
 * boot-time host_pump and the steady-state loop -- route through here so quit,
 * F11 and resize behave identically in both, and adding a shortcut is a one-line
 * change in one function rather than two that have to be kept in sync. */
static void handle_sdl_event(const SDL_Event *e) {
    if (e->type == SDL_QUIT ||
        (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_ESCAPE)) {
        handle_quit();
    } else if (e->type == SDL_KEYDOWN && e->key.keysym.sym == SDLK_F11 && e->key.repeat == 0) {
        toggle_fullscreen();
    } else if (e->type == SDL_WINDOWEVENT &&
               e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
        update_window_size();
    }
    /* Also fed to the input queue: F11/ESC map to no guest key, so this is safe,
     * and anything typed or clicked during the boot still reaches the engine. */
    feed_input_event(e);
}

/* Once-per-half-second FPS in the window title, from real presented frames.
 * This replaced the old per-frame "Native_onDrawFrame[N]" status text, which
 * churned a mutex and a SetWindowTitle every frame. Measured off the wall clock,
 * so it is honest about dropped/duplicated frames. */
static void update_fps_title(void) {
    static Uint32 window_start = 0;
    static int frames = 0;
    static int started = 0;
    Uint32 now = SDL_GetTicks();
    if (!started) { window_start = now; started = 1; }
    ++frames;
    Uint32 elapsed = now - window_start;
    if (elapsed >= 500) {
        double fps = (double)frames * 1000.0 / (double)elapsed;
        char title[64];
        SDL_snprintf(title, sizeof(title), "PvZ2Native - %.0f FPS", fps);
        if (g_window) SDL_SetWindowTitle(g_window, title);
        frames = 0;
        window_start = now;
    }
}

/* Called from inside long guest calls (see pvz2_session_set_host_pump). The
 * boot sequence runs for minutes in one call, so without draining the message
 * queue the OS marks the window "Not responding" for all of startup. Rate
 * limited because it is invoked on every JIT slice boundary, which during
 * instruction-level tracing is every few dozen ticks. */
static void host_pump(void) {
    static Uint32 last_pump = 0;
    Uint32 now = SDL_GetTicks();
    if (now - last_pump < 100) return;
    last_pump = now;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        /* The boot runs for seconds inside a single guest call, so events
         * (including a close or an F11) have to be acted on here too, not just
         * in the steady-state loop -- hence the shared handler. */
        handle_sdl_event(&event);
    }
    sync_text_input();

    /* The boot takes minutes behind a dark window; without this there is no way
     * to tell a working startup from a hang. Only while booting: once the game
     * is running the steady-state loop owns the title and shows the FPS instead
     * (host_pump still runs between JIT slices inside a frame, so this guard is
     * what stops it from fighting update_fps_title over the title). Title updates
     * need no GL, so they are safe while the guest owns the context. */
    if (g_window && !g_booted) {
        char status[160];
        char title[224];
        pvz2_session_status(status, sizeof(status));
        SDL_snprintf(title, sizeof(title), "PvZ2Native - %s", status);
        SDL_SetWindowTitle(g_window, title);
    }

    /* Deliberately no SDL_GL_SwapWindow here. The guest owns the GL state, so
     * we cannot redraw, and swapping without drawing just flips between the
     * two buffers -- one holding the colour painted before the boot, the other
     * undefined -- which reads on screen as a dark tone that keeps changing.
     * The single clear+swap done before the boot already painted the window;
     * draining the message queue is all that is needed to keep it alive. */
}

int main(int argc, char **argv) {
    parse_game_parameters(argc, argv);

    printf("pvz2native skeleton build OK\n");
    printf("game_path=%s home_path=%s\n", game_parameters.game_path, game_parameters.home_path);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Read config.ini from the folder the .exe lives in, and resolve the game
     * data paths relative to it (<exe>/lib/...) unless the file overrides them.
     * SDL_GetBasePath returns that folder with a trailing separator. */
    char *base_path = SDL_GetBasePath();
    char ini_path[1024];
    SDL_snprintf(ini_path, sizeof(ini_path), "%sconfig.ini", base_path ? base_path : "");
    pvz2_config_load(ini_path, base_path ? base_path : "");
    if (base_path) SDL_free(base_path);
    const pvz2_config_t *cfg = pvz2_config();
    printf("so=%s\n", cfg->so_path);
    printf("obb=%s\n", cfg->obb_path);

    /* PvZ2's .so only imports gl* symbols, never egl* -- on real Android the
     * Java-side GLSurfaceView owns EGL context creation and the native side
     * just receives an already-current context in onSurfaceCreated. SDL's
     * GL context here plays that same role. GL2.0 compatibility profile
     * (matches gles_compat's `glad_add_library` config) keeps both the
     * fixed-function pipeline (matrix stack, client-state arrays) GLES1.1
     * code needs and the shader entry points GLES2 code needs. */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    /* The launch resolution, resolved from [video] (auto-aspect by default) and
     * the display -- see gfx/video_mode. The engine renders AT this size and the
     * touch space is frozen to it; the window stays freely resizable and
     * update_window_size re-fits the engine on any later resize. pvz2_set_render_size
     * has to run before pvz2_session_start so the guest surface starts here too. */
    int start_fullscreen = 0;
    pvz2_choose_window_size(&g_render_w, &g_render_h, &start_fullscreen);
    pvz2_set_render_size(g_render_w, g_render_h);
    g_win_w = g_render_w;
    g_win_h = g_render_h;
    printf("video: launching at %dx%d%s\n", g_render_w, g_render_h,
           start_fullscreen ? " (fullscreen)" : "");
    SDL_Window *window = SDL_CreateWindow("PvZ2Native", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                           g_render_w, g_render_h,
                                           SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    if (!gl_context) {
        printf("SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_context);

    /* Cap the frame rate ([video] fps_limit, vsync + pacing -- see gfx/frame_limiter).
     * The engine advances its simulation from the real wall clock (gettimeofday),
     * not from a frame count, so presenting fewer frames does NOT slow the game
     * down -- it only stops the loop from spinning onDrawFrame thousands of times
     * a second and pinning a core at 100%. That is the single biggest win for a
     * weak CPU like the N2805: it drops the per-second work by roughly the ratio
     * of that uncapped rate to the capped one. */
    pvz2_frame_limit_init();

    int glad_version = gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress);
    if (glad_version == 0) {
        printf("gladLoadGL failed to load any GL functions\n");
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    printf("GL context ready: %s (loaded via glad %d.%d)\n", (const char *)glGetString(GL_VERSION),
           GLAD_VERSION_MAJOR(glad_version), GLAD_VERSION_MINOR(glad_version));

    /* Verify the GPU can actually run the engine (OpenGL 2.0 + framebuffer
     * objects) BEFORE loading the .so. A GPU that falls short is shown a message
     * box naming it and the versions, then the app exits cleanly instead of
     * booting for seconds only to render a silent black window. Running this also
     * latches the GLSL dialect the shader shim will emit (120 vs 130). */
    if (!pvz2_gl_check_requirements(window)) {
        SDL_GL_DeleteContext(gl_context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    /* Apply the configured fullscreen state now that the window and context
     * exist; the resulting size change is handled like any other resize. */
    if (start_fullscreen) {
        g_fullscreen = 1;
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }

    /* Resolved from config.ini above: <exe>/lib/libPVZ2.so by default, or the
     * [paths] so override. The matching .obb is resolved the same way and read
     * by the VFS layer from the config. */
    const char *so_path = cfg->so_path;

    /* Paint a defined colour before the (multi-minute) boot so the window
     * shows something deliberate rather than uninitialised backbuffer, then
     * let the boot keep the message queue drained through host_pump. */
    g_window = window;
    glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);
    SDL_PumpEvents();

    pvz2_session_set_host_pump(host_pump);
    pvz2_session_t *session = pvz2_session_start(so_path);
    /* Nonzero exit when the session never started -- a missing .so, or a build
     * of the game whose addresses we do not have (see src/game/symbols.cpp). */
    int exit_code = session ? 0 : 1;

    /* Boot is done: from here the title shows FPS, not boot progress. */
    g_booted = 1;

    if (session) {
        /* The window's real drawable size (HiDPI can differ from the requested
         * size), handed to the compositor before the first frame. */
        update_window_size();

        /* host_pump may already have seen a close during the boot. */
        int running = !g_quit_requested;
        while (running) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                handle_sdl_event(&event);
            }
            sync_text_input();
            if (!running || g_quit_requested) break;

            /* The guest renders its scene into an offscreen FBO and composites
             * it to the default framebuffer with a degenerate (zero-width)
             * viewport, so it never actually writes the backbuffer. With
             * double buffering, swapping every frame then alternates between
             * two buffers holding different stale content, which reads on
             * screen as flicker. Clearing here gives a stable window until the
             * composite works; the guest binds its own FBO immediately after,
             * so this does not disturb its rendering. */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glClearColor(0.06f, 0.07f, 0.10f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            /* The guest issues its GL calls into the context made current on
             * this thread, so the frame has to run here, between the pump and
             * the swap -- not on a worker. */
            if (!pvz2_session_frame(session)) break;
            SDL_GL_SwapWindow(window);
            update_fps_title();
            /* After the swap and the title, so the wait is the last thing in the
             * frame and the FPS shown is measured over the paced interval. */
            pvz2_frame_limit_wait();
        }
        pvz2_session_end(session);
    }

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exit_code;
}
