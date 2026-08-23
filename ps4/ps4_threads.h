/* RetroArch - A frontend for libretro.
 *
 * Which thread is burning the CPU, sampled from the kernel rather than guessed.
 *
 * ⚠ WHY THIS EXISTS. The driver's own BUDGET instrumentation reports the whole process -
 * "1.00 cores busy on average, and it waited 0 ms for the GPU" - which established that this
 * port is CPU-bound and the GPU is idle, and could not say by what. That one number has now
 * been the basis for three rounds of tuning, and each round had to guess which of the
 * emulated CPU, the GPU-command translation or the SPU it was moving.
 *
 * sceKernelGetCpuUsage returns per-thread user and system time and sceKernelGetThreadName
 * names them, so "one saturated thread" can become "that thread, this percentage".
 *
 * ⚠ OFF BY DEFAULT, and the run config turns it on: ORBIS_THREAD_CENSUS=1 in
 * /data/retroarch-env.txt. A frontend that ships diagnostics enabled is the inversion this
 * workshop's own files argue against - see tempest-env.example.txt.
 */

#ifndef PS4_THREADS_H__
#define PS4_THREADS_H__

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Reads ORBIS_THREAD_CENSUS and, if asked, starts a sampling thread. Cheap and silent
 * otherwise - one getenv and a log line saying it is off. */
void ps4_threads_census_start(void);

/* Asks the sampler to finish and waits for it. Safe if it was never started. */
void ps4_threads_census_stop(void);

RETRO_END_DECLS

#endif
