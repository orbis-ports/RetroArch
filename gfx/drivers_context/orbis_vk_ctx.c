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

/* Vulkan on the PlayStation 4, through Mesa's RADV linked in as a static ICD.
 *
 * ⚠ THIS REPLACES gfx/drivers_context/orbis_ctx.c, WHICH WAS NOT A VULKAN DRIVER AT ALL.
 * That file drove Sony's GLES2 out of the VSH process through libScePigletv2VSH; it is
 * deleted, and nothing here is descended from it.
 *
 * ⚠ AND THE SURFACE IS "HEADLESS", WHICH IS NOT A STATEMENT ABOUT THE TELEVISION.
 * VK_EXT_headless_surface is the surface a driver offers when the platform's scan-out is
 * not any WSI Vulkan knows about. This console has no DRM, no dma-buf and no compositor, so
 * none of Mesa's WSI platforms describes it - what it has is sceVideoOut, and RADV's own
 * Orbis arm drives that behind the swapchain: it opens video-out, registers the scan-out
 * buffers and flips. See mesa-ps4's src/vulkan/wsi/wsi_orbis.c. From this side it is an
 * ordinary VkSwapchainKHR, which is exactly why the extension was chosen over inventing a
 * surface type and touching Mesa's XML and generated entry points to get one.
 */

#include <stdint.h>

#include <retro_miscellaneous.h>
#include <retro_timers.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include "../../frontend/frontend_driver.h"
#include "../common/vulkan_common.h"
#include "../../verbosity.h"
#include "../../configuration.h"

/* The console's main video-out mode, and the size RADV's WSI arm registers its buffers at.
 * There is no mode set here: the display is whatever the system gave the process. */
#define ORBIS_VK_WIDTH  1920
#define ORBIS_VK_HEIGHT 1080

typedef struct
{
   gfx_ctx_vulkan_data_t vk;
   int      swap_interval;
   unsigned width;
   unsigned height;
} orbis_vk_ctx_data_t;

static void gfx_ctx_orbis_vk_destroy(void *data)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   if (!ctx)
      return;

   vulkan_context_destroy(&ctx->vk, true);
#ifdef HAVE_THREADS
   if (ctx->vk.context.queue_lock)
      slock_free(ctx->vk.context.queue_lock);
#endif
   free(ctx);
}

static void *gfx_ctx_orbis_vk_init(void *video_driver)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)calloc(1, sizeof(*ctx));

   if (!ctx)
      return NULL;

   /* ⚠ NO dylib_load HERE OR ANYWHERE. vulkan_context_init() takes the loader path that
    * vulkan_common.c chose, and on this platform that is the static ICD reached through
    * orbis-compat's vkloader rather than a libvulkan.so that does not exist. */
   if (!vulkan_context_init(&ctx->vk, VULKAN_WSI_HEADLESS))
   {
      RARCH_ERR("[PS4] Could not create a Vulkan instance against RADV.\n");
      free(ctx);
      return NULL;
   }

   ctx->width  = ORBIS_VK_WIDTH;
   ctx->height = ORBIS_VK_HEIGHT;

   return ctx;
}

static void gfx_ctx_orbis_vk_get_video_size(void *data,
      unsigned *width, unsigned *height)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   *width  = ctx ? ctx->width  : ORBIS_VK_WIDTH;
   *height = ctx ? ctx->height : ORBIS_VK_HEIGHT;
}

static bool gfx_ctx_orbis_vk_set_video_mode(void *data,
      unsigned width, unsigned height, bool fullscreen)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   /* There is no windowed mode and no mode set: the size is the console's, and asking for
    * anything else would be a request nothing here can honour. */
   if (!vulkan_surface_create(&ctx->vk, VULKAN_WSI_HEADLESS, NULL, NULL,
            ORBIS_VK_WIDTH, ORBIS_VK_HEIGHT, ctx->swap_interval))
   {
      RARCH_ERR("[PS4] Could not create a headless surface or its swapchain.\n");
      gfx_ctx_orbis_vk_destroy(data);
      return false;
   }

   ctx->width  = ctx->vk.context.swapchain_width;
   ctx->height = ctx->vk.context.swapchain_height;

   RARCH_LOG("[PS4] Vulkan up: %ux%u swapchain on RADV.\n", ctx->width, ctx->height);
   return true;
}

static void gfx_ctx_orbis_vk_check_window(void *data, bool *quit,
      bool *resize, unsigned *width, unsigned *height)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   *quit   = false;
   *resize = false;

   if (!ctx)
      return;

   /* The only thing that changes the swapchain here is the driver deciding it is out of
    * date; there is no window to be resized or closed. */
   if (ctx->vk.flags & VK_DATA_FLAG_NEED_NEW_SWAPCHAIN)
   {
      *resize = true;
      *width  = ctx->width;
      *height = ctx->height;
   }
}

static bool gfx_ctx_orbis_vk_set_resize(void *data,
      unsigned width, unsigned height)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   if (!ctx)
      return false;

   if (!vulkan_create_swapchain(&ctx->vk, width, height, ctx->swap_interval))
   {
      RARCH_ERR("[PS4] Could not rebuild the swapchain at %ux%u.\n", width, height);
      return false;
   }

   ctx->vk.context.flags |= VK_CTX_FLAG_INVALID_SWAPCHAIN;
   ctx->vk.flags         &= ~VK_DATA_FLAG_NEED_NEW_SWAPCHAIN;
   return true;
}

static void gfx_ctx_orbis_vk_swap_buffers(void *data)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   if (ctx->vk.context.flags & VK_CTX_FLAG_HAS_ACQUIRED_SWAPCHAIN)
   {
      ctx->vk.context.flags &= ~VK_CTX_FLAG_HAS_ACQUIRED_SWAPCHAIN;

      /* A swapchain that went away mid-frame is not an error to report every frame; the
       * driver rebuilds it and the next acquire picks it up. Sleeping keeps a torn-down
       * swapchain from spinning the run loop. */
      if (ctx->vk.swapchain == VK_NULL_HANDLE)
         retro_sleep(10);
      else
         vulkan_present(&ctx->vk, ctx->vk.context.current_swapchain_index);
   }
   vulkan_acquire_next_image(&ctx->vk);
}

static void gfx_ctx_orbis_vk_set_swap_interval(void *data, int swap_interval)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;

   if (ctx->swap_interval == swap_interval)
      return;

   ctx->swap_interval = swap_interval;
   if (ctx->vk.swapchain)
      ctx->vk.flags |= VK_DATA_FLAG_NEED_NEW_SWAPCHAIN;
}

static void gfx_ctx_orbis_vk_input_driver(void *data,
      const char *name, input_driver_t **input, void **input_data)
{
   /* The pad is the joypad driver's, not the context's. */
   *input      = NULL;
   *input_data = NULL;
}

static enum gfx_ctx_api gfx_ctx_orbis_vk_get_api(void *data)
{
   return GFX_CTX_VULKAN_API;
}

static bool gfx_ctx_orbis_vk_bind_api(void *data,
      enum gfx_ctx_api api, unsigned major, unsigned minor)
{
   /* Vulkan and nothing else: the GL path on this console was Piglet's and is gone. */
   return api == GFX_CTX_VULKAN_API;
}

static bool gfx_ctx_orbis_vk_has_focus(void *data)  { return true; }
static bool gfx_ctx_orbis_vk_suppress_screensaver(void *data, bool enable)
{
   return false;
}
static void gfx_ctx_orbis_vk_set_flags(void *data, uint32_t flags) { }

static gfx_ctx_proc_t gfx_ctx_orbis_vk_get_proc_address(const char *symbol)
{
   return NULL;
}

static uint32_t gfx_ctx_orbis_vk_get_flags(void *data)
{
   uint32_t flags = 0;
   BIT32_SET(flags, GFX_CTX_FLAGS_SHADERS_SLANG);
   return flags;
}

static void *gfx_ctx_orbis_vk_get_context_data(void *data)
{
   orbis_vk_ctx_data_t *ctx = (orbis_vk_ctx_data_t*)data;
   return &ctx->vk.context;
}

const gfx_ctx_driver_t gfx_ctx_orbis_vk = {
   gfx_ctx_orbis_vk_init,
   gfx_ctx_orbis_vk_destroy,
   gfx_ctx_orbis_vk_get_api,
   gfx_ctx_orbis_vk_bind_api,
   gfx_ctx_orbis_vk_set_swap_interval,
   gfx_ctx_orbis_vk_set_video_mode,
   gfx_ctx_orbis_vk_get_video_size,
   NULL,                                    /* get_refresh_rate */
   NULL,                                    /* get_video_output_size */
   NULL,                                    /* get_video_output_prev */
   NULL,                                    /* get_video_output_next */
   NULL,                                    /* get_metrics */
   NULL,
   NULL,                                    /* update_title */
   gfx_ctx_orbis_vk_check_window,
   gfx_ctx_orbis_vk_set_resize,
   gfx_ctx_orbis_vk_has_focus,
   gfx_ctx_orbis_vk_suppress_screensaver,
   false,                                   /* has_windowed */
   gfx_ctx_orbis_vk_swap_buffers,
   gfx_ctx_orbis_vk_input_driver,
   gfx_ctx_orbis_vk_get_proc_address,
   NULL,
   NULL,
   NULL,
   "orbis_vk",
   gfx_ctx_orbis_vk_get_flags,
   gfx_ctx_orbis_vk_set_flags,
   NULL,
   gfx_ctx_orbis_vk_get_context_data,
   NULL,                                    /* make_current */
   NULL,                                    /* create_surface */
   NULL                                     /* destroy_surface */
};
