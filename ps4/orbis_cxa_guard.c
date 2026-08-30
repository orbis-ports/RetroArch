/* RetroArch - A frontend for libretro.
 *
 * __cxa_guard_acquire / __cxa_guard_release / __cxa_guard_abort - the C++ runtime's
 * function-local-static guards, replacing libc++abi's on this platform.
 *
 * ⚠ THIS IS A CORRECTION, NOT A HACK, AND THE NEXT PERSON TO FIND A HAND-WRITTEN
 * __cxa_guard_acquire IN A GAME CONSOLE PORT IS RIGHT TO BE SUSPICIOUS. So: the measurement.
 *
 * melondsds aborted on hardware 6.5 seconds into loading content, with
 *
 *     [orbis] libc++abi: __cxa_guard_acquire detected recursive initialization
 *
 * and there is no recursive initialization. Nothing in that module initialises a
 * function-local static from its own initializer: all 34 guard variables were enumerated and
 * all 39 acquire sites mapped to their guards, and a whole-module call graph (16410 functions,
 * 23460 direct-call edges) has exactly one guard-owning function reachable from itself - and
 * only along the abort path, which is unreachable in normal execution. The message is a FALSE
 * POSITIVE, and this is how it is produced.
 *
 * libc++abi detects recursion by storing the initialising thread's id in the guard and
 * comparing it against the caller's. In melondsds.elf:
 *
 *     9eba0:  cmpl  $0x0, %edx                       ; init byte PENDING - someone is running it
 *     9ebbd:  callq LazyValue<PlatformThreadID>::get()
 *     9ebd2:  cmpl  (%rcx), %eax                     ; that thread's id == mine ?
 *     9ebe3:  callq abort_message                    ; -> "detected recursive initialization"
 *
 * and PlatformThreadID, in full:
 *
 *     000000000009eae0 <__cxxabiv1::(anonymous namespace)::PlatformThreadID()>:
 *       9eae4:  movl  $0x14, %edi                    ; syscall 20
 *       9eaeb:  callq syscall
 *       9eaf1:  retq
 *
 * Syscall 20 is getpid, because the SDK's own headers say so:
 *
 *     openorbis/include/bits/syscall.h:18    #define __NR_getpid  20
 *     openorbis/include/bits/syscall.h:410   #define __NR_gettid  __NR_getpid
 *     openorbis/include/bits/syscall.h:827   #define SYS_gettid   __NR_getpid
 *
 * This kernel has no gettid, so musl's headers alias it to getpid - a reasonable thing for a
 * header to do, and a disaster for the one caller that wanted the two to differ. libc++abi's
 * cxa_guard_impl.h takes its `#if defined(SYS_gettid)` arm, believes the platform has thread
 * ids, and gets THE PROCESS ID - the same value on every thread. So "the thread initialising
 * this static is me" is true for ANY two threads, and the first time two of them race on the
 * same function-local static - the ordinary case guard variables exist to serialise - the
 * second one aborts instead of waiting.
 *
 * ⚠ AND FIXING THE SDK HEADER WOULD NOT HELP, WHICH IS THE WHOLE REASON THIS FILE EXISTS.
 * libc++.a is shipped PREBUILT. `#define SYS_gettid __NR_getpid` was read when Sony's libc++
 * was compiled and the syscall number is baked into cxa_guard.cpp.o; deleting the line today
 * changes nothing until somebody rebuilds libc++ from source. Overriding the three entry
 * points is the only fix available from this repository.
 *
 * ⚠ SO IT IS CAUGHT BY DEFINITION ORDER, exactly as ps4/orbis_abort_report.c catches abort()
 * and __assert_fail - read that file for the mechanism in full. The precondition it warns
 * about ("overriding a symbol whose member defined anything else would be a different and much
 * worse idea") was checked before this was written:
 *
 *     libc++.a(cxa_guard.cpp.o):  T __cxa_guard_acquire  T __cxa_guard_release
 *                                 T __cxa_guard_abort    ... and nothing else strong
 *
 * Three strong symbols, all three defined here, so that member is never pulled and there is no
 * duplicate. The core support archive and the frontend's own object list both sit ahead of
 * -lc++ on their link lines.
 *
 * ⚠ ONLY THE IDENTITY IS CHANGED. THE LOCKING IS libc++abi's, DELIBERATELY.
 *
 * The synchronisation below - one process-wide statically-initialised mutex and condition
 * variable - is the same shape as libc++abi's InitByteGlobalMutex, and that is a decision
 * rather than an accident: the failing run proves those primitives WORK here. The abort came
 * from inside libc++abi's own LockGuard, so a statically zero-initialised pthread_mutex_t was
 * locked successfully on this kernel before the wrong comparison was reached. Replacing the
 * one broken part and keeping every part that is known to work is the smallest change that
 * fixes it; a lock-free rewrite would be a second, unmeasured thing to be wrong about.
 *
 * ⚠ THE THREAD IDENTITY IS %fs:0, AND IT IS THE ONE CANDIDATE THAT IS PROVEN ON HARDWARE.
 *
 * That is the musl TCB self-pointer: on x86-64 the thread pointer lives in %fs, and the first
 * word of the thread control block points at the block itself, so one instruction yields an
 * address that is different for every thread by construction.
 *
 * ⚠ AND IT IS NOT AN ASSUMPTION HERE - IT IS IN THE FAILING RUN'S OWN EVIDENCE. This module's
 * stdio does exactly this, on every character:
 *
 *     000000000008acd4 <__lockfile>:
 *       8acdf:  movq  %fs:0x0, %rcx        ; the TCB
 *       8ace8:  movl  0x38(%rcx), %edx     ; ...this thread's id inside it
 *
 * and /data/retroarch-abort.log exists on the console, written by fputs through that very
 * function. If %fs:0 read the same on every thread, __lockfile's owner check would be broken
 * and stdio would have been corrupting itself process-wide long before this. The console
 * wrote the file, so %fs:0 is a real per-thread pointer here.
 *
 * The alternatives were all worse:
 *
 *   - syscall(SYS_gettid) is the bug.
 *   - pthread_self() is an UNRESOLVED IMPORT here (`U pthread_self` in the module, defined in
 *     none of libc.a, libkernel.a or libc++.a - the console's PRX supplies it at load). It is
 *     probably fine. "Probably fine" about a thread identity is what caused this, and it costs
 *     an import in every core that links this file.
 *   - The address of a __thread byte of our own is distinct per thread too, but it compiles to
 *     general-dynamic TLS - `U __tls_get_addr` - and that goes THROUGH %fs:0 anyway to reach
 *     the DTV at 0x8(%rax), so it assumes everything below plus a working DTV, and adds a call
 *     into libc from inside the C++ runtime's own bootstrap. Strictly more to be wrong about
 *     for the same answer. (Measured: no object in melondsds references __tls_get_addr, so
 *     that path is not exercised by any core today and would be new ground.)
 *
 * ⚠ AND IF IT EVER READS 0 IT IS TREATED AS "NO IDENTITY" rather than as a match - see
 * owner_is_self. A thread that somehow reached a static initializer without a TCB loses
 * recursion detection and waits, which is the safe direction.
 *
 * ⚠ AND IT IS C, NOT C++, FOR THE SAME REASON. A function-local static anywhere in this
 * translation unit would compile into a call to the function below it.
 *
 * ⚠ WHERE THE DETECTION DEGRADES, IT DEGRADES TOWARDS WAITING. If the owner table is full, or
 * the thread anchor reads as 0, the recursion check is skipped and the caller waits. A missed
 * detection turns a genuine recursive initializer - a real C++ bug, and a rare one - into a
 * hang. A false detection kills a working core. Those are not equally bad, and the fallback
 * picks the survivable one; it is also what libc++abi itself does on a platform with no thread
 * id at all, where PlatformThreadID is a null pointer and the check is compiled out.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

/* Declared rather than included: this file is linked into BOTH the frontend and every core,
 * and the frontend's ps4 objects do not carry the SDK's <orbis/...> headers on their include
 * path. One prototype is cheaper than changing that. */
int32_t sceKernelDebugOutText(int32_t channel, const char *text, ...);

/* ⚠ BYTE 0 IS THE ABI, AND THE REST OF THE GUARD IS OURS.
 *
 * The Itanium C++ ABI guard is 8 bytes (llvm-nm confirms: every _ZGV* in the module has size
 * 8), and the compiler inlines a fast path that reads THE FIRST BYTE and skips the call
 * entirely when it is non-zero. From anchorPath in melondsds.elf:
 *
 *     80e6d:  movzbl 0x4e3674(%rip), %eax   # _ZGVZN5orbis10anchorRootEvE4root
 *     80e74:  testb  %al, %al
 *     80e76:  je     <slow path: call __cxa_guard_acquire>
 *
 * So byte 0 must become non-zero exactly when initialization is complete, and it must be
 * published with release ordering because that reader does not take our mutex. Byte 1 is
 * private to this file; bytes 2-7 are left alone. Nothing here needs to match libc++abi's
 * layout - a guard is only ever handled by the implementation that claimed it, and the three
 * entry points are replaced together. */
#define GUARD_DONE_BYTE(g)  ((unsigned char *)(g))
#define GUARD_FLAG_BYTE(g)  (((unsigned char *)(g)) + 1)

#define FLAG_PENDING  0x01u   /* some thread is running the initializer */
#define FLAG_WAITING  0x02u   /* at least one thread is parked on the condvar */

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond  = PTHREAD_COND_INITIALIZER;

/* One instruction, no library, no import, no relocation, no TLS block of our own. */
static uintptr_t orbis_guard_self(void)
{
   uintptr_t tp = 0;
   __asm__ __volatile__("movq %%fs:0, %0" : "=r" (tp));
   return tp;
}

/* Who is initialising what, for the recursion check. Keyed by guard address, holding the
 * owner's thread anchor. Sixty-four simultaneous in-flight static initializations across all
 * threads is far past anything real; running out means the check is skipped, not that anything
 * breaks. Every access is under s_mutex, which is the same lock the guard state itself is
 * under, so it needs no locking of its own. */
#define OWNERS 64
static struct { uint64_t *guard; uintptr_t owner; } s_owners[OWNERS];

static void owner_set(uint64_t *guard, uintptr_t owner)
{
   int i;
   for (i = 0; i < OWNERS; i++)
   {
      if (!s_owners[i].guard)
      {
         s_owners[i].guard = guard;
         s_owners[i].owner = owner;
         return;
      }
   }
}

static void owner_clear(uint64_t *guard)
{
   int i;
   for (i = 0; i < OWNERS; i++)
   {
      if (s_owners[i].guard == guard)
      {
         s_owners[i].guard = NULL;
         s_owners[i].owner = 0;
         return;
      }
   }
}

/* 1 only when this guard is recorded AND its owner is this very thread. An unrecorded guard
 * (table was full) answers 0, which means "wait" - see the note above about which way this
 * degrades. */
static int owner_is_self(uint64_t *guard, uintptr_t self)
{
   int i;
   if (!self)
      return 0;
   for (i = 0; i < OWNERS; i++)
      if (s_owners[i].guard == guard)
         return s_owners[i].owner == self;
   return 0;
}

int  __cxa_guard_acquire(uint64_t *guard);
void __cxa_guard_release(uint64_t *guard);
void __cxa_guard_abort(uint64_t *guard);

/* ⚠ NOT THROUGH orbis_report(). That lives in ps4/orbis_abort_report.c, which the CORE support
 * archive carries and the frontend does not, and a weak reference to it would come back as
 * `Failed to build FSELF: missing library for symbol` on whichever image lacked it. klog is
 * the channel orbis_report reaches first and the one it calls synchronous and reliable, so
 * this writes there directly and stays linkable in both images.
 *
 * The guard's address is printed with the address of a function from this file beside it, for
 * the same reason abort() in orbis_abort_report.c prints two: a module lands wherever the
 * kernel puts it, and subtracting this function's .elf offset gives the base needed to
 * symbolize the guard and name the static. */
static void guard_recursion_abort(uint64_t *guard)
{
   char msg[320];

   snprintf(msg, sizeof(msg),
         "[orbis] cxa_guard: RECURSIVE initialization of the static guarded by %p - its "
         "initializer re-entered itself on one thread (__cxa_guard_acquire is at %p in this "
         "image; subtract its .elf offset for the module base)\n",
         (void*)guard, (void*)&__cxa_guard_acquire);

   sceKernelDebugOutText(0, msg);
   abort();
}

int __cxa_guard_acquire(uint64_t *guard)
{
   const uintptr_t self = orbis_guard_self();

   pthread_mutex_lock(&s_mutex);
   for (;;)
   {
      /* Acquire, to pair with the release store below for anything that got here without
       * taking the inlined fast path. */
      if (__atomic_load_n(GUARD_DONE_BYTE(guard), __ATOMIC_ACQUIRE))
      {
         pthread_mutex_unlock(&s_mutex);
         return 0;                      /* already initialized - do not run it again */
      }

      if (!(*GUARD_FLAG_BYTE(guard) & FLAG_PENDING))
      {
         *GUARD_FLAG_BYTE(guard) |= FLAG_PENDING;
         owner_set(guard, self);
         pthread_mutex_unlock(&s_mutex);
         return 1;                      /* ours to initialize */
      }

      /* Someone is already running it. The ONLY case that is an error is that someone being
       * us; every other case is two threads doing exactly what a guard is for. */
      if (owner_is_self(guard, self))
      {
         pthread_mutex_unlock(&s_mutex);
         guard_recursion_abort(guard);  /* does not return */
      }

      /* ⚠ THE ONE PRIMITIVE HERE THAT THE FAILING RUN DID NOT PROVE, HENCE THE FALLBACK.
       * The mutex is proven - libc++abi locked a statically zero-initialised pthread_mutex_t
       * on this kernel before it reached the wrong comparison. The CONDITION VARIABLE is not:
       * the abort happened on the contended path INSTEAD of waiting, so nothing has ever
       * parked on one of these here. If pthread_cond_wait refuses a zero-initialised
       * pthread_cond_t it would return without releasing the mutex, and a waiter spinning
       * with the mutex held would block the initializing thread's __cxa_guard_release - a
       * deadlock built out of the fix. Dropping to unlock/sleep/relock on a non-zero return
       * costs nothing when the condvar works and cannot deadlock when it does not. */
      *GUARD_FLAG_BYTE(guard) |= FLAG_WAITING;
      if (pthread_cond_wait(&s_cond, &s_mutex) != 0)
      {
         pthread_mutex_unlock(&s_mutex);
         usleep(200);
         pthread_mutex_lock(&s_mutex);
      }
   }
}

void __cxa_guard_release(uint64_t *guard)
{
   int wake;

   pthread_mutex_lock(&s_mutex);
   /* Release, so a thread that sees this byte through the compiler's inlined acquire load also
    * sees everything the initializer wrote. */
   __atomic_store_n(GUARD_DONE_BYTE(guard), 1, __ATOMIC_RELEASE);
   wake = (*GUARD_FLAG_BYTE(guard) & FLAG_WAITING) != 0;
   *GUARD_FLAG_BYTE(guard) = 0;
   owner_clear(guard);
   pthread_mutex_unlock(&s_mutex);

   if (wake)
      pthread_cond_broadcast(&s_cond);
}

/* The initializer threw. The static is NOT initialized and the next caller must try again, so
 * this puts the guard back to untouched rather than marking it done. */
void __cxa_guard_abort(uint64_t *guard)
{
   int wake;

   pthread_mutex_lock(&s_mutex);
   wake = (*GUARD_FLAG_BYTE(guard) & FLAG_WAITING) != 0;
   *GUARD_FLAG_BYTE(guard) = 0;
   owner_clear(guard);
   pthread_mutex_unlock(&s_mutex);

   if (wake)
      pthread_cond_broadcast(&s_cond);
}
