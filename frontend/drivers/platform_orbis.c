/* RetroArch - A frontend for libretro.
 * Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 * Copyright (C) 2011-2017 - Daniel De Matteis
 *
 * RetroArch is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Found-
 * ation, either version 3 of the License, or (at your option) any later version.
 *
 * RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with RetroArch.
 * If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#include <stdio.h>
#include <stdlib.h>

/* â  THE orbisdev HEADERS ARE GONE, and the code that used them with them. orbis2d.h,
 * orbisPad.h, orbisAudio.h, modplayer.h, ps4link.h, orbisKeyboard.h, debugnet.h,
 * orbisFile.h, user_mem.h and the libSce*.h spellings are all psxdev's SDK. This port
 * builds against OpenOrbis plus the orbis-compat overlay, where the same services are
 * <orbis/UserService.h>, <orbis/SystemService.h>, <orbis/Sysmodule.h>. See
 * ps4/PLAN.md section 1 for why none of the old port survived. */
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include <orbis/libkernel.h>
#include <orbis/UserService.h>
#include <orbis/SystemService.h>
#include <orbis/Sysmodule.h>

/* The console's log channel and its termination policy - orbis-compat/optional. */
#include <ps4_app.h>

#include "../../ps4/ps4_mem.h"

#include <string/stdstring.h>
#include <boolean.h>
#include <file/file_path.h>
#ifndef IS_SALAMANDER
#include <lists/file_list.h>
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../frontend_driver.h"
#include "../../defaults.h"
#include "../../file_path_special.h"
#include "../../paths.h"
#include "../../retroarch.h"
#include "../../verbosity.h"

#define CONTENT_PATH_ARG_INDEX 1

/* â  /app0 IS THE PACKAGE MOUNT AND IS READ-ONLY. Assets that ship inside the pkg live
 * there and nothing may be written back to them. /data is the writable side, and every
 * path RetroArch creates, rewrites or grows has to resolve under it - which is why the
 * core and core-info directories moved there too. */
#define EBOOT_PATH "/app0/"
#define USER_PATH "/data/retroarch/"
#define CORE_DIR "cores"
#define CORE_INFO_PATH USER_PATH
#define CORE_PATH USER_PATH

static char eboot_path[512]     = {0};

static enum frontend_fork orbis_fork_mode = FRONTEND_FORK_NONE;


static void frontend_orbis_get_env(int *argc, char *argv[],
      void *args, void *params_data)
{
   unsigned i;
   char user_path[512];
   struct rarch_main_wrap *params = NULL;

   strlcpy(eboot_path, EBOOT_PATH, sizeof(eboot_path));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_PORT], eboot_path, sizeof(g_defaults.dirs[DEFAULT_DIR_PORT]));
   strlcpy(user_path, USER_PATH, sizeof(user_path));

   /* bundle data */
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CORE], CORE_PATH,
         CORE_DIR, sizeof(g_defaults.dirs[DEFAULT_DIR_CORE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CORE_INFO], CORE_INFO_PATH,
         "info", sizeof(g_defaults.dirs[DEFAULT_DIR_CORE_INFO]));
   /* user data*/
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_ASSETS], user_path,
         "assets", sizeof(g_defaults.dirs[DEFAULT_DIR_ASSETS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_DATABASE], user_path,
         "database/rdb", sizeof(g_defaults.dirs[DEFAULT_DIR_DATABASE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CHEATS], user_path,
         "cheats", sizeof(g_defaults.dirs[DEFAULT_DIR_CHEATS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG], user_path,
         "config", sizeof(g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CORE_ASSETS], user_path,
         "downloads", sizeof(g_defaults.dirs[DEFAULT_DIR_CORE_ASSETS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_PLAYLIST], user_path,
         "playlists", sizeof(g_defaults.dirs[DEFAULT_DIR_PLAYLIST]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_REMAP], g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG],
         "remaps", sizeof(g_defaults.dirs[DEFAULT_DIR_REMAP]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SRAM], user_path,
         "savefiles", sizeof(g_defaults.dirs[DEFAULT_DIR_SRAM]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SAVESTATE], user_path,
         "savestates", sizeof(g_defaults.dirs[DEFAULT_DIR_SAVESTATE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SYSTEM], user_path,
         "system", sizeof(g_defaults.dirs[DEFAULT_DIR_SYSTEM]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_SHADER], user_path,
	       "shaders", sizeof(g_defaults.dirs[DEFAULT_DIR_SHADER]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_CACHE], user_path,
         "temp", sizeof(g_defaults.dirs[DEFAULT_DIR_CACHE]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_OVERLAY], user_path,
         "overlays", sizeof(g_defaults.dirs[DEFAULT_DIR_OVERLAY]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_OSK_OVERLAY], user_path,
         "overlays/keyboards", sizeof(g_defaults.dirs[DEFAULT_DIR_OSK_OVERLAY]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_THUMBNAILS], user_path,
         "thumbnails", sizeof(g_defaults.dirs[DEFAULT_DIR_THUMBNAILS]));
   fill_pathname_join(g_defaults.dirs[DEFAULT_DIR_LOGS], user_path,
         "logs", sizeof(g_defaults.dirs[DEFAULT_DIR_LOGS]));
   strlcpy(g_defaults.dirs[DEFAULT_DIR_CONTENT_HISTORY],
         user_path, sizeof(g_defaults.dirs[DEFAULT_DIR_CONTENT_HISTORY]));
   fill_pathname_join(g_defaults.path_config, user_path,
         FILE_PATH_MAIN_CONFIG, sizeof(g_defaults.path_config));

#ifndef IS_SALAMANDER
   params          = (struct rarch_main_wrap*)params_data;
   params->flags  |=   RARCH_MAIN_WRAP_FLAG_VERBOSE;

   if (argv[CONTENT_PATH_ARG_INDEX] && *argv[CONTENT_PATH_ARG_INDEX])
   {
      static char path[PATH_MAX_LENGTH] = {0};
      struct rarch_main_wrap      *args =
         (struct rarch_main_wrap*)params_data;

      if (args)
      {
         strlcpy(path, argv[CONTENT_PATH_ARG_INDEX], sizeof(path));

         params->flags       &= ~(RARCH_MAIN_WRAP_FLAG_VERBOSE
                                | RARCH_MAIN_WRAP_FLAG_NO_CONTENT);
         params->flags       |=   RARCH_MAIN_WRAP_FLAG_TOUCHED;
         args->config_path    = NULL;
         args->sram_path      = NULL;
         args->state_path     = NULL;
         args->content_path   = path;
         args->libretro_path  = NULL;
      }
   }

   /* ⚠ THIS SAID "host0:app/custom.ini", WHICH IS A VITA PATH. It is the opt-out: if the
    * file exists, RetroArch skips creating its default directories and assumes the user
    * laid them out. A path that can never exist here made the opt-out unreachable rather
    * than wrong, so first boot did create the directories under /data/retroarch -- by
    * accident rather than by decision. Named for this platform now. */
   dir_check_defaults("/app0/custom.ini");
#endif
}

static void frontend_orbis_deinit(void *data) { }

/* â  DO NOT RETURN FROM main() ON THIS CONSOLE. Returning tears the process down outside
 * the system's expected path and pops CE-34878-0, which reads to a user as a crash.
 * ps4_idle_forever() holds the process on a slow heartbeat instead - and returns anyway
 * when the host left autoexit=1 in /app0/ps4-run.cfg, which is how an automated run
 * still gets an exit code out of the same bytes. */
static void frontend_orbis_shutdown(bool unused)
{
   ps4_idle_forever("retroarch shutdown");
}

static void frontend_orbis_init(void *data)
{
   /* First thing the port does: bring up the log channel and read the run config.
    * ps4_log() and the termination policy both answer out of this call. */
   ps4_app_init("retroarch", PS4_APP_STAMP);

   /* Take the flexible-memory reading before the frontend has allocated anything much.
    * There is no way to ask this kernel for the flexible ceiling, so the first reading
    * is the closest thing to a denominator mem_stats.c will ever get - ps4/ps4_mem.h. */
   ps4_mem_baseline();

   /* UserService has to be up before anything can ask for the logged-in user id, and
    * the pad will need the real one: scePadOpen refuses the 0xFF "main user" constant
    * on hardware (0x809b0001) while accepting it under the emulator. */
   sceUserServiceInitialize(NULL);

   sceSystemServiceHideSplashScreen();

   verbosity_enable();
   ps4_log("frontend up, build %s", PS4_APP_STAMP);
}

/* â  NOT IMPLEMENTED, AND SAYING SO. The old body assembled an argv into a local named
 * argp that shadowed the buffer above it, set args = 2, and then returned - it looked
 * like a core swap and was a no-op with two unused-variable warnings. Re-launching
 * another .self is how a console frontend changes core without dlopen; that is Phase 6b
 * work. Until then a log line beats a convincing nothing. */
static void frontend_orbis_exec(const char *path, bool should_load_game)
{
   ps4_log("exec(%s, load_game=%d): not implemented in this port yet",
         path ? path : "(null)", (int)should_load_game);
}

#ifndef IS_SALAMANDER
static bool frontend_orbis_set_fork(enum frontend_fork fork_mode)
{
   switch (fork_mode)
   {
      case FRONTEND_FORK_CORE:
         orbis_fork_mode  = fork_mode;
         break;
      case FRONTEND_FORK_CORE_WITH_ARGS:
         orbis_fork_mode  = fork_mode;
         break;
      case FRONTEND_FORK_RESTART:
         /* NOTE: We don't implement Salamander, so just turn
          * this into FRONTEND_FORK_CORE. */
         orbis_fork_mode  = FRONTEND_FORK_CORE;
         break;
      case FRONTEND_FORK_NONE:
      default:
         return false;
   }

   return true;
}
#endif

static void frontend_orbis_exitspawn(char *s, size_t len, char *args)
{
   bool should_load_game = false;
#ifndef IS_SALAMANDER
   if (orbis_fork_mode == FRONTEND_FORK_NONE)
      return;

   switch (orbis_fork_mode)
   {
      case FRONTEND_FORK_CORE_WITH_ARGS:
         should_load_game = true;
         break;
      case FRONTEND_FORK_NONE:
      default:
         break;
   }
#endif
   frontend_orbis_exec(s, should_load_game);
}

enum frontend_architecture frontend_orbis_get_arch(void)
{
   return FRONTEND_ARCH_X86_64;
}

static int frontend_orbis_parse_drive_list(void *data, bool load_content)
{
#ifndef IS_SALAMANDER
   file_list_t *list = (file_list_t*)data;
   enum msg_hash_enums enum_idx = load_content ?
      MENU_ENUM_LABEL_FILE_DETECT_CORE_LIST_PUSH_DIR :
      MENU_ENUM_LABEL_FILE_BROWSER_DIRECTORY;

   menu_entries_append(list,
         "/",
         msg_hash_to_str(MENU_ENUM_LABEL_FILE_DETECT_CORE_LIST_PUSH_DIR),
         enum_idx,
         FILE_TYPE_DIRECTORY, 0, 0, NULL);

   menu_entries_append(list,
         "/data",
         msg_hash_to_str(MENU_ENUM_LABEL_FILE_DETECT_CORE_LIST_PUSH_DIR),
         enum_idx,
         FILE_TYPE_DIRECTORY, 0, 0, NULL);

   menu_entries_append(list,
         "/usb0",
         msg_hash_to_str(MENU_ENUM_LABEL_FILE_DETECT_CORE_LIST_PUSH_DIR),
         enum_idx,
         FILE_TYPE_DIRECTORY, 0, 0, NULL);
#endif
   return 0;
}



frontend_ctx_driver_t frontend_ctx_orbis = {
   frontend_orbis_get_env,
   frontend_orbis_init,
   frontend_orbis_deinit,
   frontend_orbis_exitspawn,
   NULL,                         /* process_args */
   frontend_orbis_exec,
#ifdef IS_SALAMANDER
   NULL,
#else
   frontend_orbis_set_fork,
#endif
   frontend_orbis_shutdown,
   NULL,                         /* get_name */
   NULL,                         /* get_os */
   NULL,                         /* content_loaded */
   frontend_orbis_get_arch,
   NULL,
   frontend_orbis_parse_drive_list,
   NULL,                         /* install_signal_handler */
   NULL,                         /* get_sighandler_state */
   NULL,                         /* set_sighandler_state */
   NULL,                         /* destroy_sighandler_state */
   NULL,                         /* attach_console */
   NULL,                         /* detach_console */
   NULL,                         /* get_lakka_version */
   NULL,                         /* set_screen_brightness */
   NULL,                         /* set_sustained_performance_mode */
   NULL,                         /* get_cpu_model_name */
   NULL,                         /* get_user_language */
   NULL,                         /* is_narrator_running */
   NULL,                         /* accessibility_speak */
   NULL,                         /* set_gamemode */
   NULL, /* get_display_type */
   "orbis",                      /* ident */
   NULL                          /* get_video_driver */
};
