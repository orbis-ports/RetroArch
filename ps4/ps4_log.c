/* RetroArch - A frontend for libretro. See ps4/ps4_log.h. */

#include <stdio.h>
#include <stdarg.h>

#include <rthreads/rthreads.h>

#include <ps4_app.h>

#include "ps4_log.h"

/* One line at a time, and never more than this. RetroArch's own log lines are bounded
 * well below it; a core's need not be, and a datagram that does not fit is worth
 * truncating rather than growing a buffer on a stack whose size this console decides. */
#define PS4_LOG_LINE_MAX 512

/* ⚠ ONE WRITER AT A TIME, BECAUSE THERE IS MORE THAN ONE THREAD NOW. Since audio got a
 * thread of its own, RARCH_LOG is called from it and from the main loop concurrently, and
 * the channel underneath is not built for that: a hardware capture caught
 *
 *   [INFO] [PS4] Audio up: 48000 Hz, S16 stereo, [INFO] [Audio] Started synchronous...
 *
 * - one line truncated mid-sentence with another spliced into it. Formatting into a local
 * buffer is not enough; the EMIT has to be serialised too. A truncated log line is the
 * kind of evidence that sends somebody after the wrong bug.
 *
 * Created on first use rather than in an initialiser: this file has no init hook, and the
 * first log line happens long before anything could call one. */
static slock_t *ps4_log_lock;

/* ⚠ AND CREATING IT HAD THE RACE THE LOCK EXISTS TO CLOSE.
 *
 *     if (!ps4_log_lock)
 *        ps4_log_lock = slock_new();
 *
 * Two threads reaching that at the same time both see NULL, both call slock_new(), and both
 * proceed - each holding a DIFFERENT mutex, so neither excludes the other. One of the two
 * pointers is then leaked and, worse, whichever store lands second is the one every later
 * caller uses, so a thread already inside the critical section is holding an object nobody
 * will ever lock again. That is the spliced-line symptom above, reproduced by the fix for it,
 * and it is worse than no lock because it looks correct.
 *
 * ⚠ WHY A COMPARE-EXCHANGE AND NOT AN INIT HOOK, pthread_once, OR A STATIC INITIALISER.
 *
 *   * An init hook exists now - frontend_orbis_init() calls orbis_install_crash_handlers() -
 *     but it runs well after the first log line: the boot banner, the run-config note and
 *     orbis-compat's own interposers all log before the frontend driver is reached, and the
 *     thread census shows threads already created by then. A hook would leave the lazy path
 *     in place AND add an ordering assumption that nothing enforces, so it would not remove
 *     this race, only make it rarer and harder to see.
 *   * pthread_once and PTHREAD_MUTEX_INITIALIZER are unverifiable on this platform without
 *     hardware. The SDK's <pthread.h> is musl's, but libc.a leaves pthread_mutex_lock and
 *     pthread_once UNDEFINED - they bind to the system's FreeBSD libthr at link time, against
 *     a musl-shaped pthread_mutex_t. rthreads gets away with that because it always calls
 *     pthread_mutex_init first; a static initialiser would be relying on the two libraries
 *     agreeing about the meaning of a zeroed object, which is exactly the kind of assumption
 *     this port keeps finding to be false.
 *   * __atomic_compare_exchange_n needs no header, no platform contract and no ordering: the
 *     loser of the race frees its own mutex and adopts the winner's. It is also the idiom
 *     already used elsewhere under ps4/ (orbis_abort_report.c, orbis_cxa_guard.c).
 *
 * Returns NULL only if slock_new() fails, in which case the caller logs unserialised - a
 * spliced line is better than a lost one. */
static slock_t *ps4_log_lock_get(void)
{
   slock_t *lock = __atomic_load_n(&ps4_log_lock, __ATOMIC_ACQUIRE);
   slock_t *mine;
   slock_t *winner = NULL;

   if (lock)
      return lock;

   if (!(mine = slock_new()))
      return NULL;

   /* `winner` is in/out: on failure the compare-exchange overwrites it with the pointer that
    * was actually stored, which is the one to use. */
   if (__atomic_compare_exchange_n(&ps4_log_lock, &winner, mine,
            false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
      return mine;

   slock_free(mine);
   return winner;
}

/* ⚠ THE FATAL BRANCH CALLS ps4_log_fatal, NOT ps4_log, AND THE DIFFERENCE IS THE WHOLE POINT
 * OF THIS FILE.
 *
 * The two names read as "the careful one" and "the cheap one", and ps4_app.h still describes
 * them that way - "Tagged \"[<app>] ...\" line on BOTH channels" over ps4_log. That stopped
 * being true when orbis-compat introduced klogWanted(). In optional/ps4_app.cpp today:
 *
 *     static void ps4_vlog(...)      { netLine(buf); if (klogWanted()) sceKernelDebugOutText(...); }
 *     void        ps4_log_frame(...) { netLine(buf); if (klogWanted()) sceKernelDebugOutText(...); }
 *
 * - the same two statements, and klogWanted() is `s_frameKlog || orbis_netlog_ready()==0`,
 * which is FALSE on any console whose netlog came up. So both branches of this function were
 * the SAME CHANNEL, and the table at the top of ps4_log.h described nothing - one layer below
 * where it described nothing the first time, and for the same reason: a contract stated in a
 * comment and enforced by nobody.
 *
 * ⚠ MEASURED, NOT REASONED. The 2026-08-30 capture pair carries a real SIGSEGV at 23:27:04:
 *
 *     ps4-udp-20260830-183440.log   4 lines - `fatal: signal 11`, `frame[00..00]`, `rip`, `trapno`
 *     ps4-klog-20260830-183435.log  ZERO occurrences of "fatal:" in the whole 14252-line file
 *
 * The dump was legible that night only because ps4_idle_forever() held the process open long
 * enough for the datagrams to leave. A process the kernel actually kills does not grant that,
 * which is the exact scenario ps4_log.h's two-channel split exists for.
 *
 * ps4_log_fatal writes sceKernelDebugOutText FIRST and unconditionally, then the netlog. It
 * costs 8-15 ms a line; the error path is bounded (9 [ERROR] lines in the 23:26 run of that
 * same capture, 0 in the 23:44 run, plus 4-6 lines of dump) and a dying process can afford it.
 * RARCH_DBG/LOG/WARN/LOG_OUTPUT stay on ps4_log_frame and stay off klog. */
static void ps4_log_emit(const char *line, bool fatal_channel)
{
   slock_t *lock = ps4_log_lock_get();

   if (lock)
      slock_lock(lock);

   if (fatal_channel)
      ps4_log_fatal("%s", line);
   else
      ps4_log_frame("%s", line);

   if (lock)
      slock_unlock(lock);
}

/* ⚠ THE va_list VARIANTS EXIST BECAUSE THE OLD ONES WERE BROKEN, not because the sink
 * needed them. verbosity.h's ORBIS block defined
 *
 *     #define RARCH_LOG_V(tag, fmt, vp) debugNetPrintf(DEBUGNET_DEBUG, tag, fmt, vp)
 *
 * which hands a va_list to a variadic function as an ordinary argument: the callee reads
 * `tag` as its format string and prints the address of the va_list wherever `fmt`'s first
 * conversion falls. Every RARCH_LOG_V/ERR_V/WARN_V call site on this platform printed
 * garbage. Formatting here, with vsnprintf, is the fix. */
static void ps4_log_format(char *buf, size_t len, const char *level,
      const char *tag, const char *fmt, va_list ap)
{
   size_t _len = 0;

   if (level)
      _len += snprintf(buf + _len, len - _len, "%s ", level);
   if (tag && _len < len)
      _len += snprintf(buf + _len, len - _len, "%s", tag);
   if (_len < len)
      vsnprintf(buf + _len, len - _len, fmt, ap);
}

void ps4_rarch_log_v(const char *level, const char *tag,
      const char *fmt, va_list ap)
{
   char line[PS4_LOG_LINE_MAX];
   ps4_log_format(line, sizeof(line), level, tag, fmt, ap);
   ps4_log_emit(line, false);
}

void ps4_rarch_log(const char *level, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   ps4_rarch_log_v(level, NULL, fmt, ap);
   va_end(ap);
}

void ps4_rarch_err_v(const char *level, const char *tag,
      const char *fmt, va_list ap)
{
   char line[PS4_LOG_LINE_MAX];
   ps4_log_format(line, sizeof(line), level, tag, fmt, ap);
   ps4_log_emit(line, true);
}

void ps4_rarch_err(const char *level, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   ps4_rarch_err_v(level, NULL, fmt, ap);
   va_end(ap);
}
