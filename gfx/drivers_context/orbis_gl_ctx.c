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

/* OpenGL ES on the PlayStation 4, through Mesa's EGL and zink.
 *
 * ⚠ WHAT THIS UNBLOCKS, BECAUSE IT IS NOT "OPENGL CORES NOW WORK". The reason this file was
 * written is Nintendo 64. mupen64plus-next's fast renderer is GLideN64 driven by the HLE RSP,
 * and without a GL context the only pairing left is ParaLLEl-RDP with the LLE RSP - measured on
 * hardware at 22-25 fps, of which the LLE RSP alone is 24 ms against a 16.7 ms budget (see
 * ps4/HANDOFF.md). No amount of tuning reaches full speed from there; a different renderer does.
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
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
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

static bool gfx_ctx_orbis_gl_set_video_mode(void *data,
      unsigned width, unsigned height, bool fullscreen)
{
   orbis_gl_ctx_data_t *ctx = (orbis_gl_ctx_data_t*)data;

   /* ⚠ THREE, THEN TWO, AND THE FALLBACK IS NOT DECORATION. GLideN64 wants GLES 3.1 for its full
    * path and has a reduced GLES 2 one; zink's ceiling here depends on what RADV reports for this
    * GPU, which is a question about the driver rather than about the console. Asking for 3 and
    * accepting 2 means a core that only needs GLES 2 still runs on a day when 3 is unavailable,
    * instead of the whole video driver failing to initialise. */
   static const EGLint attribs_es3[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   static const EGLint attribs_es2[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

   if (!ctx)
      return false;

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
   return GFX_CTX_OPENGL_ES_API;
}

static bool gfx_ctx_orbis_gl_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   /* ⚠ ES ONLY, AND THAT IS A FACT ABOUT THE BUILD RATHER THAN A CHOICE. mesa-ps4 is configured
    * with glvnd disabled and no GLX, so the build produces libGLESv2.a and libGLESv1_CM.a over
    * shared-glapi and no libGL at all. Desktop GL has no entry points to call here whatever zink
    * is capable of underneath. */
   if (api != GFX_CTX_OPENGL_ES_API)
      return false;
   return egl_bind_api(EGL_OPENGL_ES_API);
}

static bool gfx_ctx_orbis_gl_has_focus(void *data) { return true; }

static bool gfx_ctx_orbis_gl_suppress_screensaver(void *data, bool enable)
{
   return false;
}

static void gfx_ctx_orbis_gl_set_flags(void *data, uint32_t flags) { }

static uint32_t gfx_ctx_orbis_gl_get_flags(void *data)
{
   uint32_t flags = 0;
   BIT32_SET(flags, GFX_CTX_FLAGS_NONE);
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
