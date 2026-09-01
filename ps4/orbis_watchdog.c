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
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

#include <retro_timers.h>
#include <rthreads/rthreads.h>
#include <pthread.h>

#include "orbis_watchdog.h"

/* ⚠ EVERY LINE THIS FILE PRINTS GOES TO A FILE ON THE CONSOLE AS WELL, AND THAT IS NOT BELT AND
 * BRACES. Twice on 2026-08-31 a diagnostic ran correctly and was lost: the glcore build's Mesa log
 * was being truncated by the other eboot, and a whole run's watchdog output vanished because the
 * maintainer's log receiver had disconnected - the netlog and the klog capture come from one
 * script, so losing it loses both. A watchdog that only speaks to a listener is a watchdog for the
 * runs that did not need one.
 *
 * orbis_report writes sceKernelDebugOutText first and then /data/retroarch-abort.log, synchronously.
 * It costs 8-15 ms a line, which is why this file is the only place it is used outside a crash: every
 * line here is an anomaly, rate-limited to one per five seconds at worst. RARCH_ERR stays alongside
 * it so the netlog still carries them when somebody is listening. */
static void wd_to_file(const char *fmt, ...)
{
   /* ⚠ fopen/append, NOT orbis_report: that lives in the core-support archive and carries
    * overrides for abort() and __assert_fail() with it. The frontend has its own and must keep
    * them. A watchdog is not worth changing what happens when this process dies. */
   va_list ap;
   FILE *fp = fopen("/data/retroarch-watchdog.log", "a");
   if (!fp)
      return;
   va_start(ap, fmt);
   vfprintf(fp, fmt, ap);
   va_end(ap);
   fputc('\n', fp);
   fclose(fp);
}

#define WD_ERR(...) do { RARCH_ERR(__VA_ARGS__); wd_to_file(__VA_ARGS__); } while (0)

#include "../verbosity.h"

/* ⚠ FOR THE ONE-OFF FACTS A RUN IS WORTHLESS WITHOUT, AND WHICH RARCH_LOG DOES NOT PUT ON DISK.
 *
 * RARCH_LOG on this port goes to klog and the netlog, and both need a listener. A whole run's
 * evidence has already been lost that way once. A line that answers "which context did we get" or
 * "which driver is this" is worth the 8-15 ms it costs, because a run that does not carry it has to
 * be repeated. Call it for facts, not for events: this is not a logging channel. */
void orbis_watchdog_note(const char *fmt, ...)
{
   char buf[512];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
   RARCH_ERR("%s\n", buf);
   wd_to_file("%s", buf);
}


/* Milliseconds of no movement before the first report, and between the ones after it. A
 * frame that takes five seconds is already a hang by any measure this port cares about, and
 * flycast's slowest content load on this console is under three. */
#define ORBIS_WD_STALL_MS  5000

/* ⚠ AND IT IS A KNOB NOW, BECAUSE FIVE SECONDS IS TUNED FOR A HANG AND THE OPEN QUESTION IS A HITCH.
 *
 * Measured with a stopwatch by the maintainer, 2026-09-01: in the menu, from startup, with no core
 * loaded, the picture stops dead for about half a second every four seconds and runs at 60 fps in
 * between. Half a second is two orders of magnitude above the 8-15 ms a synchronous log write costs,
 * so it is not this file's own output - the watchdog was disabled outright with ORBIS_WD=0 and the
 * hitch was unchanged.
 *
 * But this file already records WHERE the run loop is, which is the one thing that would name it.
 * ORBIS_WD_STALL=250 lowers the threshold to catch the hitch instead of only a hang. Below about
 * 100 ms this starts reporting ordinary content loads and shader compiles as stalls, which is why
 * the default stays where it is. */
static uint64_t orbis_wd_stall_ms(void)
{
   static int64_t cached = -1;
   if (cached < 0)
   {
      const char *const e = getenv("ORBIS_WD_STALL");
      const long v = (e && *e) ? strtol(e, NULL, 10) : 0;
      cached = (v >= 50 && v <= 60000) ? v : ORBIS_WD_STALL_MS;
   }
   return (uint64_t)cached;
}

/* ⚠ AND A SECOND QUESTION THIS FILE COULD NOT ANSWER, WHICH COST A WHOLE INVESTIGATION.
 *
 * 2026-08-31, melonDS DS on the glcore eboot: the PS4 audio driver reported 1875 underruns in
 * 1875 grains - every grain empty, so the core produced not one sample - for two minutes and
 * twenty seconds, while this watchdog said NOTHING. Silence here was read as "the instrument did
 * not fire", and it is the opposite: the run loop was moving the whole time. A run loop that
 * iterates while the core is never called looks identical from the log to a core that runs and
 * produces nothing, and the difference is the entire diagnosis - the first is a menu or a pause,
 * the second is a wedged core.
 *
 * So the mark for retro_run gets a counter of its own, and this thread says out loud which of the
 * two it is. It stays silent while the core is advancing, which is every healthy frame. */
#define ORBIS_WD_IDLE_MS   5000

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

/* Bumped only by the retro_run mark. wd_seq counts iterations of the run loop; this counts the
 * ones that actually entered the core. */
static volatile uint64_t     wd_core_seq;

/* ⚠ THE POOL PROBE, AND IT IS HERE RATHER THAN IN orbis-compat BECAUSE IT NEEDS A CLOCK AND A
 * THREAD, AND THIS FILE ALREADY HAS BOTH.
 *
 * `[ScePthread/System] Internal Memory is running out.` is the console's own message about
 * libkernel's pthread object pool (technote 235), and it has ended three runs of this port -
 * mupen64plus-next and Play! on 2026-08-28, melonDS DS on 2026-08-31. It arrives only in klog, in
 * bursts of 117 that are the transport's drain quantum rather than the event rate, and it names
 * neither the object nor the caller.
 *
 * ⚠ THE POOL BACKS MORE THAN THREADS - mutexes, condition variables, rwlocks, keys and attrs all
 * come out of it - so "did a thread fail" is the wrong question and orbis-compat's create counter
 * cannot answer it alone. Two allocations and two frees, once every ten seconds, cost nothing and
 * turn the exhaustion into ONE dated line on this port's own channel with a return code on it.
 *
 * ⚠ THE PROBE IS A ROUND TRIP, WHICH ALSO MAKES IT A TEST OF ITSELF. It takes two objects and
 * gives them straight back. If this platform's pthread_mutex_destroy did NOT return the object -
 * which nobody here has measured - then this probe alone would drain the pool at twelve objects a
 * minute and say so on its own after a long enough run. That would be a finding rather than a
 * defect in the instrument, and it is worth knowing which it is.
 *
 * ⚠ AND IT REPORTS ONLY ON A CHANGE OF STATE, in both directions. A line per probe would be six an
 * hour saying nothing; a line when the pool stops answering, and another if it starts again, is the
 * whole of what a reader needs and dates both edges. */
#define ORBIS_WD_POOL_MS   10000

/* ⚠ AND THE PROBE ABOVE CAME BACK CLEAN WHILE THE CONSOLE SAID THE POOL WAS EMPTY, WHICH IS THE
 * WHOLE REASON THIS SECOND ONE EXISTS.
 *
 * Measured on hardware 2026-08-31, 12:45:16 onwards: `[ScePthread/System] Internal Memory is
 * running out.` repeated 16221 times over four minutes, and across the whole of it
 * pthread_mutex_init, pthread_cond_init, pthread_attr_init and scePthreadCreate ALL kept
 * succeeding - orbis-compat's failure counters never moved and the probe above never printed a
 * line. So the resource being exhausted is not the one the message's category names, and no
 * amount of counting pthread objects will find it.
 *
 * ⚠ libkernel EXPORTS THE NUMBER ITSELF. `llvm-nm --dynamic` over the SDK's libkernel.so lists
 * sceKernelInternalMemoryGetAvailableSize, sceKernelInternalGetMapStatistics,
 * sceKernelInternalGetKmemStatistics and sceKernelInternalHeapPrintBacktraceWithModuleInfo -
 * only the last of which appears in any SDK header. The first one is the measurement this
 * investigation has been missing: a number with a slope, sampled against what the frontend is
 * doing, instead of a message with no units.
 *
 * ⚠ THE SIGNATURE IS NOT DECLARED ANYWHERE, SO IT IS NOT GUESSED - IT IS COVERED. The two
 * plausible Sony shapes are
 *
 *     int    f(size_t *out)    returns 0 and writes the size through the pointer
 *     size_t f(void)           ignores its arguments and returns the size
 *
 * and calling it with a REAL pointer to a zeroed 64-byte scratch satisfies both: the first
 * writes into the scratch and returns 0, the second leaves the scratch at zero and returns the
 * size. Both numbers are printed and one boot settles which it is. rsi and rdx are given defined
 * values rather than left as whatever happened to be in them, so a third shape that takes a
 * length cannot be handed garbage.
 *
 * ⚠ AND IT IS WEAK, so a toolchain that cannot resolve it produces a probe that says "not
 * available" rather than a frontend that will not link. */
extern uint64_t sceKernelInternalMemoryGetAvailableSize(void *out, uint64_t out_len, uint64_t zero)
   __attribute__((weak));

/* Declared by the SDK (orbis/libkernel.h:263) with an empty parameter list. It prints, with module
 * information, whoever holds libkernel's internal heap - which if the pool above is that heap is
 * the name this port has been trying to establish for four days.
 *
 * ⚠ GATED BEHIND A FILE, AND CALLED AT MOST ONCE, for the same reason ps4/orbis_profile.c is: the
 * size and destination of what it prints are unknown, it goes to klog, and klog is already the
 * channel the failure is drowning. Create /data/retroarch-heap-backtrace to arm it. */

static uint64_t orbis_wd_internal_memory(uint64_t *raw_ret, int *available)
{
   uint64_t scratch[8];

   if (&sceKernelInternalMemoryGetAvailableSize == NULL)
   {
      *available = 0;
      *raw_ret   = 0;
      return 0;
   }

   memset(scratch, 0, sizeof(scratch));
   *available = 1;
   *raw_ret   = sceKernelInternalMemoryGetAvailableSize(scratch, sizeof(scratch), 0);

   /* The out-parameter shape wrote into the scratch and returned 0; the plain-return shape left
    * the scratch alone. Whichever is non-zero is the answer, and the caller prints both. */
   return scratch[0] != 0 ? scratch[0] : *raw_ret;
}

static int orbis_wd_pool_probe(int *mutex_rc, int *cond_rc)
{
   pthread_mutex_t m;
   pthread_cond_t  c;

   *mutex_rc = pthread_mutex_init(&m, NULL);
   if (*mutex_rc == 0)
      pthread_mutex_destroy(&m);

   *cond_rc = pthread_cond_init(&c, NULL);
   if (*cond_rc == 0)
      pthread_cond_destroy(&c);

   return *mutex_rc == 0 && *cond_rc == 0;
}

static uint64_t orbis_wd_now_ms(void)
{
   /* A plain kernel call rather than clock_gettime: the SDK routes that one through musl's
    * __clock_gettime64 shim, and this is one syscall with no shim. Same reasoning, and the
    * same call, as ps4/orbis_profile.c. */
   return sceKernelGetProcessTime() / 1000ull;
}

/* ⚠ A KILL SWITCH, BECAUSE PROVING THIS THREAD INNOCENT SHOULD NOT COST A BUILD.
 *
 * Every line this thread emits goes through wd_to_file, which is a SYNCHRONOUS 8-15 ms write to
 * /data, and in the menu it has a standing complaint to make - retro_run is never entered, so the
 * "run loop is MOVING but the core is not being called" line fires on a timer from startup. That is
 * a plausible cause of a periodic hitch and an untested one, and the way to test it is to turn the
 * thread off and see whether the hitch goes with it. ORBIS_WD=0 does that; anything else, or an
 * unset variable, leaves the watchdog exactly as it was. */
static void orbis_watchdog_thread(void *unused)
{
   {
      const char *const e = getenv("ORBIS_WD");
      if (e && *e == '0')
      {
         wd_to_file("[PS4] watchdog: ORBIS_WD=0 - this thread is disabled for this run and "
                    "nothing below will be reported.");
         return;
      }
   }

   uint64_t last_seq       = 0;
   uint64_t last_move      = orbis_wd_now_ms();
   uint64_t said           = 0;

   uint64_t last_core_seq  = 0;
   uint64_t last_core_move = last_move;
   uint64_t said_idle      = 0;

   uint64_t last_pool      = last_move;
   int      pool_ok        = 1;

   uint64_t last_free      = 0;
   uint64_t first_free     = 0;
   uint64_t last_free_said = 0;
   int      free_reported  = 0;

   (void)unused;

   for (;;)
   {
      uint64_t seq;
      uint64_t core_seq;
      uint64_t now;
      uint64_t stalled;

      /* ⚠ THE SAMPLE PERIOD HAS TO BE WELL UNDER THE THING BEING CAUGHT. At 500 ms a half-second
         hitch can begin and end between two samples and never be seen at all. When the threshold
         is lowered to chase a hitch, the sampling follows it down - a quarter of the threshold, so
         four samples fall inside the shortest event that is meant to be reported. */
      retro_sleep((int)(orbis_wd_stall_ms() < 1000 ? orbis_wd_stall_ms() / 4 : 500));

      seq      = wd_seq;
      core_seq = wd_core_seq;
      now      = orbis_wd_now_ms();

      /* ------------------------------ libkernel's internal memory, every sample (500 ms) */
      {
         uint64_t raw   = 0;
         int      avail = 0;
         uint64_t freeb = orbis_wd_internal_memory(&raw, &avail);

         if (!free_reported)
         {
            /* Once, on the record, so every later number has a total to be read against. */
            free_reported = 1;
            first_free    = freeb;
            last_free     = freeb;
            WD_ERR("[PS4] watchdog: libkernel internal memory at startup: %llu bytes free "
                  "(raw return %llu, symbol %s). Falling numbers here are the resource behind "
                  "\"[ScePthread/System] Internal Memory is running out\" - technote 235.\n",
                  (unsigned long long)freeb,
                  (unsigned long long)raw,
                  avail ? "resolved" : "NOT AVAILABLE - nothing below this line means anything");
         }
         else if (avail && freeb != last_free)
         {
            /* ⚠ ONE LINE PER FIVE SECONDS, AND IT CARRIES THE SLOPE RATHER THAN THE VALUE. What
             * matters is bytes per second while the frontend is in a known state - the run-loop
             * and retro_run counters on the same line are what turn a slope into an attribution. */
            /* ⚠ FIVE SECONDS WHILE SOMETHING IS DRAINING, A MINUTE WHEN NOTHING IS.
             *
             * This line existed to catch a monotone descent to zero, and it did: zink's undestroyed
             * copy_lock, 146 rwlocks a frame at 64 bytes each. With that fixed the pool oscillates
             * by a few kilobytes either way and a line every five seconds says nothing while costing
             * a synchronous 8-15 ms write each time. A megabyte below startup is far outside that
             * oscillation and well inside the 14 MB the pool holds, so it is a threshold that stays
             * silent in health and speaks early in illness. */
            const int64_t below = (int64_t)first_free - (int64_t)freeb;
            const uint64_t period = (below > 1024 * 1024) ? 5000 : 60000;
            if (now - last_free_said >= period)
            {
               const int64_t delta = (int64_t)freeb - (int64_t)last_free;
               WD_ERR("[PS4] watchdog: libkernel internal memory %llu bytes free (%lld since the "
                     "last line, %lld below startup) - run loop seq %llu, retro_run entries %llu, "
                     "phase \"%s\" (%d).\n",
                     (unsigned long long)freeb,
                     (long long)delta,
                     (long long)((int64_t)first_free - (int64_t)freeb),
                     (unsigned long long)seq,
                     (unsigned long long)core_seq,
                     wd_phase ? wd_phase : "(null)",
                     wd_detail);
               last_free_said = now;
               last_free      = freeb;
            }
         }
      }

      /* ------------------------------------------------------- the pool, every ten seconds */
      if (now - last_pool >= ORBIS_WD_POOL_MS)
      {
         int mutex_rc = 0;
         int cond_rc  = 0;
         int ok       = orbis_wd_pool_probe(&mutex_rc, &cond_rc);

         last_pool    = now;

         if (ok != pool_ok)
         {
            pool_ok = ok;
            if (!ok)
               WD_ERR("[PS4] watchdog: the ScePthread object pool has STOPPED ANSWERING - "
                     "pthread_mutex_init=%d, pthread_cond_init=%d. This is libkernel's internal "
                     "pool (technote 235); it backs mutexes, condvars, rwlocks, keys and attrs as "
                     "well as threads, and something in this process has been taking objects out "
                     "of it without giving them back. Run loop seq %llu, retro_run entries %llu, "
                     "last phase \"%s\" (%d).\n",
                     mutex_rc, cond_rc,
                     (unsigned long long)seq,
                     (unsigned long long)core_seq,
                     wd_phase ? wd_phase : "(null)",
                     wd_detail);
            else
               WD_ERR("[PS4] watchdog: the ScePthread object pool is answering again - "
                     "whatever held it has given it back.\n");
         }
      }

      /* ------------------------------- the core, when the run loop is moving and it is not */
      if (core_seq != last_core_seq)
      {
         if (said_idle)
            WD_ERR("[PS4] watchdog: retro_run is being entered again after %llu ms.\n",
                  (unsigned long long)(now - last_core_move));
         last_core_seq  = core_seq;
         last_core_move = now;
         said_idle      = 0;
      }
      else if (seq != last_seq)
      {
         /* ⚠ THE RUN LOOP IS ALIVE AND THE CORE IS NOT BEING CALLED. Reported separately from a
          * stall because it is a different fault: a menu, a pause or a frontend-side loop, not a
          * wedged core. Audio reads as 100% underruns in both cases. */
         uint64_t idle = now - last_core_move;
         if (idle >= ORBIS_WD_IDLE_MS && (idle / ORBIS_WD_IDLE_MS) != said_idle)
         {
            said_idle = idle / ORBIS_WD_IDLE_MS;
            WD_ERR("[PS4] watchdog: the run loop is MOVING (seq %llu) but retro_run has not "
                  "been entered for %llu ms - the core is not being called. Last phase \"%s\" "
                  "(%d), retro_run entries %llu.\n",
                  (unsigned long long)seq,
                  (unsigned long long)idle,
                  wd_phase ? wd_phase : "(null)",
                  wd_detail,
                  (unsigned long long)core_seq);
         }
      }

      if (seq != last_seq)
      {
         /* It moved. If we had complained about it, say that it came back - a hang that
          * recovers is a different bug from one that does not, and the log has to be able
          * to tell them apart after the fact. */
         if (said)
            WD_ERR("[PS4] watchdog: the run loop moved again after %llu ms - now at \"%s\" (%d)\n",
                  (unsigned long long)(now - last_move),
                  wd_phase ? wd_phase : "(null)",
                  wd_detail);
         last_seq  = seq;
         last_move = now;
         said      = 0;
         continue;
      }

      stalled = now - last_move;
      if (stalled < orbis_wd_stall_ms())
         continue;

      /* One line per ORBIS_WD_STALL_MS window, not one per sample. Repeating rather than
       * saying it once is deliberate: an unchanging phase across several reports is what
       * distinguishes a block from a slow frame that eventually finished. */
      if ((stalled / orbis_wd_stall_ms()) == said)
         continue;
      said = stalled / orbis_wd_stall_ms();

      WD_ERR("[PS4] watchdog: the run loop has not moved for %llu ms - last phase \"%s\" (%d), "
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

   /* ⚠ strcmp RATHER THAN A SECOND ENTRY POINT, so no call site outside this file changes. Six
    * marks a frame at sixty frames a second is 360 comparisons of a nine-byte literal per second,
    * which is below the noise of the store above it. */
   if (phase != NULL && phase[0] == 'r' && strcmp(phase, "retro_run") == 0)
      wd_core_seq++;

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
