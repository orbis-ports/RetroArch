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
/* ⚠ THE TABLE IS CLEARED ON UNLOAD, AND LEAVING THAT OUT COST A CORE. sceKernelStopUnloadModule
 * frees the id and the kernel HANDS THE SAME ONE TO THE NEXT MODULE. Measured 2026-08-24:
 * nestopia loaded and was unloaded, quicknes loaded into the same id, this table still held it,
 * its two constructors were skipped, and the core died on a null read at 0x20 - a SIGSEGV that
 * looks exactly like a broken core and was the loader forgetting to forget. */
static int32_t dylib_orbis_ctors_done[16];
static unsigned dylib_orbis_n_done;

static void dylib_orbis_forget(dylib_t lib)
{
   int32_t  mod = (int32_t)(intptr_t)lib;
   unsigned i;

   for (i = 0; i < dylib_orbis_n_done; i++)
   {
      if (dylib_orbis_ctors_done[i] != mod)
         continue;
      dylib_orbis_ctors_done[i] = dylib_orbis_ctors_done[--dylib_orbis_n_done];
      return;
   }
}

static void dylib_orbis_run_init_array(dylib_t lib, const char *path)
{
   typedef void (*orbis_ctor_t)(void);
   /* ⚠ ONCE PER MODULE, AND THE FIRST VERSION OF THIS GOT IT WRONG. sceKernelLoadStartModule on
    * an already-loaded module hands back the SAME id without reloading it, and RetroArch loads a
    * core several times over - to read its info, then to run it. Measured: the four constructors
    * of mednafen_gba ran EIGHT times in one session, so every global was constructed over itself
    * repeatedly. A leak at best; for anything holding a mutex or a buffer length, corruption.
    *
    * Sixteen slots because a session with more distinct cores loaded than that is not a case this
    * needs to be clever about - past the end it simply stops running constructors, which is the
    * behaviour every core had before this function existed. */
   unsigned j;
   orbis_ctor_t *first = NULL, *last = NULL;
   int32_t       mod   = (int32_t)(intptr_t)lib;
   unsigned      ran   = 0;
   unsigned      i;

   for (j = 0; j < dylib_orbis_n_done; j++)
      if (dylib_orbis_ctors_done[j] == mod)
         return;

   if (sceKernelDlsym(mod, "__init_array_start", (void**)&first) != 0 || !first)
      return;
   if (sceKernelDlsym(mod, "__init_array_end", (void**)&last) != 0 || !last)
      return;
   if (last <= first || (size_t)(last - first) > 4096)
      return;

   for (; first < last; first++)
   {
      if (*first)
      {
         (*first)();
         ran++;
      }
   }

   if (dylib_orbis_n_done < sizeof(dylib_orbis_ctors_done) / sizeof(dylib_orbis_ctors_done[0]))
      dylib_orbis_ctors_done[dylib_orbis_n_done++] = mod;

   if (ran)
      RARCH_LOG("[PS4] ran %u global constructor(s) for %s\n", ran, path);
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
   int res;
   dylib_orbis_forget(lib);
   sceKernelStopUnloadModule((OrbisKernelModule)(intptr_t)lib, 0, NULL, 0, NULL, &res);
#else
#ifndef NO_DLCLOSE
   dlclose(lib);
#endif
#endif
}

#endif
