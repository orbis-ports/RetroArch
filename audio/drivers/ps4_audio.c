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
 *
 * ⚠ AND IT NEEDS A THREAD OF ITS OWN. sceAudioOutOutput blocks until the grain it was given
 * has been CONSUMED, so by the time it returns the port has nothing queued behind it. Fed
 * from RetroArch's main loop that is a guaranteed underrun: the loop also blocks on vsync,
 * so between the last write of one frame and the first of the next the port runs dry, and
 * what a dry port sounds like is a click on every frame boundary. That is exactly what the
 * first working build did.
 *
 * The fix is the shape ~/src/ps4doom uses and the one switch_thread_audio.c uses in this
 * tree: a dedicated thread sitting in sceAudioOutOutput permanently, pulling from a ring
 * buffer that write() fills. There is always a grain in flight, the frontend never blocks
 * on the port, and an empty ring plays silence instead of stuttering.
 */

#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <retro_miscellaneous.h>
#include <queues/fifo_queue.h>
#include <rthreads/rthreads.h>

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
   int32_t        handle;

   fifo_buffer_t *fifo;
   slock_t       *lock;
   scond_t       *cond;
   sthread_t     *thread;

   /* The thread's own staging grain. Only the thread touches it, so it needs no lock. */
   int16_t        grain[PS4_AUDIO_GRAIN * PS4_AUDIO_CHANNELS];

   volatile bool  quit;
   bool           nonblock;
   bool           running;

   /* ⚠ DIAGNOSTIC, AND DELIBERATELY BOUNDED. underruns is the number that says whether the
    * thread is winning: it counts grains the ring could not fill, which is what a click
    * used to be. A handful at startup is the pipeline filling; a rising count is the
    * frontend not keeping up. */
   uint64_t       grains;
   uint64_t       underruns;
   uint64_t       underruns_said;
   int32_t        last_rc;
} ps4_audio_t;

/* ⚠ SILENCE ON UNDERRUN, NOT A SHORT WRITE. The port takes exactly one grain and will not
 * take a partial one, so an empty ring has to hand it something. Silence costs a click;
 * skipping the call costs the port its continuity, which costs every following frame. */
static void ps4_audio_thread(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   while (!ps4->quit)
   {
      size_t want = sizeof(ps4->grain);

      slock_lock(ps4->lock);
      if (FIFO_READ_AVAIL(ps4->fifo) >= want)
         fifo_read(ps4->fifo, ps4->grain, want);
      else
      {
         memset(ps4->grain, 0, want);
         ps4->underruns++;
      }
      /* Whatever was waiting for room now has some. */
      scond_signal(ps4->cond);
      slock_unlock(ps4->lock);

      ps4->last_rc = sceAudioOutOutput(ps4->handle, ps4->grain);
      ps4->grains++;

      /* ⚠ ONLY WHEN SOMETHING IS WRONG, and the counter is why. This used to report every
       * ten seconds unconditionally, which is what a bring-up wants and what a shipped
       * frontend should not do: a hundred identical lines an hour hide the one line that
       * differs. Underruns are still worth a line each time the count MOVES - that is the
       * one audio symptom this port has, and silence now means the ring is keeping up. */
      if (ps4->underruns != ps4->underruns_said
            && (ps4->grains % (PS4_AUDIO_RATE * 10 / PS4_AUDIO_GRAIN)) == 0)
      {
         RARCH_WARN("[PS4] audio: %llu underruns in %llu grains, rc=%d\n",
               (unsigned long long)(ps4->underruns - ps4->underruns_said),
               (unsigned long long)ps4->grains,
               (int)ps4->last_rc);
         ps4->underruns_said = ps4->underruns;
      }
   }
}

static void ps4_audio_free(void *data);

static void *ps4_audio_init(const char *device, unsigned rate,
      unsigned latency, unsigned block_frames, unsigned *new_rate)
{
   ps4_audio_t *ps4;
   int32_t      rc;
   int          vol[8];
   unsigned     i;

   sceUserServiceInitialize(NULL);

   rc = sceAudioOutInit();

   /* ⚠ ALREADY_INIT IS NOT A FAILURE, AND THE MAGIC NUMBER THIS USED WAS THE WRONG ONE.
    * RetroArch initialises its audio driver twice - once at startup and again when content
    * loads - so the second sceAudioOutInit in a process ALWAYS returns ALREADY_INIT. This
    * tolerated 0x8026000d, copied verbatim from ~/src/ps4doom together with a comment
    * calling it ALREADY_INIT. The SDK says otherwise: 0x8026000D is OUT_OF_MEMORY and
    * ALREADY_INIT is 0x8026000E. So the driver tolerated an allocation failure and treated
    * "already up" as fatal - exactly backwards - and audio died on the second init with
    * "Failed to initialize audio driver. Will continue without audio."
    *
    * ps4doom has the same wrong constant and never noticed, because it initialises once.
    * The named constant is used here so the question cannot come up again. */
   if (rc != 0 && rc != (int32_t)ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT)
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

   /* The ring holds `latency` milliseconds. RetroArch's default is 64 ms, which at 48 kHz
    * stereo s16 is 12288 bytes - twelve grains of headroom for the thread to draw on while
    * the frontend is busy drawing a frame. A floor of four grains keeps a caller that asks
    * for a very low latency from producing a ring the thread empties instantly. */
   {
      size_t fifo_size = (size_t)PS4_AUDIO_RATE * PS4_AUDIO_CHANNELS
         * sizeof(int16_t) * (latency ? latency : 64) / 1000;
      size_t floor_sz  = sizeof(ps4->grain) * 4;

      if (fifo_size < floor_sz)
         fifo_size = floor_sz;

      ps4->fifo = fifo_new(fifo_size);
      ps4->lock = slock_new();
      ps4->cond = scond_new();

      if (!ps4->fifo || !ps4->lock || !ps4->cond)
      {
         RARCH_ERR("[PS4] Could not create the audio ring.\n");
         ps4_audio_free(ps4);
         return NULL;
      }

      ps4->running = true;
      ps4->thread  = sthread_create(ps4_audio_thread, ps4);

      if (!ps4->thread)
      {
         RARCH_ERR("[PS4] Could not start the audio thread.\n");
         ps4_audio_free(ps4);
         return NULL;
      }

      if (new_rate)
         *new_rate = PS4_AUDIO_RATE;

      RARCH_LOG("[PS4] Audio up: %u Hz, S16 stereo, %u-frame grain, %u-byte ring.\n",
            PS4_AUDIO_RATE, PS4_AUDIO_GRAIN, (unsigned)fifo_size);
   }
   return ps4;
}

static ssize_t ps4_audio_write(void *data, const void *s, size_t len)
{
   ps4_audio_t *ps4  = (ps4_audio_t*)data;
   size_t       done = 0;

   if (!ps4 || !s || !ps4->fifo)
      return -1;

   if (!ps4->running)
      return (ssize_t)len;   /* stopped: consumed and discarded, not an error */

   slock_lock(ps4->lock);

   while (done < len)
   {
      size_t avail = FIFO_WRITE_AVAIL(ps4->fifo);

      if (avail == 0)
      {
         /* ⚠ IN NON-BLOCKING MODE THE REST IS DROPPED, ON PURPOSE. That is fast-forward
          * asking to run faster than real time; waiting for room would throttle it back to
          * 1x, which is not a fast-forward. */
         if (ps4->nonblock)
            break;

         /* The thread signals every time it takes a grain, so this wakes within one grain
          * period rather than spinning. */
         scond_wait(ps4->cond, ps4->lock);
         continue;
      }

      if (avail > len - done)
         avail = len - done;

      fifo_write(ps4->fifo, (const uint8_t*)s + done, avail);
      done += avail;
   }

   slock_unlock(ps4->lock);
   return (ssize_t)done;
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
   /* The thread keeps running and keeps the port fed with silence. Tearing it down and
    * standing it back up on every pause would mean an open/close cycle per menu entry, and
    * a port that stops being written is a port that has to be primed again. */
   ps4->running = false;
   return true;
}

static bool ps4_audio_start(void *data, bool is_shutdown)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;

   if (!ps4)
      return false;

   slock_lock(ps4->lock);
   /* Whatever is still in the ring belongs to the audio from before the pause. */
   fifo_clear(ps4->fifo);
   slock_unlock(ps4->lock);

   ps4->running = true;
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

   if (ps4->thread)
   {
      ps4->quit = true;
      /* The thread may be waiting for the port rather than for us, so it can take up to one
       * grain period to notice. sthread_join waits for exactly that. */
      if (ps4->lock)
      {
         slock_lock(ps4->lock);
         scond_signal(ps4->cond);
         slock_unlock(ps4->lock);
      }
      sthread_join(ps4->thread);
      ps4->thread = NULL;
   }

   if (ps4->handle >= 0)
      sceAudioOutClose(ps4->handle);
   if (ps4->fifo)
      fifo_free(ps4->fifo);
   if (ps4->cond)
      scond_free(ps4->cond);
   if (ps4->lock)
      slock_free(ps4->lock);

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

   {
      size_t avail;
      slock_lock(ps4->lock);
      avail = FIFO_WRITE_AVAIL(ps4->fifo);
      slock_unlock(ps4->lock);
      return avail;
   }
}

static size_t ps4_audio_buffer_size(void *data)
{
   ps4_audio_t *ps4 = (ps4_audio_t*)data;
   return (ps4 && ps4->fifo) ? ps4->fifo->size : 0;
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
