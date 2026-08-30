/*  RetroArch - A frontend for libretro.
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

/* OpenGL on the PlayStation 4, through Mesa's EGL and zink - ES or desktop, decided at BUILD
 * time by HAVE_OPENGLES.
 *
 * ⚠ TWO EBOOTS, NOT ONE BINARY THAT CHOOSES. HAVE_OPENGLES is a global -D rather than a
 * per-file one, and it decides more than this file: without it runloop.c REJECTS
 * RETRO_HW_CONTEXT_OPENGLES2/3 outright, and glsym_gl.c and glsym_es2.c both define
 * rglgen_symbol_map behind the same include guard. One binary can therefore carry one GL
 * flavour. The GLES package (RTRV00001) and the desktop-GL one (RTRG00001) are built from this
 * same tree by flags alone and installed side by side. See Makefile.orbis.
 *
 * ⚠ WHAT THIS UNBLOCKS, BECAUSE IT IS NOT "OPENGL CORES NOW WORK". The reason this file was
 * written is Nintendo 64. mupen64plus-next's fast renderer is GLideN64 driven by the HLE RSP,
 * and without a GL context the only pairing left is ParaLLEl-RDP with the LLE RSP - measured on
 * hardware at 22-25 fps, of which the LLE RSP alone is 24 ms against a 16.7 ms budget (see
 * ps4-mesa-docs docs/retroarch/HANDOFF.md). No amount of tuning reaches full speed from there; a different renderer does.
 *
 * ⚠ AND THE STACK UNDER IT IS DEEPER THAN IT LOOKS. There is no GL hardware path on this console.
 * Every call here ends up as Vulkan:
 *
 *     eglSwapBuffers -> kopper -> vkQueuePresentKHR -> VK_EXT_headless_surface -> wsi_orbis -> flip
 *
 * zink translates GL to Vulkan, kopper drives the swapchain, and RADV's Orbis WSI arm opens
 * sceVideoOut and flips. So a GL core on this console is a GL-on-Vulkan core, and its performance
 * is a question nobody has measured yet - the first frame rate off this driver is data, not a
 * confirmation.
 *
 * ⚠ THE VIDEO-OUT IS OPENED BY THE DRIVER, NOT BY US. wsi_orbis calls sceVideoOutOpen behind the
 * swapchain, exactly as it does for gfx_ctx_orbis_vk. Two context drivers must therefore never be
 * live at once, which RetroArch guarantees by construction - one video driver, one context - but
 * it is the reason this file opens no display of its own and has no business calling
 * ps4_video_out_*.
 *
 * Modelled on switch_ctx.c, which is the closest existing case: EGL on a console with one screen,
 * no window system and no display server.
 */

#include <stdint.h>
#include <stdlib.h>

#include <retro_miscellaneous.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../frontend/frontend_driver.h"
#include "../../verbosity.h"
#include "../common/egl_common.h"

#include <ps4_app.h>

#ifndef HAVE_OPENGLES
/* The desktop build reports what it actually got, which is the only honest answer: a driver may
 * hand back a context newer than the one asked for, and this one already has. glGetString is GL
 * 1.0 and resolves from the same shared-glapi dispatch the ES entry points use. */
#include <GL/gl.h>
#endif

/* The console's main video-out mode. There is no mode set here: the display is whatever the
 * system gave the process, and mesa's orbis platform reports the same figure back through
 * orbis_get_scanout_size() when it builds the surface. */
#define ORBIS_GL_WIDTH  1920
#define ORBIS_GL_HEIGHT 1080

typedef struct
{
   egl_ctx_data_t egl;
   unsigned width;
   unsigned height;
} orbis_gl_ctx_data_t;

static void gfx_ctx_orbis_gl_destroy(void *data)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;

   if (!ctx)
      return;

   egl_destroy(&ctx->egl);
   free(ctx);
}

static void *gfx_ctx_orbis_gl_init(void *video_driver)
{
   EGLint major, minor, n;
   orbis_gl_ctx_data_t *ctx = NULL;

   /* ⚠ R8G8B8A8 AND NOTHING ELSE, and asking for it is not a preference. mesa's orbis EGL
    * platform exposes only R-first configs on purpose: wsi_orbis registers its scan-out buffers
    * as A8B8G8R8_SRGB unconditionally and presents with no swizzle, so a B-first config produces
    * a picture with red and blue exchanged - silently, with no validation and no error return.
    * The platform not offering those configs is what protects us; this list only has to agree
    * with it. */
   static const EGLint attribs[] = {
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
#ifdef HAVE_OPENGLES
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
#else
      /* ⚠ THE CONFIG MUST ADVERTISE DESKTOP GL OR eglCreateContext NEVER GETS THE CHANCE TO
       * REFUSE. egl_dri2.c gives every EGLConfig RenderableType = disp->ClientAPIs, and the
       * screen's api_mask carries EGL_OPENGL_BIT because this Mesa is built with
       * -DHAVE_OPENGL=1. Leaving EGL_OPENGL_ES2_BIT here would match a config that cannot
       * carry the context we are about to ask for. */
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
#endif
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_NONE
   };

   if (!(ctx = (orbis_gl_ctx_data_t*)calloc(1, sizeof(*ctx))))
      return NULL;

   ctx->width  = ORBIS_GL_WIDTH;
   ctx->height = ORBIS_GL_HEIGHT;

   /* EGL_DEFAULT_DISPLAY reaches the orbis platform through _EGL_NATIVE_PLATFORM: it is built as
    * an undetectable platform and selected ahead of the surfaceless one EGL always appends. There
    * is one display and no display server, so there is nothing to name. */
   if (!egl_init_context(&ctx->egl, EGL_NONE, EGL_DEFAULT_DISPLAY,
            &major, &minor, &n, attribs, NULL))
   {
      egl_report_error();
      RARCH_ERR("[PS4] EGL would not initialise - no GL context on this run.\n");
      goto error;
   }

   RARCH_LOG("[PS4] EGL %d.%d up, %d config(s) matched.\n", (int)major, (int)minor, (int)n);
   return ctx;

error:
   gfx_ctx_orbis_gl_destroy(ctx);
   return NULL;
}

static void gfx_ctx_orbis_gl_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;
   *width  = ctx ? ctx->width  : ORBIS_GL_WIDTH;
   *height = ctx ? ctx->height : ORBIS_GL_HEIGHT;
}

#ifndef HAVE_OPENGLES
/* What bind_api was asked for. bind_api runs BEFORE init and set_video_mode - see
 * video_context_driver_init() - so by the time a context is created these are set. Statics
 * because the context-driver interface hands bind_api the VIDEO driver, not our own data. */
static unsigned orbis_gl_req_major = 3;
static unsigned orbis_gl_req_minor = 2;
static bool     orbis_gl_core_ctx  = true;

/* ⚠ THE CEILING IS 3.3, MEASURED, AND A LAPTOP SAID 4.6. mesa-ps4's glcaps probe was run on
 * hardware on 2026-08-30 and is the only evidence that counts here:
 *
 *     OpenGL 4.6 / 4.5   eglCreateContext refused (0x3009 = EGL_BAD_MATCH)
 *     OpenGL 3.3         OK -> "3.3 (Core Profile) Mesa 26.3.0-devel", GLSL 3.30, 226 exts
 *     OpenGL 3.2         OK -> the same 3.3 core context
 *     OpenGL 2.1         OK -> "3.3 (Compatibility Profile)", 304 exts
 *
 * An earlier probe against a drm-shim on the build host reported 4.6 and was wrong about this
 * console. So the ladder below tops out at 3.3 rather than asking for what a core requested and
 * failing the whole video driver when it is 4.x.
 *
 * ⚠ AND THE PROFILE IS DECIDED BY THE VERSION WHEN NO MASK IS GIVEN. EGL's default for
 * EGL_CONTEXT_OPENGL_PROFILE_MASK is the core bit, which is why 3.3 with no mask came back as a
 * core profile and 2.1 - too old for the mask to mean anything - came back as compatibility.
 * The mask is still passed explicitly on the rungs that want core, and each of those has a
 * bare-attribute twin immediately after it: the mask is an EGL 1.5 token and the console reports
 * EGL 1.5, but a rung that costs nothing is cheaper than a black screen. */
static bool orbis_gl_create_desktop(orbis_gl_ctx_data_t *ctx)
{
   unsigned i;
   /* major, minor, ask-for-core */
   static const struct { unsigned major, minor; int core; } rungs[] = {
      { 0, 0, 1 },   /* whatever bind_api was asked for, clamped below */
      { 3, 3, 1 },
      { 3, 3, 0 },
      { 3, 2, 1 },
      { 3, 2, 0 },
      { 2, 1, 0 }
   };

   for (i = 0; i < sizeof(rungs) / sizeof(rungs[0]); i++)
   {
      EGLint attribs[8];
      unsigned n     = 0;
      unsigned major = rungs[i].major;
      unsigned minor = rungs[i].minor;
      int      core  = rungs[i].core;

      if (i == 0)
      {
         major = orbis_gl_req_major;
         minor = orbis_gl_req_minor;
         core  = orbis_gl_core_ctx;
         /* Clamped to the measured ceiling rather than passed through: a core asking for 4.1
          * would otherwise spend a rung on a request this driver is known to refuse. */
         if (major > 3 || (major == 3 && minor > 3))
         {
            major = 3;
            minor = 3;
         }
         if (major < 3 || (major == 3 && minor < 2))
            core = 0;
      }

      attribs[n++] = EGL_CONTEXT_MAJOR_VERSION;
      attribs[n++] = (EGLint)major;
      attribs[n++] = EGL_CONTEXT_MINOR_VERSION;
      attribs[n++] = (EGLint)minor;
      if (core)
      {
         attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
         attribs[n++] = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
      }
      attribs[n++] = EGL_NONE;

      if (egl_create_context(&ctx->egl, attribs))
      {
         RARCH_LOG("[PS4] desktop GL %u.%u %s context created.\n",
               major, minor, core ? "core" : "(no profile mask)");
         return true;
      }
      RARCH_WARN("[PS4] GL %u.%u %s refused; trying the next rung.\n",
            major, minor, core ? "core" : "bare");
   }

   return false;
}
#endif

static bool gfx_ctx_orbis_gl_set_video_mode(void *data,
      unsigned width, unsigned height, bool fullscreen)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;

#ifdef HAVE_OPENGLES
   /* ⚠ THREE, THEN TWO, AND THE FALLBACK IS NOT DECORATION. GLideN64 wants GLES 3.1 for its full
    * path and has a reduced GLES 2 one; zink's ceiling here depends on what RADV reports for this
    * GPU, which is a question about the driver rather than about the console. Asking for 3 and
    * accepting 2 means a core that only needs GLES 2 still runs on a day when 3 is unavailable,
    * instead of the whole video driver failing to initialise. */
   static const EGLint attribs_es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   static const EGLint attribs_es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
#endif

   if (!ctx)
      return false;

#ifdef HAVE_OPENGLES
   if (egl_create_context(&ctx->egl, attribs_es3))
      RARCH_LOG("[PS4] GLES 3 context created.\n");
   else if (egl_create_context(&ctx->egl, attribs_es2))
      RARCH_WARN("[PS4] GLES 3 refused; running on a GLES 2 context. A core that needs 3 will "
                 "say so in its own words.\n");
   else
   {
      egl_report_error();
      RARCH_ERR("[PS4] no GLES context could be created.\n");
      goto error;
   }
#else
   if (!orbis_gl_create_desktop(ctx))
   {
      egl_report_error();
      RARCH_ERR("[PS4] no desktop GL context could be created - every rung from %u.%u down to "
                "2.1 was refused.\n", orbis_gl_req_major, orbis_gl_req_minor);
      goto error;
   }
#endif

   /* ⚠ ANY NON-ZERO TOKEN, AND IT MUST NOT BE NULL. A "window" here is the scan-out itself and
    * there is no handle to obtain - sceVideoOutOpen happens below us, inside the swapchain. But
    * _eglCreateWindowSurfaceCommon rejects NULL with EGL_BAD_NATIVE_WINDOW before the platform is
    * ever consulted, so the contract mesa's orbis platform documents is to pass 1. The core's
    * own duplicate check then enforces one window surface per token, which is the behaviour we
    * would otherwise have had to write: there is exactly one screen. */
   if (!egl_create_surface(&ctx->egl, (void*)(uintptr_t)1))
   {
      egl_report_error();
      RARCH_ERR("[PS4] EGL would not give us a window surface.\n");
      goto error;
   }

#ifndef HAVE_OPENGLES
   /* ⚠ ONLY MEANINGFUL AFTER THE SURFACE, because egl_create_surface is what calls
    * eglMakeCurrent - there is no current context before this point and glGetString would
    * return NULL. This is the line that says whether the desktop variant is doing what it
    * claims: "3.3 (Core Profile) Mesa ..." is the success case. */
   {
      const GLubyte *ver  = glGetString(GL_VERSION);
      const GLubyte *slv  = glGetString(GL_SHADING_LANGUAGE_VERSION);
      RARCH_LOG("[PS4] GL context is: %s | GLSL %s\n",
            ver ? (const char*)ver : "(null)",
            slv ? (const char*)slv : "(null)");
   }
#endif

   return true;

error:
   gfx_ctx_orbis_gl_destroy(data);
   return false;
}

static void gfx_ctx_orbis_gl_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;

   /* The scan-out cannot change under us: there is no window manager, no rotation and no mode
    * set. So this reports the one size it was given and never asks for a resize. */
   *width  = ctx ? ctx->width  : ORBIS_GL_WIDTH;
   *height = ctx ? ctx->height : ORBIS_GL_HEIGHT;
   *resize = false;
   *quit   = false;
}

static bool gfx_ctx_orbis_gl_set_resize(void *data,
      unsigned width, unsigned height)
{
   return false;
}

static void gfx_ctx_orbis_gl_set_swap_interval(void *data, int swap_interval)
{
   /* ⚠ SAID NOTHING, BECAUSE MESA ALREADY SAYS IT. The flip is submitted with
    * ORBIS_VIDEO_OUT_FLIP_VSYNC and the rate is pinned by sceVideoOutSetFlipRate, so min = max =
    * default = 1 on this platform and the EGL core clamps the request before it reaches the
    * driver. mesa's orbis platform logs the refusal once. Repeating it here would be two lines
    * about one thing that cannot be changed. */
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;
   if (ctx)
      egl_set_swap_interval(&ctx->egl, 1);
}

static void gfx_ctx_orbis_gl_swap_buffers(void *data)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;
   if (ctx)
      egl_swap_buffers(&ctx->egl);
}

static void gfx_ctx_orbis_gl_input_driver(void *data,
      const char *name, input_driver_t **input, void **input_data)
{
   /* The pad is the frontend's, opened in platform_orbis.c. Nothing about it belongs to the
    * graphics context here, exactly as in gfx_ctx_orbis_vk. */
   *input      = NULL;
   *input_data = NULL;
}

static enum gfx_ctx_api gfx_ctx_orbis_gl_get_api(void *data)
{
#ifdef HAVE_OPENGLES
   return GFX_CTX_OPENGL_ES_API;
#else
   return GFX_CTX_OPENGL_API;
#endif
}

static bool gfx_ctx_orbis_gl_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   /* ⚠ THE ES-ONLY CLAIM THAT STOOD HERE WAS WRONG, and the correction is worth stating rather
    * than deleting. It read: mesa-ps4 is built with glvnd disabled and no GLX, therefore it
    * produces libGLESv2.a and no libGL, therefore "desktop GL has no entry points to call here".
    * The first two clauses are true and the conclusion does not follow.
    *
    * There is no libGL.a, but there is no need for one. Every GL entry point in this Mesa - ES
    * and desktop alike - is a stub that jumps through the mapi dispatch table, and that table is
    * API-AGNOSTIC: disassembling glClear out of the orbis libGLESv2.a gives `jmp *0x658(%rax)`,
    * slot 203, which is _gloffset_Clear, the same slot desktop GL uses. The desktop stubs are
    * present too - the orbis build's generated shared_glapi_mapi_tmp.h carries 2312 of them,
    * covering the full desktop API, inside libgallium-26.3.0-devel.a, which this eboot already
    * links. They are simply not EXPORTED as link-time symbols, so they are reached through
    * eglGetProcAddress, which is exactly how RetroArch's glsym layer reaches everything anyway.
    *
    * And the context itself is creatable: `_eglIsApiValid(EGL_OPENGL_API)` is true here,
    * egl_dri2.c sets EGL_OPENGL_BIT from the screen's api_mask, and glcaps got a real
    * 3.3 core-profile context on hardware. What decides which of the two this binary speaks is
    * HAVE_OPENGLES, for the reasons in the note at the top of this file. */
#ifdef HAVE_OPENGLES
   if (api != GFX_CTX_OPENGL_ES_API)
      return false;
   return egl_bind_api(EGL_OPENGL_ES_API);
#else
   if (api != GFX_CTX_OPENGL_API)
      return false;
   /* Remembered for set_video_mode, which is where the context is actually asked for. */
   orbis_gl_req_major = major;
   orbis_gl_req_minor = minor;
   return egl_bind_api(EGL_OPENGL_API);
#endif
}

static bool gfx_ctx_orbis_gl_has_focus(void *data) { return true; }

static bool gfx_ctx_orbis_gl_suppress_screensaver(void *data, bool enable)
{
   return false;
}

static void gfx_ctx_orbis_gl_set_flags(void *data, uint32_t flags)
{
#ifndef HAVE_OPENGLES
   /* ⚠ THIS WAS AN EMPTY STUB AND THE FRONTEND WAS ALREADY TALKING INTO IT. gl3.c and
    * runloop.c both push GFX_CTX_FLAGS_GL_CORE_CONTEXT here when a core asks for
    * RETRO_HW_CONTEXT_OPENGL_CORE (runloop.c, RETRO_ENVIRONMENT_SET_HW_RENDER), and every one
    * of those pushes was being discarded silently.
    *
    * ⚠ IT IS STILL NOT THE PRIMARY SIGNAL, AND MUST NOT BE TREATED AS ONE.
    * video_context_driver_set_flags() only reaches a context driver that is already installed;
    * both of the callers above run BEFORE this driver is chosen, so the flag is stashed in
    * video_st->deferred_flag_data and arrives here late, if at all. The version handed to
    * bind_api is what actually selects the profile. This exists so that a late arrival is
    * honoured on the next context creation instead of being thrown away. */
   if (BIT32_GET(flags, GFX_CTX_FLAGS_GL_CORE_CONTEXT))
      orbis_gl_core_ctx = true;
#endif
}

static uint32_t gfx_ctx_orbis_gl_get_flags(void *data)
{
   uint32_t flags = 0;

   /* ⚠ THIS IS WHERE gl2 LEARNS THAT SHADERS EXIST, AND SAYING NOTHING IS NOT NEUTRAL.
    * gl2_get_fallback_shader_type() asks the CONTEXT driver which shader languages are
    * available - "for gl2, shader support is completely defined by the context driver shader
    * flags" - and returns RARCH_SHADER_NONE when the answer is empty. gl2_shader_init() then
    * logs "Couldn't find any supported shader backend! Continuing without shaders" and returns
    * TRUE, so the driver comes up fully initialised with gl->shader NULL and draws nothing.
    *
    * Observed on hardware 2026-08-25: EGL up, GLES 3.1 on zink, HW render initialised, FBO
    * supported, 133 GL entry points resolved, GoldHEN's counter showing a steady 60 fps - and a
    * black screen with no menu, because a frame was being presented with no program to draw it
    * with. The one flag below is the whole difference. */
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_GLSL);

#if !defined(HAVE_OPENGLES) && defined(HAVE_SLANG)
   /* ⚠ AND glcore ASKS A DIFFERENT QUESTION THAN gl2 DOES. gl3.c's filter chain is the slang
    * one - gl3_filter_chain_* in gfx/drivers_shader/shader_gl3.cpp, with spirv_opengl.c under
    * it - and gl3_get_fallback_shader_type() prefers RARCH_SHADER_SLANG, falling back to GLSL
    * only when the context driver does not advertise it. Both are linked in this build (slang
    * arrives with the Vulkan arm, which desktop GL requires anyway for zink), so both are
    * declared and gl3 gets its native one. */
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_SLANG);
#endif

   return flags;
}

static gfx_ctx_proc_t gfx_ctx_orbis_gl_get_proc_address(const char *symbol)
{
   return egl_get_proc_address(symbol);
}

static void gfx_ctx_orbis_gl_bind_hw_render(void *data, bool enable)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;
   if (ctx)
      egl_bind_hw_render(&ctx->egl, enable);
}

const gfx_ctx_driver_t gfx_ctx_orbis_gl = {
   gfx_ctx_orbis_gl_init,
   gfx_ctx_orbis_gl_destroy,
   gfx_ctx_orbis_gl_get_api,
   gfx_ctx_orbis_gl_bind_api,
   gfx_ctx_orbis_gl_set_swap_interval,
   gfx_ctx_orbis_gl_set_video_mode,
   gfx_ctx_orbis_gl_get_video_size,
   NULL,                                    /* get_refresh_rate */
   NULL,                                    /* get_video_output_size */
   NULL,                                    /* get_video_output_prev */
   NULL,                                    /* get_video_output_next */
   NULL,                                    /* get_metrics */
   NULL,
   NULL,                                    /* update_title */
   gfx_ctx_orbis_gl_check_window,
   gfx_ctx_orbis_gl_set_resize,
   gfx_ctx_orbis_gl_has_focus,
   gfx_ctx_orbis_gl_suppress_screensaver,
   false,                                   /* has_windowed */
   gfx_ctx_orbis_gl_swap_buffers,
   gfx_ctx_orbis_gl_input_driver,
   gfx_ctx_orbis_gl_get_proc_address,
   NULL,
   NULL,
   NULL,
   "orbis_gl",
   gfx_ctx_orbis_gl_get_flags,
   gfx_ctx_orbis_gl_set_flags,
   gfx_ctx_orbis_gl_bind_hw_render,
   NULL,                                    /* get_context_data */
   NULL,                                    /* make_current */
   NULL,                                    /* create_surface */
   NULL                                     /* destroy_surface */
};
