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

/* ⚠ THE MOUSE ABI IS NOT IN THIS SDK. <orbis/Mouse.h> declares five entry points as
 * `void sceMouseOpen();` - an empty parameter list, which in C means "unspecified", not "none" -
 * and no data type at all. So both the prototypes and the struct below were established on
 * hardware by ps4/orbis_mouse_probe.c, which dumps the raw bytes the call writes and watches which
 * offsets move. Measured 2026-08-28, 1773 reads over 30 seconds:
 *
 *     0x00  uint64 timestamp    rose by ~16000 per read, i.e. microseconds
 *     0x08  uint32 connected    constant 1 while a receiver was attached
 *     0x0C  uint32 buttons      0x00 x701, 0x01 x19, 0x02 x14, 0x03 x2  -> bit 0 left, bit 1 right
 *     0x10  int32  x            +0x11..+0x28 moving right, -1..-0x40 moving left
 *     0x14  int32  y            -8 .. +11, sign following the direction
 *     0x18  int32  wheel        0x01 up x13, 0xffffffff down x9
 *     0x1C  int32  tilt         never moved in this sample
 *
 * ⚠ THE MIDDLE BUTTON WAS NEVER PRESSED IN THAT SAMPLE, so bit 2 is HID convention rather than
 * measurement. It is mapped, and this comment is the record that it is the one bit here nobody
 * has seen. Everything else in this block is evidence.
 *
 * ⚠ AND THE OPEN SIGNATURE IS THE FAMILY'S, CONFIRMED RATHER THAN ASSUMED: scePadOpen and
 * sceKeyboardOpen both take (userId, type, index, param), and sceMouseOpen(user, 0, 0, NULL)
 * returned handle 0x008b0700 on the first attempt. */
typedef struct
{
   uint64_t timestamp;
   uint32_t connected;
   uint32_t buttons;
   int32_t  x;
   int32_t  y;
   int32_t  wheel;
   int32_t  tilt;
} ps4_mouse_data_t;

int32_t sceMouseInit(void);
int32_t sceMouseOpen(int32_t user_id, int32_t type, int32_t index, void *param);
int32_t sceMouseClose(int32_t handle);
int32_t sceMouseRead(int32_t handle, void *data, int32_t num);

#define PS4_MOUSE_BTN_LEFT    0x01
#define PS4_MOUSE_BTN_RIGHT   0x02
#define PS4_MOUSE_BTN_MIDDLE  0x04   /* see above: convention, not measurement */

/* The scan-out this port opens - see gfx/drivers_context/orbis_vk_ctx.c. */
#define PS4_MOUSE_MAX_X      1919
#define PS4_MOUSE_MAX_Y      1079

typedef struct ps4_input
{
   const input_device_driver_t *joypad;
   int32_t                      kbd_handle;
   /* Indexed by HID usage, which is what the console reports and what the map is keyed on. */
   bool                         kbd_state[256];
   uint16_t                     kbd_mods;
   int32_t                      mouse_handle;
   /* ⚠ TWO ANSWERS ARE NEEDED, NOT ONE, AND THE MENU ASKS FOR THE ONE THIS CONSOLE CANNOT GIVE.
    * RETRO_DEVICE_MOUSE is RELATIVE - what the hardware reports - and a core like dosbox_pure
    * reads exactly that. But RetroArch's own menu asks with RARCH_DEVICE_MOUSE_SCREEN, which is an
    * ABSOLUTE pixel position (menu_driver.c:2016). Answering 0 to that pins the pointer at the top
    * left corner while the buttons and wheel work perfectly - which is what it did.
    *
    * The console only ever reports deltas, so the absolute position is this driver's to keep:
    * accumulated here and clamped to the screen, the way every port with a relative-only mouse
    * has to. The deltas are flushed as they are read, so a frame that is polled twice before the
    * frontend looks does not lose movement. */
   int32_t                      mouse_handle_pad;
   int32_t                      mouse_dx;
   int32_t                      mouse_dy;
   int32_t                      mouse_abs_x;
   int32_t                      mouse_abs_y;
   int32_t                      mouse_wheel;
   uint32_t                     mouse_buttons;
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

/* ⚠ HELD FOR THE WHOLE RUN, FOR THE REASON THE KEYBOARD IS - see ps4_kbd_open above. This console
 * hands out a finite number of these per process and does not take them back; audio ran out after
 * eight. The mouse is opened once and the refusal is remembered too, so a machine with no mouse
 * does not re-ask on every driver rebuild. */
static int32_t  shared_mouse_handle = -1;
static bool     shared_mouse_opened = false;

static void ps4_mouse_open(ps4_input_t *ps4)
{
   OrbisUserServiceLoginUserIdList user_id_list;
   OrbisUserServiceUserId          user_id = ORBIS_USER_SERVICE_USER_ID_INVALID;
   unsigned                        i;
   int32_t                         rc;

   ps4->mouse_handle = -1;

   if (shared_mouse_opened)
   {
      ps4->mouse_handle = shared_mouse_handle;
      return;
   }
   shared_mouse_opened = true;

   /* ⚠ THE MODULE FIRST, ALWAYS. For a .sprx, presence is not even a call: sceKeyboardInit() on an
    * unloaded libSceKeyboard ended the process rather than returning an error, and libSceMouse is
    * the same kind of thing. 0x00A9 is ORBIS_SYSMODULE_MOUSE. */
   if (sceSysmoduleIsLoaded(ORBIS_SYSMODULE_MOUSE) != 0)
   {
      rc = sceSysmoduleLoadModule(ORBIS_SYSMODULE_MOUSE);
      if (rc != 0)
      {
         RARCH_WARN("[PS4] mouse: sceSysmoduleLoadModule(0x00A9) -> 0x%08x; no mouse.\n",
               (unsigned)rc);
         return;
      }
   }

   sceMouseInit();

   memset(&user_id_list, 0, sizeof(user_id_list));
   if (sceUserServiceGetLoginUserIdList(&user_id_list) == 0)
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

   rc = sceMouseOpen(user_id, 0, 0, NULL);
   if (rc < 0)
   {
      RARCH_WARN("[PS4] mouse: sceMouseOpen -> 0x%08x; no mouse.\n", (unsigned)rc);
      ps4_log("mouse: sceMouseOpen(user %d) -> 0x%08x - no mouse", (int)user_id, (unsigned)rc);
      return;
   }

   shared_mouse_handle = rc;
   ps4->mouse_handle   = rc;
   RARCH_LOG("[PS4] mouse: open, handle 0x%08x.\n", (unsigned)rc);
   /* ⚠ ps4_log AND NOT ONLY RARCH_LOG. RARCH_LOG does not reach the console's log channel on this
    * port - measured: a whole boot of this driver produced no keyboard or mouse line at all, while
    * ps4_log's output from the same run was there. Anything meant to be diagnosable on hardware
    * has to go through ps4_log. */
   ps4_log("mouse: open, handle 0x%08x", (unsigned)rc);
}

/* ⚠ ONE READ PER FRAME IS NOT ONE EVENT PER FRAME. sceMouseRead takes a COUNT and this library is
 * queue-shaped (that is why it is Read and not ReadState), so asking for a single event leaves the
 * rest queued and the cursor arrives late, in steps, whenever the queue is deeper than one. Drain
 * it instead: the deltas are summed, and the last event's buttons win because that is the current
 * state rather than a history. */
#define PS4_MOUSE_QUEUE 16

static void ps4_mouse_poll(ps4_input_t *ps4)
{
   ps4_mouse_data_t data[PS4_MOUSE_QUEUE];
   int32_t          rc;
   int32_t          i;
   int32_t          n;

   if (ps4->mouse_handle < 0)
      return;

   memset(data, 0, sizeof(data));
   /* ⚠ sceMouseRead RETURNS AN EVENT COUNT, NOT A STATUS, AND TREATING IT AS ONE COSTS EVERY
    * MOVEMENT. The name is the clue and it was written down in ps4/orbis_mouse_probe.c before this
    * driver existed: the keyboard's call is sceKeyboardReadState - a snapshot - while this library
    * says sceMouseRead, matching the queue-shaped sceKeyboardRead. The probe only logged its FIRST
    * return, which was 0 because the mouse had not moved yet, and 0 was then taken for "success".
    * With `!= 0` meaning failure, a read that reported one pending event was discarded - so the
    * driver dropped precisely the reads that carried data, opened a handle, and saw nothing move.
    * Only a NEGATIVE value is an error here. */
   /* ⚠ ONE EVENT PER POLL, AND THE ATTEMPT TO DRAIN A QUEUE MADE IT MEASURABLY WORSE.
    * Asking for PS4_MOUSE_QUEUE events and summing every returned record produced motion that was
    * far coarser and faster than one record per poll - which is what it looks like when the call
    * fills the whole array with the SAME current state rather than with distinct queued events.
    * So this is not a queue in the sense the name suggested; it is a snapshot, like the keyboard's
    * ReadState, and asking for more copies of it multiplies the delta instead of recovering
    * history. The count below stays at 1 until somebody measures otherwise. */
   rc = sceMouseRead(ps4->mouse_handle, data, 1);
   if (rc < 0)
      return;

   /* ⚠ rc IS AN EVENT COUNT AND ZERO MEANS NOTHING HAPPENED - MEASURED, and acting on the buffer
    * anyway is what made slow movement jump.
    *
    * The log settled it: over a whole session the return was only ever 0 or 1, and every distinct
    * value was reported once. So a poll with rc == 0 carries NO new event, while the buffer still
    * holds whatever it held before. Accumulating it regardless applied the SAME delta again on
    * every idle poll - and idle polls are exactly what a slow hand produces, because most frames
    * then have no movement to report. Fast movement fills nearly every poll, so the duplicates
    * vanished and it felt correct. That is why this only showed up at low speed. */
   if (rc == 0)
      return;
   n = 1;

   /* ⚠ AN ALL-ZERO BUFFER IS A REAL OUTCOME, NOT AN ERROR. sceMouseRead returns 0 whether or not
    * it had anything to say, and when it had nothing it leaves the buffer untouched - which the
    * probe saw as a run of zeroed lines between movements. `connected` is what separates the two,
    * and it also covers the receiver being pulled out mid-session. */
   if (!data[0].connected)
   {
      ps4->mouse_buttons = 0;
      return;
   }

   for (i = 0; i < n; i++)
   {
      if (!data[i].connected)
         break;
      ps4->mouse_dx      += data[i].x;
      ps4->mouse_dy      += data[i].y;
      ps4->mouse_wheel   += data[i].wheel;
      ps4->mouse_abs_x   += data[i].x;
      ps4->mouse_abs_y   += data[i].y;
      ps4->mouse_buttons  = data[i].buttons;
   }

   /* ⚠ THE CLAMP IS THE SCAN-OUT'S OWN SIZE, AND IT IS A CONSTANT HERE ON PURPOSE. The obvious
    * call - video_driver_get_size - is not in this build's link (`undefined symbol` at the first
    * attempt), and the number is not really variable anyway: gfx/drivers_context/orbis_vk_ctx.c
    * opens the video-out at ORBIS_VK_WIDTH x ORBIS_VK_HEIGHT and nothing on this console
    * negotiates another mode. If that context ever gains a second resolution, these two follow. */
   if (ps4->mouse_abs_x < 0)                        ps4->mouse_abs_x = 0;
   else if (ps4->mouse_abs_x > PS4_MOUSE_MAX_X)     ps4->mouse_abs_x = PS4_MOUSE_MAX_X;
   if (ps4->mouse_abs_y < 0)                        ps4->mouse_abs_y = 0;
   else if (ps4->mouse_abs_y > PS4_MOUSE_MAX_Y)     ps4->mouse_abs_y = PS4_MOUSE_MAX_Y;

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
      case RETRO_DEVICE_MOUSE:
      case RARCH_DEVICE_MOUSE_SCREEN:
         if (ps4->mouse_handle < 0)
            break;
         {
            /* RARCH_DEVICE_MOUSE_SCREEN wants where the pointer IS; RETRO_DEVICE_MOUSE wants how
             * far it moved since the last read. Same buttons, same wheel. */
            const bool screen = (device == RARCH_DEVICE_MOUSE_SCREEN);
            int16_t    val    = 0;

            switch (id)
            {
               case RETRO_DEVICE_ID_MOUSE_X:
                  if (screen)
                     return (int16_t)ps4->mouse_abs_x;
                  val           = (int16_t)ps4->mouse_dx;
                  ps4->mouse_dx = 0;   /* flushed as it is read */
                  break;
               case RETRO_DEVICE_ID_MOUSE_Y:
                  if (screen)
                     return (int16_t)ps4->mouse_abs_y;
                  val           = (int16_t)ps4->mouse_dy;
                  ps4->mouse_dy = 0;
                  break;
               case RETRO_DEVICE_ID_MOUSE_LEFT:
                  return (ps4->mouse_buttons & PS4_MOUSE_BTN_LEFT)   ? 1 : 0;
               case RETRO_DEVICE_ID_MOUSE_RIGHT:
                  return (ps4->mouse_buttons & PS4_MOUSE_BTN_RIGHT)  ? 1 : 0;
               case RETRO_DEVICE_ID_MOUSE_MIDDLE:
                  return (ps4->mouse_buttons & PS4_MOUSE_BTN_MIDDLE) ? 1 : 0;
               case RETRO_DEVICE_ID_MOUSE_WHEELUP:
                  if (ps4->mouse_wheel > 0)
                  {
                     val              = (int16_t)ps4->mouse_wheel;
                     ps4->mouse_wheel = 0;
                  }
                  break;
               case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
                  if (ps4->mouse_wheel < 0)
                  {
                     val              = (int16_t)ps4->mouse_wheel;
                     ps4->mouse_wheel = 0;
                  }
                  break;
            }
            return val;
         }
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
   ps4_mouse_open(ps4);
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
   ps4_mouse_poll(ps4);
}

static uint64_t ps4_input_get_capabilities(void *data)
{
   ps4_input_t *ps4 = (ps4_input_t*)data;

   /* ⚠ ONLY CLAIM THE KEYBOARD WHEN ONE IS OPEN. A frontend told the device exists will offer
    * keyboard remapping and a keyboard-only core will start expecting input that cannot come. */
   return   (1 << RETRO_DEVICE_JOYPAD)
          | (1 << RETRO_DEVICE_ANALOG)
          | ((ps4 && ps4->kbd_handle   >= 0) ? (1 << RETRO_DEVICE_KEYBOARD) : 0)
          | ((ps4 && ps4->mouse_handle >= 0) ? (1 << RETRO_DEVICE_MOUSE)    : 0);
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
