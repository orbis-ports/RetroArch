/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2020 The RetroArch team
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

/* The DualShock 4, through libScePad.
 *
 * ⚠ REWRITTEN FROM THE orbisdev VERSION, WHICH WAS A HYBRID. It included
 * <orbis/libScePad.h> (not a header this SDK has; it is <orbis/Pad.h>) alongside
 * <orbis/orbisPad.h> (psxdev's), took its handle from orbisPadGetConf()->padHandle when
 * scePadOpen said "already opened", and declared its own SceUserServiceLoginUserIdList
 * because the header it wanted did not exist. None of that compiles here.
 *
 * The scePad* calls it made around all that were right, and are kept.
 */

#include <stdint.h>
#include <string.h>
#include <boolean.h>

#include <orbis/Pad.h>
#include <orbis/UserService.h>

#include "../input_driver.h"

#include "../../tasks/tasks_internal.h"
#include "../../verbosity.h"

/* ⚠ FOUR, NOT SIXTEEN. defines/ps4_defines.h said PS4_MAX_ORBISPADS 16 and
 * SCE_USER_SERVICE_MAX_LOGIN_USERS 16; the SDK says ORBIS_USER_SERVICE_MAX_LOGIN_USERS is
 * 4, and sceUserServiceGetLoginUserIdList fills exactly that many. Reading sixteen was
 * reading twelve uninitialised int32_ts as user ids. */
#define PS4_MAX_PADS ORBIS_USER_SERVICE_MAX_LOGIN_USERS

typedef struct
{
   OrbisUserServiceUserId user_id;
   int32_t                handle;
   bool                   connected;
} ds_joypad_state;

/* TODO/FIXME - static globals */
static ds_joypad_state ds_joypad_states[PS4_MAX_PADS];
static uint64_t        pad_state[PS4_MAX_PADS];
static int16_t         analog_state[PS4_MAX_PADS][2][2];
static unsigned        num_players;

/* The sticks are 0..255 with 128 at rest. -0x8000 is excluded because RetroArch's range is
 * symmetric about zero and a core that negates the axis would overflow on it. */
static INLINE int16_t convert_u8_to_s16(uint8_t val)
{
   if (val == 0)
      return -0x7fff;
   return (int16_t)(val * 0x0101 - 0x8000);
}

static const char *ps4_joypad_name(unsigned pad)
{
   return "PS4 Controller";
}

static void *ps4_joypad_init(void *data)
{
   OrbisUserServiceLoginUserIdList user_id_list;
   unsigned i;

   num_players = 0;
   memset(ds_joypad_states, 0, sizeof(ds_joypad_states));
   memset(pad_state,        0, sizeof(pad_state));
   memset(analog_state,     0, sizeof(analog_state));

   scePadInit();

   if (sceUserServiceGetLoginUserIdList(&user_id_list) != 0)
   {
      RARCH_ERR("[PS4] sceUserServiceGetLoginUserIdList failed; no pads.\n");
      return (void*)-1;
   }

   for (i = 0; i < PS4_MAX_PADS; i++)
   {
      OrbisUserServiceUserId user_id = user_id_list.userId[i];
      int32_t                handle;

      /* ⚠ THE INVALID MARKER IS -1, NOT 0xFFFFFFFF. The id is a signed int32_t, and the
       * old header's unsigned spelling meant this test never rejected an empty slot. */
      if (user_id == ORBIS_USER_SERVICE_USER_ID_INVALID)
         continue;

      /* ⚠ AND IT MUST BE THE REAL ID. scePadOpen refuses the 0xFF "main user" constant on
       * hardware with 0x809b0001 while the emulator accepts it - which is exactly the kind
       * of difference that costs a day. */
      handle = scePadOpen(user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0, NULL);

      /* Already open is not a failure: something else in this process got there first, and
       * the handle for that user is still the handle we want. */
      if (handle == ORBIS_PAD_ERROR_ALREADY_OPENED)
         handle = scePadGetHandle(user_id, ORBIS_PAD_PORT_TYPE_STANDARD, 0);

      if (handle <= 0)
      {
         RARCH_WARN("[PS4] scePadOpen(user %d) failed: 0x%08x\n",
               (int)user_id, (unsigned)handle);
         continue;
      }

      ds_joypad_states[num_players].user_id   = user_id;
      ds_joypad_states[num_players].handle    = handle;
      ds_joypad_states[num_players].connected = true;

      input_autoconfigure_connect(
            ps4_joypad_name(num_players),
            NULL, NULL,
            ps4_joypad.ident,
            num_players,
            0,
            0);
      num_players++;
   }

   RARCH_LOG("[PS4] %u pad(s) opened.\n", num_players);
   return (void*)-1;
}

static int32_t ps4_joypad_button(unsigned port, uint16_t joykey)
{
   if (port >= PS4_MAX_PADS)
      return 0;
   return pad_state[port] & (UINT64_C(1) << joykey);
}

static int16_t ps4_joypad_axis(unsigned port, uint32_t joyaxis)
{
   if (joyaxis == AXIS_NONE || port >= PS4_MAX_PADS)
      return 0;

   if (AXIS_NEG_GET(joyaxis) < 4)
   {
      int16_t axis = AXIS_NEG_GET(joyaxis);
      int16_t val  = analog_state[port][axis / 2][axis % 2];
      if (val < 0)
         return val;
   }
   else if (AXIS_POS_GET(joyaxis) < 4)
   {
      int16_t axis = AXIS_POS_GET(joyaxis);
      int16_t val  = analog_state[port][axis / 2][axis % 2];
      if (val > 0)
         return val;
   }
   return 0;
}

static int16_t ps4_joypad_state(
      rarch_joypad_info_t *joypad_info,
      const struct retro_keybind *binds,
      unsigned port)
{
   int16_t  ret      = 0;
   uint16_t port_idx = joypad_info->joy_idx;

   if (port_idx < PS4_MAX_PADS)
   {
      int i;
      for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
      {
         /* Auto-binds are per joypad, not per user. */
         const uint64_t joykey = (binds[i].joykey != NO_BTN)
            ? binds[i].joykey : joypad_info->auto_binds[i].joykey;
         if (     (uint16_t)joykey != NO_BTN
               && pad_state[port_idx] & (UINT64_C(1) << (uint16_t)joykey))
            ret |= (1 << i);
      }
   }

   return ret;
}

static void ps4_joypad_get_buttons(unsigned port_num, input_bits_t *state)
{
   if (port_num < PS4_MAX_PADS)
   {
      BITS_COPY16_PTR(state, pad_state[port_num]);
   }
   else
      BIT256_CLEAR_ALL_PTR(state);
}

static void ps4_joypad_poll(void)
{
   unsigned i;

   for (i = 0; i < num_players; i++)
   {
      OrbisPadData data;
      uint32_t     b;

      /* ⚠ NEVER STOP READING A SLOT, WHICH IS WHAT THIS USED TO DO. The loop began by skipping
       * any pad whose `connected` was false, and the disconnect branch below set it false and
       * nothing ever set it back - so the flag was a latch rather than a state. A DualShock 4
       * that goes to sleep reports `connected == 0` for one frame, and from then on its slot
       * was never polled again: the pad was gone for the rest of the process and only a
       * restart brought it back. Measured on hardware 2026-08-28.
       *
       * The read is cheap and the console answers it whether or not a pad is awake, so the
       * flag is now DERIVED from what the read said, every frame. */
      if (scePadReadState(ds_joypad_states[i].handle, &data) != 0)
      {
         /* A failed read means this frame said nothing, not that the buttons held last frame
          * are still held. Latching them is how a menu runs away on its own.
          *
          * ⚠ AND IT DOES NOT MEAN THE PAD IS GONE. The handle is reported as still ours; only
          * a state that says `connected == 0` is a disconnection, so a transient read error
          * leaves the slot alone rather than tearing it down. */
         pad_state[i] = 0;
         continue;
      }

      /* ⚠ THIS TEST WAS `!~data.connected`, which is true only when the field is all ones.
       * `connected` is a uint8_t holding 0 or 1, so the disconnect branch could never run
       * and a pad that went away stayed "connected" forever. */
      if (!data.connected)
      {
         if (ds_joypad_states[i].connected)
            RARCH_LOG("[PS4] pad %u went away.\n", i);
         ds_joypad_states[i].connected = false;
         pad_state[i]                  = 0;
         memset(analog_state[i], 0, sizeof(analog_state[i]));
         continue;
      }

      /* Back, on the same handle. Nothing has to be reopened - which is why the slot must keep
       * being read rather than written off. */
      if (!ds_joypad_states[i].connected)
      {
         ds_joypad_states[i].connected = true;
         RARCH_LOG("[PS4] pad %u is back.\n", i);
      }

      b            = data.buttons;
      pad_state[i] = 0;

      pad_state[i] |= (b & ORBIS_PAD_BUTTON_LEFT)      ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_LEFT)   : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_DOWN)      ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_DOWN)   : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_RIGHT)     ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_RIGHT)  : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_UP)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_UP)     : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_OPTIONS)   ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_START)  : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_TOUCH_PAD) ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_SELECT) : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_TRIANGLE)  ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_X)      : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_SQUARE)    ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_Y)      : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_CROSS)     ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_B)      : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_CIRCLE)    ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_A)      : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_R1)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_R)      : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_L1)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_L)      : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_R2)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_R2)     : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_L2)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_L2)     : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_R3)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_R3)     : 0;
      pad_state[i] |= (b & ORBIS_PAD_BUTTON_L3)        ? (UINT64_C(1) << RETRO_DEVICE_ID_JOYPAD_L3)     : 0;

      analog_state[i][RETRO_DEVICE_INDEX_ANALOG_LEFT ][RETRO_DEVICE_ID_ANALOG_X] =
         convert_u8_to_s16(data.leftStick.x);
      analog_state[i][RETRO_DEVICE_INDEX_ANALOG_LEFT ][RETRO_DEVICE_ID_ANALOG_Y] =
         convert_u8_to_s16(data.leftStick.y);
      analog_state[i][RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_X] =
         convert_u8_to_s16(data.rightStick.x);
      analog_state[i][RETRO_DEVICE_INDEX_ANALOG_RIGHT][RETRO_DEVICE_ID_ANALOG_Y] =
         convert_u8_to_s16(data.rightStick.y);
   }
}

/* ⚠ THIS ASKED pad_state[pad], i.e. "is a button held right now". A pad with nothing
 * pressed reported itself absent, every frame, which is not what any caller means by
 * query_pad. */
static bool ps4_joypad_query_pad(unsigned pad)
{
   return pad < num_players && ds_joypad_states[pad].connected;
}

static bool ps4_joypad_rumble(unsigned pad,
      enum retro_rumble_effect effect, uint16_t strength)
{
   /* The motors are 8-bit and RetroArch's strength is 16-bit. Both motors are set on every
    * call because OrbisPadVibeParam carries the pair and writing one would zero the other;
    * the effect not being addressed keeps whatever it had. */
   static OrbisPadVibeParam params[PS4_MAX_PADS];

   if (pad >= num_players || !ds_joypad_states[pad].connected)
      return false;

   switch (effect)
   {
      case RETRO_RUMBLE_WEAK:
         params[pad].smMotor = (uint8_t)(strength >> 8);
         break;
      case RETRO_RUMBLE_STRONG:
         params[pad].lgMotor = (uint8_t)(strength >> 8);
         break;
      default:
         return false;
   }

   return scePadSetVibration(ds_joypad_states[pad].handle, &params[pad]) == 0;
}

static void ps4_joypad_destroy(void)
{
   unsigned i;

   for (i = 0; i < num_players; i++)
   {
      if (ds_joypad_states[i].handle > 0)
         scePadClose(ds_joypad_states[i].handle);
      ds_joypad_states[i].handle    = 0;
      ds_joypad_states[i].connected = false;
   }

   num_players = 0;
}

input_device_driver_t ps4_joypad = {
   ps4_joypad_init,
   ps4_joypad_query_pad,
   ps4_joypad_destroy,
   ps4_joypad_button,
   ps4_joypad_state,
   ps4_joypad_get_buttons,
   ps4_joypad_axis,
   ps4_joypad_poll,
   ps4_joypad_rumble,
   NULL, /* set_rumble_gain */
   NULL, /* set_sensor_state */
   NULL, /* get_sensor_input */
   ps4_joypad_name,
   "ps4",
};
