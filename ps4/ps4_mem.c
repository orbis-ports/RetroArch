/* RetroArch - A frontend for libretro. See ps4/ps4_mem.h. */

#include <stddef.h>
#include <sys/types.h>

#include "ps4_mem.h"

/* ⚠ BOUND BY ASM LABEL, NOT DECLARED BY NAME, because the SDK header disagrees with the
 * SDK: <orbis/libkernel.h>:55 declares sceKernelAvailableFlexibleMemorySize as taking a
 * size_t BY VALUE, which cannot be a query. Upstream OpenOrbis corrected it to size_t*,
 * and that is the form used here. orbis-compat's src/orbis_mem.cpp reaches the same
 * export the same way and for the same reason - this is a transcription of a correction
 * that has already run on hardware, not a new guess. */
extern int32_t ps4_flexible_available(size_t *out)
   __asm__("sceKernelAvailableFlexibleMemorySize");

static uint64_t ps4_mem_high_water;

uint64_t ps4_mem_free(void)
{
   size_t v = 0;

   if (ps4_flexible_available(&v) != 0)
      return 0;

   if ((uint64_t)v > ps4_mem_high_water)
      ps4_mem_high_water = (uint64_t)v;

   return (uint64_t)v;
}

uint64_t ps4_mem_total(void)
{
   /* A caller that asks for the total before anything asked for the free figure would
    * otherwise get 0, which reads as "no memory" rather than "not measured yet". */
   if (ps4_mem_high_water == 0)
      ps4_mem_free();

   return ps4_mem_high_water;
}

void ps4_mem_baseline(void)
{
   ps4_mem_free();
}
