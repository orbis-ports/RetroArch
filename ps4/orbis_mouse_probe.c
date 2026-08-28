/* RetroArch - A frontend for libretro.
 *
 * What is the sceMouse ABI, and does this process get to read a USB mouse?
 *
 * ⚠ THIS IS THE HALF ps4/orbis_kbd_probe.c DELIBERATELY REFUSED TO DO. That probe calls
 * sceMouseInit() and stops, because every other entry point in <orbis/Mouse.h> is a name with no
 * type - `void sceMouseOpen();` - and it judged that calling those with arguments guessed by
 * analogy is how a stack gets corrupted rather than how a question gets answered. The keyboard
 * did not need it: <orbis/Keyboard.h> ships REAL signatures. The mouse has none, so the ABI has
 * to be established before a driver can exist, and that is what this file is for.
 *
 * ⚠ AND THE STACK ARGUMENT DOES NOT ACTUALLY HOLD ON x86-64, WHICH IS WHY THIS IS DOABLE.
 * Under the System V AMD64 ABI the first six integer arguments travel in rdi/rsi/rdx/rcx/r8/r9 and
 * the CALLER cleans up anything that reached the stack. Passing four arguments to a function that
 * reads two leaves the extra two in registers the callee ignores; passing too few leaves the
 * callee reading a register that happens to hold something else. Neither corrupts the frame. The
 * real hazard is a BAD POINTER, so every pointer handed over here is a real, zeroed, oversized
 * buffer, and nothing is read back until the call has returned and said how much it wrote.
 *
 * ⚠ THE SHAPE IS GUESSED FROM THE FAMILY, NOT INVENTED. Every input library in this SDK that HAS
 * been reversed opens the same way:
 *
 *     scePadOpen     (userId, type, index, param)
 *     sceKeyboardOpen(userId, type, index, param)
 *
 * so sceMouseOpen is called that way. The read is the part that differs: the keyboard's is
 * sceKeyboardReadState(handle, data) - a snapshot - while this library names its call
 * sceMouseRead, matching sceKeyboardRead in the un-reversed list, which suggests a QUEUE:
 * (handle, buffer, count) returning how many events were filled. So it is called that way and the
 * return value is reported rather than assumed.
 *
 * ⚠ NOTHING HERE DECLARES A STRUCT LAYOUT, and that is the whole method. Inventing an
 * OrbisMouseData and reading d->buttons would produce numbers that look plausible and mean
 * nothing. Instead the buffer is a byte array, and the probe reports the RAW BYTES that changed
 * between reads. Move the mouse left, then right, then click: the offsets that move are the
 * fields, and their width and sign follow from how they move. That is evidence. A struct copied
 * from a forum post is not.
 *
 * ⚠ AND THE MODULE IS LOADED FIRST, BECAUSE FOR A `.sprx` PRESENCE IS NOT EVEN A CALL.
 * sceKeyboardInit() on an unloaded module ENDED THE PROCESS - it did not return an error - which
 * is the finding orbis_kbd_probe.c was rewritten around. ORBIS_SYSMODULE_MOUSE is 0x00A9, and the
 * keyboard probe has already reported whether it loads on this firmware.
 *
 * Turned on by a FILE, so it ships harmlessly and costs no rebuild to run:
 *
 *     /data/retroarch-mouse-probe
 *
 * ⚠ Tested with stat(), never access(): access() is refused with EPERM on this console while
 * stat() on the same path succeeds - measured 2026-08-28.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include "ps4_log.h"

/* ⚠ DECLARED HERE RATHER THAN TAKEN FROM <orbis/Mouse.h>, and the two cannot both be included.
 * That header says `void sceMouseOpen();` - an empty parameter list, which in C is "unspecified",
 * not "none" - so calling it with arguments compiles but tells the compiler nothing. These give
 * the calls a real prototype so the arguments are placed deliberately. If a future SDK reverses
 * these for real, the build breaks on a conflicting declaration, which is the correct failure. */
int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t user_id, int32_t type, int32_t index, void *param);
int32_t sceMouseClose(int32_t handle);
int32_t sceMouseRead(int32_t handle, void *data, int32_t num);

/* Far larger than any plausible event struct, so a call that writes more than expected still lands
 * inside the buffer. Only the first MOUSE_PROBE_SHOW bytes are ever printed. */
#define MOUSE_PROBE_BUF       256
#define MOUSE_PROBE_SHOW      32
#define MOUSE_PROBE_SECONDS   30
#define MOUSE_PROBE_INTERVAL  16000   /* microseconds between reads, about a frame */

static void mouse_report_bytes(const char *tag, const uint8_t *buf, int len)
{
   char line[256];
   int  n = 0;
   int  i;

   for (i = 0; i < len && n < (int)sizeof(line) - 4; i++)
      n += snprintf(line + n, sizeof(line) - n, "%02x", (unsigned)buf[i]);

   ps4_rarch_err("INFO", "[PS4] mouse: %s %s\n", tag, line);
}

void orbis_mouse_probe(void)
{
   struct stat                     st;
   OrbisUserServiceLoginUserIdList user_id_list;
   uint8_t                         buf[MOUSE_PROBE_BUF];
   uint8_t                         last[MOUSE_PROBE_BUF];
   int32_t                         rc;
   int32_t                         handle  = -1;
   OrbisUserServiceUserId          user_id = ORBIS_USER_SERVICE_USER_ID_INVALID;
   unsigned                        i;
   unsigned                        reads   = 0;
   unsigned                        changes = 0;
   bool                            loaded  = false;
   uint64_t                        deadline;

   if (stat("/data/retroarch-mouse-probe", &st) != 0)
      return;

   ps4_rarch_err("INFO", "[PS4] mouse: /data/retroarch-mouse-probe exists - probing\n");

   /* 0. THE MODULE, BEFORE ANYTHING IN IT IS TOUCHED. */
   ps4_rarch_err("INFO", "[PS4] mouse: calling sceSysmoduleIsLoaded(0x00A9)\n");
   rc = sceSysmoduleIsLoaded(ORBIS_SYSMODULE_MOUSE);
   ps4_rarch_err("INFO", "[PS4] mouse: sceSysmoduleIsLoaded(MOUSE) -> 0x%08x\n", (unsigned)rc);
   loaded = (rc == 0);

   if (!loaded)
   {
      ps4_rarch_err("INFO", "[PS4] mouse: calling sceSysmoduleLoadModule(0x00A9)\n");
      rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);
      ps4_rarch_err("INFO", "[PS4] mouse: sceSysmoduleLoadModule(MOUSE) -> 0x%08x\n", (unsigned)rc);
      loaded = (rc == 0);
   }

   if (!loaded)
   {
      ps4_rarch_err("INFO", "[PS4] mouse: module did not load, so nothing in libSceMouse is "
            "called. That is the answer for today\n");
      return;
   }

   /* 1. Init. Safe by family: every *Init in this SDK (scePadInit, sceAudioOutInit,
    *    sceKeyboardInit) takes void, and the keyboard probe already called this one and returned. */
   ps4_rarch_err("INFO", "[PS4] mouse: calling sceMouseInit\n");
   rc = sceMouseInit();
   ps4_rarch_err("INFO", "[PS4] mouse: sceMouseInit -> 0x%08x\n", (unsigned)rc);

   /* 2. The user id, the same way the keyboard driver gets it. A mouse opened against the wrong
    *    user is a plausible reason for a handle that reads nothing. */
   memset(&user_id_list, 0, sizeof(user_id_list));
   rc = sceUserServiceGetLoginUserIdList(&user_id_list);
   if (rc == 0)
   {
      for (i = 0; i < 4; i++)
      {
         if (user_id_list.userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID)
         {
            user_id = user_id_list.userId[i];
            break;
         }
      }
   }
   ps4_rarch_err("INFO", "[PS4] mouse: user id %d (list rc 0x%08x)\n", (int)user_id, (unsigned)rc);

   /* 3. Open. THE FIRST REAL GUESS, and the one the family argument stands behind. */
   ps4_rarch_err("INFO", "[PS4] mouse: calling sceMouseOpen(user %d, type 0, index 0, NULL)\n",
         (int)user_id);
   handle = sceMouseOpen(user_id, 0, 0, NULL);
   ps4_rarch_err("INFO", "[PS4] mouse: sceMouseOpen -> 0x%08x\n", (unsigned)handle);

   if (handle < 0)
   {
      ps4_rarch_err("INFO", "[PS4] mouse: not opened, so Read is not attempted. A negative handle "
            "here is still a finding: it separates 'refused' from 'no hardware'\n");
      return;
   }

   /* 4. Read, and report BYTES rather than fields. See the header: the point is to find the
    *    layout, not to assume one. */
   memset(last, 0, sizeof(last));
   deadline = sceKernelGetProcessTime() + (uint64_t)MOUSE_PROBE_SECONDS * 1000000ull;

   ps4_rarch_err("INFO", "[PS4] mouse: reading for %d s. MOVE THE MOUSE LEFT, THEN RIGHT, THEN "
         "CLICK EACH BUTTON, THEN SCROLL - the offsets that move are the fields\n",
         MOUSE_PROBE_SECONDS);

   while (sceKernelGetProcessTime() < deadline)
   {
      memset(buf, 0, sizeof(buf));
      rc = sceMouseRead(handle, buf, 1);
      reads++;

      if (reads == 1)
         ps4_rarch_err("INFO", "[PS4] mouse: sceMouseRead(handle, buf, 1) -> 0x%08x (first call; "
               "a positive value is probably an event count)\n", (unsigned)rc);

      if (memcmp(buf, last, MOUSE_PROBE_SHOW) != 0)
      {
         mouse_report_bytes("bytes", buf, MOUSE_PROBE_SHOW);
         memcpy(last, buf, sizeof(last));
         changes++;
      }

      sceKernelUsleep(MOUSE_PROBE_INTERVAL);
   }

   ps4_rarch_err("INFO", "[PS4] mouse: %u reads, %u change(s) in %d s. Zero changes with a mouse "
         "attached means it opened but sees nothing - which would point at the open arguments "
         "rather than at permission\n", reads, changes, MOUSE_PROBE_SECONDS);

   sceMouseClose(handle);
}
