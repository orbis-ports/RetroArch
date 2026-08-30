/* RetroArch - A frontend for libretro.
 *
 * __cxa_thread_atexit_impl - the destructor hook this SDK's libc++ calls and its libc never
 * defines.
 *
 * ⚠ THE HOLE IS BETWEEN TWO LIBRARIES THE SDK SHIPS, NOT IN ANY CORE. A `thread_local` object
 * with a non-trivial destructor makes the compiler emit a call to __cxa_thread_atexit, which
 * lives in libc++.a; that function is a three-line forwarder to __cxa_thread_atexit_impl, which
 * is the C library's job everywhere else (glibc and musl both export it) and which this libc
 * does not have. So the reference only appears once a core uses such an object, and it appears
 * as a hole in the toolchain rather than as anything the core did wrong:
 *
 *     ld.lld: error: undefined symbol: __cxa_thread_atexit_impl
 *     >>> referenced by cxa_thread_atexit.cpp
 *     >>>               cxa_thread_atexit.cpp.o:(__cxa_thread_atexit) in archive
 *     >>>               .../openorbis/lib/libc++.a
 *
 * flycast is the first core here to hit it. Anything with a thread_local std::string, a
 * thread_local container, or a per-thread RAII logger will hit the same one.
 *
 * ⚠ AND A NO-OP WOULD BE THE WRONG ANSWER, WHICH IS WHY THIS FILE IS 90 LINES AND NOT 3.
 * Returning 0 without registering anything makes the link succeed and silently stops every
 * thread-local destructor in the module from ever running - std::string buffers, file handles
 * and mutexes leaked once per thread, forever, with no symptom to trace it back by. The
 * contract is small enough to actually implement, so it is implemented.
 *
 * The implementation is the one glibc documents: destructors run on the thread that registered
 * them, in reverse order of registration, at thread exit. pthread_key_create's own destructor
 * callback is exactly that hook, and this SDK's libc has the whole pthread_key family
 * (pthread_key_create.lo, pthread_setspecific.lo and pthread_once.lo are all in libc.a).
 *
 * ⚠ ONE DOCUMENTED GAP: THE MAIN THREAD. POSIX runs key destructors when a thread returns from
 * its start routine or calls pthread_exit, and the initial thread of a process does neither - it
 * falls off the end of main() or calls exit(). So a thread_local destructor registered on the
 * main thread will not run at process exit. That matches what glibc-based systems do for
 * pthread_key destructors and differs from what they do for __cxa_thread_atexit_impl, which is
 * wired into the thread teardown path proper. On a console where the module is unloaded with the
 * process this costs nothing observable; it is written down because it is a real difference.
 *
 * ⚠ THE `dso_handle` ARGUMENT IS IGNORED ON PURPOSE. It exists so a dlclose() can refuse to
 * unload a library that still has live thread-locals. Nothing here is ever dlclose()d - a core
 * is linked into one static module - so there is no unload for it to guard.
 *
 * This file is compiled into the core-build support archive, so a module picks it up only if it
 * has the symbol undefined and nothing else provides it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdlib.h>

struct orbis_thread_dtor
{
   void                     (*func)(void *);
   void                      *obj;
   struct orbis_thread_dtor  *next;
};

static pthread_key_t  orbis_thread_dtor_key;
static pthread_once_t orbis_thread_dtor_once = PTHREAD_ONCE_INIT;

/* Reverse order of registration, which is what a stack gives for free: each new entry is pushed
 * at the head below, so walking the list forward here unwinds it. */
static void orbis_thread_dtor_run(void *head)
{
   struct orbis_thread_dtor *d = (struct orbis_thread_dtor*)head;

   while (d)
   {
      struct orbis_thread_dtor *next = d->next;
      d->func(d->obj);
      free(d);
      d = next;
   }
}

static void orbis_thread_dtor_init(void)
{
   pthread_key_create(&orbis_thread_dtor_key, orbis_thread_dtor_run);
}

int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_handle);

int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_handle)
{
   struct orbis_thread_dtor *d;

   (void)dso_handle;

   if (pthread_once(&orbis_thread_dtor_once, orbis_thread_dtor_init) != 0)
      return -1;

   if (!(d = (struct orbis_thread_dtor*)malloc(sizeof(*d))))
      return -1;

   d->func = func;
   d->obj  = obj;
   d->next = (struct orbis_thread_dtor*)pthread_getspecific(orbis_thread_dtor_key);

   /* ⚠ pthread_setspecific IS WHAT ARMS THE DESTRUCTOR, so its failure has to unwind this entry
    * rather than leave it linked into a list nothing will ever walk. */
   if (pthread_setspecific(orbis_thread_dtor_key, d) != 0)
   {
      free(d);
      return -1;
   }

   return 0;
}
