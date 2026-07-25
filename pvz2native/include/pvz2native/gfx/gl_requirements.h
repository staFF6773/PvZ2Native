#ifndef PVZ2NATIVE_GFX_GL_REQUIREMENTS_H
#define PVZ2NATIVE_GFX_GL_REQUIREMENTS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The minimum host OpenGL this build needs, and the single source of truth for
 * which GLSL dialect the shader shim has to emit.
 *
 * The engine renders through the GLES2 path -- everything is a programmable
 * shader -- so OpenGL 2.0 is the hard floor: below it there are no shaders at
 * all and there is literally nothing to draw. It also renders into an offscreen
 * FBO, which is core only in 3.0 but is provided on 2.1 by
 * GL_ARB/EXT_framebuffer_object (present on essentially every desktop GPU that
 * reaches 2.1), so the real requirement is "2.0 + framebuffer objects", which
 * the check below verifies directly by testing the entry points rather than
 * trusting a version number.
 *
 * The shaders themselves are GLSL ES 1.00. A 3.0+ context compiles them by
 * prepending "#version 130" (1.30 accepts the ES precision qualifiers as
 * no-ops); a 2.1 context needs "#version 120" plus a rewrite that strips those
 * qualifiers, since 1.20 does not know them. pvz2_gl_target_glsl() reports which,
 * so the GL layer holds no version policy of its own. */
#define PVZ2_REQUIRED_GL_MAJOR 2
#define PVZ2_REQUIRED_GL_MINOR 0

struct SDL_Window;

/* Detects the host GL version and capabilities, then verifies they meet the
 * requirement above. Must be called after the context is current and glad has
 * loaded (glGetString has to work). Running it also latches the answer for
 * pvz2_gl_target_glsl().
 *
 * Returns 1 when the requirement is met (and prints the GPU line to stdout).
 * Returns 0 when it is not, after showing a modal message box naming the GPU,
 * the detected OpenGL version and the required one -- so the user sees why the
 * game will not start rather than a silent black window. `window` may be NULL
 * (the box then has no parent). */
int pvz2_gl_check_requirements(struct SDL_Window *window);

/* Which GLSL version the shader shim must target: 130 for a 3.0+ context, 120
 * for a 2.x one. Returns 130 (the safe, already-proven path) until
 * pvz2_gl_check_requirements() has run. */
int pvz2_gl_target_glsl(void);

#ifdef __cplusplus
}
#endif

#endif
