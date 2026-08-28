/* The one channel a core can report on when the process is about to die.
 *
 * ⚠ WHY NOT stderr, AND WHY NOT log_cb. A core's stderr goes nowhere on this console - fd 2 is
 * unwired and dup2 onto it returns EPERM, so redirecting it is not available either (measured
 * 2026-08-25, frontend_orbis_capture_stderr). libretro's log_cb does reach the frontend, but it
 * is NULL until the core has been given one, which is after the point where a module's global
 * constructors run - exactly where the failures this exists for happen.
 *
 * So this writes sceKernelDebugOutText first and unconditionally. It is synchronous: it has
 * returned by the time the call returns, which a UDP datagram cannot promise from a process
 * three instructions from abort(). It also costs 8-15 ms a line on this console, so it is for
 * initialisation and failure and nothing that repeats.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ORBIS_REPORT_H__
#define ORBIS_REPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Tagged, formatted, klog first and then /data/retroarch-abort.log. */
void orbis_report(const char *tag, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif
