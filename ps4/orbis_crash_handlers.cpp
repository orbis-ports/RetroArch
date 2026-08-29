/* RetroArch - A frontend for libretro.
 *
 * Installs orbis-compat's crash handlers, which this frontend was never asking for.
 *
 * ⚠ THE HANDLERS EXISTED AND NOBODY HAD CONNECTED THEM. orbis-compat has carried
 * orbis::installCrashHandlers() - a std::terminate handler plus SA_SIGINFO handlers for
 * SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT on an alternate stack - for as long as this port has
 * existed, and OpenGothic calls it. RetroArch does not, so a SIGSEGV here has always killed the
 * process with nothing written down.
 *
 * ⚠ THAT WAS MISREAD FOR A WHOLE SESSION, AND THE MISREADING IS WORTH RECORDING. The evidence was
 * "zero `fatal: signal` lines in any log this project has captured", and the conclusion drawn from
 * it was that the crash reporter ran and died inside itself. Half right: the handler DID have a
 * fatal bug - the SDK's SA_SIGINFO is Linux's 4 while this kernel reads 4 as SA_RESETHAND, so it
 * was installed one-shot and without siginfo, and orbis_boot.cpp's first read of info->si_code
 * faulted. But that only ever affected the titles that INSTALLED it. Every one of those zero
 * lines from RetroArch had a duller cause: nothing was listening at all. Two different faults,
 * one symptom, and the symptom was read as one fault.
 *
 * The signal-number-only fallback in orbis_boot.cpp and the corrected SA_* values in
 * orbis-compat/include/signal.h are the other half; this file is what makes either reach a
 * RetroArch crash.
 *
 * ⚠ AND IT DOES NOT REPLACE ps4/orbis_abort_report.c, which catches a different class entirely:
 * abort_message() and __assert_fail() are CALLS a dying library makes on purpose, not signals.
 * That is why `libc++abi: terminating with uncaught exception` has been reaching the log while
 * `fatal: signal` never has - one path was wired and the other was not.
 *
 * ⚠⚠ AND INSTALLING THEM IS ONLY HALF: THE OVERLAY'S LOGGER HAS TO BE POINTED SOMEWHERE.
 * orbis_log.h says it plainly - orbis_log() is "a no-op if nothing was registered" and
 * orbis_log_fatal() writes "through the fatal logger, or through the ordinary one if none was
 * registered, or NOWHERE". RetroArch registers neither, because it logs through ps4_log, which is
 * a different channel in the same library. So a first attempt at this file installed the handlers
 * and changed nothing observable: no "crash handlers installed" at boot and no "fatal: signal" on
 * a real SIGSEGV, because both lines were being written to a null sink.
 *
 * ⚠ AND THE FATAL SINK IS klog, NOT THE UDP ONE, ON PURPOSE. ps4_rarch_log_v is UDP-only, and a
 * datagram from a process the kernel is about to kill may never leave the machine - which is the
 * exact reason orbis_log.h separates the two. ps4_rarch_err_v writes klog as well: it costs 8-15 ms
 * a line, and a dying process can afford that once.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>

#include <orbis_boot.h>
#include <orbis_log.h>

#include "ps4_log.h"

static void orbis_crash_log_sink(const char *fmt, va_list ap)
{
   ps4_rarch_log_v("INFO", "[PS4] ", fmt, ap);
}

static void orbis_crash_fatal_sink(const char *fmt, va_list ap)
{
   ps4_rarch_err_v("ERROR", "[PS4] ", fmt, ap);
}

extern "C" void orbis_install_crash_handlers(void)
{
   /* The sinks first: installCrashHandlers() reports what it managed, and that report is the only
    * evidence that any of this is wired up. */
   orbis_set_log(orbis_crash_log_sink);
   orbis_set_log_fatal(orbis_crash_fatal_sink);
   orbis::installCrashHandlers();
}
