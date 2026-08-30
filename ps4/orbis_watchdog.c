/* A stack trace for a frontend that has stopped, for a console that cannot be attached to.
 *
 * ⚠ WHY THIS EXISTS. On 2026-08-30 flycast's Dreamcast BIOS was asked for 60 Hz and the
 * frontend stopped producing frames. The process did not die: no signal, no abort, and the
 * PS4 audio thread kept logging its underrun line every ten seconds for as long as the
 * maintainer left it. Nothing in either log channel says where the main thread went, because
 * a thread that is blocked writes nothing - and the LAST line it wrote ("[Display] Found
 * display driver") is the tail of drivers_init, which proves only that the reinit finished.
 *
 * ⚠ AND THE QUESTION IT ANSWERS IS THE ONE THE LOG CANNOT. "Frontend or core?" - a hang
 * inside RetroArch's driver reinit, a hang inside the core's retro_run, and a hang inside the
 * video driver's present all look identical from outside: frames stop, audio starves, the
 * process lives. This thread samples a phase word that the main loop stores as it crosses
 * each boundary, and says out loud which boundary it did not come back from.
 *
 * ⚠ IT SPEAKS THROUGH RARCH_ERR ON PURPOSE, which on this port is klog AND UDP (see
 * ps4/ps4_log.h). A hung process may have a netlog nobody is draining; klog has already
 * written by the time the call returns. The 8-15 ms that costs is paid once every ten seconds
 * and only while the frontend is already frozen, which is a frame budget that has stopped
 * mattering.
 *
 * ⚠ ALWAYS ON, UNLIKE ps4/orbis_profile.c. The profile is gated behind a file because it
 * reports every five seconds whether or not anything is wrong. This one is silent unless the
 * run loop has not moved for five seconds, which never happens in a healthy run - so there is
 * nothing to turn off, and a hang that shows up on a machine the maintainer was not
 * instrumenting is still explained.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include <orbis/libkernel.h>

#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <pthread.h>

#include "orbis_watchdog.h"
#include "../verbosity.h"

/* Milliseconds of no movement before the first report, and between the ones after it. A
 * frame that takes five seconds is already a hang by any measure this port cares about, and
 * flycast's slowest content load on this console is under three. */
#define ORBIS_WD_STALL_MS  5000

static const char * volatile wd_phase  = "pre-runloop";
static volatile int          wd_detail;
static volatile uint64_t     wd_seq;
static volatile int          wd_started;

/* ⚠ WHICH THREAD LEFT THE MARK, BECAUSE MORE THAN ONE THREAD LEAVES THEM AND THAT IS THE
 * WHOLE ANSWER IN AT LEAST ONE HANG.
 *
 * The phase word is a single global written by whoever calls the macro, and the callers are not
 * all on the main thread. A libretro core may invoke the environment callback from a thread of
 * its own - flycast reaches RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO from a PVR register write
 * executed by emulated SH4 code, which with threaded rendering runs on flycast's emulator
 * thread while the main thread is parked in retro_run waiting for a frame. Read without the
 * thread id, the report then says "environ:returned" and reads as though the MAIN thread were
 * sitting there, which is the opposite of what happened: the main thread was somewhere else
 * entirely and never got the chance to move.
 *
 * One extra store per mark, and it turns an ambiguous line into an unambiguous one. */
static volatile uintptr_t    wd_thread;
static volatile uintptr_t    wd_first_thread;

static uint64_t orbis_wd_now_ms(void)
{
   /* A plain kernel call rather than clock_gettime: the SDK routes that one through musl's
    * __clock_gettime64 shim, and this is one syscall with no shim. Same reasoning, and the
    * same call, as ps4/orbis_profile.c. */
   return sceKernelGetProcessTime() / 1000ull;
}

static void orbis_watchdog_thread(void *unused)
{
   uint64_t last_seq  = 0;
   uint64_t last_move = orbis_wd_now_ms();
   uint64_t said      = 0;

   (void)unused;

   for (;;)
   {
      uint64_t seq;
      uint64_t now;
      uint64_t stalled;

      retro_sleep(500);

      seq = wd_seq;
      now = orbis_wd_now_ms();

      if (seq != last_seq)
      {
         /* It moved. If we had complained about it, say that it came back - a hang that
          * recovers is a different bug from one that does not, and the log has to be able
          * to tell them apart after the fact. */
         if (said)
            RARCH_ERR("[PS4] watchdog: the run loop moved again after %llu ms - now at \"%s\" (%d)\n",
                  (unsigned long long)(now - last_move),
                  wd_phase ? wd_phase : "(null)",
                  wd_detail);
         last_seq  = seq;
         last_move = now;
         said      = 0;
         continue;
      }

      stalled = now - last_move;
      if (stalled < ORBIS_WD_STALL_MS)
         continue;

      /* One line per ORBIS_WD_STALL_MS window, not one per sample. Repeating rather than
       * saying it once is deliberate: an unchanging phase across several reports is what
       * distinguishes a block from a slow frame that eventually finished. */
      if ((stalled / ORBIS_WD_STALL_MS) == said)
         continue;
      said = stalled / ORBIS_WD_STALL_MS;

      RARCH_ERR("[PS4] watchdog: the run loop has not moved for %llu ms - last phase \"%s\" (%d), "
            "seq %llu, marked by thread %p (%s)\n",
            (unsigned long long)stalled,
            wd_phase ? wd_phase : "(null)",
            wd_detail,
            (unsigned long long)seq,
            (void*)wd_thread,
            wd_thread == wd_first_thread ? "the one that started the run loop"
                                         : "NOT the one that started the run loop");
   }
}

void orbis_watchdog_mark(const char *phase, int detail)
{
   wd_phase  = phase;
   wd_detail = detail;
   wd_thread = (uintptr_t)pthread_self();
   wd_seq++;

   /* Started from the first mark rather than from a frontend init hook: the first mark is on
    * the main thread, long before anything else races it, and this file then needs no call
    * site outside the ones that already exist. */
   if (!wd_started)
   {
      wd_started      = 1;
      wd_first_thread = wd_thread;
      sthread_create(orbis_watchdog_thread, NULL);
   }
}
