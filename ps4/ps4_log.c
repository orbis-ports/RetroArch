/* RetroArch - A frontend for libretro. See ps4/ps4_log.h. */

#include <stdio.h>
#include <stdarg.h>

#include <ps4_app.h>

#include "ps4_log.h"

/* One line at a time, and never more than this. RetroArch's own log lines are bounded
 * well below it; a core's need not be, and a datagram that does not fit is worth
 * truncating rather than growing a buffer on a stack whose size this console decides. */
#define PS4_LOG_LINE_MAX 512

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
   ps4_log_frame("%s", line);
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
   ps4_log("%s", line);
}

void ps4_rarch_err(const char *level, const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   ps4_rarch_err_v(level, NULL, fmt, ap);
   va_end(ap);
}
