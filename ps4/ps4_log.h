/* RetroArch - A frontend for libretro.
 *
 * The PS4 log sink behind RARCH_LOG and friends.
 *
 * ⚠ THIS REPLACED debugnet, which is orbisdev's and which verbosity.h included
 * unconditionally under #ifdef ORBIS. See ps4/PLAN.md section 1.
 *
 * ⚠ AND IT SPLITS THE CHANNEL IN TWO, WHICH debugnet DID NOT. The console's klog
 * (sceKernelDebugOutText) costs 8-15 ms per line, measured - it blocks the calling
 * thread until something drains the debug channel. A frontend that puts RARCH_LOG on
 * klog stops being a frontend. So:
 *
 *   RARCH_DBG / RARCH_LOG / RARCH_WARN / RARCH_LOG_OUTPUT  -> UDP datagram only
 *   RARCH_ERR                                              -> klog AND UDP
 *
 * The error path pays the cost on purpose: a line describing a process about to die
 * cannot go out over UDP alone, because sceNetSendto hands the datagram to a stack that
 * needs the process to survive long enough for it to leave.
 */

#ifndef PS4_LOG_H__
#define PS4_LOG_H__

#include <stdarg.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* UDP only. Safe from a render loop. */
void ps4_rarch_log(const char *level, const char *fmt, ...);
void ps4_rarch_log_v(const char *level, const char *tag,
      const char *fmt, va_list ap);

/* klog + UDP. For the lines that describe something going wrong. */
void ps4_rarch_err(const char *level, const char *fmt, ...);
void ps4_rarch_err_v(const char *level, const char *tag,
      const char *fmt, va_list ap);

RETRO_END_DECLS

#endif
