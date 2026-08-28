/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2020 The RetroArch team
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

/* sceAudioOut, S16 stereo, 48 kHz.
 *
 * ⚠ WRITTEN RATHER THAN ADAPTED FROM audio/drivers/psp_audio.c, whose ORBIS arm this
 * replaces. That file is the Vita's driver bent around orbisdev: it included
 * <libSceAudioOut.h> (not a header this SDK has), hard-coded 0xff as a magic it did not
 * explain, carried a SceUID thread field from a different SDK, and had psp_audio_stop()
 * return false unconditionally as a workaround for something nobody wrote down.
 *
 * ⚠ THE PORT WANTS THE SYSTEM USER, NOT THE LOGGED-IN ONE, and this is the opposite of
 * scePadOpen. sceAudioOutOpen with a real user id fails on hardware with 0x809b0001;
 * ORBIS_USER_SERVICE_USER_ID_SYSTEM (0xFF) is what the SDK's own audio sample passes.
 * ~/src/ps4doom/platform/doom_sound_ps4.c paid for that difference; this is a
 * transcription of what it learned, not a fresh guess.
 *
 * ⚠ AND THE PORT OPENS SILENT. The volume after sceAudioOutOpen can be 0 on every channel,
 * so a driver that skips sceAudioOutSetVolume works perfectly and is inaudible.
 */

#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <retro_miscellaneous.h>

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>

#include "../audio_driver.h"
#include "../../verbosity.h"

/* The MAIN port's rate. RetroArch is told this through *new_rate and resamples to it, so
 * asking for anything else here would be a second resampler behind its back. */
#define PS4_AUDIO_RATE   48000

/* Frames per sceAudioOutOutput call. The port is opened with this granularity and every
 * output must be exactly this many frames - it is not a hint. 256 at 48 kHz is 5.33 ms,
 * which is the value ps4doom runs with on hardware. */
#define PS4_AUDIO_GRAIN  256

#define PS4_AUDIO_CHANNELS 2

typedef struct ps4_audio
{
   int32_t  handle;
   /* One grain under construction. RetroArch writes whatever size it likes; the port
    * accepts exactly one grain, so this is where the two meet. */
   int16_t  grain[PS4_AUDIO_GRAIN * PS4_AUDIO_CHANNELS];
   size_t   grain_fill;      /* samples, not frames */
   bool     nonblock;
   bool     running;
} ps4_audio_t;

static void *ps4_audio_init(const char *device, unsigned rate,
      unsigned latency, unsigned block_frames, unsigned *new_rate)
{
   ps4_audio_t *ps4;
   int32_t      rc;
   int          vol[8];
   unsigned     i;

   sceUserServiceInitialize(NULL);

   rc = sceAudioOutInit();
   /* Already initialised is not a failure - something else in this process got there
    * first, which on a frontend that restarts its audio driver is the normal case. */
   if (rc != 0 && rc != (int32_t)0x8026000d /* ALREADY_INIT */)
   {
      RARCH_ERR("[PS4] sceAudioOutInit failed: 0x%08x\n", (unsigned)rc);
      return NULL;
   }

   if (!(ps4 = (ps4_audio_t*)calloc(1, sizeof(*ps4))))
      return NULL;

   ps4->handle = sceAudioOutOpen(ORBIS_USER_SERVICE_USER_ID_SYSTEM,
         ORBIS_AUDIO_OUT_PORT_TYPE_MAIN,
         0,
         PS4_AUDIO_GRAIN,
         PS4_AUDIO_RATE,
         ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);

   if (ps4->handle < 0)
   {
      RARCH_ERR("[PS4] sceAudioOutOpen failed: 0x%08x\n", (unsigned)ps4->handle);
      free(ps4);
      return NULL;
   }

   /* Flag bit i selects channel i; 0xff covers all of them. Without this the port can sit
    * at zero and every write below succeeds into silence. */
   for (i = 0; i < 8; i++)
      vol[i] = 32768;
   sceAudioOutSetVolume(ps4->handle, 0xff, vol);

   ps4->running = true;

   if (new_rate)
      *new_rate = PS4_AUDIO_RATE;

   RARCH_LOG("[PS4] Audio up: %u Hz, S16 stereo, %u-frame grain.\n",
         PS4_AUDIO_RATE, PS4_AUDIO_GRAIN);
   return ps4;
}

static ssize_t ps4_audio_write(void *data, const void *s, size_t len)
{
   ps4_audio_t   *ps4  = (ps4_audio_t*)data;
   const int16_t *src  = (const int16_t*)s;
   size_t         left = len / sizeof(int16_t);   /* samples */
   size_t         done = 0;

   if (!ps4 || !src)
      return -1;

   while (left > 0)
   {
      size_t room = (PS4_AUDIO_GRAIN * PS4_AUDIO_CHANNELS) - ps4->grain_fill;
      size_t take = (left < room) ? left : room;

      memcpy(ps4->grain + ps4->grain_fill, src + done, take * sizeof(int16_t));
      ps4->grain_fill += take;
      done            += take;
      left            -= take;

      if (ps4->grain_fill < PS4_AUDIO_GRAIN * PS4_AUDIO_CHANNELS)
         break;

      ps4->grain_fill = 0;

      /* ⚠ sceAudioOutOutput BLOCKS until the grain has been consumed, and that is what
       * paces the frontend when vsync is not doing it. In non-blocking mode - which is
       * what fast-forward asks for - blocking here would throttle the run loop to real
       * time, so the grain is dropped instead. Silence while fast-forwarding is what every
       * other frontend does; a fast-forward that runs at 1x is not. */
      if (!ps4->running || ps4->nonblock)
         continue;

      sceAudioOutOutput(ps4->handle, ps4->grain);
   }

   return (ssize_t)(done * sizeof(int16_t));
}

static bool ps4_audio_stop(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   if (!ps4)
      return false;

   /* The port stays open: reopening it on every menu entry would mean an init sequence per
    * pause, and the grain in flight would be lost either way. Writes are discarded while
    * stopped, which is what "stopped" has to mean for a port that only accepts whole
    * grains. */
   ps4->running = false;
   return true;
}

static bool ps4_audio_start(void *data, bool is_shutdown)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   if (!ps4)
      return false;

   ps4->running    = true;
   /* Anything half-collected belongs to the audio from before the pause. */
   ps4->grain_fill = 0;
   return true;
}

static bool ps4_audio_alive(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   return ps4 && ps4->running;
}

static void ps4_audio_set_nonblock_state(void *data, bool toggle)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   if (ps4)
      ps4->nonblock = toggle;
}

static void ps4_audio_free(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   if (!ps4)
      return;

   if (ps4->handle >= 0)
      sceAudioOutClose(ps4->handle);

   free(ps4);
}

/* ⚠ S16, NOT FLOAT, ALTHOUGH THE PORT SUPPORTS BOTH.
 * ORBIS_AUDIO_OUT_PARAM_FORMAT_FLOAT_STEREO exists and would save RetroArch a conversion,
 * because its mixer is float all the way to here. S16 is what has run on this console;
 * float is an easy experiment and an unmeasured one. */
static bool ps4_audio_use_float(void *data)
{
   return false;
}

static size_t ps4_audio_write_avail(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   if (!ps4)
      return 0;

   /* What can be taken without blocking: the rest of the grain being filled. Anything
    * beyond that reaches sceAudioOutOutput and waits. */
   return ((PS4_AUDIO_GRAIN * PS4_AUDIO_CHANNELS) - ps4->grain_fill)
      * sizeof(int16_t);
}

static size_t ps4_audio_buffer_size(void *data)
{
   return PS4_AUDIO_GRAIN * PS4_AUDIO_CHANNELS * sizeof(int16_t);
}

audio_driver_t audio_ps4 = {
   ps4_audio_init,
   ps4_audio_write,
   ps4_audio_stop,
   ps4_audio_start,
   ps4_audio_alive,
   ps4_audio_set_nonblock_state,
   ps4_audio_free,
   ps4_audio_use_float,
   "ps4",
   NULL,                      /* device_list_new */
   NULL,                      /* device_list_free */
   ps4_audio_write_avail,
   ps4_audio_buffer_size,
   NULL                       /* write_raw */
};
