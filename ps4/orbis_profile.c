/* Where a core's frame time actually goes, measured on the console.
 *
 * ⚠ WHY THIS EXISTS RATHER THAN AN ARGUMENT ABOUT WHERE THE TIME GOES. The PlayStation 4 port
 * has now twice had a performance story told about it that turned out to be wrong - once when a
 * recompiler was believed impossible and the frame rate more than doubled when it was written,
 * and once on 2026-08-25 when mupen64plus-next's 20 fps was attributed to the wrong half of the
 * machine. A profile is cheap. Reasoning about a console nobody has profiled is not.
 *
 * ⚠ AND IT IS OFF UNLESS ASKED FOR, by a file rather than an environment variable. Each image
 * here carries its own static musl, so setenv() in the frontend is invisible to a .prx - the
 * trap orbis-compat's orbis_env.h exists for. A file is visible to everyone and can be created
 * over FTP without rebuilding anything:
 *
 *     /data/retroarch-profile
 *
 * Slots are named by the caller. Reports every five seconds through orbis_report, which is klog
 * and costs 8-15 ms a line - once per five seconds, against a frame that is missing its budget
 * anyway.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "orbis_report.h"

#define ORBIS_PROFILE_SLOTS   10
#define ORBIS_PROFILE_WINDOW  5000000ull   /* microseconds between reports */

static const char *orbis_profile_name[ORBIS_PROFILE_SLOTS];
static uint64_t    orbis_profile_us[ORBIS_PROFILE_SLOTS];
static uint64_t    orbis_profile_window_start;
static unsigned    orbis_profile_frames;
static int         orbis_profile_on = -1;  /* -1 not asked, 0 off, 1 on */

/* Microseconds, monotonic, and a plain kernel call rather than clock_gettime: the SDK routes
 * that one through musl's __clock_gettime64 shim, and this is one syscall with no shim. */
uint64_t orbis_profile_now(void)
{
   return sceKernelGetProcessTime();
}

int orbis_profile_enabled(void)
{
   if (orbis_profile_on < 0)
   {
      FILE *f = fopen("/data/retroarch-profile", "r");
      orbis_profile_on = f ? 1 : 0;
      if (f)
      {
         fclose(f);
         orbis_report("profile", "on - /data/retroarch-profile exists");
      }
   }
   return orbis_profile_on;
}

void orbis_profile_add(int slot, const char *name, uint64_t us)
{
   if (slot < 0 || slot >= ORBIS_PROFILE_SLOTS)
      return;
   orbis_profile_name[slot] = name;
   orbis_profile_us[slot]  += us;
}

/* Once per frame, after the slots for that frame have been added. */
void orbis_profile_tick(void)
{
   uint64_t now = orbis_profile_now();
   uint64_t span;
   int      i;

   orbis_profile_frames++;

   if (!orbis_profile_window_start)
   {
      orbis_profile_window_start = now;
      return;
   }

   span = now - orbis_profile_window_start;
   if (span < ORBIS_PROFILE_WINDOW)
      return;

   /* ⚠ PER FRAME AND AS A PERCENTAGE OF WALL, not totals. A total says which slot is biggest; a
    * share of wall says whether the frame is full, which is the question. A core at 100% of a
    * 60 Hz budget and a core at 100% of a 20 Hz budget produce the same totals. */
   orbis_report("profile", "%u frames in %llu ms = %llu.%02llu fps",
         orbis_profile_frames, (unsigned long long)(span / 1000),
         (unsigned long long)((uint64_t)orbis_profile_frames * 1000000ull / span),
         (unsigned long long)(((uint64_t)orbis_profile_frames * 100000000ull / span) % 100));

   for (i = 0; i < ORBIS_PROFILE_SLOTS; i++)
   {
      if (!orbis_profile_name[i] || !orbis_profile_frames)
         continue;
      orbis_report("profile", "  %-16s %llu.%02llu ms/f  %llu%% of wall",
            orbis_profile_name[i],
            (unsigned long long)(orbis_profile_us[i] / orbis_profile_frames / 1000),
            (unsigned long long)((orbis_profile_us[i] * 100 / orbis_profile_frames / 1000) % 100),
            (unsigned long long)(orbis_profile_us[i] * 100 / span));
      orbis_profile_us[i] = 0;
   }

   orbis_profile_frames       = 0;
   orbis_profile_window_start = now;
}
