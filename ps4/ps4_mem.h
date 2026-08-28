/* RetroArch - A frontend for libretro.
 *
 * What "free memory" means on a PS4, for mem_stats.c.
 *
 * ⚠ THIS REPLACED get_user_mem_size(), which came from memory/ps4/user_mem.c until that
 * file was deleted in 2021 (6366fcf8e3) and replaced by an external -luser_mem_sys the
 * link line still names. mem_stats.c has referenced a symbol nothing provides ever since.
 *
 * ⚠ AND IT MEASURES THE OTHER POOL. A PS4 process has two budgets that never help each
 * other: DIRECT memory, carved with sceKernelAllocateDirectMemory (the pool people mean
 * when they say "the console has 8 GB", and where the video-out framebuffers will come
 * from), and FLEXIBLE memory, which plain mmap(MAP_ANON) draws from and therefore what
 * musl's malloc grows into. RetroArch's allocations are flexible memory, so a report
 * about direct memory would be a number about a pool the frontend never touches - and
 * would read as healthy at the exact moment malloc starts failing.
 */

#ifndef PS4_MEM_H__
#define PS4_MEM_H__

#include <stdint.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Flexible memory the process can still get, in bytes. 0 if the query failed. */
uint64_t ps4_mem_free(void);

/* ⚠ NOT THE BUDGET - the largest free figure this process has ever been seen to have.
 * The kernel exposes no way to ask for the flexible ceiling, so there is no honest total
 * to report; this is a high-water mark, seeded by ps4_mem_baseline() before the frontend
 * has allocated anything much. Useful as the denominator of a gauge, worthless as a
 * statement about the hardware. */
uint64_t ps4_mem_total(void);

/* Take the first reading. Called from frontend_orbis_init(), as early as anything runs. */
void ps4_mem_baseline(void);

RETRO_END_DECLS

#endif
