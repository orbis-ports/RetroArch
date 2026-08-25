/* std::condition_variable, rebuilt against this platform's errno values.
 *
 * ⚠ THE BUG IS IN THE TOOLCHAIN'S PREBUILT libc++.a, NOT IN ANY CORE. Measured on hardware
 * 2026-08-25, mupen64plus-next loading a ROM:
 *
 *     [orbis] libc++abi: terminating with uncaught exception of type std::__1::system_error:
 *             condition_variable timed_wait failed: Operation timed out
 *
 * "Operation timed out" IS errno 60, ETIMEDOUT, and a timeout is the ordinary outcome of a timed
 * wait - wait_for is supposed to return cv_status::timeout, not throw. It threw because the
 * comparison that filters that case out was compiled against a different number. Disassembling
 * condition_variable.cpp.o from libc++.a:
 *
 *     322: 83 7d 94 6e    cmpl $0x6e, -0x6c(%rbp)     ; 0x6e = 110
 *
 * 110 is Linux's ETIMEDOUT. This console is FreeBSD underneath and its pthread returns 60, which
 * the SDK's own bits/errno.h agrees with. So libc++ was built against Linux errno values and
 * shipped to a FreeBSD target, and `ec != ETIMEDOUT` is true for every timeout there has ever
 * been on this platform.
 *
 * ⚠ AND THE CONSEQUENCE IS NOT LIMITED TO condition_variable. std::future::wait_for,
 * shared_timed_mutex and everything else that waits with a deadline goes through
 * __do_timed_wait, so on this console a C++ core that waits on anything with a timeout dies the
 * first time the timeout expires. That is a normal thing for a worker queue to do while a ROM is
 * being parsed and nothing is being submitted yet.
 *
 * ⚠ WHY NOT FIX IT AT pthread_cond_timedwait INSTEAD, which would be one function rather than
 * four. Because the two callers disagree and translating breaks the honest one: everything
 * compiled in this workshop sees ETIMEDOUT as 60 through the SDK's headers and tests for 60,
 * while only the prebuilt library expects 110. Rewriting the return value to 110 would fix
 * libc++ by breaking every direct caller. The mismatch belongs where it was introduced.
 *
 * ⚠ SO ALL FIVE STRONG SYMBOLS OF THE MEMBER HAVE TO BE HERE, not just the broken one.
 * condition_variable.cpp.o defines notify_one, notify_all, wait, __do_timed_wait and
 * notify_all_at_thread_exit, and an archive member is all-or-nothing: providing our own
 * __do_timed_wait means the member is never pulled and the other four would go undefined. They
 * are reproduced from libc++ 11's own source (_LIBCPP_VERSION 11000, which is what this SDK
 * ships) so the only thing that differs is the constant. notify_all_at_thread_exit is the
 * exception and is deliberately absent - it needs libc++'s private per-thread bookkeeping, and
 * no core here has ever referenced it. If one does, the link says so by name.
 *
 * The real fix is a libc++ rebuilt against the target's headers, which is an OpenOrbis SDK
 * change rather than ours.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>

#include <condition_variable>
#include <limits>
#include <system_error>

_LIBCPP_BEGIN_NAMESPACE_STD

void condition_variable::notify_one() _NOEXCEPT
{
   __libcpp_condvar_signal(&__cv_);
}

void condition_variable::notify_all() _NOEXCEPT
{
   __libcpp_condvar_broadcast(&__cv_);
}

void condition_variable::wait(unique_lock<mutex>& lk) _NOEXCEPT
{
   if (!lk.owns_lock())
      __throw_system_error(EPERM, "condition_variable::wait: mutex not locked");

   int ec = __libcpp_condvar_wait(&__cv_, lk.mutex()->native_handle());
   if (ec)
      __throw_system_error(ec, "condition_variable wait failed");
}

void condition_variable::__do_timed_wait(unique_lock<mutex>& lk,
      chrono::time_point<chrono::system_clock, chrono::nanoseconds> tp) _NOEXCEPT
{
   using namespace chrono;

   if (!lk.owns_lock())
      __throw_system_error(EPERM, "condition_variable::timed wait: mutex not locked");

   nanoseconds d = tp.time_since_epoch();

   /* Upstream's clamp, kept verbatim: a deadline further out than this overflows the timespec
    * on some platforms, and 0x59682F000000E941 ns is roughly the year 2262. */
   if (d > nanoseconds(0x59682F000000E941))
      d = nanoseconds(0x59682F000000E941);

   timespec ts;
   seconds  s = duration_cast<seconds>(d);
   typedef decltype(ts.tv_sec) ts_sec;
   const ts_sec ts_sec_max = numeric_limits<ts_sec>::max();

   if (s.count() < ts_sec_max)
   {
      ts.tv_sec  = static_cast<ts_sec>(s.count());
      ts.tv_nsec = static_cast<decltype(ts.tv_nsec)>((d - s).count());
   }
   else
   {
      ts.tv_sec  = ts_sec_max;
      ts.tv_nsec = 999999999;
   }

   int ec = __libcpp_condvar_timedwait(&__cv_, lk.mutex()->native_handle(), &ts);

   /* ⚠ THE ONE LINE THIS FILE EXISTS FOR. ETIMEDOUT here is the platform's 60, because this
    * translation unit is compiled against the SDK's headers rather than against Linux's. */
   if (ec != 0 && ec != ETIMEDOUT)
      __throw_system_error(ec, "condition_variable timed_wait failed");
}

_LIBCPP_END_NAMESPACE_STD
