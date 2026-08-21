# The running account of the PS4 port

Newest entry last. Everything here is an account of work; nothing in the build reads this file.
The plan it is executing is `ps4/PLAN.md`.

---

## 2026-08-22 — Phases 0 and 1, in one sitting

`ps4-base` = c59b1833f7, the tree before any of this. Work on `ps4-support`.

### What was expected, and what happened

The plan gave Phase 1 — "compile all 261 objects and read the link errors" — 2 to 6 sessions, and
called it the only real unknown in the whole estimate. **It cost nothing.** The build compiles 239
objects with **zero warnings** and links, and the only symbols missing at the end were four dangling
driver-table entries and one function deleted from the tree in 2021.

Not one libc or POSIX shim had to be written. RetroArch touches `stat`, `mmap`, `dirent`,
`pthread`, `clock_gettime`, POSIX timers, `open`/`rename`/`unlink` and sockets, and
`orbis-compat` already answered all of it — the overlay was built for a game engine, a Vulkan
driver and a conformance suite, and a frontend turns out to need nothing those three did not.

**This collapses the estimate.** PLAN.md §5 put the whole software leg at 9-16 sessions with the
spread almost entirely in Phase 1. That spread is gone.

### What actually had to change

Five things, none of them a shim:

1. **The Makefile.** Rewritten against OpenOrbis; see the commit. Its OBJ list had also been empty
   since 2021 (a `\` continuation running into a `#` line).
2. **`verbosity.h`** put every `RARCH_LOG` on `debugNetPrintf`. Now on the overlay's channel, and
   split: DBG/LOG/WARN over UDP only, ERR also on klog. klog costs 8-15 ms a line on this console.
   The `_V` variants were passing a `va_list` to a variadic function as an ordinary argument and
   printed garbage at every call site; they format now.
3. **`platform_orbis.c`** included ten orbisdev headers. Rewritten against `<orbis/*.h>`.
4. **`mem_stats.c`** called `get_user_mem_size()`, deleted in 2021 and replaced by an external
   `-luser_mem_sys` the link line still named. `ps4/ps4_mem.c` answers out of flexible memory.
5. **Four driver tables** named drivers that cannot build here.

### It boots

Under `unemups4`, from a plain `.elf`:

```
[retroarch] alive - main() entered, built Aug 22 2026 01:01:04
[SYSCALL] sceKernelOpen('/app0/ps4-run.cfg', ...)
[USER_SERVICE] sceUserServiceInitialize
[ERROR] [Config] Config not found at: "/data/retroarch/retroarch.cfg".
[INFO] RetroArch 1.22.2 (Git c81e53c9af)
[INFO] Capabilities: MMX MMXEXT SSE SSE2 SSE3 SSSE3 SSE4 SSE42 AES PCLMUL
```

The log channel works, the run config is read, UserService comes up, the config system runs and
says what it could not find, and RetroArch prints its own banner. Then it creates its first thread
and stops.

### ⚠ The emulator leg is blocked, and the port is not what is wrong

`unemups4` kills the process on the first call to `pthread_attr_get_np`, and behind it are
seventeen more its linker reports as `Stubbed missing`:

```
cpuset_getaffinity  getrlimit  mprotect  msync  _nanosleep
ktimer_{create,delete,getoverrun,gettime,settime}
pthread_attr_{getdetachstate,setdetachstate,getguardsize,setguardsize,setschedpolicy}
pthread_cancel  pthread_setcancelstate  pthread_set_name_np
```

**All eighteen are real PS4 exports** — every one of them is in `unemups4`'s own
`data/ps4_names.txt`, the NID name database. So this is not a port that calls something the console
does not have; it is an emulator that has not implemented what the console does. Nothing in
RetroArch or `orbis-compat` should be contorted to avoid these.

Several are one word of work in unemups4, because the Sce-spelled twin is already implemented and
the POSIX spelling merely is not in its `names = [...]` list: `scePthreadAttrGet`,
`scePthreadSetName`/`scePthreadRename`, `scePthreadCancel`, the detachstate and schedpolicy
setters, `SYS_NANOSLEEP`. The rest (`mprotect`, `msync`, the `ktimer_*` family,
`cpuset_getaffinity`, `getrlimit`, the guardsize pair, `pthread_setcancelstate`) have no
implementation there at all and want real ones — and getting their semantics wrong would be worse
than leaving them missing, in exactly the way that repository's own `sys_sigaltstack` comment
argues.

⚠ **One line of unemups4 was changed to get as far as the banner** and is left UNCOMMITTED in that
working tree: `crates/libs/src/libkernel/pthread.rs`, adding `"pthread_attr_get_np"` to
`scePthreadAttrGet`'s `names`. Its release binary was rebuilt from that. Keep it or drop it; it is
one word and it is correct. (That tree was already dirty with an unrelated, finished
`sys_sigaltstack` change from another session.)

### ⚠ orbis-compat creates `/data/OpenGothic` for every consumer

`src/orbis_paths.cpp:19` hard-codes the anchor root. RetroArch's run makes an empty
`/data/OpenGothic/` directory it will never use. Harmless — the anchor only rewrites RELATIVE
paths, and RetroArch's are all absolute — but it is a title's name compiled into a shared overlay,
and the fourth consumer is where that stops being invisible.

### Where the next session starts

Phase 2 is effectively done (the frontend driver). Phase 3 is the software video driver, and
nothing above blocks writing it. What IS blocked is watching it run, so the choice at the top of
the next session is: teach `unemups4` the eighteen exports first, or write Phases 3-5 blind and
verify them on hardware.

---

## 2026-08-22, later — the survey of what else is still orbisdev-shaped

A sweep of every `ORBIS`/`__PS4__`/`PS4` preprocessor condition left in the tree. Three of the
findings were live in code that compiles TODAY and are fixed (commits `63f7566167`, `9ba278d81b`);
the rest is a work list for the phases that revive each file.

### Fixed, because they were live

* `vfs_implementation.c` `dirent_check_err()` classed ORBIS with the Vita and compared a `DIR*`
  against 0 — always false, so a failed `opendir()` read as success.
* `vfs_implementation.c` `retro_vfs_truncate_impl()` excluded ORBIS from `ftruncate`, which this
  SDK has, so it returned -1 for everyone.
* `dylib.c`: a negative `sceKernelLoadStartModule` error cast to `dylib_t` made every failed load
  look successful; `SceKernelModule` is orbisdev's spelling; `dylib_error()` called `dlerror()` on
  a platform with no `<dlfcn.h>`. The whole branch had never been compiled.

### ⚠ `libretro-common/include/defines/ps4_defines.h` should be DELETED, not fixed

Nothing compiled still includes it — its only four includers (`psp_audio.c`, `orbis_ctx.c`,
`ps4_input.c`, `ps4_joypad.c`) are all out of the object list. Every macro in it is either supplied
correctly by an `<orbis/_types/*.h>` or is wrong, and the two that are wrong are exactly the ones
that would keep Phase 4 subtly broken:

* `SCE_USER_SERVICE_MAX_LOGIN_USERS 16` — **the SDK says 4**. `ps4_joypad.c:75` declares a
  16-element login-id list, `sceUserServiceGetLoginUserIdList` fills 4, and the loop then reads 12
  uninitialised `int32_t`s as user ids.
* `SCE_USER_SERVICE_USER_ID_INVALID 0xFFFFFFFF` — the SDK's is `-1` on a signed id, so
  `ps4_joypad.c:84`'s "reject invalid" guard never rejects anything.

Also invented with no SDK counterpart: `SCE_KERNEL_PROT_CPU_RW`, `SCE_KERNEL_MAP_FIXED`,
`SCE_PAD_PORT_TYPE_REMOTE_CONTROL`, the `SCE_MOUSE_*` set.

### The driver names are already chosen, and nothing answers to them yet

`configuration.c` sets the ORBIS defaults to audio `"orbis"` (via `msg_hash_lbl_str.h:48`), input
`"ps4"` and joypad `"ps4"`. None of those three drivers is registered any more, so each falls back
silently. Phases 4 and 5 must either adopt those exact `ident` strings or change these three
places with them — whichever, the pair has to move together.

`input_autodetect_builtin.c:800` binds "PS4 Controller" to the joypad driver `"ps4"` **using the
PS3 bind table**. A DualShock 4 is not a DualShock 3; Phase 4 owns this.

### Work list by phase

* **Phase 4** — rewrite `ps4_joypad.c` (half orbisdev: `orbisPad.h`, `orbisPadGetConf`) and
  `ps4_input.c` (`<orbis/libScePad.h>` is not a header; the SDK's is `<orbis/Pad.h>`); delete
  `ps4_defines.h`; fix the DS4 bind table.
* **Phase 5** — a real `sceAudioOut` driver. `psp_audio.c`'s ORBIS arm is the Vita's driver bent
  around orbisdev: `<libSceAudioOut.h>` does not exist, the user id is hard-coded `0xff`, and
  `psp_audio_stop` returns false unconditionally as a workaround.
* **Phase 7** — delete `orbis_ctx.c` rather than port it (it is Piglet end to end, and Phase 7 is
  Vulkan, not a GL context driver); strip the Piglet shader-binary cache from `shader_glsl.c`; drop
  ORBIS from `gl2.c`'s `glTexStorage` exclusion, which encodes "PS4 GL means Piglet" and is false
  under Mesa; fix the `stb.c` system-font paths, which are the Vita's PVF filenames with the
  directory swapped.
* **Housekeeping** — `Makefile.orbis.salamander` is orbisdev top to bottom and has been linking an
  empty `$(OBJ)` since 2019 (it defines `OBJS`). The CI jobs
  (`.github/workflows/PS4-ORBIS.yml`, `.gitlab-ci.yml:897`) run the orbisdev container, call
  `orbis-ld`, and expect a `.self` that the rewritten Makefile does not emit; the stub header
  `.github/workflows/scripts/stubs/orbis/orbis/libkernel.h` still declares orbisdev's
  `get_user_mem_size`. That stub build is what hid every defect above for six years.
