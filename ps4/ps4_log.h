/* RetroArch - A frontend for libretro.
 *
 * The PS4 log sink behind RARCH_LOG and friends.
 *
 * ⚠ THIS REPLACED debugnet, which is orbisdev's and which verbosity.h included
 * unconditionally under #ifdef ORBIS. See ps4-mesa-docs docs/retroarch/PLAN.md section 1.
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
 *
 * ⚠ AND FOR A LONG TIME THAT TABLE WAS A DESCRIPTION OF NOTHING. The macros that were
 * meant to enforce it lived inside verbosity.h's `#if defined(HAVE_LOGGER)` block, and
 * HAVE_LOGGER is only set by HAVE_NETLOGGER=1, which Makefile.orbis does not set. So
 * every RARCH_LOG and every RARCH_ERR went to verbosity.c's ordinary sink instead -
 * fprintf(stderr), i.e. klog and only klog - while the ps4_log() calls that orbis-compat
 * makes went to UDP and only UDP. Two disjoint captures, and the last words of a dying
 * process in whichever one the reader was not grepping. Measured on the 2026-08-30 pair:
 * 1305 [INFO] lines in the klog capture, zero in the netlog capture beside it.
 *
 * The table is true now because verbosity.c has an ORBIS arm in RARCH_LOG_V and a real
 * RARCH_ERR_V that call the four functions below. Nothing in verbosity.h routes PS4 log
 * lines any more; if you are looking for where a RARCH_LOG line goes, look there.
 *
 * ⚠ AND THE SECOND HALF OF THE TABLE WAS FALSE FOR A SECOND, INDEPENDENT REASON. Wiring
 * RARCH_ERR to ps4_rarch_err() only moves the line to whichever orbis-compat entry point
 * ps4_log_emit() picks, and it picked ps4_log() - which has been netlog-only on a console
 * since orbis-compat introduced klogWanted(). The klog half of the error channel therefore
 * existed on paper and nowhere else: zero "fatal:" lines in the 2026-08-30 klog capture
 * against four in the netlog capture beside it, for a real SIGSEGV. ps4_log_emit() calls
 * ps4_log_fatal() now; the note there carries the measurement.
 *
 * ⚠ AND A MISSING "[retroarch] " TAG IS NOT EVIDENCE OF A BYPASS. orbis-compat's tagLine()
 * prefixes the tag ONCE, to the front of the buffer, and RetroArch hands it messages that
 * already contain newlines - retroarch.c's build banner builds "Capabilities: ...\n[INFO]
 * Version: ...\n[INFO] Git: ...\n[INFO] Built: ...\n" and passes the lot to one
 * RARCH_LOG_OUTPUT. That is ONE datagram; the receiver splits it, so lines 2..n arrive
 * untagged and with the first line's timestamp to the millisecond. Three such lines per run
 * in the 2026-08-30 netlog capture, all at 23:44:52.183 with the Capabilities line. They went
 * through the sink; only the tag did not repeat.
 *
 * ⚠ NOT EVERY PS4 LINE COMES THROUGH HERE, AND THAT IS DELIBERATE. ps4/orbis_report.h is a
 * separate, klog-first channel for code linked into CORES, where log_cb is NULL until after
 * global constructors and stderr goes nowhere - ps4/orbis_exec_mem.c is its main caller, six
 * lines per core load. Those lines are klog-ONLY, so a reader watching the netlog alone will
 * not see them. That is a property of the reader's setup, not a leak in this sink.
 */

#ifndef PS4_LOG_H__
#define PS4_LOG_H__

#include <stdarg.h>
#include <boolean.h>
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
