/* Copyright  (C) 2010-2020 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (dylib.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <dynamic/dylib.h>
#include <encodings/utf.h>
#include <string/stdstring.h>
#include <retro_miscellaneous.h>
#include <file/file_path.h>

#if defined(ORBIS)
#include <orbis/libkernel.h>
#include "../../verbosity.h"
#endif

#ifdef NEED_DYNAMIC

#ifdef _WIN32
#include <compat/posix_string.h>
#include <windows.h>
#else
#if !defined(ORBIS)
#include <dlfcn.h>
#endif
#endif

#if defined(ORBIS)
/* ⚠ THERE IS NO dlerror() ON THIS PLATFORM, and dylib_error() called it anyway - the ORBIS
 * build has no <dlfcn.h> at all, so the moment HAVE_DYNAMIC is turned on the file stops
 * compiling. It cannot have been built here since the branch was written. The module
 * loader reports through a return code instead, so that is what gets kept. */
static char last_dyn_err[128];
#endif

#ifdef _WIN32
static char last_dyn_err[512];

static void set_dl_err(void)
{
   DWORD err = GetLastError();
   if (FormatMessage(
              FORMAT_MESSAGE_IGNORE_INSERTS
            | FORMAT_MESSAGE_FROM_SYSTEM,
            NULL, err,
            MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
            last_dyn_err, sizeof(last_dyn_err) - 1,
            NULL) == 0)
      snprintf(last_dyn_err, sizeof(last_dyn_err) - 1,
            "unknown error %lu", err);
}
#endif

/**
 * dylib_load:
 * @path                         : Path to libretro core library.
 *
 * Platform independent dylib loading.
 *
 * @return Library handle on success, otherwise NULL.
 **/
#if defined(ORBIS)
/* ⚠ THE MODULE'S GLOBAL CONSTRUCTORS DO NOT RUN BY THEMSELVES ON THIS CONSOLE, AND THE FRONTEND
 * HAS TO DO IT. Measured 2026-08-24, and it took a control run to see it.
 *
 * Every C++-dominant core crashed on load with
 *
 *     terminating with uncaught exception of type std::length_error:
 *     allocator<T>::allocate(size_t n) 'n' exceeds maximum supported size
 *
 * which is what an unconstructed global container looks like the moment something asks it for a
 * size. C-only cores were unaffected, so the split ran exactly along the language - seventeen for
 * seventeen before the cause was known.
 *
 * The chain, each step measured rather than assumed:
 *
 *   1. crtlib.o's module_start does walk __init_array_start..__init_array_end and call every
 *      entry. The code is correct.
 *   2. Those two symbols are BSS VARIABLES IN crtlib.o - eight bytes apart, never filled - not
 *      linker-provided section bounds, and OpenOrbis's link.x collects .init_array without
 *      defining anything around it. Bracketing the section in our own script fixed the symbols
 *      and changed nothing, which is how we learned the walk was not happening at all.
 *   3. ⚠ module_start is GLOBAL HIDDEN in crtlib.o, so the linker correctly makes it LOCAL in
 *      the output. create-fself builds a module's export table from the GLOBAL symbols in
 *      .symtab - which is why retro_run is reachable while .dynsym holds neither - so a local
 *      module_start can never be found by the loader. It is not called for any module.
 *   4. A constructor placed first in .init_array, in a core that WORKS, never ran. That control
 *      is what separated "this core dies early" from "no module ever runs constructors".
 *
 * So the frontend walks the array itself, after the module is loaded and before anything calls
 * into it. The bounds are ordinary global symbols, so sceKernelDlsym finds them.
 *
 * ⚠ AND IT SKIPS NULL ENTRIES ON PURPOSE. A core linked WITHOUT the corrected script still
 * exports crtlib.o's BSS pair: a one-entry array containing zero. Calling that would jump to
 * address zero and take the process down - worse than the bug being fixed. Skipping nulls makes
 * an old core a no-op instead. */
/* ⚠ THE QUESTION IS NOT WHICH IMAGE THIS IS, IT IS WHETHER THAT IMAGE STILL HOLDS ITS DATA -
 * AND FOUR VERSIONS OF THIS TABLE GUESSED AT IT BEFORE ONE MEASURED IT.
 *
 * Constructors must run exactly once per image INSTANCE. Run them twice over a live image and
 * every global with a dynamic initializer is rebuilt underneath the code using it; skip them on a
 * freshly mapped image and every such global stays zero. Both were reproduced on hardware, and
 * both look like bugs in the core rather than in the loader:
 *
 *   0x54, a WRITE to a not-present page (err 6). flycast keeps `static std::vector<sched_list>
 *   sch_list` - a global WITH a dynamic initializer - and hands out indices into it. `int
 *   vblank_schid` is zero-initialised, has NO dynamic initializer, and so is not in .init_array.
 *   Re-running the array empties the vector (begin = nullptr, its heap block orphaned) and leaves
 *   vblank_schid holding 2, so sh4_sched_request(2, ...) writes sch_list[2].start:
 *       nullptr + 2 * 32 + 0x14 = 0x54          (rip = sh4_sched_request+0x55, measured 19:00)
 *
 *   rip = 0, an INSTRUCTION FETCH at a not-present page (err 0x14). A call through a function
 *   pointer that a skipped constructor never installed. (measured 18:35)
 *
 * Version one keyed on the module id and never cleared it: the kernel recycles ids, so nestopia's
 * record matched quicknes and quicknes died on a null read at 0x20 (2026-08-24).
 * Version two cleared on close: the id is FRESH on every load, so the lookup never matched at all
 * and the constructors ran on every load - five times for flycast in one session.
 * Version three keyed on identity (.init_array address plus path) and never cleared: 18:35, a
 * load after "[Core] Unloading core..." skipped them and died at rip = 0.
 * Version four added clear-on-close: 19:00, constructors ran on every load again - RetroArch's
 * info probe loads and closes the core WITHOUT logging an unload, so the clear fired on those
 * too - and one of those runs landed mid-game, on a running emulator, giving 0x54.
 *
 * ⚠ AND ONE OF THE ARGUMENTS FOR VERSION FOUR WAS SIMPLY WRONG, WHICH IS WHY IT IS WRITTEN
 * DOWN HERE. It read `emu.state` coming back as Uninitialized on the 18:35 reload as proof that
 * the module's .bss had been zeroed. It is not proof: the load before it was an info probe -
 * there is no environment-callback block against it in the log - so retro_init had never run and
 * Emulator::init() had never set that field. The state was Uninitialized because nothing had
 * initialised it, not because anything had erased it. That reasoning is retracted; what caused
 * the 18:35 rip = 0 is still open, and if it returns while the measurement below says the image
 * was STILL MAPPED then the null belongs to the core's own init/deinit ordering and not here.
 *
 * So the table identifies an image - the address its .init_array landed at, and the file it came
 * from, both read out of the module rather than lent by the kernel - and dylib_close ASKS THE
 * KERNEL whether that image survived the unload. See dylib_orbis_image_resident(). */
#define DYLIB_ORBIS_PATH_MAX 256
static struct
{
   int32_t  mod;
   void    *init_array;
   char     path[DYLIB_ORBIS_PATH_MAX];
} dylib_orbis_ctors_done[16];
static unsigned dylib_orbis_n_done;

/* ⚠ ASK THE KERNEL WHETHER THE IMAGE IS STILL THERE, RATHER THAN INFERRING IT FROM THE CLOSE.
 *
 * `at` is the address this module's .init_array occupied. sceKernelGetModuleList walks what is
 * actually mapped in this process and sceKernelGetModuleInfo gives each module's segments, so an
 * address that still falls inside a loaded segment belongs to an image that still exists - and an
 * image that still exists still holds the data its constructors wrote. That is the whole question,
 * asked of the thing that knows the answer.
 *
 * ⚠ A FAILURE TO ENUMERATE IS ANSWERED "STILL THERE", AND THE CHOICE IS NOT ARBITRARY. Guessing
 * "gone" makes the next load re-run constructors, which is the fault that corrupted flycast's
 * scheduler (0x54); guessing "still there" makes it skip them, which is only wrong if the image
 * really was torn down. Every module load measured on this console so far has come back at the
 * same address with its data intact, so "still there" is the likelier of the two - and the call
 * below says so in the log rather than deciding in silence. */
static bool dylib_orbis_image_resident(const void *at)
{
   OrbisKernelModule list[128];
   size_t            avail = 0;
   size_t            i;
   unsigned          s;
   const uint8_t    *addr  = (const uint8_t*)at;

   if (!at)
      return false;

   if (sceKernelGetModuleList(list, sizeof(list) / sizeof(list[0]), &avail) != 0)
   {
      ps4_log("dylib: sceKernelGetModuleList failed - cannot tell whether %p is still mapped, "
              "assuming it is and NOT re-running constructors", at);
      return true;
   }
   if (avail > sizeof(list) / sizeof(list[0]))
      avail = sizeof(list) / sizeof(list[0]);

   for (i = 0; i < avail; i++)
   {
      OrbisKernelModuleInfo info;
      memset(&info, 0, sizeof(info));
      info.size = sizeof(info);
      if (sceKernelGetModuleInfo(list[i], &info) != 0)
         continue;
      for (s = 0; s < info.segmentCount && s < 4; s++)
      {
         const uint8_t *base = (const uint8_t*)info.segmentInfo[s].address;
         if (base && addr >= base && addr < base + info.segmentInfo[s].size)
            return true;
      }
   }
   return false;
}

/* Drop a module's record. Found by id because that is all dylib_close is given - the id is stable
 * for the length of one open, which is exactly the span this needs. */
static void dylib_orbis_forget(dylib_t lib)
{
   int32_t  mod = (int32_t)(intptr_t)lib;
   unsigned i   = 0;

   while (i < dylib_orbis_n_done)
   {
      if (dylib_orbis_ctors_done[i].mod != mod)
      {
         i++;
         continue;
      }
      dylib_orbis_ctors_done[i] = dylib_orbis_ctors_done[--dylib_orbis_n_done];
   }
}

/* The .init_array address recorded for an open module id, or NULL if we have no record. */
static void *dylib_orbis_recorded_init_array(dylib_t lib)
{
   int32_t  mod = (int32_t)(intptr_t)lib;
   unsigned i;

   for (i = 0; i < dylib_orbis_n_done; i++)
      if (dylib_orbis_ctors_done[i].mod == mod)
         return dylib_orbis_ctors_done[i].init_array;
   return NULL;
}

/* ps4_log: RARCH_LOG is not guaranteed to reach the console channel. */
#include "../../ps4/ps4_log.h"

static void dylib_orbis_run_init_array(dylib_t lib, const char *path)
{
   typedef void (*orbis_ctor_t)(void);
   /* ⚠ ONCE PER RESIDENT IMAGE. sceKernelLoadStartModule on an already-loaded module hands back
    * the SAME id without reloading it, and RetroArch loads a core several times over - to read
    * its info, then to run it. Measured: the four constructors of mednafen_gba ran EIGHT times
    * in one session, and flycast's 61 ran five times, the last one over a fully initialised
    * emulator. See the table above for what that did and how the key is chosen.
    *
    * Sixteen slots because a session with more distinct cores loaded than that is not a case this
    * needs to be clever about - past the end it simply stops running constructors, which is the
    * behaviour every core had before this function existed. */
   unsigned j;
   orbis_ctor_t *first = NULL, *last = NULL;
   int32_t       mod   = (int32_t)(intptr_t)lib;
   unsigned      ran   = 0;
   unsigned      slot  = (unsigned)-1;
   orbis_ctor_t *start = NULL;

   if (!path)
      return;

   if (sceKernelDlsym(mod, "__init_array_start", (void**)&first) != 0 || !first)
      return;
   if (sceKernelDlsym(mod, "__init_array_end", (void**)&last) != 0 || !last)
      return;
   if (last <= first || (size_t)(last - first) > 4096)
      return;
   start = first;

   /* ⚠ THE LOOKUP HAPPENS AFTER __init_array_start IS KNOWN, BECAUSE IT IS PART OF THE KEY. An
    * entry matching on id alone is a recycled id or a re-mapped image, not this image. */
   /* ⚠ AND THE ID IS NOT PART OF THE MATCH, BECAUSE IT IS NOT STABLE. Version three keyed on it
    * first - `if (entry.mod != mod) continue;` - and every load still ran the constructors:
    * measured 2026-08-30, melondsds' two constructors ran FIVE times in one session with this
    * table in place, and flycast's sixty-one four times, all reporting the same retro_run address
    * and therefore the same resident image. sceKernelLoadStartModule hands back a FRESH handle
    * for a module it did not reload, so a lookup that starts at the id never finds the entry it
    * wrote a moment ago, and simply appends another one until the table fills.
    *
    * What identifies an image is where its .init_array landed and which file it came from. Both
    * are read out of the module itself rather than lent by the kernel. The id is still recorded
    * and refreshed on every match, but only so dylib_close - which is handed nothing else - can
    * find the entry to drop; no LOOKUP keys on it.
    *
    * This is also strictly safer than the id ever was for the 2026-08-24 case that started all
    * this: nestopia unloaded and quicknes given the same id differ by PATH, so quicknes runs its
    * constructors. */
   for (j = 0; j < dylib_orbis_n_done; j++)
   {
      if (dylib_orbis_ctors_done[j].init_array != (void*)start)
         continue;
      if (strncmp(dylib_orbis_ctors_done[j].path, path, DYLIB_ORBIS_PATH_MAX - 1))
         continue;
      dylib_orbis_ctors_done[j].mod = mod;
      return;
   }

   for (; first < last; first++)
   {
      if (*first)
      {
         (*first)();
         ran++;
      }
   }

   if (slot == (unsigned)-1
         && dylib_orbis_n_done < sizeof(dylib_orbis_ctors_done) / sizeof(dylib_orbis_ctors_done[0]))
      slot = dylib_orbis_n_done++;
   if (slot != (unsigned)-1)
   {
      dylib_orbis_ctors_done[slot].mod        = mod;
      dylib_orbis_ctors_done[slot].init_array = (void*)start;
      strncpy(dylib_orbis_ctors_done[slot].path, path, DYLIB_ORBIS_PATH_MAX - 1);
      dylib_orbis_ctors_done[slot].path[DYLIB_ORBIS_PATH_MAX - 1] = '\0';
   }

   if (ran)
      RARCH_LOG("[PS4] ran %u global constructor(s) for %s\n", ran, path);

   /* ⚠ WHERE THE MODULE LANDED, BECAUSE NOTHING ELSE SAYS IT ANY MORE. A crash used to be located
    * by taking the kernel's dump - it prints each module's text range - and subtracting. Since
    * orbis-compat's signal handlers were connected the process SURVIVES a SIGSEGV and idles, so
    * the kernel writes no dump at all: the fault address arrives with nothing to measure it
    * against. One exported symbol is enough, because its offset inside the .elf is known on the
    * build machine:
    *
    *     module base = <printed address> - <retro_run's address in the core's .elf>
    *
    * retro_run specifically: every libretro core has it, ps4/build-cores.sh already refuses to
    * publish a module without it, and llvm-nm on the .elf gives the other half. */
   {
      void *probe = NULL;
      if (sceKernelDlsym(mod, "retro_run", &probe) == 0 && probe)
         ps4_log("dylib: %s has retro_run at %p - subtract its .elf address for the module base",
               path, probe);
   }
}
#endif /* ORBIS */

dylib_t dylib_load(const char *path)
{
#ifdef _WIN32
#ifndef __WINRT__
   int prevmode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
#endif
#ifdef __WINRT__
   dylib_t lib;
   /* On UWP, you can only load DLLs inside your install directory, using a special function that takes a relative path */
   char relative_path_abbrev[PATH_MAX_LENGTH];
   char *relative_path = relative_path_abbrev;
   wchar_t *path_wide  = NULL;

   relative_path_abbrev[0] = '\0';

   if (!path_is_absolute(path))
      RARCH_WARN("Relative path in dylib_load! This is likely an attempt to load a system library that will fail.\n");

   fill_pathname_abbreviate_special(relative_path_abbrev, path, sizeof(relative_path_abbrev));

   /* Path to dylib_load is not inside app install directory.
    * Loading will probably fail. */
   if (relative_path[0] != ':' || !PATH_CHAR_IS_SLASH(relative_path[1])) { }
   else
      relative_path += 2;

   path_wide = utf8_to_utf16_string_alloc(relative_path);
   lib       = LoadPackagedLibrary(path_wide, 0);
   free(path_wide);
#elif defined(LEGACY_WIN32_RUNTIME)
   dylib_t lib        = NULL;

   if (win32_needs_local_encoding())
      lib             = LoadLibraryA(path);
   else
   {
      wchar_t *path_wide = utf8_to_utf16_string_alloc(path);
      lib             = LoadLibraryW(path_wide);
      free(path_wide);
   }
#elif defined(LEGACY_WIN32)
   dylib_t lib        = LoadLibrary(path);
#else
   wchar_t *path_wide = utf8_to_utf16_string_alloc(path);
   dylib_t lib        = LoadLibraryW(path_wide);
   free(path_wide);
#endif

#ifndef __WINRT__
   SetErrorMode(prevmode);
#endif

   if (!lib)
   {
      set_dl_err();
      return NULL;
   }
   last_dyn_err[0] = 0;
#elif defined(ORBIS)
   /* ⚠ THE RETURN VALUE IS A MODULE ID OR A NEGATIVE ERROR, NOT A POINTER. Casting it
    * straight to dylib_t made every FAILED load look like a successful one: an error such
    * as 0x8002... is non-NULL, so the caller went on to resolve symbols out of a module
    * that was never loaded. Check the sign first, and only then carry the id as an opaque
    * handle. */
   int      res;
   int32_t  mod    = (int32_t)sceKernelLoadStartModule(path, 0, NULL, 0, NULL, &res);
   dylib_t  lib    = (mod < 0) ? NULL : (dylib_t)(intptr_t)mod;

   if (mod < 0)
      snprintf(last_dyn_err, sizeof(last_dyn_err),
            "sceKernelLoadStartModule(\"%s\") failed: 0x%08x", path, (unsigned)mod);
   else
   {
      last_dyn_err[0] = '\0';
      dylib_orbis_run_init_array(lib, path);
   }
#elif defined(IOS) || defined(OSX)
    dylib_t lib;
    static const char fw_suffix[] = ".framework";
    if (string_ends_with(path, fw_suffix))
    {
        char fw_path[PATH_MAX_LENGTH];
        const char *fw_name = path_basename(path);
        size_t _len         = strlcpy(fw_path, path, sizeof(fw_path));
        _len += strlcpy(fw_path + _len, "/", sizeof(fw_path) - _len);
        /* Assume every framework binary is named for the framework. Not always
         * a great assumption but correct enough for our uses. */
        strlcpy(fw_path + _len, fw_name, strlen(fw_name) - STRLEN_CONST(fw_suffix) + 1);
        lib = dlopen(fw_path, RTLD_LAZY | RTLD_LOCAL);
    }
    else
        lib = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
#else
   dylib_t lib = dlopen(path, RTLD_LAZY | RTLD_LOCAL);
#endif
   return lib;
}

char *dylib_error(void)
{
#if defined(_WIN32) || defined(ORBIS)
   if (last_dyn_err[0])
      return last_dyn_err;
   return NULL;
#else
   return (char*)dlerror();
#endif
}

function_t dylib_proc(dylib_t lib, const char *proc)
{
   function_t sym;

#ifdef _WIN32
   HMODULE mod = (HMODULE)lib;
   if (!mod)
   {
#ifdef __WINRT__
      /* GetModuleHandle is not available on UWP */
      /* It's not possible to lookup symbols in current executable
       * on UWP. */
      DebugBreak();
      return NULL;
#else
      mod = GetModuleHandle(NULL);
#endif
   }
   if (!(sym = (function_t)GetProcAddress(mod, proc)))
   {
      set_dl_err();
      return NULL;
   }
   last_dyn_err[0] = 0;
#elif defined(ORBIS)
   void *ptr_sym = NULL;
   sym = NULL;

   if (lib)
   {
     /* sceKernelDlsym takes the module id as an int32_t (orbis/libkernel.h:125). The
      * SceKernelModule spelling this used is orbisdev's; OpenOrbis calls the type
      * OrbisKernelModule, and this entry point does not take it at all. */
     sceKernelDlsym((int32_t)(intptr_t)lib, proc, &ptr_sym);
     memcpy(&sym, &ptr_sym, sizeof(void*));
   }
#else
   void *ptr_sym = NULL;

   if (lib)
      ptr_sym = dlsym(lib, proc);
   else
   {
      void *handle = dlopen(NULL, RTLD_LAZY);
      if (handle)
      {
         ptr_sym = dlsym(handle, proc);
         dlclose(handle);
      }
   }

   /* Dirty hack to workaround the non-legality of
    * (void*) -> fn-pointer casts. */
   memcpy(&sym, &ptr_sym, sizeof(void*));
#endif

   return sym;
}

/**
 * dylib_close:
 * @lib                          : Library handle.
 *
 * Frees library handle.
 **/
void dylib_close(dylib_t lib)
{
#ifdef _WIN32
   if (!FreeLibrary((HMODULE)lib))
      set_dl_err();
   last_dyn_err[0] = 0;
#elif defined(ORBIS)
   int    res         = 0;
   int32_t rc;
   /* ⚠ WHETHER THIS CLOSE ENDS THE IMAGE IS MEASURED AFTERWARDS, NOT ASSUMED EITHER WAY.
    * Three versions of the table above guessed at it and each guess was falsified on hardware:
    * assuming the image survives skipped constructors a fresh image needed (rip = 0), and
    * assuming it dies re-ran them over a live one (the 0x54 write into a re-constructed
    * std::vector). The record is kept or dropped according to what the kernel says is still
    * mapped once the unload has been asked for. */
   void  *init_array  = dylib_orbis_recorded_init_array(lib);
   rc = sceKernelStopUnloadModule((OrbisKernelModule)(intptr_t)lib, 0, NULL, 0, NULL, &res);

   if (init_array)
   {
      bool resident = dylib_orbis_image_resident(init_array);
      /* ⚠ LOGGED EVERY TIME, BECAUSE THIS IS THE MEASUREMENT THE NEXT DIAGNOSIS RESTS ON. Which
       * way this answers decides whether the next load constructs or not, and a wrong answer
       * shows up as one of the two faults named above rather than as anything mentioning modules. */
      ps4_log("dylib: unload of module %d -> rc 0x%08x res %d; .init_array %p is %s, so the next "
              "load %s run its constructors",
            (int)(intptr_t)lib, (unsigned)rc, res, init_array,
            resident ? "STILL MAPPED" : "GONE",
            resident ? "will NOT" : "will");
      if (!resident)
         dylib_orbis_forget(lib);
   }
#else
#ifndef NO_DLCLOSE
   dlclose(lib);
#endif
#endif
}

#endif
