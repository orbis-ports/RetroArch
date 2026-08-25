/* Executable memory for a recompiler that brings its own buffer.
 *
 * ⚠ HOW THIS DIFFERS FROM beetle-psx-libretro/ps4/orbis_lightrec_mem.c, WHICH CAME FIRST.
 * That file MAPS the buffer: Lightrec asks for memory and gets direct memory, mapped
 * read-write and promoted with sceKernelMprotect. mupen64plus's new_dynarec does not ask -
 * its buffer is `g_dev.r4300.extra_memory`, a 32 MiB array inside a static struct, so the
 * pages already exist and belong to the module. All that is left to do is promote them.
 *
 * ⚠ AND WHY IT MUST BE THAT BUFFER AND NOT ONE OF OURS. The x64 emitter writes rel32 for
 * every call and jump out of generated code (`emit_call`: 0xe8 + a 32-bit displacement,
 * assem_x64.c), so the code buffer has to sit within +-2 GiB of the module's own text.
 * Upstream asserts this on every emit - and the libretro build compiles with -DNDEBUG, which
 * deletes all of them. A buffer out of range therefore does not trip an assertion; it writes
 * a truncated displacement and jumps somewhere arbitrary. Keeping the module's own .bss as
 * the buffer makes the range question unanswerable-by-construction, which is worth more here
 * than the freedom to allocate elsewhere.
 *
 * ⚠ THE PROMOTION IS THE ONLY THING IN DOUBT, and it is a different question from Lightrec's.
 * There, execute was granted on pages this process had just mapped from direct memory. Here
 * the pages were laid down by the module loader, and whether the kernel will promote loader-
 * owned memory to executable is not something the Lightrec result answers. If it refuses, the
 * caller clamps to the interpreter rather than running a recompiler with nowhere to write.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "orbis_report.h"

/* Direct memory granularity, and also what sceKernelMprotect rounds to. The array is declared
 * ALIGN(4096) upstream, which is not enough on its own - see orbis_exec_mem_promote. */
#define ORBIS_GRANULE   0x4000ull
#define ORBIS_PROT_RW   (ORBIS_KERNEL_PROT_CPU_RW)                              /* 0x03 */
#define ORBIS_PROT_RWX  (ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_CPU_EXEC) /* 0x07 */

/* Sony returns 0x8002xxxx where the low half is an errno, and two of them mean opposite things:
 * EACCES says the kernel understood the request and declined it, EINVAL says the request was
 * malformed and no policy was ever consulted. Telling them apart is the difference between "this
 * console will not do that" and "we asked wrong". */
static const char *orbis_exec_err(int32_t rc)
{
   switch ((uint32_t)rc)
   {
      case 0x80020001u: return "EPERM - not permitted";
      case 0x8002000Cu: return "ENOMEM - out of memory or address space";
      case 0x8002000Du: return "EACCES - understood and refused on policy";
      case 0x8002000Eu: return "EFAULT - bad address argument";
      case 0x80020016u: return "EINVAL - malformed request, policy never reached";
      default:          return "not in this file's table";
   }
}

/* -1 not asked, 0 refused, 1 granted and executed. "Not asked" and "refused" lead to opposite
 * decisions about the recompiler, so they are not allowed to share a value. */
static int orbis_exec_state = -1;

/* ⚠ EVERY REPORT GOES THROUGH orbis_report AND NOT THROUGH log_cb. This file is linked into cores
 * that have not been given a libretro logger yet at the point the probe runs - the first caller
 * is a global constructor - and a refusal nobody can read is the same as no probe at all. It is
 * not stderr either; ps4/orbis_report.h says why neither is available. */

/* ⚠ A GRANTED PROTECTION IS NOT AN HONOURED ONE, AND THE WAY TO TELL IS TO RUN SOMETHING.
 *
 * sceKernelQueryMemoryProtection reports the protection a range was MAPPED with, not what
 * mprotect has since made it: on hardware it answered 0x03 for ranges that were executing
 * code. Using it as the check vetoed a working recompiler sixteen times out of sixteen
 * (ps4/HANDOFF.md, 2026-08-23). So the check is the one the original probe made - put six
 * bytes of x86-64 in the buffer and call them:
 *
 *     b8 ee ff c0 00   mov eax, 0x00c0ffee
 *     c3               ret
 *
 * Self-modifying code needs no cache maintenance on x86-64, so a wrong answer here is about
 * mapping rather than coherency. A page that will not execute does not return an error - it
 * ends the process - which is why the line announcing the attempt goes out BEFORE the call.
 */
static int orbis_exec_verify(void *at)
{
   static const uint8_t stub[] = { 0xb8, 0xee, 0xff, 0xc0, 0x00, 0xc3 };
   uint32_t (*fn)(void);
   uint32_t got;

   memcpy(at, stub, sizeof(stub));
   memcpy(&fn, &at, sizeof(fn));

   orbis_report("exec_mem", "calling a stub at %p to prove the promotion", at);
   got = fn();

   if (got != 0x00c0ffeeu)
   {
      orbis_report("exec_mem", "stub ran and returned 0x%08x, not 0x00c0ffee",
              (unsigned)got);
      return 0;
   }
   return 1;
}

/* Promote an existing range to read-write-execute. Returns 1 if code can be written and run
 * there, 0 if it cannot. Idempotent: the answer is cached, because a caller that asks twice
 * is asking about the same pages and a second stub call would overwrite generated code. */
int orbis_exec_mem_promote(void *addr, size_t len)
{
   uintptr_t base = (uintptr_t)addr;
   uintptr_t end  = base + len;
   int32_t   rc;

   if (orbis_exec_state >= 0)
      return orbis_exec_state;

   /* ⚠ ROUNDED OUTWARDS, NOT INWARDS. sceKernelMprotect takes 16 KiB units and the array is
    * only guaranteed 4 KiB aligned, so rounding the base UP would leave the first pages of
    * the buffer unpromoted - and the recompiler writes its first block exactly there. Rounding
    * down covers whatever else shares that granule, which is other .bss of this module and no
    * more dangerous for being executable than the buffer beside it. */
   base &= ~(uintptr_t)(ORBIS_GRANULE - 1);
   end   = (end + (ORBIS_GRANULE - 1)) & ~(uintptr_t)(ORBIS_GRANULE - 1);

   rc = sceKernelMprotect((void*)base, (size_t)(end - base), ORBIS_PROT_RWX);
   if (rc != 0)
   {
      orbis_report("exec_mem", "%p (%lu KiB) would not take execute - "
                      "sceKernelMprotect returned 0x%08x. The recompiler has nowhere to write "
                      "and must not be used.",
              (void*)base, (unsigned long)((end - base) / 1024), (unsigned)rc);
      orbis_exec_state = 0;
      return 0;
   }

   orbis_exec_state = orbis_exec_verify(addr) ? 1 : 0;
   orbis_report("exec_mem", "%lu KiB at %p is %s",
           (unsigned long)(len / 1024), addr,
           orbis_exec_state ? "writable and executable" : "NOT executable");
   return orbis_exec_state;
}

/* What the promotion decided, for callers that must choose a CPU mode before the recompiler
 * has been anywhere near its buffer. -1 until something has asked; a caller reading -1 has
 * asked too early and should treat it as "unknown" rather than as "no". */
int orbis_exec_mem_state(void)
{
   return orbis_exec_state;
}

/* ⚠ THE OTHER HALF OF THE PROBLEM, AND IT IS A DIFFERENT ONE. orbis_exec_mem_promote takes pages
 * that already exist. A recompiler that brings no buffer of its own - paraLLEl-RSP's JIT, GNU
 * lightning underneath it - asks the operating system for one, and what it asks with is
 *
 *     mmap(nullptr, size, PROT_NONE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0)
 *
 * which cannot work here for two independent reasons. MAP_ANON is 0x1002 on this kernel and the
 * SDK's musl header says 0x0020, so the flags word is wrong before the size is even considered;
 * and anonymous memory is not where executable pages come from on this console. Direct memory is.
 * That is the same conclusion beetle-psx-libretro/ps4/orbis_lightrec_mem.c reached for Lightrec,
 * and this is the smaller version of it: no fixed address, no mirrors, one range.
 *
 * ⚠ AND IT IS REALLY ALLOCATED, NOT RESERVED. The caller's design reserves a gigabyte of address
 * space and commits pages as it fills - a distinction direct memory does not offer, because
 * physical pages are handed out at allocation time. So the caller has to ask for what it will
 * actually use rather than for room to grow, and the patch that calls this says what it picked
 * and why. Asking for the reservation size here would fail outright on a console with no swap.
 */
/* ⚠ THE PHYSICAL PAGES HAVE TO BE REMEMBERED SEPARATELY FROM THE MAPPING, because releasing them
 * takes the offset the allocator returned and unmapping takes the address - two different handles
 * for one allocation, and a caller who only kept the address cannot give the memory back. Four
 * slots: an arena per JIT, and this port has two. */
#define ORBIS_EXEC_MAX_OWNED 4
static struct { void *addr; size_t size; off_t phys; } orbis_exec_owned[ORBIS_EXEC_MAX_OWNED];

void *orbis_exec_mem_alloc(size_t size)
{
   const size_t len = (size + (ORBIS_GRANULE - 1)) & ~(size_t)(ORBIS_GRANULE - 1);
   off_t        phys = 0;
   void        *at   = NULL;
   int32_t      rc;
   unsigned     i;

   rc = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len,
                                      ORBIS_GRANULE, ORBIS_KERNEL_WB_ONION, &phys);
   if (rc != 0)
   {
      orbis_report("exec_mem", "no direct memory for %lu KiB -> 0x%08x (%s)",
              (unsigned long)(len / 1024), (unsigned)rc, orbis_exec_err(rc));
      return NULL;
   }

   /* ⚠ READ-WRITE AT MAP TIME, EXECUTE ONLY AFTERWARDS. Asking sceKernelMapDirectMemory for
    * READ|EXECUTE up front returns EACCES - understood and declined. The policy lives at map
    * time, not at protect time, and mapping read-write and then promoting is granted. Anyone who
    * tries the direct form first concludes a recompiler is impossible on this console. */
   rc = sceKernelMapDirectMemory(&at, len, ORBIS_PROT_RW, 0, phys, ORBIS_GRANULE);
   if (rc != 0 || !at)
   {
      orbis_report("exec_mem", "could not map %lu KiB -> 0x%08x (%s)",
              (unsigned long)(len / 1024), (unsigned)rc, orbis_exec_err(rc));
      sceKernelReleaseDirectMemory(phys, len);
      return NULL;
   }

   rc = sceKernelMprotect(at, len, ORBIS_PROT_RWX);
   if (rc != 0)
   {
      orbis_report("exec_mem", "%lu KiB at %p would not take execute -> 0x%08x (%s)",
              (unsigned long)(len / 1024), at, (unsigned)rc, orbis_exec_err(rc));
      sceKernelMunmap(at, len);
      sceKernelReleaseDirectMemory(phys, len);
      return NULL;
   }

   /* Same argument as orbis_exec_mem_promote: a granted protection is not an honoured one, and
    * the only way to tell on this console is to run something. Six bytes at the start of an
    * arena the caller has not written to yet. */
   if (!orbis_exec_verify(at))
   {
      sceKernelMunmap(at, len);
      sceKernelReleaseDirectMemory(phys, len);
      return NULL;
   }

   for (i = 0; i < ORBIS_EXEC_MAX_OWNED; i++)
   {
      if (!orbis_exec_owned[i].addr)
      {
         orbis_exec_owned[i].addr = at;
         orbis_exec_owned[i].size = len;
         orbis_exec_owned[i].phys = phys;
         break;
      }
   }
   if (i == ORBIS_EXEC_MAX_OWNED)
      orbis_report("exec_mem", "no slot to record %p - %lu KiB of direct memory will not be "
                   "released", at, (unsigned long)(len / 1024));

   orbis_report("exec_mem", "%lu KiB of executable direct memory at %p",
           (unsigned long)(len / 1024), at);
   return at;
}

/* Give an arena back. Quiet about an address it never handed out: a destructor that runs for a
 * failed construction is a normal thing, and there is nothing to say about it. */
void orbis_exec_mem_free(void *addr, size_t size)
{
   unsigned i;

   if (!addr)
      return;

   for (i = 0; i < ORBIS_EXEC_MAX_OWNED; i++)
   {
      if (orbis_exec_owned[i].addr != addr)
         continue;

      sceKernelMunmap(addr, orbis_exec_owned[i].size);
      sceKernelReleaseDirectMemory(orbis_exec_owned[i].phys, orbis_exec_owned[i].size);
      memset(&orbis_exec_owned[i], 0, sizeof(orbis_exec_owned[i]));
      return;
   }
   (void)size;
}
