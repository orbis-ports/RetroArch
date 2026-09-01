/* Where the run loop is when it stops moving. See ps4/orbis_watchdog.c.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef ORBIS_WATCHDOG_H__
#define ORBIS_WATCHDOG_H__

#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* Records "the main thread is here now" and bumps a sequence number. Cheap: three
 * stores, no lock, no allocation, no syscall. Call it at the boundaries that matter,
 * not inside them. */
void orbis_watchdog_mark(const char *phase, int detail);

/* Writes one line to /data/retroarch-watchdog.log as well as to RARCH_ERR, so a fact that decides
   how a run is read survives a session with no log listener. Costs 8-15 ms; for facts, not events. */
void orbis_watchdog_note(const char *fmt, ...);

RETRO_END_DECLS

#define ORBIS_WD(phase, detail) orbis_watchdog_mark((phase), (detail))

#endif
