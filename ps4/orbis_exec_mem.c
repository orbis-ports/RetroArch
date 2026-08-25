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

/* Direct memory granularity, and also what sceKernelMprotect rounds to. The array is declared
 * ALIGN(4096) upstream, which is not enough on its own - see orbis_exec_mem_promote. */
#define ORBIS_GRANULE   0x4000ull
#define ORBIS_PROT_RWX  (ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_CPU_EXEC) /* 0x07 */

/* -1 not asked, 0 refused, 1 granted and executed. "Not asked" and "refused" lead to opposite
 * decisions about the recompiler, so they are not allowed to share a value. */
static int orbis_exec_state = -1;

/* ⚠ EVERY REPORT GOES TO stderr AND NOT THROUGH log_cb. This file is linked into cores that
 * have not set a libretro logger yet at the point the probe runs, and a refusal that nobody
 * can read is the same as no probe at all. The frontend's log capture takes stderr. */

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

   fprintf(stderr, "[orbis] exec_mem: calling a stub at %p to prove the promotion\n", at);
   got = fn();

   if (got != 0x00c0ffeeu)
   {
      fprintf(stderr, "[orbis] exec_mem: stub ran and returned 0x%08x, not 0x00c0ffee\n",
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
      fprintf(stderr, "[orbis] exec_mem: %p (%lu KiB) would not take execute - "
                      "sceKernelMprotect returned 0x%08x. The recompiler has nowhere to write "
                      "and must not be used.\n",
              (void*)base, (unsigned long)((end - base) / 1024), (unsigned)rc);
      orbis_exec_state = 0;
      return 0;
   }

   orbis_exec_state = orbis_exec_verify(addr) ? 1 : 0;
   fprintf(stderr, "[orbis] exec_mem: %lu KiB at %p is %s\n",
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
