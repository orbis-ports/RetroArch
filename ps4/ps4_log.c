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

static void ps4_log_emit(const char *line, bool fatal_channel)
{
   if (!ps4_log_lock)
      ps4_log_lock = slock_new();

   if (ps4_log_lock)
      slock_lock(ps4_log_lock);

   if (fatal_channel)
      ps4_log("%s", line);
   else
      ps4_log_frame("%s", line);

   if (ps4_log_lock)
      slock_unlock(ps4_log_lock);
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
