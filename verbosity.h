/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __RARCH_VERBOSITY_H
#define __RARCH_VERBOSITY_H

#include <stdarg.h>
#include <stdlib.h>

#include <boolean.h>
#include <retro_common_api.h>

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* ⚠ THE PS4 SINK IS SELECTED IN verbosity.c, NOT HERE, AND FOR A MEASURED REASON.
 *
 * This header used to route RARCH_LOG and friends to ps4_rarch_log() with macros -
 * but those macros sat inside the `#if defined(HAVE_LOGGER)` block below, and
 * HAVE_LOGGER is only defined when HAVE_NETLOGGER=1 (Makefile.common). Makefile.orbis
 * never sets it. So on every build this port has ever shipped, the ORBIS macros were
 * dead text and RARCH_LOG went to verbosity.c's ordinary sink - fprintf(stderr) -
 * which on this console is the klog channel that ps4/ps4_log.h exists to keep log
 * lines OFF. Measured on the 2026-08-30 captures: 1305 [INFO] lines in the klog, none
 * of them in the netlog, at 8-15 ms of blocked render thread apiece. The watchdog's
 * own header claimed "RARCH_ERR is klog AND UDP on this port" and it was klog alone.
 *
 * A macro layer here cannot be the fix either: it would bypass verbosity.c's log-level
 * filtering (RARCH_DBG would start emitting unconditionally) and the log-to-file
 * setting. So ORBIS keeps the real functions and only the SINK inside RARCH_LOG_V /
 * RARCH_ERR_V changes - see the ORBIS arm in verbosity.c.
 *
 * The include stays because the files under ps4/ call ps4_rarch_err() directly through it. */
#ifdef ORBIS
#include "ps4/ps4_log.h"
#endif

RETRO_BEGIN_DECLS

#define FILE_PATH_LOG_DBG   "[DEBUG]"
#define FILE_PATH_LOG_INFO  "[INFO]"
#define FILE_PATH_LOG_ERROR "[ERROR]"
#define FILE_PATH_LOG_WARN  "[WARN]"

bool verbosity_is_enabled(void);

void verbosity_enable(void);

void verbosity_disable(void);

void verbosity_set_log_level(unsigned level);

bool *verbosity_get_ptr(void);

void retro_main_log_file_deinit(void);

void retro_main_log_file_init(const char *path, bool append);

bool is_logging_to_file(void);

#if defined(HAVE_LOGGER)

void logger_init (void);
void logger_shutdown (void);
void logger_send (const char *__format,...);
void logger_send_v(const char *__format, va_list args);

#ifdef IS_SALAMANDER

#define RARCH_DBG(...) do { \
   logger_send("RetroArch Salamander: " __VA_ARGS__); \
} while (0)

#define RARCH_LOG(...) do { \
   logger_send("RetroArch Salamander: " __VA_ARGS__); \
} while (0)

#define RARCH_LOG_V(tag, fmt, vp) do { \
   logger_send("RetroArch Salamander: " tag); \
   logger_send_v(fmt, vp); \
} while (0)

#define RARCH_LOG_OUTPUT(...) do { \
   logger_send("[OUTPUT] " __VA_ARGS__); \
} while (0)

#define RARCH_LOG_OUTPUT_V(tag, fmt, vp) do { \
   logger_send("[OUTPUT] " tag); \
   logger_send_v(fmt, vp); \
} while (0)

#define RARCH_ERR(...) do { \
   logger_send("[ERROR] " __VA_ARGS__); \
} while (0)

#define RARCH_ERR_V(tag, fmt, vp) do { \
   logger_send("[ERROR] " tag); \
   logger_send_v(fmt, vp); \
} while (0)

#define RARCH_WARN(...) do { \
   logger_send("[WARN] " __VA_ARGS__); \
} while (0)

#define RARCH_WARN_V(tag, fmt, vp) do { \
   logger_send("[WARN] " tag); \
   logger_send_v(fmt, vp); \
} while (0)
#else /* IS_SALAMANDER */

#define RARCH_DBG(...) do { \
   logger_send("" __VA_ARGS__); \
} while (0)

#define RARCH_LOG(...) do { \
   logger_send("" __VA_ARGS__); \
} while (0)

#define RARCH_LOG_V(tag, fmt, vp) do { \
   logger_send("" tag); \
   logger_send_v(fmt, vp); \
} while (0)

#define RARCH_ERR(...) do { \
   logger_send("[ERROR] " __VA_ARGS__); \
} while (0)

#define RARCH_ERR_V(tag, fmt, vp) do { \
   logger_send("[ERROR] " tag); \
   logger_send_v(fmt, vp); \
} while (0)

#define RARCH_WARN(...) do { \
   logger_send("[WARN] " __VA_ARGS__); \
} while (0)

#define RARCH_WARN_V(tag, fmt, vp) do { \
   logger_send("[WARN] :: " tag); \
   logger_send_v(fmt, vp); \
} while (0)

#define RARCH_LOG_OUTPUT(...) do { \
   logger_send("[OUTPUT] " __VA_ARGS__); \
} while (0)

#define RARCH_LOG_OUTPUT_V(tag, fmt, vp) do { \
   logger_send("[OUTPUT] " tag); \
   logger_send_v(fmt, vp); \
} while (0)
#endif
#define RARCH_LOG_BUFFER(...) do { } while (0)

#else /* HAVE_LOGGER */
void RARCH_LOG_V(const char *tag, const char *fmt, va_list ap);
void RARCH_DBG(const char *fmt, ...);
void RARCH_LOG(const char *fmt, ...);
void RARCH_LOG_BUFFER(uint8_t *buffer, size_t len);
void RARCH_LOG_OUTPUT(const char *msg, ...);
void RARCH_WARN(const char *fmt, ...);
void RARCH_ERR(const char *fmt, ...);

#define RARCH_LOG_OUTPUT_V RARCH_LOG_V
#define RARCH_WARN_V RARCH_LOG_V

#ifdef ORBIS
/* ⚠ NOT AN ALIAS OF RARCH_LOG_V ON THIS PLATFORM. RARCH_LOG_V writes UDP only; the
 * error channel additionally pays for klog, because a line describing a process about
 * to die cannot go out over a datagram alone (ps4/ps4_log.h). Everything else -
 * DBG/LOG/WARN/LOG_OUTPUT - stays on RARCH_LOG_V and stays off klog. */
void RARCH_ERR_V(const char *tag, const char *fmt, va_list ap);
#else
#define RARCH_ERR_V RARCH_LOG_V
#endif
#endif /* HAVE_LOGGER */

void rarch_log_file_init(
      bool log_to_file,
      bool log_to_file_timestamp,
      const char *log_dir);

void rarch_log_file_deinit(void);

size_t rarch_log_file_set_override(const char *path);


RETRO_END_DECLS

#endif
