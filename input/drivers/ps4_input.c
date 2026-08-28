/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

/* <orbis/libScePad.h> is not a header this SDK has - it is orbisdev's spelling - and
 * defines/ps4_defines.h is deleted: every macro in it was either supplied correctly by one
 * of the SDK's own type headers, or wrong. Neither was needed here - the joypad driver
 * owns the pad. */

#include <boolean.h>
#include <libretro.h>
#include <retro_miscellaneous.h>

#include <orbis/Keyboard.h>
#include <orbis/Sysmodule.h>
#include <orbis/UserService.h>

#include "../input_driver.h"
#include "../input_keymaps.h"
#include "../../ps4/ps4_log.h"

/* ⚠ A USB KEYBOARD WORKS HERE, AND THE HALF THAT SAID SO WAS ALREADY IN THE TREE.
 *
 * `rarch_key_map_ps4[]` (input/input_keymaps.c, under `#if defined(ORBIS)`) is a complete
 * HID-usage to RETROK_ table left behind by the orbisdev-era port - declared in the header,
 * referenced by nothing, for as long as this port has existed. Measured on hardware
 * 2026-08-28 against a Logitech K860 on a Unifying receiver, it is correct as it stands:
 * 0x04 a, 0x0e k, 0x16 s, 0x1a w, 0x20 3, 0x26 9, 0x33 semicolon. Nothing had to be added.
 *
 * ⚠ AND THE LIBRARY IS A .sprx THAT HAS TO BE LOADED FIRST. sceKeyboardInit() on an unloaded
 * libSceKeyboard does not fail - it ENDS THE PROCESS, with no message and no return. That cost
 * two boots before the load was added (ps4/orbis_kbd_probe.c carries the account). This port
 * had never called sceSysmoduleLoadModule for anything, because the pad, audio and video-out
 * libraries are loaded for every title automatically and the keyboard's is not.
 *
 * ⚠ THE PUBLIC ID IS THE ONE THAT WORKS. `ORBIS_SYSMODULE_KEYBOARD` (0x0106) returned 0;
 * <orbis/_types/sysmodule.h> also has ORBIS_SYSMODULE_INTERNAL_KEYBOARD = 0x80000008 with the
 * comment "libSceKeyboard", which is what made it look like the internal loader was wanted.
 *
 * ⚠ AND THERE IS NO PRIVILEGE GATE, WHICH THE HEADER SUGGESTS THERE IS. The un-reversed part of
 * <orbis/Keyboard.h> holds sceKeyboardSetProcessPrivilege and sceKeyboardSetProcessFocus, and
 * neither is needed: Open returned a handle to an ordinary GoldHEN-loaded title, first try. */

/* ⚠ AN EMPTY STATE IS nkeys == 1 WITH keycodes[0] == 0, NOT nkeys == 0. Measured. A loop that
 * treats every one of `nkeys` entries as a pressed key therefore reports keycode 0 as held in
 * every frame where nothing is down - which lands on whatever RETROK_ the table maps 0 to. */
#define PS4_KBD_NONE 0x00

typedef struct ps4_input
{
   const input_device_driver_t *joypad;
   int32_t                      kbd_handle;
   /* Indexed by HID usage, which is what the console reports and what the map is keyed on. */
   bool                         kbd_state[256];
   uint16_t                     kbd_mods;
} ps4_input_t;

static uint16_t ps4_kbd_mods(uint32_t mods, uint32_t locks)
{
   uint16_t out = 0;
   if (mods & (ORBIS_KEYBOARD_MOD_LEFT_CTRL  | ORBIS_KEYBOARD_MOD_RIGHT_CTRL))
      out |= RETROKMOD_CTRL;
   if (mods & (ORBIS_KEYBOARD_MOD_LEFT_SHIFT | ORBIS_KEYBOARD_MOD_RIGHT_SHIFT))
      out |= RETROKMOD_SHIFT;
   if (mods & (ORBIS_KEYBOARD_MOD_LEFT_ALT   | ORBIS_KEYBOARD_MOD_RIGHT_ALT))
      out |= RETROKMOD_ALT;
   if (mods & (ORBIS_KEYBOARD_MOD_LEFT_META  | ORBIS_KEYBOARD_MOD_RIGHT_META))
      out |= RETROKMOD_META;
   if (locks & ORBIS_KEYBOARD_NUM_LOCK)
      out |= RETROKMOD_NUMLOCK;
   if (locks & ORBIS_KEYBOARD_CAPS_LOCK)
      out |= RETROKMOD_CAPSLOCK;
   if (locks & ORBIS_KEYBOARD_SCROLL_LOCK)
      out |= RETROKMOD_SCROLLOCK;
   return out;
}

/* ⚠ ONE HANDLE FOR THE LIFE OF THE PROCESS, AND THAT IS NOT TIDINESS. RetroArch reinitialises
 * its input driver on every content load and every video-driver switch, and each call used to
 * open a fresh keyboard. Measured from one session's log: ELEVEN opens in a single boot, handles
 * 0x01210700, 0x01230700, 0x01250700 ... climbing by 0x20000 and never reissued - across the
 * whole day's fourteen boots the counter only ever went up.
 *
 * That is the audio port's signature exactly (ps4/../audio/drivers/ps4_audio.c): this console
 * hands out a finite number of these per process, reports the close as succeeding, and does not
 * give the number back. Audio ran out after eight and went silent; nobody has found the keyboard's
 * limit, and the way to not find it is to stop asking. Nothing is lost by holding one - the open
 * takes no parameters that can change between calls. */
static int32_t  shared_kbd_handle = -1;
static bool     shared_kbd_opened = false;

static void ps4_kbd_open(ps4_input_t *ps4)
{
   OrbisUserServiceLoginUserIdList user_id_list;
   OrbisUserServiceUserId          user_id = ORBIS_USER_SERVICE_USER_ID_INVALID;
   unsigned                        i;
   int32_t                         rc;

   ps4->kbd_handle = -1;

   if (shared_kbd_opened)
   {
      ps4->kbd_handle = shared_kbd_handle;
      if (shared_kbd_handle >= 0)
         input_keymaps_init_keyboard_lut(rarch_key_map_ps4);
      return;
   }
   shared_kbd_opened = true;

   /* Nothing in libSceKeyboard may be called before this returns 0 - see the note above. */
   if (sceSysmoduleIsLoaded(ORBIS_SYSMODULE_KEYBOARD) != 0)
   {
      if ((rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_KEYBOARD)) != 0)
      {
         RARCH_WARN("[PS4] keyboard: sceSysmoduleLoadModule -> 0x%08x; no keyboard.\n",
               (unsigned)rc);
         return;   /* shared_kbd_opened stays true: a refusal does not improve on retry. */
      }
   }

   if ((rc = sceKeyboardInit()) != 0)
   {
      RARCH_WARN("[PS4] keyboard: sceKeyboardInit -> 0x%08x; no keyboard.\n", (unsigned)rc);
      return;
   }

   /* The real id, for the same reason the pad needs it: scePadOpen refuses the 0xFF "main user"
    * constant on hardware while accepting it under the emulator. */
   if (sceUserServiceGetLoginUserIdList(&user_id_list) != 0)
   {
      RARCH_WARN("[PS4] keyboard: no login user id; no keyboard.\n");
      return;
   }
   for (i = 0; i < 4; i++)
      if (user_id_list.userId[i] != ORBIS_USER_SERVICE_USER_ID_INVALID)
      {
         user_id = user_id_list.userId[i];
         break;
      }

   rc = sceKeyboardOpen(user_id, 0, 0, NULL);
   if (rc < 0)
   {
      RARCH_WARN("[PS4] keyboard: sceKeyboardOpen -> 0x%08x; no keyboard.\n", (unsigned)rc);
      return;
   }

   shared_kbd_handle = rc;
   ps4->kbd_handle   = rc;
   input_keymaps_init_keyboard_lut(rarch_key_map_ps4);
   RARCH_LOG("[PS4] keyboard: open, handle 0x%08x.\n", (unsigned)rc);
}

static void ps4_kbd_poll(ps4_input_t *ps4)
{
   OrbisKeyboardData data;
   bool              now[256];
   uint16_t          mod;
   unsigned          i;

   if (ps4->kbd_handle < 0)
      return;

   memset(&data, 0, sizeof(data));
   if (sceKeyboardReadState(ps4->kbd_handle, &data) != 0)
      return;

   mod = ps4_kbd_mods(data.mods, data.locks);
   memset(now, 0, sizeof(now));

   for (i = 0; i < (unsigned)data.nkeys && i < 32; i++)
   {
      const uint16_t k = data.keycodes[i];
      /* ⚠ The empty state is one entry of zero - see PS4_KBD_NONE above. */
      if (k != PS4_KBD_NONE && k < 256)
         now[k] = true;
   }

   /* ⚠ EVERY HID USAGE, NOT ONLY THE ONES REPORTED THIS FRAME. A key released between two
    * reads never appears in keycodes[] again, so a loop over the current report alone can
    * only ever press keys and never let one go. */
   for (i = 0; i < 256; i++)
   {
      if (now[i] == ps4->kbd_state[i])
         continue;
      ps4->kbd_state[i] = now[i];
      input_keyboard_event(now[i],
            input_keymaps_translate_keysym_to_rk((unsigned)i),
            0, mod, RETRO_DEVICE_KEYBOARD);
   }

   ps4->kbd_mods = mod;
}

/* ⚠ THE HANDLE IS DELIBERATELY NOT CLOSED - see the note above ps4_kbd_open. The driver may be
 * torn down and rebuilt many times in one run; the keyboard is opened once and kept. */
static void ps4_kbd_close(ps4_input_t *ps4)
{
   if (ps4)
      ps4->kbd_handle = -1;
}

int16_t ps4_input_state(void *data,
         const input_device_driver_t *joypad_data,
         const input_device_driver_t *sec_joypad_data,
         rarch_joypad_info_t *joypad_info,
         const retro_keybind_set *retro_keybinds,
         bool keyboard_mapping_blocked,
         unsigned port, unsigned device, unsigned index, unsigned id)
{
   ps4_input_t *ps4           = (ps4_input_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;
            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
            {
               /* Auto-binds are per joypad, not per user. */
               const uint64_t joykey  = (retro_keybinds[port][i].joykey != NO_BTN)
                  ? retro_keybinds[port][i].joykey : joypad_info->auto_binds[i].joykey;
               const uint32_t joyaxis = (retro_keybinds[port][i].joyaxis != AXIS_NONE)
                  ? retro_keybinds[port][i].joyaxis : joypad_info->auto_binds[i].joyaxis;

               if ((uint16_t)joykey != NO_BTN && ps4->joypad->button(
                        joypad_info->joy_idx, (uint16_t)joykey))
               {
                  ret |= (1 << i);
                  continue;
               }
               if (((float)abs(ps4->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
               {
                  ret |= (1 << i);
                  continue;
               }
            }

            return ret;
         }
         else
         {
            /* Auto-binds are per joypad, not per user. */
            const uint64_t joykey  = (retro_keybinds[port][id].joykey != NO_BTN)
               ? retro_keybinds[port][id].joykey : joypad_info->auto_binds[id].joykey;
            const uint32_t joyaxis = (retro_keybinds[port][id].joyaxis != AXIS_NONE)
               ? retro_keybinds[port][id].joyaxis : joypad_info->auto_binds[id].joyaxis;

            if ((uint16_t)joykey != NO_BTN && ps4->joypad->button(
                     joypad_info->joy_idx, (uint16_t)joykey))
               return 1;
            if (((float)abs(ps4->joypad->axis(joypad_info->joy_idx, joyaxis)) / 0x8000) > joypad_info->axis_threshold)
               return 1;
         }
         break;
      case RETRO_DEVICE_ANALOG:
#if 0
         if (retro_keybinds[port])
            return input_joypad_analog(ps4->joypad, joypad_info, port, idx, id, retro_keybinds[port]);
#endif
         break;
      case RETRO_DEVICE_KEYBOARD:
         /* rarch_keysym_lut maps a RETROK_ back to the platform's own code, which here is the
          * HID usage the array is indexed by. */
         return (id && id < RETROK_LAST)
             && ps4->kbd_state[rarch_keysym_lut[(enum retro_key)id] & 0xff];
   }

   return 0;
}
static void ps4_input_free_input(void *data) 
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   if (ps4)
      ps4_kbd_close(ps4);
   if (ps4 && ps4->joypad)
      ps4->joypad->destroy();

   free(data); 

}
static void* ps4_input_initialize(const char *joypad_driver) 
{
   ps4_input_t *ps4 = (ps4_input_t*)calloc(1, sizeof(*ps4));
   if (!ps4)
      return NULL;

   ps4->joypad = input_joypad_init_driver(joypad_driver, ps4);
   ps4_kbd_open(ps4);
   return ps4; 
}
static void ps4_input_poll(void *data)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   if (!ps4)
      return;
   if (ps4->joypad)
      ps4->joypad->poll();
   ps4_kbd_poll(ps4);
}

static uint64_t ps4_input_get_capabilities(void *data)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   /* ⚠ ONLY CLAIM THE KEYBOARD WHEN ONE IS OPEN. A frontend told the device exists will offer
    * keyboard remapping and a keyboard-only core will start expecting input that cannot come. */
   return   (1 << RETRO_DEVICE_JOYPAD)
          | (1 << RETRO_DEVICE_ANALOG)
          | ((ps4 && ps4->kbd_handle >= 0) ? (1 << RETRO_DEVICE_KEYBOARD) : 0);
}

input_driver_t input_ps4 = {
   ps4_input_initialize,
   ps4_input_poll,                         /* poll */
   ps4_input_state,                         /* input_state */
   ps4_input_free_input,
   NULL,
   NULL,
   ps4_input_get_capabilities,
   "ps4",
   NULL,                         /* grab_mouse */
   NULL,
   NULL
};
