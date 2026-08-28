/* RetroArch - A frontend for libretro. See ps4/ps4_video_out.h. */

#include <stdlib.h>
#include <string.h>

#include <orbis/libkernel.h>
#include <orbis/VideoOut.h>
#include <orbis/UserService.h>

#include "ps4_video_out.h"
#include "ps4_log.h"

#define PS4_VIDEO_OUT_MAX_BUFFERS 3

/* 2 MiB, which is what the SDK's own samples/_common/graphics.cpp asks for. Direct memory
 * is carved in pages this size and an allocation that is not aligned to one is refused. */
#define PS4_DIRECT_MEM_ALIGN 0x200000

/* WB_ONION: write-back, CPU and GPU coherent. The CPU writes every pixel here, so a
 * write-combined (GARLIC) arena would be the wrong trade - it is fast to write linearly
 * and painful to read, and the scaler reads its own destination when it blends the menu. */
#define PS4_DIRECT_MEM_TYPE_WB_ONION 3

/* CPU read/write plus GPU read/write. Nothing here uses the GPU, but the buffers are
 * registered with the display, which does. */
#define PS4_DIRECT_MEM_PROT_RW 0x33

struct ps4_video_out
{
   uint32_t         *fb[PS4_VIDEO_OUT_MAX_BUFFERS];
   void             *mem;         /* the one mapping the buffers are carved out of */
   off_t             mem_off;     /* its direct-memory offset, which the release wants */
   size_t            mem_size;
   OrbisKernelEqueue flip_queue;
   int64_t           flip_arg;    /* monotone; what GetFlipStatus is compared against */
   int32_t           handle;
   unsigned          width;
   unsigned          height;
   unsigned          pitch_px;
   unsigned          count;
   unsigned          back;        /* index of the buffer the caller draws into */
};

/* ⚠ UNMAPPING IS NOT RELEASING. sceKernelMapDirectMemory hands back a mapping of a
 * RESERVATION made by sceKernelAllocateDirectMemory, and munmap drops only the first: the
 * physical pages stay carved out of the process's direct budget until
 * sceKernelReleaseDirectMemory is called with the offset the allocation returned. The SDK's
 * own sample does both (samples/_common/graphics.cpp, deallocateVideoMem). A driver that
 * only unmapped would leak a whole framebuffer set per re-init, and re-init is what a
 * resolution change is. */
static void ps4_video_out_release_mem(ps4_video_out_t *vo)
{
   if (vo->mem)
   {
      sceKernelMunmap(vo->mem, vo->mem_size);
      vo->mem = NULL;
   }
   if (vo->mem_size)
   {
      sceKernelReleaseDirectMemory(vo->mem_off, vo->mem_size);
      vo->mem_size = 0;
   }
}

ps4_video_out_t *ps4_video_out_open(unsigned width, unsigned height, unsigned buffers)
{
   ps4_video_out_t *vo;
   OrbisVideoOutBufferAttribute attr;
   size_t   fb_bytes;
   off_t    dmem_off = 0;
   unsigned i;
   int32_t  rc;

   if (buffers < 2 || buffers > PS4_VIDEO_OUT_MAX_BUFFERS)
      buffers = 2;

   if (!(vo = (ps4_video_out_t*)calloc(1, sizeof(*vo))))
      return NULL;

   vo->width    = width;
   vo->height   = height;
   vo->pitch_px = width;
   vo->count    = buffers;

   /* ORBIS_VIDEO_USER_MAIN (0xFF) is right HERE and wrong for scePadOpen: video-out takes
    * the "main user" constant, the pad wants the real id from sceUserServiceGetInitialUser
    * and refuses 0xFF on hardware with 0x809b0001. Two APIs, two conventions. */
   vo->handle = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
   if (vo->handle < 0)
   {
      ps4_rarch_err("[ERROR]", "[PS4] sceVideoOutOpen failed: 0x%x\n",
            (unsigned)vo->handle);
      free(vo);
      return NULL;
   }

   /* ⚠ DIRECT MEMORY, NOT malloc. sceVideoOutRegisterBuffers refuses ordinary memory on a
    * real console (0x80290013) while accepting it under the emulator. One allocation,
    * carved into the buffers, so there is one thing to release. */
   fb_bytes     = (size_t)vo->pitch_px * height * sizeof(uint32_t);
   vo->mem_size = ((fb_bytes * buffers) + PS4_DIRECT_MEM_ALIGN - 1)
                / PS4_DIRECT_MEM_ALIGN * PS4_DIRECT_MEM_ALIGN;

   rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(),
         vo->mem_size, PS4_DIRECT_MEM_ALIGN, PS4_DIRECT_MEM_TYPE_WB_ONION, &dmem_off);
   if (rc < 0)
   {
      ps4_rarch_err("[ERROR]", "[PS4] AllocateDirectMemory(%u bytes) failed: 0x%x\n",
            (unsigned)vo->mem_size, (unsigned)rc);
      goto error;
   }

   vo->mem_off = dmem_off;

   rc = sceKernelMapDirectMemory(&vo->mem, vo->mem_size, PS4_DIRECT_MEM_PROT_RW, 0,
         dmem_off, PS4_DIRECT_MEM_ALIGN);
   if (rc < 0)
   {
      ps4_rarch_err("[ERROR]", "[PS4] MapDirectMemory failed: 0x%x\n", (unsigned)rc);
      goto error;
   }

   for (i = 0; i < buffers; i++)
   {
      vo->fb[i] = (uint32_t*)((uintptr_t)vo->mem + (size_t)i * fb_bytes);
      memset(vo->fb[i], 0, fb_bytes);
   }

   sceVideoOutSetBufferAttribute(&attr,
         ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
         ORBIS_VIDEO_OUT_TILING_MODE_LINEAR,
         ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
         width, height, vo->pitch_px);

   rc = sceVideoOutRegisterBuffers(vo->handle, 0, (void * const*)vo->fb, buffers, &attr);
   if (rc < 0)
   {
      ps4_rarch_err("[ERROR]", "[PS4] sceVideoOutRegisterBuffers failed: 0x%x\n",
            (unsigned)rc);
      goto error;
   }

   sceVideoOutSetFlipRate(vo->handle, ORBIS_VIDEO_OUT_FLIP_60HZ);

   /* ⚠ REQUIRED, NOT DIAGNOSTIC. Without a flip event queue the display never processes a
    * submitted flip, flipArg never advances, and ps4_video_out_flip()'s wait below never
    * completes - measured as minutes per frame, not a stall. */
   sceKernelCreateEqueue(&vo->flip_queue, "retroarch flip");
   sceVideoOutAddFlipEvent(vo->flip_queue, vo->handle, NULL);

   ps4_rarch_log("[INFO]", "[PS4] video-out up: %ux%u, %u buffers, %u MiB direct\n",
         width, height, buffers, (unsigned)(vo->mem_size >> 20));
   return vo;

error:
   ps4_video_out_release_mem(vo);
   sceVideoOutClose(vo->handle);
   free(vo);
   return NULL;
}

void ps4_video_out_close(ps4_video_out_t *vo)
{
   if (!vo)
      return;

   sceVideoOutUnregisterBuffers(vo->handle, 0);
   ps4_video_out_release_mem(vo);
   sceVideoOutClose(vo->handle);
   free(vo);
}

uint32_t *ps4_video_out_backbuffer(ps4_video_out_t *vo)
{
   return vo ? vo->fb[vo->back] : NULL;
}

unsigned ps4_video_out_pitch_px(const ps4_video_out_t *vo)
{
   return vo ? vo->pitch_px : 0;
}

void ps4_video_out_size(const ps4_video_out_t *vo, unsigned *width, unsigned *height)
{
   if (!vo)
      return;
   if (width)
      *width  = vo->width;
   if (height)
      *height = vo->height;
}

bool ps4_video_out_flip(ps4_video_out_t *vo, bool wait)
{
   int32_t rc;

   if (!vo)
      return false;

   rc = sceVideoOutSubmitFlip(vo->handle, (int32_t)vo->back,
         ORBIS_VIDEO_OUT_FLIP_VSYNC, vo->flip_arg);
   if (rc < 0)
   {
      ps4_rarch_err("[ERROR]", "[PS4] sceVideoOutSubmitFlip failed: 0x%x\n", (unsigned)rc);
      return false;
   }

   if (wait)
   {
      /* Block on the event queue rather than sleeping: a usleep spin is what turns "the
       * flip has not been taken yet" into a stall nobody can attribute. The timeout keeps
       * a display that stopped answering from hanging the frontend outright - a dropped
       * frame is recoverable, a wedged run loop is not. */
      for (;;)
      {
         OrbisVideoOutFlipStatus st;
         OrbisKernelEvent        evt;
         int                     out_count = 0;
         OrbisKernelUseconds     timeout   = 100000;

         sceVideoOutGetFlipStatus(vo->handle, &st);
         if (st.flipArg == vo->flip_arg)
            break;

         if (sceKernelWaitEqueue(vo->flip_queue, &evt, 1, &out_count, &timeout) != 0)
            break;
      }
   }

   vo->flip_arg++;
   vo->back = (vo->back + 1) % vo->count;
   return true;
}
