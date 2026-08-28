/* RetroArch - A frontend for libretro.
 *
 * Does this process get to read a USB keyboard, and what does it see?
 *
 * ⚠ WHY A PROBE AND NOT A DRIVER. The tree already carries half of one: `rarch_key_map_ps4[]`
 * (input/input_keymaps.c, under `#if defined(ORBIS)`) is a complete HID-usage to RETROK_ table,
 * left behind by the orbisdev-era port, declared in the header and referenced by nothing. And the
 * SDK's <orbis/Keyboard.h> gives five entry points with REAL signatures, not the names-without-
 * types this SDK hands out when nobody reversed the call:
 *
 *     sceKeyboardInit(void)
 *     sceKeyboardOpen(userID, type, index, param)
 *     sceKeyboardReadState(handle, OrbisKeyboardData*)
 *
 * So the code to write is small. What is unknown is whether it is ALLOWED, and this port has been
 * caught six times by a call that links and is refused at run time - fcntl(F_SETFL), ioctl(FIONBIO),
 * dup2 onto fd 2, sceKernelGetCpuUsage, sceKernelQueryMemoryProtection and access(), that last one
 * with EPERM against a stat() of the same path in the same instruction. ⚠ AND THERE IS A NAMED
 * REASON TO EXPECT A GATE HERE: the same header's un-reversed list holds
 * `sceKeyboardSetProcessPrivilege` and `sceKeyboardSetProcessFocus`. A privilege call exists
 * because something is privileged.
 *
 * ⚠ THE FIRST TWO ANSWERS NEED NO KEYBOARD ATTACHED, which is the point. Init and Open decide
 * whether this is worth pursuing at all; only ReadState needs hardware. So the probe reports what
 * it can whatever is plugged in, and says which half it got.
 *
 * ⚠ AND THE MOUSE IS DELIBERATELY BARELY TOUCHED. Every sceMouse* entry point in this SDK is a name
 * with no signature - `void sceMouseOpen();` - so calling it with arguments guessed by analogy is
 * how a stack gets corrupted rather than how a question gets answered. Only sceMouseInit() is
 * called, because every *Init in this family (scePadInit, sceAudioOutInit, sceKeyboardInit) takes
 * void, and its return code says whether the library answers this process at all. Anything past
 * that needs the ABI reversed first.
 *
 * ⚠ AND THE FIRST VERSION OF THIS PROBE TOOK THE PROCESS DOWN, WHICH IS ITSELF THE FIRST FINDING.
 * It called sceKeyboardInit() straight away; the line after that call never printed, twice, on two
 * boots. libSceKeyboard and libSceMouse are `.sprx` MODULES that a title has to load before their
 * entry points resolve - <orbis/_types/sysmodule.h> names them, ORBIS_SYSMODULE_INTERNAL_KEYBOARD
 * = 0x80000008 with the comment "libSceKeyboard" and ORBIS_SYSMODULE_MOUSE = 0x00A9 with
 * "libSceMouse.sprx" - and this port had never called sceSysmoduleLoadModule for anything.
 *
 * ⚠ THAT IS A DIFFERENT HAZARD FROM THE ONE THIS PORT KEEPS MEETING, AND WORSE. The six calls
 * before it - fcntl(F_SETFL), ioctl(FIONBIO), dup2 onto fd 2, sceKernelGetCpuUsage,
 * sceKernelQueryMemoryProtection, access() - all LINK, are called, and return an error. This one
 * links, is called, and ends the process. So the rule "presence is not permission" now has a
 * second half: for a `.sprx` entry point, presence is not even a call. Every future -l<SceThing>
 * on this port's link line has to be paired with the module load, or the first call is a crash
 * with no message.
 *
 * So nothing here calls sceKeyboard* or sceMouse* until the module reports itself loaded, and
 * every call is announced BEFORE it is made rather than after - a probe that logs its result is
 * useless about the call that never returns.
 *
 * Turned on by a FILE rather than a define, so it ships harmlessly and costs no rebuild to run:
 *
 *     /data/retroarch-kbd-probe
 *
 * ⚠ Tested for with stat(), never access(). access() is refused with EPERM on this console while
 * stat() on the same path succeeds - measured 2026-08-28.
 *
 * SPDX-License-Identifier: MIT
 */

/* ⚠ <stdbool.h> BEFORE <orbis/Keyboard.h>, AND THAT IS THE SDK'S BUG RATHER THAN A STYLE CHOICE.
 * That header declares sceKeyboardGetKey2Char with a `bool` parameter and includes only <stdint.h>,
 * so it does not compile from C on its own. Another prebuilt-for-somewhere-else edge of this SDK,
 * in the same family as the libc++ built against Linux's ETIMEDOUT. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <orbis/libkernel.h>
#include <orbis/Keyboard.h>
#include <orbis/Mouse.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include "ps4_log.h"

/* Bounded, because a probe that never returns is a frontend that never boots. */
#define KBD_PROBE_SECONDS   20
#define KBD_PROBE_INTERVAL  16000  /* microseconds between reads, about a frame */

static void kbd_report_state(const OrbisKeyboardData *d)
{
   char line[256];
   int  n = 0;
   int  i;

   n += snprintf(line + n, sizeof(line) - n,
         "keys %d locks 0x%02x mods 0x%02x:",
         (int)d->nkeys, (unsigned)d->locks, (unsigned)d->mods);

   for (i = 0; i < (int)d->nkeys && i < 32 && n < (int)sizeof(line) - 8; i++)
      n += snprintf(line + n, sizeof(line) - n, " 0x%02x", (unsigned)d->keycodes[i]);

   ps4_rarch_err("INFO", "[PS4] kbd: %s\n", line);
}

void orbis_kbd_probe(void)
{
   struct stat                     st;
   OrbisUserServiceLoginUserIdList user_id_list;
   OrbisKeyboardData               data;
   OrbisKeyboardData               last;
   int32_t                         rc;
   int32_t                         handle   = -1;
   OrbisUserServiceUserId          user_id  = ORBIS_USER_SERVICE_USER_ID_INVALID;
   unsigned                        i;
   unsigned                        reads    = 0;
   unsigned                        changes  = 0;
   bool                            loaded   = false;
   uint64_t                        deadline;

   if (stat("/data/retroarch-kbd-probe", &st) != 0)
      return;

   ps4_rarch_err("INFO", "[PS4] kbd: /data/retroarch-kbd-probe exists - probing\n");

   /* 0. LOAD THE MODULE FIRST. Nothing below may be called until one of these succeeds - see the
    *    note at the top of this file for what happened when it was. Both spellings are tried and
    *    both are reported, because the header ties libSceKeyboard to the INTERNAL id while a
    *    public ORBIS_SYSMODULE_KEYBOARD also exists and nothing here says which this firmware
    *    wants. */
   ps4_rarch_err("INFO", "[PS4] kbd: calling sceSysmoduleIsLoaded(0x0106)\n");
   rc = sceSysmoduleIsLoaded(ORBIS_SYSMODULE_KEYBOARD);
   ps4_rarch_err("INFO", "[PS4] kbd: sceSysmoduleIsLoaded(KEYBOARD) -> 0x%08x\n", (unsigned)rc);
   loaded = (rc == 0);

   if (!loaded)
   {
      ps4_rarch_err("INFO", "[PS4] kbd: calling sceSysmoduleLoadModule(0x0106)\n");
      rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_KEYBOARD);
      ps4_rarch_err("INFO", "[PS4] kbd: sceSysmoduleLoadModule(KEYBOARD) -> 0x%08x\n", (unsigned)rc);
      loaded = (rc == 0);
   }

   if (!loaded)
   {
      ps4_rarch_err("INFO", "[PS4] kbd: calling sceSysmoduleLoadModuleInternal(0x80000008)\n");
      rc = (int32_t)sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_KEYBOARD);
      ps4_rarch_err("INFO", "[PS4] kbd: sceSysmoduleLoadModuleInternal(KEYBOARD) -> 0x%08x\n",
            (unsigned)rc);
      loaded = (rc == 0);
   }

   if (!loaded)
   {
      ps4_rarch_err("INFO", "[PS4] kbd: the module did not load, so NOTHING in libSceKeyboard is "
            "called - an unresolved .sprx entry point ends this process rather than returning an "
            "error, which is how the first version of this probe died\n");
      return;
   }

   /* 1. Does the library answer at all. No hardware needed. */
   ps4_rarch_err("INFO", "[PS4] kbd: calling sceKeyboardInit\n");
   rc = sceKeyboardInit();
   ps4_rarch_err("INFO", "[PS4] kbd: sceKeyboardInit -> 0x%08x\n", (unsigned)rc);

   /* ⚠ THE REAL USER ID, NOT THE 0xFF "main user" CONSTANT. scePadOpen refuses that constant on
    * hardware with 0x809b0001 while accepting it under the emulator, and there is no reason to
    * expect the keyboard to be more forgiving than the pad. */
   if (sceUserServiceGetLoginUserIdList(&user_id_list) != 0)
      ps4_rarch_err("INFO", "[PS4] kbd: sceUserServiceGetLoginUserIdList failed - no user id to open with\n");
   else
   {
      for (i = 0; i < 4; i++)
         if (user_id_list.userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID)
         {
            user_id = user_id_list.userId[i];
            break;
         }
   }
   ps4_rarch_err("INFO", "[PS4] kbd: user id %d\n", (int)user_id);

   /* 2. Is this process permitted to open one. Still no hardware needed - an unplugged keyboard
    *    and a refused one are different answers, and this is where the privilege gate would show. */
   ps4_rarch_err("INFO", "[PS4] kbd: calling sceKeyboardOpen\n");
   handle = sceKeyboardOpen(user_id, 0, 0, NULL);
   ps4_rarch_err("INFO", "[PS4] kbd: sceKeyboardOpen(user %d, type 0, index 0) -> 0x%08x\n",
         (int)user_id, (unsigned)handle);

   /* The mouse: load it, and only then the single call - see the header comment for why nothing
    * else in that library is touched. */
   ps4_rarch_err("INFO", "[PS4] mouse: calling sceSysmoduleLoadModule(0x00A9)\n");
   rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);
   ps4_rarch_err("INFO", "[PS4] mouse: sceSysmoduleLoadModule(MOUSE) -> 0x%08x\n", (unsigned)rc);
   if (rc == 0)
   {
      ps4_rarch_err("INFO", "[PS4] mouse: calling sceMouseInit\n");
      sceMouseInit();
      ps4_rarch_err("INFO", "[PS4] mouse: sceMouseInit returned (its declaration carries no type, "
            "so nothing is read from it); every other sceMouse* entry point is a name without a "
            "signature and is NOT called here\n");
   }
   else
      ps4_rarch_err("INFO", "[PS4] mouse: module not loaded, so nothing in libSceMouse is called\n");

   if (handle < 0)
   {
      ps4_rarch_err("INFO", "[PS4] kbd: not opened, so ReadState is not attempted. The first two "
            "answers stand on their own: this is the half that needed no keyboard\n");
      return;
   }

   /* 3. What does it see. This half is the one that needs something plugged in. */
   memset(&last, 0, sizeof(last));
   deadline = sceKernelGetProcessTime() + (uint64_t)KBD_PROBE_SECONDS * 1000000ull;

   while (sceKernelGetProcessTime() < deadline)
   {
      memset(&data, 0, sizeof(data));
      rc = sceKeyboardReadState(handle, &data);
      reads++;

      if (rc != 0)
      {
         /* Once, not once per read: a refused read every 16 ms would be the whole log. */
         if (reads == 1)
            ps4_rarch_err("INFO", "[PS4] kbd: sceKeyboardReadState -> 0x%08x\n", (unsigned)rc);
      }
      else if (   data.nkeys != last.nkeys
               || data.mods  != last.mods
               || data.locks != last.locks
               || memcmp(data.keycodes, last.keycodes, sizeof(data.keycodes)) != 0)
      {
         kbd_report_state(&data);
         last = data;
         changes++;
      }

      sceKernelUsleep(KBD_PROBE_INTERVAL);
   }

   ps4_rarch_err("INFO", "[PS4] kbd: %u reads, %u state change(s) in %d s. Zero changes with a "
         "keyboard attached means it opened but sees nothing; zero reads means the loop never "
         "ran\n", reads, changes, KBD_PROBE_SECONDS);

   sceKeyboardClose(handle);
}
