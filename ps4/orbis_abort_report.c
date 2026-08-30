/* The message a core prints on its way to abort(), on a console where stderr goes nowhere.
 *
 * ⚠ WHY THIS FILE EXISTS AT ALL, AND WHY THE OBVIOUS FIX DOES NOT WORK.
 *
 * assert(), libc++abi's abort_message() and __cxa_pure_virtual all say exactly what went wrong
 * and then call abort(). Every one of those messages goes to stderr, and on this kernel fd 2
 * goes nowhere. What reaches the log is a fatal signal with SIGABRT in %rsi and a backtrace
 * that stops at abort() - a crash with its own explanation discarded.
 *
 * The obvious fix is to point fd 2 somewhere. It is not available: dup2 onto 1 or 2 returns
 * EPERM on this kernel (measured 2026-08-25, frontend_orbis_capture_stderr). The descriptor
 * table is not ours to rearrange, so the message has to be caught before it is written rather
 * than after.
 *
 * ⚠ SO IT IS CAUGHT BY DEFINITION ORDER, WHICH IS THE PART WORTH UNDERSTANDING. Both
 * abort_message and __assert_fail live in archive members of their own - abort_message.cpp.o in
 * libc++.a, assert.lo in libc.a - and each member defines that one symbol and nothing else. An
 * archive member is pulled only for a symbol still undefined, so a definition that reaches the
 * linker FIRST means the toolchain's member is never pulled and there is no duplicate. The core
 * support archive is on the link line ahead of -lc and -lc++, which is what makes this work; see
 * ps4/build-cores.sh. Overriding a symbol whose member defined anything else would be a
 * different and much worse idea.
 *
 * ⚠ AND IT REPORTS THROUGH klog, NOT THE NICE CHANNEL. sceKernelDebugOutText is synchronous -
 * it has returned by the time the call returns - while a UDP datagram needs the process to live
 * long enough for the network stack to send it, and this process is three instructions from
 * abort(). The same argument orbis-compat's ps4_log_fatal makes, for the same reason. The file
 * is written second, because it is the copy that can be read later at leisure.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "orbis_report.h"

#define ORBIS_ABORT_LOG "/data/retroarch-abort.log"

/* Re-entrancy flag for the file copy below; see the note at the fopen. */
static int s_in_report;

void orbis_report(const char *tag, const char *fmt, ...)
{
   char    line[1024];
   size_t  n;
   va_list ap;
   FILE   *f;

   n = (size_t)snprintf(line, sizeof(line), "[orbis] %s: ", tag);
   va_start(ap, fmt);
   vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
   va_end(ap);
   n = strlen(line);
   line[n]     = '\n';
   line[n + 1] = '\0';

   /* First, and unconditionally. */
   sceKernelDebugOutText(0, line);

   /* ⚠ APPEND, NOT TRUNCATE. A core can abort during the menu's info read and again when it is
    * loaded for real, and the first message is usually the one that explains the second.
    *
    * ⚠ AND GUARDED, BECAUSE THE FILE COPY IS THE ONLY PART OF THIS THAT CAN COME BACK HERE.
    * fopen reaches orbis-compat's interposed open(), which calls anchorPath(), which calls
    * anchorRoot() - a function-local static. Today that is a cycle on paper and not in practice:
    * anchorPath returns on the first character of an ABSOLUTE path, before it ever touches the
    * guard, and ORBIS_ABORT_LOG is absolute. From anchorPath in a linked core:
    *
    *     80e5e:  cmpl   $0x2f, %eax           ; '/'
    *     80e61:  je     0x80f39               ; ...returns here
    *     80e6d:  movzbl _ZGVZN5orbis10anchorRootEvE4root(%rip), %eax   ; guard, only after that
    *
    * So this is defence in depth against the day someone gives it a relative path or another
    * interposer grows a static of its own - and the day is worth guarding against, because the
    * consequence changed. It used to be a second abort message; with ps4/orbis_cxa_guard.c
    * answering the guards it would be a DEADLOCK, the process parked on a condition variable
    * waiting for an initializer that is itself waiting for this report to finish.
    *
    * klog above is deliberately outside the guard: sceKernelDebugOutText is a direct kernel call
    * that cannot re-enter anything here, so the message still gets out even when the file copy is
    * skipped. A global rather than a thread-local flag - two threads reporting at once lose one
    * FILE copy and keep both klog lines, and a thread-local in the abort path would be one more
    * thing that has to work while the process is dying. */
   if (__atomic_exchange_n(&s_in_report, 1, __ATOMIC_ACQ_REL) == 0)
   {
      if ((f = fopen(ORBIS_ABORT_LOG, "a")))
      {
         fputs(line, f);
         fflush(f);
         fclose(f);
      }
      __atomic_store_n(&s_in_report, 0, __ATOMIC_RELEASE);
   }
}

/* libc++abi's. Reached by std::terminate, an uncaught exception, __cxa_pure_virtual, a failed
 * dynamic_cast and every other place the C++ runtime gives up. The format string carries the
 * whole diagnosis - "terminating with uncaught exception of type std::bad_alloc", and so on. */
void abort_message(const char *format, ...)
{
   char    msg[960];
   va_list ap;

   va_start(ap, format);
   vsnprintf(msg, sizeof(msg), format, ap);
   va_end(ap);

   orbis_report("libc++abi", "%s", msg);
   abort();
}

/* musl's. ⚠ STILL REACHABLE DESPITE -DNDEBUG: that define is per translation unit, and a core
 * built with it links against libretro-common, orbis-compat and vendored dependencies that were
 * not. An assertion firing in one of those looks identical to a C++ runtime failure from the
 * outside - both arrive as SIGABRT - which is why both are named here. */
void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
   char msg[960];

   snprintf(msg, sizeof(msg), "%s:%d: %s: assertion failed: %s",
         file ? file : "?", line, func ? func : "?", expr ? expr : "?");

   orbis_report("assert", "%s", msg);
   abort();
}

/* ⚠ AND THE ONE THAT MATTERS WHEN THE OTHER TWO ARE NEVER REACHED.
 *
 * Plenty of code calls abort() directly and says nothing first - libunwind's _LIBUNWIND_ABORT
 * writes to stderr and aborts without going near abort_message, GNU lightning aborts on an
 * unencodable instruction, paraLLEl-RSP's allocator aborts when its arena is exhausted. All of
 * those arrive as a bare SIGABRT with the module's abort() as the only frame, because a core is
 * built -fomit-frame-pointer and the console's backtracer cannot walk through it. mupen64plus-next
 * did exactly that twice before this existed.
 *
 * So the one thing this must produce is the RETURN ADDRESS - who called abort. That is on the
 * stack whether or not there is a frame pointer, which is what makes it available when a
 * backtrace is not.
 *
 * ⚠ AND A SECOND ADDRESS WITH IT, because the first is useless on its own: a module is loaded
 * wherever the kernel puts it, and nothing in the log says where. Printing the address of a
 * function from THIS file alongside it gives the reader both ends - subtract its known offset in
 * the .elf and the module base falls out, and with it the offset to symbolize.
 *
 * ⚠ musl's abort.lo ALSO DEFINES __abort_lock, so overriding abort() means that member is never
 * pulled and anything else referencing that symbol would fail to link. It is defined here for
 * that reason; if it ever collides, the collision is the loud kind and easy to unpick.
 */
volatile int __abort_lock[1];

void abort(void)
{
   char  msg[512];
   void *ra = __builtin_return_address(0);

   snprintf(msg, sizeof(msg),
         "abort() called from %p  (orbis_report is at %p in this image - "
         "subtract its .elf offset for the module base)",
         ra, (void*)&orbis_report);
   orbis_report("abort", "%s", msg);

   /* Die the way the platform expects, so the crash dump and the reason code stay comparable
    * with every abort that happened before this file existed. */
   raise(SIGABRT);
   _Exit(134);
}
