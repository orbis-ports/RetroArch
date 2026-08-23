/* RetroArch - A frontend for libretro. See ps4/ps4_threads.h. */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <rthreads/rthreads.h>
#include <features/features_cpu.h>
#include <retro_timers.h>

#include <orbis/libkernel.h>

#include "ps4_threads.h"
#include "ps4_log.h"

#include "../verbosity.h"

/* ⚠ THE SIZE ARGUMENT'S UNITS ARE NOT KNOWN, AND THIS IS ARRANGED SO THAT BOTH READINGS ARE
 * SAFE. OpenOrbis declares
 *
 *     int32_t sceKernelGetCpuUsage(struct Proc_Stats *out, int32_t *size);
 *
 * and says nothing about whether `size` counts entries or bytes. Guessing costs more than it
 * looks: passing a byte count that the kernel reads as an entry count would have it write
 * 2560 * 0x28 = 100 KiB into a 2.5 KiB buffer, and this workshop has already established that
 * a wrong memory call on this console is not a wrong answer, it is a dead process.
 *
 * So the buffer holds 256 entries and the call is passed 256. Read as entries that is exactly
 * the buffer; read as BYTES it is 256 bytes, six entries, which is fewer threads than we want
 * but harmless. The first sample logs what came back so the contract gets established from a
 * measurement instead of staying a guess. */
#define PS4_CENSUS_MAX 256

/* sceKernelGetThreadName's output size is undocumented too. The kernel's own thread names are
 * 32 bytes (MAXCOMLEN+1 territory); 64 is slack for a name that is not. */
#define PS4_CENSUS_NAME 64

struct ps4_census_prev
{
   uint32_t tid;
   uint64_t nsec;
};

static sthread_t *ps4_census_thread;
static volatile bool ps4_census_stop;

static uint64_t ps4_census_nsec(const OrbisKernelTimespec *t)
{
   return (uint64_t)t->tv_sec * 1000000000ull + (uint64_t)t->tv_nsec;
}

static void ps4_census_sample(struct ps4_census_prev *prev, unsigned *prev_count,
      int64_t *prev_usec, bool *said_contract)
{
   struct Proc_Stats stats[PS4_CENSUS_MAX];
   struct ps4_census_prev next[PS4_CENSUS_MAX];
   char    line[512];
   int32_t size = PS4_CENSUS_MAX;
   int32_t rc;
   int64_t now;
   int64_t window;
   unsigned i, n = 0, len = 0;

   memset(stats, 0, sizeof(stats));
   rc  = sceKernelGetCpuUsage(stats, &size);
   now = cpu_features_get_time_usec();

   if (rc != 0)
   {
      RARCH_ERR("[PS4] census: sceKernelGetCpuUsage -> 0x%08x - no per-thread figures this run\n",
            (unsigned)rc);
      ps4_census_stop = true;
      return;
   }

   /* Trust what was written, not what was asked for: an entry with no thread id was not
    * filled, whichever way the kernel read the size argument. */
   for (i = 0; i < PS4_CENSUS_MAX && stats[i].td_tid != 0; i++)
      n++;

   if (!*said_contract)
   {
      *said_contract = true;
      RARCH_LOG("[PS4] census: asked for %d, size came back %d, %u entr%s carried a thread id"
                " - so the argument counts %s\n",
            PS4_CENSUS_MAX, (int)size, n, n == 1 ? "y" : "ies",
            n > 6 ? "ENTRIES" : "bytes, or there are simply few threads");
   }

   window = now - *prev_usec;
   for (i = 0; i < n; i++)
   {
      next[i].tid  = stats[i].td_tid;
      next[i].nsec = ps4_census_nsec(&stats[i].user_cpu_usage_time)
                   + ps4_census_nsec(&stats[i].system_cpu_usage_time);
   }

   /* The first sample has nothing to subtract from; it only seeds the baseline. */
   if (*prev_count && window > 0)
   {
      for (i = 0; i < n; i++)
      {
         char     name[PS4_CENSUS_NAME];
         unsigned j;
         uint64_t was = 0;
         unsigned pct;

         for (j = 0; j < *prev_count; j++)
         {
            if (prev[j].tid != next[i].tid)
               continue;
            was = prev[j].nsec;
            break;
         }

         if (j == *prev_count)     /* a thread that appeared during the window */
            continue;
         if (next[i].nsec <= was)
            continue;

         /* Tenths of a percent of one core over the window. */
         pct = (unsigned)(((next[i].nsec - was) / 100ull) / (uint64_t)window);
         if (pct < 5)              /* below 0.5% of a core; noise, and there are many */
            continue;

         name[0] = '\0';
         if (sceKernelGetThreadName(next[i].tid, name) != 0 || !name[0])
            snprintf(name, sizeof(name), "tid %u", (unsigned)next[i].tid);

         if (len < sizeof(line) - 1)
            len += snprintf(line + len, sizeof(line) - len, "%s%s %u.%u%%",
                  len ? ", " : "", name, pct / 10, pct % 10);
      }

      if (len)
         RARCH_LOG("[PS4] census over %d ms: %s\n", (int)(window / 1000), line);
      else
         RARCH_LOG("[PS4] census over %d ms: nothing above 0.5%% of a core\n",
               (int)(window / 1000));
   }

   memcpy(prev, next, n * sizeof(*prev));
   *prev_count = n;
   *prev_usec  = now;
}

static void ps4_census_loop(void *unused)
{
   struct ps4_census_prev prev[PS4_CENSUS_MAX];
   unsigned prev_count    = 0;
   int64_t  prev_usec     = cpu_features_get_time_usec();
   bool     said_contract = false;
   unsigned tick          = 0;

   (void)unused;

   while (!ps4_census_stop)
   {
      /* 250 ms granularity so a shutdown does not wait five seconds for this thread. */
      retro_sleep(250);
      if (++tick < 20)
         continue;
      tick = 0;
      ps4_census_sample(prev, &prev_count, &prev_usec, &said_contract);
   }
}

void ps4_threads_census_start(void)
{
   const char *e = getenv("ORBIS_THREAD_CENSUS");

   if (!e || e[0] == '0' || e[0] == '\0')
      return;

   ps4_census_stop   = false;
   ps4_census_thread = sthread_create(ps4_census_loop, NULL);

   if (ps4_census_thread)
      RARCH_LOG("[PS4] census: sampling every 5 s (ORBIS_THREAD_CENSUS=%s)\n", e);
   else
      RARCH_ERR("[PS4] census: could not start the sampling thread\n");
}

void ps4_threads_census_stop(void)
{
   if (!ps4_census_thread)
      return;

   ps4_census_stop = true;
   sthread_join(ps4_census_thread);
   ps4_census_thread = NULL;
}
