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

---

## 2026-08-22, later still — Phase 3 written, not seen

`gfx/drivers/ps4_gfx.c` and `ps4/ps4_video_out.c` exist, the build links them and the driver
registers as `"ps4"`. **No pixel has been drawn.** The emulator stops before video init on symbols
it has not implemented, so everything below is reasoned, not observed.

What went in:

* `configuration.c` had **no ORBIS arm at all** in the default-video-driver chain, so this platform
  fell through to `VIDEO_NULL`. That is the real reason the port could boot to its banner without a
  display, and it is fixed.
* One scaler pass does the scale and the format conversion together, because video-out's only
  format is byte-for-byte RetroArch's `SCALER_FMT_ABGR8888`.
* RGUI's menu format comes from a table keyed on the driver ident; `"ps4"` is deliberately not in
  it, so it takes the RGBA4444 default, which the scaler reads directly.

Known gaps, written into the source at the point they matter rather than listed here:

* **No font backend** - on-screen messages go to the log. RGUI draws its own bitmap font, which is
  why the menu is unaffected.
* **The menu replaces the frame, it does not blend over it** - the scaler converts, it does not
  composite.
* **The aspect-ratio setting is accepted and ignored** - the driver preserves the source's own
  pixel ratio, so a core asking for 4:3 from a 256x224 buffer is drawn at 8:7.
* **1080p is unmeasured.** Every frame is a scale into 2.07 million pixels on a Jaguar core. If
  60 Hz does not hold, 720p halves the fill; PLAN.md says settle this by measurement.
* **Alpha is left as the scaler wrote it.** ps4doom ORed `0xFF000000` into every pixel and never
  established whether scan-out reads the primary plane's alpha. If the screen comes up black or
  translucent, that is the first thing to try.

### The order of the next session

1. Decide the emulator question (teach `unemups4` the eighteen exports, or go straight to
   hardware). Nothing else can be verified until one of those happens.
2. Whichever way: the first thing to look for is a picture, and the first suspects if there is
   none are the alpha above and the direct-memory buffer registration.
3. Phases 4 and 5 are unblocked and independent of this - but `ps4_defines.h` has to go first, or
   the joypad driver inherits a 16-vs-4 user list and a guard that never fires.

---

## 2026-08-22 — on hardware: menu, pad, a game, and a core as a module

Everything below was run on a retail console under GoldHEN, in this order, each installed over
the last.

| build | result |
|---|---|
| dummy core, software video | **menu on screen, first try.** Phase 3 had never drawn a pixel anywhere. |
| + libScePad | **pad works.** D-pad, buttons, and analog all reach the frontend. |
| + sceAudioOut | port opens, volume set. Nothing to play yet. |
| 2048 linked statically | **a game runs.** Phase 6. |
| 2048 as a .prx, `HAVE_DYNAMIC=1` | **the module loads and runs**, and picked up the save the static build had written. |

### D3's three unknowns, answered

The plan said static first because it removed three questions at once, and that a core-sized PRX
was unproven. All three came back:

1. **A libretro core builds and loads as a PRX.** `create-fself --lib` with `crtlib.o` in place of
   `crt1.o`; `sceKernelLoadStartModule` finds it, `sceKernelDlsym` resolves `retro_*` by name.
   185 KB, so this says nothing yet about a core of tens of megabytes.
2. **The libc/heap question did not bite.** The module links its own `-lc -lc++` and the frontend
   has its own; nothing crashed, `retro_init` ran, and the core allocated and drew.
3. **`crtlib.o`'s init path is enough** for a core to be usable by the time `retro_init` is called.

⚠ **This was the first time `libretro-common/dynamic/dylib.c`'s ORBIS branch had ever executed.**
It could not compile until the fixes in `9ba278d81b`, so the code that loads every dynamic core on
this platform went from "never built" to "running a game" in one step. Treat its error paths as
unexercised.

### What a dynamic core costs that a static one does not

* It must carry its own `libretro-common`. `-DSTATIC_LINKING` omits those files for the static
  build because the frontend supplies them; a module cannot see the frontend's.
* It must NOT carry the copy it shipped with, if that copy predates this port - `libretro-2048`'s
  still has `#include <orbisFile.h>`. `ps4/build-core.sh --common` points it at the frontend's.

### Still unconfirmed

* **Audio.** The port opens and the volume is set, but 2048 makes no sound, so nothing has proved a
  sample reached the speakers. Needs a core with audio and no content requirement.
* **60 Hz at 1080p.** 2048 is not a load. A core drawing a full framebuffer every frame is.
* **A large PRX.** 185 KB proves the mechanism, not the scale.

### A rough edge worth fixing

RetroArch creates `/data/retroarch/*` as `drwxr-x---`, and GoldHEN's FTP daemon runs as another
user, so `put` into `cores/` is refused until something chmods it. Copying a core over the network
is the only way to deliver one, so the directory mode is part of the delivery path, not a detail.

### ⚠ A correction to the record

An earlier entry in this session claimed `dir_check_defaults("/app0/custom.ini")` had stopped the
default directories being created on hardware. **It had not.** The directories were there all
along, dated from an earlier boot; the evidence for the claim was an FTP listing truncated by
`head -30` before it reached `retroarch/` alphabetically. The change that came out of it - passing
`NULL`, and checking the result - is kept on its own merits, and its comment now says what actually
happened.
