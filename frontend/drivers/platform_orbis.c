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
#include <fcntl.h>
#include <errno.h>
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


#if defined(ORBIS_NET_TRACE)
void orbis_net_probe(void);
#endif

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

   /* NULL rather than a path. The argument is an opt-out: if the named file EXISTS,
    * RetroArch creates none of its default directories, on the theory that whoever put it
    * there laid them out already. Upstream passed "host0:app/custom.ini", which is a Vita
    * path; there is no custom.ini convention on this platform, so there is nothing to opt
    * out of, and NULL says that without naming a file that has to be checked.
    *
    * (An earlier version of this comment claimed the /app0 spelling had broken directory
    * creation on hardware. It had not - the evidence was a truncated FTP listing. The
    * directories were being created the whole time.) */
   dir_check_defaults(NULL);

   /* The result is checked, because dir_check_defaults calls path_mkdir and discards what
    * it says. Nothing anywhere reports a writable root that could not be made, and the
    * symptom of that would be a frontend that runs and silently keeps nothing. One line a
    * boot is cheap insurance against a long afternoon. */
   if (!path_is_directory(g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG]))
      ps4_log("writable root FAILED: '%s' does not exist after dir_check_defaults - "
              "config, saves and playlists all have nowhere to go",
              g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG]);
   else
      ps4_log("writable root ok: '%s'", g_defaults.dirs[DEFAULT_DIR_MENU_CONFIG]);
#endif

#if defined(ORBIS_NET_TRACE)
   /* Which socket API this process is ALLOWED to use, answered on hardware rather than by
    * reading a symbol table. See the note at the top of ps4/orbis_net_probe.c. */
   orbis_net_probe();
#endif
}

static void frontend_orbis_deinit(void *data) { }

/* â  THIS IDLED FOREVER, AND THAT WAS THE WRONG POLICY FOR A FRONTEND.
 *
 * The rule it was copied from is real: returning from main() on a retail console tears the
 * process down outside the system's expected path and pops CE-34878-0, which reads to a
 * user as a crash. ps4_idle_forever() exists so a DEMO - something with no quit, that would
 * otherwise fall off the end of main() - holds the screen instead of showing that dialog.
 *
 * A frontend is not a demo. It has a Quit entry, and the console has a close button, and
 * both of them mean "end". Holding the process on a heartbeat turns either into a hang that
 * only a console restart clears - which is exactly what this port did.
 *
 * â  AND IT IS THE SECOND HALF OF A PROBLEM THIS PORT DOES NOT OWN. Every title built
 * against this workshop's Mesa hangs on close; OpenGothic hit it first and now dies visibly
 * rather than hanging, which is the same trade being made here. The dialog is ugly and the
 * process ends; idling is tidy and it does not. Until the driver-side question is answered,
 * ending is the better half. */
static void frontend_orbis_shutdown(bool unused)
{
   ps4_log("shutdown requested - ending the process rather than idling; expect CE-34878-0");
}

/* ⚠ THE DRIVER'S KNOBS, AND TWO OF THEM ARE NOT OPTIONAL.
 *
 * This workshop's RADV port reads its configuration from the environment, and the titles
 * that came before it set that environment by reading a file at startup and setenv()ing
 * every line - see ~/src-ps4/OpenGothic/ps4/tempest-env.example.txt, which is the normative
 * description of the format and of what each knob does.
 *
 * RetroArch did not read it, and ran the driver in a configuration no title runs in. That
 * file says, in as many words:
 *
 *     ⚠ TWO OF THESE ARE NOT OPTIONS. ORBIS_3D_LINEAR=1 and ORBIS_NO_TESS=1 are the
 *     configuration this port runs on, and BOTH are off in the driver unless this file
 *     turns them on. [...] the title then loads and crashes on entering 3D - measured
 *     2026-08-19, twice, same binary, only this file different.
 *
 * The symptom here was not a crash but a picture: Beetle PSX HW's Vulkan renderer drew the
 * static scene correctly and shredded every animated model, with one quad showing stripes
 * of garbage where a texture belonged - which is what an image read with the wrong tiling
 * looks like. The same content through the core's software renderer was correct, so the
 * geometry reaching the GPU was fine and the GPU's reading of it was not.
 *
 * ⚠ SET BEFORE ANYTHING TOUCHES VULKAN. The driver reads these with getenv() when it first
 * needs them, and by the time a swapchain exists it is too late. First statement of the
 * frontend's life, right after the log channel.
 *
 * The file is the driver's rather than any title's, so the shared name is read as well as
 * ours: a knob discovered while debugging one title belongs to whoever runs the driver
 * next. */
static void frontend_orbis_apply_env_file(const char *path)
{
   char  line[512];
   FILE *f = fopen(path, "r");
   int   applied = 0;

   if (!f)
      return;

   while (fgets(line, sizeof(line), f))
   {
      char *eq, *key, *val, *end;

      /* Comments and blanks. */
      key = line;
      while (*key == ' ' || *key == '\t')
         key++;
      if (*key == '#' || *key == '\n' || *key == '\r' || *key == '\0')
         continue;

      if (!(eq = strchr(key, '=')))
         continue;
      *eq = '\0';
      val = eq + 1;

      /* ⚠ TRIM BOTH SIDES. The format's own documentation warns that a trailing space in a
       * value "would otherwise read as a different experiment" - a knob that silently means
       * something else is worse than one that is absent. */
      end = eq - 1;
      while (end >= key && (*end == ' ' || *end == '\t'))
         *end-- = '\0';
      while (*val == ' ' || *val == '\t')
         val++;
      end = val + strlen(val) - 1;
      while (end >= val && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r'))
         *end-- = '\0';

      if (!*key)
         continue;

      setenv(key, val, 1);
      ps4_log("env: %s=%s  (from %s)", key, val, path);
      applied++;
   }

   fclose(f);
   ps4_log("env: %d line(s) applied from %s", applied, path);
}

/* ⚠ WITHOUT THIS, A CORE THAT ABORTS DOES IT IN SILENCE - AND ABORTING IS HOW C++ CORES FAIL.
 *
 * assert(), libc++abi's abort_message() and the unwinder's own diagnostics all write to stderr
 * and then call abort(). Every one of those messages says exactly what went wrong: "terminating
 * with uncaught exception of type ...", the file and line of a failed assertion, "libc++abi:
 * terminating". On this console fd 2 goes nowhere, so what reaches the log is a fatal signal
 * with SIGABRT in %rsi and a backtrace that stops at abort() - a crash with the explanation
 * thrown away.
 *
 * ⚠ AND IT HAS TO BE THE FILE DESCRIPTOR, NOT stderr. Every image here carries its own static
 * musl, so a core's `stderr` is a different FILE from the frontend's and setvbuf or freopen on
 * ours would not touch it - the same reason setenv() in the eboot is invisible to a .prx. File
 * DESCRIPTORS are the process's, so dup2 onto 2 catches the module's writes as well as ours.
 * musl leaves stderr unbuffered, which is what makes a message written moments before abort()
 * survive it.
 *
 * Truncated on every run: this answers "what did the thing that just died say", and a file that
 * grows across boots answers it worse.
 */
static void frontend_orbis_capture_stderr(void)
{
   static const char *path = "/data/retroarch-stderr.log";
   int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

   if (fd < 0)
   {
      ps4_log("stderr: cannot open %s (errno %d) - a core that aborts will do it silently",
            path, errno);
      return;
   }

   /* stdout too. Nothing here is expected to use it, but a core that printf()s its way through
    * a failure is a core telling us something, and the alternative destination is nowhere. */
   if (dup2(fd, 2) < 0 || dup2(fd, 1) < 0)
   {
      /* ⚠ MEASURED 2026-08-25: errno 1, EPERM. The descriptor table is not ours to rearrange on
       * this kernel, so this whole approach is unavailable and the file must not be left behind -
       * an empty retroarch-stderr.log beside a crash reads as "the core said nothing", which is
       * the opposite of what happened. What replaced it catches the message before it is ever
       * written to a descriptor: ps4/orbis_abort_report.c, linked into every core, defines
       * abort_message and __assert_fail itself. */
      ps4_log("stderr: dup2 onto 1/2 refused (errno %d) - fd redirection is not available here; "
            "a core's dying message comes from ps4/orbis_abort_report.c instead", errno);
      close(fd);
      remove(path);
      return;
   }

   ps4_log("stderr: -> %s", path);

   if (fd > 2)
      close(fd);
}

static void frontend_orbis_init(void *data)
{
   /* First thing the port does: bring up the log channel and read the run config.
    * ps4_log() and the termination policy both answer out of this call. */
   ps4_app_init("retroarch", PS4_APP_STAMP);

   /* Before anything can abort - which on this port means before the first core is loaded. */
   frontend_orbis_capture_stderr();

   /* The driver's configuration, before anything can ask the driver for anything. The
    * shared file first, so a per-title one can override it. */
   frontend_orbis_apply_env_file("/data/tempest-env.txt");
   frontend_orbis_apply_env_file("/data/retroarch-env.txt");

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
