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

/* The PlayStation 4's software video driver.
 *
 * The CPU draws every pixel: the core's frame is scaled and converted straight into a
 * scan-out buffer and flipped. There is no context driver, no shader stage and no GPU
 * involved - Phase 7 of ps4/PLAN.md replaces this with the RADV port, and this driver is
 * what proves everything ELSE about the port before that lands.
 *
 * ⚠ NO SWIZZLE PASS, AND THAT IS NOT AN OVERSIGHT. Video-out's only pixel format,
 * A8B8G8R8_SRGB, is bytes R,G,B,A low to high - which is RetroArch's own
 * SCALER_FMT_ABGR8888 exactly. The frontend's scaler converts into it directly
 * (conv_argb8888_abgr8888 / conv_rgb565_abgr8888), so a 1080p frame is scaled and
 * converted in one pass instead of two.
 *
 * ⚠ ALPHA IS LEFT AS THE SCALER WROTE IT. ps4doom ORed 0xFF000000 into every pixel on the
 * way to the same buffer; whether scan-out actually reads the alpha of the primary plane
 * was never established there. Forcing it would cost a full 2-megapixel pass per frame for
 * a maybe. If the picture comes up black or translucent on hardware, this is the first
 * thing to try.
 */

#include <stdlib.h>
#include <string.h>

#include <gfx/scaler/scaler.h>
#include <retro_miscellaneous.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../font_driver.h"
#include "../video_driver.h"
#include "../../driver.h"
#include "../../verbosity.h"

#include "../../ps4/ps4_video_out.h"

/* 1080p, because that is what the console's main video-out mode is and letting the
 * frontend pick something else would mean a mode set this driver does not do yet.
 *
 * ⚠ THIS IS THE PORT'S LARGEST UNMEASURED COST. Every frame is a scale into 2.07 million
 * pixels on a Jaguar core. If 60 Hz does not hold, 1280x720 halves the fill and is the
 * first knob to turn - ps4/PLAN.md Phase 3 says to settle this by measurement, not by
 * choosing up front. */
#define PS4_GFX_WIDTH   1920
#define PS4_GFX_HEIGHT  1080

/* Two is enough to never draw into the buffer being scanned out. A third only helps a
 * driver that runs ahead of the display, which this one does not. */
#define PS4_GFX_BUFFERS 2

typedef struct ps4_video
{
   ps4_video_out_t   *vo;

   struct scaler_ctx  frame_scaler;
   struct scaler_ctx  menu_scaler;

   /* The menu's own framebuffer, kept because set_texture_frame and frame() are separate
    * calls and RGUI hands the pixels over on its own schedule. */
   uint16_t          *menu_frame;
   size_t             menu_frame_len;   /* in pixels */
   unsigned           menu_width;
   unsigned           menu_height;

   struct video_viewport vp;

   unsigned           last_width;
   unsigned           last_height;

   bool               rgb32;
   bool               vsync;
   bool               menu_active;
   bool               keep_aspect;
   bool               smooth;
   bool               clear_pending;
} ps4_video_t;

/* Where a source of in_w x in_h lands inside the display, letter- or pillarboxed. The
 * frame is centred; the parts of the screen it does not cover are cleared by
 * ps4_video_out_clear() when the geometry changes, not every frame. */
static void ps4_gfx_fit(ps4_video_t *ps4, unsigned in_w, unsigned in_h,
      unsigned *out_x, unsigned *out_y, unsigned *out_w, unsigned *out_h)
{
   unsigned disp_w = 0, disp_h = 0;

   ps4_video_out_size(ps4->vo, &disp_w, &disp_h);

   if (!ps4->keep_aspect || in_w == 0 || in_h == 0)
   {
      *out_x = *out_y = 0;
      *out_w = disp_w;
      *out_h = disp_h;
      return;
   }

   {
      float src_aspect = (float)in_w  / (float)in_h;
      float dsp_aspect = (float)disp_w / (float)disp_h;
      unsigned w, h;

      if (src_aspect > dsp_aspect)
      {
         w = disp_w;
         h = (unsigned)((float)disp_w / src_aspect + 0.5f);
      }
      else
      {
         h = disp_h;
         w = (unsigned)((float)disp_h * src_aspect + 0.5f);
      }

      if (w > disp_w)
         w = disp_w;
      if (h > disp_h)
         h = disp_h;

      *out_w = w;
      *out_h = h;
      *out_x = (disp_w - w) / 2;
      *out_y = (disp_h - h) / 2;
   }
}

static void *ps4_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data)
{
   ps4_video_t *ps4 = (ps4_video_t*)calloc(1, sizeof(*ps4));

   if (!ps4)
      return NULL;

   ps4->rgb32       = video->rgb32;
   ps4->vsync       = video->vsync;
   ps4->smooth      = video->smooth;
   ps4->keep_aspect = true;

   if (!(ps4->vo = ps4_video_out_open(PS4_GFX_WIDTH, PS4_GFX_HEIGHT, PS4_GFX_BUFFERS)))
   {
      RARCH_ERR("[PS4] Could not open video-out; the frontend has no display.\n");
      free(ps4);
      return NULL;
   }

   ps4->vp.x           = 0;
   ps4->vp.y           = 0;
   ps4->vp.width       = PS4_GFX_WIDTH;
   ps4->vp.height      = PS4_GFX_HEIGHT;
   ps4->vp.full_width  = PS4_GFX_WIDTH;
   ps4->vp.full_height = PS4_GFX_HEIGHT;

   /* This driver is not an input driver. Saying so explicitly, because a caller that is
    * handed a stale pointer here binds input to the video driver by accident. */
   if (input)
      *input      = NULL;
   if (input_data)
      *input_data = NULL;

   RARCH_LOG("[PS4] Software video driver up: %ux%u, %s core frames.\n",
         PS4_GFX_WIDTH, PS4_GFX_HEIGHT, ps4->rgb32 ? "XRGB8888" : "RGB565");
   return ps4;
}

static bool ps4_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;
   uint32_t    *fb;
   unsigned     dst_pitch_px;
   bool         drew = false;

   if (!ps4)
      return false;

   if (!(fb = ps4_video_out_backbuffer(ps4->vo)))
      return false;

   dst_pitch_px = ps4_video_out_pitch_px(ps4->vo);

   /* A NULL frame is RetroArch saying "the core produced nothing new". The previous
    * picture is still in the other buffer, not this one, so there is nothing to preserve
    * and nothing to redraw - present what is here and keep the display's cadence. */
   if (frame && width && height)
   {
      unsigned x, y, w, h;

      ps4_gfx_fit(ps4, width, height, &x, &y, &w, &h);

      if (     width  != ps4->last_width
            || height != ps4->last_height
            || (unsigned)ps4->frame_scaler.out_width  != w
            || (unsigned)ps4->frame_scaler.out_height != h)
      {
         scaler_ctx_gen_reset(&ps4->frame_scaler);

         ps4->frame_scaler.in_width    = width;
         ps4->frame_scaler.in_height   = height;
         ps4->frame_scaler.in_stride   = (int)pitch;
         ps4->frame_scaler.in_fmt      = ps4->rgb32
            ? SCALER_FMT_ARGB8888
            : SCALER_FMT_RGB565;

         ps4->frame_scaler.out_width   = w;
         ps4->frame_scaler.out_height  = h;
         ps4->frame_scaler.out_stride  = (int)(dst_pitch_px * sizeof(uint32_t));
         ps4->frame_scaler.out_fmt     = SCALER_FMT_ABGR8888;

         ps4->frame_scaler.scaler_type = ps4->smooth
            ? SCALER_TYPE_BILINEAR
            : SCALER_TYPE_POINT;

         if (!scaler_ctx_gen_filter(&ps4->frame_scaler))
         {
            RARCH_ERR("[PS4] Could not build a %ux%u -> %ux%u scaler.\n",
                  width, height, w, h);
            return false;
         }

         ps4->last_width  = width;
         ps4->last_height = height;

         /* The geometry moved, so the bands the new picture does not cover still hold the
          * old one. */
         ps4_video_out_clear(ps4->vo);

         ps4->vp.x      = x;
         ps4->vp.y      = y;
         ps4->vp.width  = w;
         ps4->vp.height = h;
      }

      scaler_ctx_scale(&ps4->frame_scaler,
            fb + (size_t)y * dst_pitch_px + x, frame);
      drew = true;
   }

#ifdef HAVE_MENU
   if (ps4->menu_active && ps4->menu_frame)
   {
      unsigned x, y, w, h;

      ps4_gfx_fit(ps4, ps4->menu_width, ps4->menu_height, &x, &y, &w, &h);

      if (     (unsigned)ps4->menu_scaler.in_width   != ps4->menu_width
            || (unsigned)ps4->menu_scaler.in_height  != ps4->menu_height
            || (unsigned)ps4->menu_scaler.out_width  != w
            || (unsigned)ps4->menu_scaler.out_height != h)
      {
         scaler_ctx_gen_reset(&ps4->menu_scaler);

         ps4->menu_scaler.in_width    = ps4->menu_width;
         ps4->menu_scaler.in_height   = ps4->menu_height;
         ps4->menu_scaler.in_stride   = (int)(ps4->menu_width * sizeof(uint16_t));

         /* RGUI hands over 16bpp in whatever the platform's format is, chosen by
          * rgui_pixel_format_map[] keyed on this driver's ident. "ps4" is not in that
          * table, so it takes the default - RGBA4444, with transparency supported. Adding
          * a table entry would be the way to change it; not being there is a decision. */
         ps4->menu_scaler.in_fmt      = SCALER_FMT_RGBA4444;

         ps4->menu_scaler.out_width   = w;
         ps4->menu_scaler.out_height  = h;
         ps4->menu_scaler.out_stride  = (int)(dst_pitch_px * sizeof(uint32_t));
         ps4->menu_scaler.out_fmt     = SCALER_FMT_ABGR8888;
         ps4->menu_scaler.scaler_type = SCALER_TYPE_POINT;

         if (!scaler_ctx_gen_filter(&ps4->menu_scaler))
         {
            RARCH_ERR("[PS4] Could not build a menu scaler.\n");
            return false;
         }

         ps4_video_out_clear(ps4->vo);
      }

      /* ⚠ THE MENU REPLACES THE FRAME, IT DOES NOT BLEND OVER IT. The scaler converts, it
       * does not composite, and RGUI's own transparency is a property of the pixels it
       * hands over rather than something a destination can honour here. RGUI's background
       * is opaque by default, so this is what a user sees either way; a menu drawn over a
       * running game needs a blend pass this driver does not have yet. */
      scaler_ctx_scale(&ps4->menu_scaler,
            fb + (size_t)y * dst_pitch_px + x, ps4->menu_frame);
      drew = true;
   }
#endif

   /* ⚠ NO ON-SCREEN MESSAGES. This driver has no font backend: RGUI draws its own bitmap
    * font into its own framebuffer, which is why the menu works, but `msg` has nowhere to
    * go. Every notification the frontend raises is invisible here and reaches the log
    * instead. A font driver is Phase 7's problem, with the rest of the widgets. */
   if (msg && *msg)
      RARCH_LOG("[PS4] %s\n", msg);

   if (!drew)
   {
      /* Nothing was written into this buffer. Presenting it would show whatever it held
       * two flips ago - a visible stutter back to an older picture - so leave what is on
       * screen alone. Pacing survives: with vsync on, the flip submitted for the LAST
       * frame already blocked until the display took it, so a duplicate frame costs a
       * display interval whether or not it flips. */
      return true;
   }

   ps4_video_out_flip(ps4->vo, ps4->vsync);
   return true;
}

static void ps4_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   /* `toggle` is "run without blocking", i.e. the inverse of vsync. All this driver can do
    * with it is stop waiting for the flip to be taken; the display still shows frames at
    * its own rate, so fast-forward gets faster without tearing. */
   if (ps4)
      ps4->vsync = !toggle;
}

static bool ps4_gfx_alive(void *data)
{
   /* There is no window to close and no way for the display to go away under us. */
   return true;
}

static bool ps4_gfx_focus(void *data)          { return true;  }
static bool ps4_gfx_suppress_screensaver(void *data, bool enable) { return false; }
static bool ps4_gfx_has_windowed(void *data)   { return false; }

static bool ps4_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path)
{
   /* Software driver: there is no shader stage to load into. Phase 7. */
   return false;
}

static void ps4_gfx_free(void *data)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   if (!ps4)
      return;

   scaler_ctx_gen_reset(&ps4->frame_scaler);
   scaler_ctx_gen_reset(&ps4->menu_scaler);

   if (ps4->menu_frame)
      free(ps4->menu_frame);

   ps4_video_out_close(ps4->vo);
   free(ps4);
}

static void ps4_gfx_set_rotation(void *data, unsigned rotation)
{
   /* Rotation would mean a transform the scaler does not do. Silently ignoring it would
    * leave a user wondering; say so once and carry on unrotated. */
   if (rotation)
      RARCH_WARN("[PS4] Rotation %u requested; this driver only draws upright.\n",
            rotation);
}

static void ps4_gfx_viewport_info(void *data, struct video_viewport *vp)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   if (ps4 && vp)
      *vp = ps4->vp;
}

static uint32_t ps4_gfx_get_flags(void *data)
{
   return 0;
}

static void ps4_gfx_set_filtering(void *data, unsigned index, bool smooth,
      bool ctx_scaling)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   if (!ps4 || ps4->smooth == smooth)
      return;

   ps4->smooth     = smooth;
   /* Force the scaler to be rebuilt on the next frame: the filter kernel is chosen when it
    * is generated, not when it runs. */
   ps4->last_width = 0;
}

static void ps4_gfx_set_aspect_ratio(void *data, unsigned aspect_ratio_idx)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   if (ps4)
      ps4->last_width = 0;
}

static void ps4_gfx_apply_state_changes(void *data)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   if (ps4)
      ps4->last_width = 0;
}

#ifdef HAVE_MENU
static void ps4_gfx_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;
   size_t       len;

   if (!ps4 || !frame)
      return;

   /* ⚠ 16bpp ONLY, AND THE CALLER DECIDES. RGUI always hands over 16bpp in the platform
    * format; rgb32 is true only for the menu drivers this build does not have (XMB,
    * Ozone, MaterialUI - all off, they want a GPU). Refusing loudly beats reading a
    * 32bpp buffer as 16bpp and showing half a menu. */
   if (rgb32)
   {
      RARCH_ERR("[PS4] 32bpp menu texture from a driver this build should not have.\n");
      return;
   }

   len = (size_t)width * height;

   if (len != ps4->menu_frame_len)
   {
      uint16_t *buf = (uint16_t*)realloc(ps4->menu_frame, len * sizeof(uint16_t));
      if (!buf)
         return;
      ps4->menu_frame     = buf;
      ps4->menu_frame_len = len;
   }

   ps4->menu_width  = width;
   ps4->menu_height = height;
   memcpy(ps4->menu_frame, frame, len * sizeof(uint16_t));
}

static void ps4_gfx_set_texture_enable(void *data, bool state, bool full_screen)
{
   ps4_video_t *ps4 = (ps4_video_t*)data;

   if (!ps4 || ps4->menu_active == state)
      return;

   ps4->menu_active = state;

   /* Leaving the menu uncovers whatever the core last drew at a different geometry. */
   ps4_video_out_clear(ps4->vo);
}
#endif

static const video_poke_interface_t ps4_poke_interface = {
   ps4_gfx_get_flags,
   NULL,                            /* load_texture */
   NULL,                            /* unload_texture */
   NULL,                            /* set_video_mode */
   NULL,                            /* get_refresh_rate */
   ps4_gfx_set_filtering,
   NULL,                            /* get_video_output_size */
   NULL,                            /* get_video_output_prev */
   NULL,                            /* get_video_output_next */
   NULL,                            /* get_current_framebuffer */
   NULL,                            /* get_proc_address */
   ps4_gfx_set_aspect_ratio,
   ps4_gfx_apply_state_changes,
#ifdef HAVE_MENU
   ps4_gfx_set_texture_frame,
   ps4_gfx_set_texture_enable,
#else
   NULL,
   NULL,
#endif
   NULL,                            /* set_osd_msg */
   NULL,                            /* show_mouse */
   NULL,                            /* grab_mouse_toggle */
   NULL,                            /* get_current_shader */
   NULL,                            /* get_current_software_framebuffer */
   NULL,                            /* get_hw_render_interface */
   NULL,                            /* set_hdr_menu_nits */
   NULL,                            /* set_hdr_paper_white_nits */
   NULL,                            /* set_hdr_expand_gamut */
   NULL,                            /* set_hdr_scanlines */
   NULL                             /* set_hdr_subpixel_layout */
};

static void ps4_gfx_get_poke_interface(void *data,
      const video_poke_interface_t **iface)
{
   *iface = &ps4_poke_interface;
}

video_driver_t video_ps4 = {
   ps4_gfx_init,
   ps4_gfx_frame,
   ps4_gfx_set_nonblock_state,
   ps4_gfx_alive,
   ps4_gfx_focus,
   ps4_gfx_suppress_screensaver,
   ps4_gfx_has_windowed,
   ps4_gfx_set_shader,
   ps4_gfx_free,
   "ps4",
   NULL,                            /* set_viewport */
   ps4_gfx_set_rotation,
   ps4_gfx_viewport_info,
   NULL,                            /* read_viewport */
   NULL,                            /* read_frame_raw */
#ifdef HAVE_OVERLAY
   NULL,                            /* overlay_interface */
#endif
   ps4_gfx_get_poke_interface,
   NULL,                            /* wrap_type_to_enum */
   NULL,                            /* shader_load_begin */
   NULL,                            /* shader_load_step */
#ifdef HAVE_GFX_WIDGETS
   NULL,                            /* gfx_widgets_enabled */
#endif
   NULL,                            /* invalidate_hw_render_cache */
   NULL,                            /* read_viewport_hdr */
   NULL                             /* font_backend */
};
