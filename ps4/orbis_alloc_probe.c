/* RetroArch - A frontend for libretro.
 *
 * Does a large allocation come back with memory behind it?
 *
 * ⚠ THE QUESTION COMES FROM A CRASH THIS PORT COULD NOT READ UNTIL TODAY. swanstation died in
 * CPU::CodeCache::AllocateFastMap, writing into the result of
 *
 *     std::make_unique<HostCodePointer[]>(num_slots)      // about 105 MB
 *
 * make_unique throws bad_alloc on failure and did not throw, so the allocation REPORTED SUCCESS.
 * The console's klog called the fault "page not present", which reads as "the allocator handed
 * back address space it never backed".
 *
 * ⚠ THE CRASH REPORTER SAYS OTHERWISE, AND IT IS THE MORE PRECISE INSTRUMENT:
 *
 *     fatal: signal 11 - SIGSEGV (bad address), si_code 2, fault address 0x803f61000
 *
 * si_code 2 is SEGV_ACCERR - MAPPED BUT NOT PERMITTED - and not SEGV_MAPERR, which is the code
 * for "no such page". So the memory exists and the write is refused, which is a different fault
 * with a different cause: a mapping made without write permission, or a suballocator handing out
 * an address beyond what it actually mapped.
 *
 * ⚠ AND orbis-compat's INTERPOSER LOOKS INNOCENT ON INSPECTION, which is exactly why this measures
 * instead: src/orbis_mmap.cpp maps with ORBIS_KERNEL_PROT_CPU_RW. Reading the source says the
 * pages should be writable. The hardware said otherwise. One of the two is wrong and only one of
 * them can be tested.
 *
 * The probe walks a range of sizes, touches EVERY page of each, and reports progress as it goes,
 * so a fault leaves behind the last offset that worked. Page size is 16 KiB here - measured twice
 * by orbis-compat, and not the 4096 that most code assumes.
 *
 * Gated on a file so it ships inert:
 *
 *     /data/retroarch-alloc-probe
 *
 * ⚠ stat(), never access(): access() is refused with EPERM on this console while stat() on the
 * same path succeeds.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "ps4_log.h"

#define ALLOC_PROBE_PAGE      (16u * 1024u)   /* measured, not assumed */
#define ALLOC_PROBE_REPORT    (8u * 1024u * 1024u)

static const size_t alloc_probe_sizes[] = {
   1ull  * 1024 * 1024,
   8ull  * 1024 * 1024,
   32ull * 1024 * 1024,
   64ull * 1024 * 1024,
   105ull * 1024 * 1024,   /* what swanstation asked for */
   128ull * 1024 * 1024,   /* orbis-compat's carve-out size - the interesting boundary */
   160ull * 1024 * 1024,
   256ull * 1024 * 1024
};

void orbis_alloc_probe(void)
{
   struct stat st;
   unsigned    i;

   if (stat("/data/retroarch-alloc-probe", &st) != 0)
      return;

   ps4_rarch_err("INFO", "[PS4] alloc: probing - page assumed %u KiB\n",
         ALLOC_PROBE_PAGE / 1024);

   for (i = 0; i < sizeof(alloc_probe_sizes) / sizeof(alloc_probe_sizes[0]); i++)
   {
      const size_t want = alloc_probe_sizes[i];
      size_t       off;
      size_t       last_ok = 0;
      volatile unsigned char *p;

      ps4_rarch_err("INFO", "[PS4] alloc: asking for %zu MiB\n", want / (1024 * 1024));

      p = (volatile unsigned char*)malloc(want);
      if (!p)
      {
         /* An honest refusal. This is the outcome that would have been FINE in swanstation:
          * make_unique would have thrown and the core would have reported it. */
         ps4_rarch_err("INFO", "[PS4] alloc: %zu MiB REFUSED by malloc - that is an honest no\n",
               want / (1024 * 1024));
         continue;
      }

      ps4_rarch_err("INFO", "[PS4] alloc: %zu MiB granted at %p - touching every page\n",
            want / (1024 * 1024), (void*)p);

      /* ⚠ THE WRITE IS THE TEST, NOT THE ALLOCATION. A pointer proves nothing here; the fault
       * being chased is SEGV_ACCERR on a write to memory that was handed over as usable. */
      for (off = 0; off < want; off += ALLOC_PROBE_PAGE)
      {
         p[off] = (unsigned char)(off & 0xff);
         last_ok = off;

         if ((off % ALLOC_PROBE_REPORT) == 0 && off != 0)
            ps4_rarch_err("INFO", "[PS4] alloc:   %zu MiB written\n", off / (1024 * 1024));
      }

      /* Read it back: a mapping that accepts a write and loses it is its own kind of wrong. */
      {
         size_t bad = 0;
         for (off = 0; off < want; off += ALLOC_PROBE_PAGE)
            if (p[off] != (unsigned char)(off & 0xff))
               bad++;
         ps4_rarch_err("INFO", "[PS4] alloc: %zu MiB OK, every page written and read back"
               " (%zu mismatched), last offset 0x%zx\n",
               want / (1024 * 1024), bad, last_ok);
      }

      free((void*)p);
   }

   ps4_rarch_err("INFO", "[PS4] alloc: done. If this got here, a plain large allocation is sound "
         "and swanstation's fault is somewhere more specific than malloc\n");
}
