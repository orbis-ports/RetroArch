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
* ~~The aspect-ratio setting is accepted and ignored~~ - fixed once Vulkan ran beside it and
  honoured the same config, which is how the divergence became visible at all.
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

### Audio, confirmed — and the two things that had to be got right

A clean 300 Hz tone from `libretro-samples/audio/audio_no_callback`, built as a PRX and dropped
into `/data/retroarch/cores` with no reinstall. Two defects stood between the driver and that, and
both are worth carrying to any other consumer of this API.

**1. `sceAudioOutInit` returns ALREADY_INIT on the second call, and the constant was wrong.**
RetroArch initialises its audio driver twice — once at startup, again when content loads — so the
second call in a process always fails this way. The driver tolerated `0x8026000d` and bailed on
anything else, which killed audio at content load with "Failed to initialize audio driver".

    0x8026000D  ORBIS_AUDIO_OUT_ERROR_OUT_OF_MEMORY
    0x8026000E  ORBIS_AUDIO_OUT_ERROR_ALREADY_INIT

⚠ The wrong number came from `~/src/ps4doom/platform/doom_sound_ps4.c`, transcribed together with
a comment naming it ALREADY_INIT. **That comment is wrong there too** and has never shown, because
ps4doom initialises audio once. Worth fixing there before it travels again. Use the SDK's named
constant; a magic number carries its explanation with it, and a wrong explanation travels just as
well as a right one.

**2. `sceAudioOutOutput` blocks until the grain is CONSUMED, so the port needs a thread.**
When it returns, nothing is queued behind it. Fed from RetroArch's main loop — which also blocks on
vsync — the port runs dry between the last write of one frame and the first of the next, and a dry
port clicks once per frame. That is exactly what the first working build did.

It is also a throughput problem: at 48 kHz and 60 fps a frame is ~3.1 grains at 5.33 ms each, so
16.6 ms of vsync plus 16.6 ms of audio serialises into 33 ms — 30 fps.

The driver now has a dedicated thread permanently inside `sceAudioOutOutput`, pulling from a ring
that `write()` fills. An empty ring plays silence rather than skipping the call: the port takes
exactly one grain and will not take a partial one, so something must be handed to it either way.
Same shape as ps4doom's mixer thread and as `switch_thread_audio.c` in this tree.

### 1080p holds 60 Hz, measured

PLAN.md Phase 3 said to settle 1920x1080 against 1280x720 by measurement and not by choosing up
front. Measured, on hardware, over several minutes:

    300 frames: scale 6124 us/frame, frame 16683 us (59.9 fps)

Stable to within 30 microseconds across every report. So:

* **The frontend is paced by the display, not by itself.** 16683 us is the vsync interval; nothing
  is costing an extra refresh.
* **The scale costs 6.1 ms of a 16.67 ms budget — 37%.** That leaves ~10.5 ms per frame for a core.
* **1080p stays.** 720p would roughly halve the scale, and there is no reason to spend the
  resolution to buy time nothing needs yet.

⚠ The measurement is a 320x240 source. Point-scaling cost is dominated by DESTINATION pixels, so a
higher-resolution core moves this number much less than it moves its own; but a core that needs
more than 10.5 ms per frame will not hold 60 Hz, and 720p is then the lever - as a runtime option,
not a new default.

⚠ And it is measured with the default filter. `video_smooth` selects SCALER_TYPE_BILINEAR instead
of POINT, which is a different and larger number. Nobody has measured that one.

### Audio under load, measured

    audio: 1875 grains, 10 underruns
    audio: 3750 grains, 10 underruns
    audio: 5625 grains, 10 underruns

Ten underruns while the pipeline fills, then flat for as long as the run lasts. And a
free confirmation that the thread keeps the port's clock exactly: 1875 grains per 10 s times 256
frames is 48 000 frames a second, to the sample.

100% underruns while sitting in the menu is correct, not a fault: there is no core, the ring is
empty, and the thread plays silence to keep the port's continuity. A thread that skipped the call
instead would have to prime the port again on the way back into a game.

### Still unconfirmed

* **A large PRX.** 185 KB proves the mechanism, not the scale.
* **A demanding core.** Nothing has yet asked for more than a fraction of the 10.5 ms of headroom.
* **The bilinear scaler path.**

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

---

## 2026-08-22 — Vulkan on RADV, and where the close-hang is not

RetroArch draws through Mesa's RADV on the console. 19 456 frames in one run at `frame 16683 us`
— the same 60 Hz the software driver holds — and the scan-out is the zero-copy path, not a copy:

    wsi/orbis: scan-out up - 1920x1080 pitch 1920, 4 swapchain buffer(s), A8B8G8R8_SRGB linear
               - ZERO COPY, the flip shows what the GPU rendered into
    wsi/orbis: the scan-out copy took 0 us for 8100 KiB (worst 27 us over 19 456 frames)

### ⚠ Vulkan teardown works, and this is the first evidence anywhere that it does

Every title built against this Mesa hangs when the console closes it. Nothing had established
whether the driver teardown was the thing that wedged, because **no capture from this console had
ever contained `wsi/orbis: scan-out down`** — the titles that came before end by idling or by
CE-34878-0, and both routes skip `vkDestroyInstance` entirely. The path had never run.

Forcing it from a live process — load a core, then Close Content, which tears the video driver
down and builds it again — gives:

    15:19:26.718  wsi/orbis: scan-out down after 493 flip(s)
    15:19:26.719  wsi/orbis: scan-out up - 1920x1080 ...
    15:19:26.719  [PS4] Vulkan up: 1920x1080 swapchain on RADV.

**One millisecond, clean, and it comes straight back up.** So the close-hang is not in destroying
the driver. It is in terminating the process while RADV is live.

That splits a problem this port did not create and cannot fix alone. And the next cut came back
narrower than expected: **Close Content, then Quit, exits cleanly even on the Vulkan driver** — and
Close Content re-initialises the driver, so RADV is live at that point. "RADV alive at kill time" is
therefore NOT the condition. What is left is the combination with a loaded core; the .prx alone was
fine for a whole session on the software driver, and Vulkan alone is fine here. Nobody has narrowed
it further than that yet.

Practically it also means the console no longer has to be restarted between tests, which is what
made every experiment above expensive.

⚠ RetroArch is a better instrument for this than the title that first hit it: the teardown is one
menu entry rather than an exit, repeatable in a single session, with the log flowing throughout.

### A side effect of the hang, worth knowing before it wastes an hour

**The config is never saved.** RetroArch writes `retroarch.cfg` on a clean exit, and there are no
clean exits yet — so a driver picked in the menu is forgotten on the next launch, every time. The
file on the console was four hours stale while the menu showed the right thing. The Vulkan default
now comes from `configuration.c` rather than from a file that does not get written.

### The software scaler is much more expensive for RGB565 cores

    XRGB8888 source:  scale  6 124 us/frame   (37% of a 16.67 ms budget)
    RGB565   source:  scale 14 450 us/frame   (87%)

Both at 320x240 into the same viewport, so the difference is the pixel format alone: an XRGB8888
source is already the scaler's internal format, and RGB565 costs a whole extra pass — convert in,
scale, convert out. 87% of the frame leaves almost nothing for a core, so on the software path a
16-bit core is the demanding case and a 32-bit one is not. Untouched for now because Vulkan is the
path that matters, but it is the number to remember if the software driver is ever the fallback for
a real core.

---

## 2026-08-22 — Phase 7 is done: XMB on RADV, icons and text

The GPU menus are back. XMB draws through the Vulkan driver's texture path with the monochrome
icon theme and readable text, which is the visible half of what Phase 7 was for — the software
build has RGUI and nothing else, because RGUI is the only menu driver that rasterises itself.

Two things had to be true and only one of them was:

* **The GPU menus need a driver that can carry them.** RADV provides it. They are enabled whenever
  `HAVE_VULKAN=1` and off otherwise, so the software build is unchanged.
* **They need assets, and the console had none.** `/data/retroarch/assets` was an empty directory
  that `dir_check_defaults` had created and nothing had ever filled. 16 MB of
  `libretro/retroarch-assets` — `xmb/monochrome`, `ozone`, and two fonts from `pkg` — copied over
  FTP is enough for both drivers.

⚠ **The system-font list was pointing at files that do not exist.** It was the Vita's seven PVF
names with the directory swapped to `/preinst/common/font`; a retail console's own directory has
`DFHEI5-SONY.ttf` and the SST family and none of those seven. Fixed, and ordered by what
`stb_truetype` can parse rather than alphabetically: SST is `.otf` with CFF outlines and
stb_truetype reads TrueType `glyf`, so the one real TTF goes first. In practice
`assets/pkg/fallback-font.ttf` is what XMB uses, so the system list is now a spare rather than a
requirement — but a spare that named seven absent files was worth nothing.

### Where the port stands

| | |
|---|---|
| build | 239 objects, no warnings, no libc shims written |
| video, software | 1080p @ 60 Hz measured, 6.1 ms/frame for a 32-bit core |
| video, Vulkan | RADV, 1080p @ 60 Hz, zero-copy scan-out |
| menus | RGUI on software; XMB, Ozone, MaterialUI, widgets on Vulkan |
| input | DualShock 4, digital and analog |
| audio | 48 kHz, threaded, no underruns under load |
| cores | static and `.prx`; saves survive across both |
| packaging | `.pkg`, `make send` over lftp |

### Still open

* **The close-hang**, which this port did not create: every title on this Mesa has it. Narrowed
  today — driver teardown is clean, and Close Content followed by Quit exits properly even on
  Vulkan, so the condition involves a loaded core rather than RADV being live.
* **Slang shaders** compile in and have never been run.
* **Assets are hand-copied.** Nothing ships them and nothing tells a user they are missing; XMB
  without them looks like a menu driver that failed to start.
* **The RGB565 software path** costs 14.4 ms of a 16.7 ms frame, against 6.1 ms for XRGB8888.

---

## 2026-08-22 — slang shaders run, and Beetle PSX HW builds as a 17 MB module

**Slang shaders work on hardware.** `crt/crt-geom.slangp` renders. That is the whole chain
executing for the first time: `.slang` -> glslang (compiled into the eboot) -> SPIR-V ->
SPIRV-Cross -> RADV/ACO -> GCN ISA on Liverpool, compiled at run time on the console. 12 MB of
`libretro/slang-shaders` (crt, interpolation, misc) copied to `/data/retroarch/shaders`.

**Beetle PSX HW is built and on the console** — 113 objects, a 17 MB `.prx` with 55 `retro_*`
exports. Three things had to be settled to get there, and each is general rather than specific to
this core:

* ~~⚠ **A dynarec needs a mirrored-mapping story this platform does not have.**~~ **SUPERSEDED
  2026-08-23 - see the section at the end of this file.** Lightrec maps the same PSX RAM pages at
  several addresses through `memfd_create`/`MAP_SHM`; neither exists here (`libretro.c:2345-2574`),
  and that part is still true. **The conclusion drawn from it was not.** The platform's own
  direct-memory API makes mirrored mappings, and it was measured doing so. `HAVE_LIGHTREC=0` is
  still what the shipped core is built with, and the reason recorded for it no longer holds.
* ⚠ **A core must use the Vulkan headers it was written against, not the driver's.** Forcing
  Mesa's current `vulkan_core.h` broke it on `VK_IMAGE_TYPE_RANGE_SIZE`, an enum removed from the
  spec years after this core started using it. The rule that the loader shim needs the exact
  headers RADV was built against does NOT generalise to consumers: a core is an ordinary Vulkan
  client and the ABI is backward compatible. The frontend is the special case, not the core.
* ⚠ **The core's Makefile has the stale-object problem too.** Turning `HAVE_LIGHTREC` off left
  `cpu.o` compiled against the old flags, and the link failed on `lightrec_destroy` for a source
  file that no longer referenced it. Same shape as the one fixed in `Makefile.orbis`; the fix
  there was a flags stamp, the fix here was `find -name '*.o' -delete`.

The core's own Makefile grew an `orbis` platform arm - the same shape as its `vita` one - which
compiles the objects; the archive and the `create-fself --lib` step are done outside it, because a
libretro module here is a PRX rather than a shared object.

### D3's last unknown, answered

The plan said 185 KB proves the mechanism and not the scale. 17 MB is the scale, and it links.
Whether it LOADS at that size is still for the console to say.

### What it needs before it can run

A PlayStation BIOS in `/data/retroarch/system` (`scph5500/5501/5502.bin`) and a disc image. Neither
is something this port can supply.

---

## 2026-08-22, close of day — hardware render works, and one core draws it wrong

Beetle PSX HW runs Spyro 3 through RADV with the libretro hardware-render callback: the core is
handed a Vulkan context and builds its own pipelines on this driver. Nothing about that path had
ever run — until today RADV only drew what RetroArch told it to, and now foreign graphics code
does. 40 fps on the MIPS interpreter, which is more than expected with no dynarec.

The picture is wrong in a specific way: the static scene is correct — sky, terrain, castle,
lighting, colours — and every animated model is shredded, with one quad showing stripes of garbage
where a texture belongs.

### What the elimination found, in order

Each of these cost one experiment and each removed a whole class of cause.

1. **The core's own software renderer draws the same content correctly.** So the geometry reaching
   the GPU is right, and the GPU's reading of it is not. That rules out the CPU side entirely,
   including the interpreter we are forced onto by `HAVE_LIGHTREC=0`.
2. **`ORBIS_3D_LINEAR=1` and `ORBIS_NO_TESS=1` were not applied, and now are.** RetroArch never
   read `/data/tempest-env.txt`, so it had been running the driver in a configuration no other
   title runs in — see the commit; the file itself says those two "are not options". Applying them
   did **not** change the picture, which is worth knowing on its own: this artefact is not the
   tiling that file exists to avoid.
3. **Doubling the internal resolution changes nothing.** Different resolutions mean different
   pipeline variants; a shader miscompile would have moved. It did not, so ACO is not the first
   suspect.

### ~~Where that leaves it: the vertex attribute path~~ SOLVED 2026-08-23

⚠ **Read the correction at the end of this file before spending time on anything below.** The
suspect named here was right in its neighbourhood and wrong in its name: the variable is not that
the formats are integer, it is that their ELEMENTS ARE LARGER THAN FOUR BYTES. The driver was
fetching them from addresses that are not multiples of their size, silently getting the wrong bytes,
and it is fixed. Spyro 3 draws correctly now.

### Where that leaves it: the vertex attribute path

`rhi/rhi_lib_vulkan.c:6032-6038` declares seven vertex attributes, and **four of the seven are
integer formats**:

    0  R32G32B32A32_SFLOAT   position
    1  R32G32B32A32_SFLOAT   color
    2  R8G8B8A8_UINT         window      <-- integer
    3  R16G16B16A16_SINT     pal_x       <-- integer
    4  R16G16B16A16_SINT     u  (UV)     <-- integer
    5  R16G16B16A16_UINT     min_u       <-- integer
    6  R32G32B32A32_SFLOAT   fog

Attributes 3 and 4 are palette and texture coordinates. A quad showing stripes of garbage instead
of a texture is what wrongly fetched UVs look like. Integer vertex formats are a far less travelled
path in any driver, and more so on GFX7.

⚠ **This is answerable with the CTS already ported to this console, without RetroArch, without
Spyro and without guessing:** `dEQP-VK.pipeline.*vertex_input*` tests attribute fetch per format
and in combination. If one of those four fails, the cause is named to the format.
`/data/deqp-cases.txt` currently holds 49 `api.object_management` cases from an earlier
investigation and has not been touched.

### ⚠ A fourth knob with no reader

`ORBIS_TILE_MODE` was going to be the next experiment. It has exactly one occurrence in mesa-ps4:

    src/amd/common/ac_orbis_drm.c:4412
       getenv("ORBIS_TILE_MODE") ? getenv("ORBIS_TILE_MODE") : "unset",

— printing its own value into a log line. Nothing acts on it. `tempest-env.example.txt` documents
it as working and lists three other names that were found readerless on 2026-08-21; this is a
fourth. Setting it would have produced exactly what that file warns about: "the run comes back
clean and reads as a measurement."

### What a console operator has to know

Delivery over FTP is a two-user problem in both directions, and the modes are not uniform:

    .prx  (modules)       777   must be executable; sceKernelLoadStartModule loads them as modules
    data files            666   BIOS, discs, .info, fonts, shaders
    directories           777

Setting `666` on a core makes it fail to load with no useful message. `mkdir` on this platform now
creates 0777 (`vfs_implementation.c`), but files written by the FTP daemon are its own and nothing
on the RetroArch side controls them.

Cores must be named `<name>_libretro.prx` with a matching `<name>_libretro.info`, and
`/data/retroarch/info/core_info.cache` must be deleted after adding one — a cache built while the
info directory was half-populated stays empty and the menu says "No cores available" for ever,
which then presents as an empty content browser because a frontend with no core info has no
extension filter.

### Still open

* The vertex-attribute question above.
* **The close-hang**, which every title on this Mesa has. Narrowed: driver teardown is clean, and
  Close Content followed by Quit exits properly, so the condition involves a loaded core.
* **`orbis_paths.cpp:19` hard-codes `/data/OpenGothic/`** as the anchor for relative paths. Every
  relative path RetroArch opens - and it does open some, `Main Menu.png` among them - lands in
  another title's directory. Flagged on 2026-08-21 as harmless; it is not.
* Assets, shaders and cores are hand-copied, and nothing tells a user when they are missing.

---

## 2026-08-23 — both open questions answered, from the driver side

Written into this file by the Mesa workshop (`~/src-ps4/mesa-ps4`, `~/src-ps4/ps4-mesa-docs`)
because both answers were measured there and both contradict something this file states as fact.
The full account, with every log line, is `ps4-mesa-docs/docs/HANDOFF.md` from
"The integer-attribute suspect has a name" onwards.

### 1. The shredded models: SOLVED, and it was alignment rather than integers

The suspect was one step off. Not "integer vertex formats are a thinly travelled path" - the
variable is **element size**. `src/amd/common/ac_shader_util.c`'s `is_fetch_size_safe()` exempts
GFX7-GFX9 from every alignment requirement: on those parts it declares any typed vertex fetch safe
at any address. That is a claim about silicon, inherited from upstream, and **it is false on
Liverpool.** A multi-byte element read from an address that is not a multiple of its size returns
the wrong bytes, with no fault and no log.

Measured with the Vulkan CTS, 1853 cases of `dEQP-VK.pipeline.monolithic.vertex_input`, same case
list and same binary, one environment line apart:

    believing the exemption   Passed 1657   Failed 98
    splitting the fetches     Passed 1754   Failed  1

97 Fail→Pass, 0 Pass→anything, 0 other verdict changes. The one survivor is a geometry-shader case
and a different defect.

**Why Beetle's picture looked the way it did:** formats built from 32-bit channels are immune,
because dword alignment IS element alignment there. So the three `R32G32B32A32_SFLOAT` attributes
were always correct and the three `R16G16B16A16_*` were not. ⚠ And the prediction this makes, which
"integer formats are less travelled" does not: **attribute 2, `R8G8B8A8_UINT`, was never affected** -
a 4-byte element is aligned wherever a dword is.

Fixed in the driver, `ORBIS_VS_STRICT_ALIGN`, **on by default on this platform** and only this one.
`ORBIS_VS_STRICT_ALIGN=0` restores the old behaviour and logs a warning, because the off state is now
the dangerous one. Nothing is needed on the RetroArch side except a rebuild against a driver from
2026-08-23 or later — the log line to check is

    orbis: vertex fetches are split to natural alignment

Its absence means the binary predates the fix. **Every title links `libvulkan_radeon.a` statically,
so an installed build carries whatever driver it was linked with.**

### 2. Where Spyro's frame actually goes, and it is not the GPU

From the driver's own BUDGET instrumentation during a Beetle PSX HW session, 97 five-second windows:

    menu           0.05 cores    60.0 fps
    3D scene       1.00 cores    40 fps, and 1.00 cores at 23 fps
    time waited for the GPU, in EVERY window without exception: 0 ms
    the whole Vulkan API, per frame:  263 us against a frame of 43967 us  = 0.6%
    23 draws, 4 render passes, 1.68 screenfuls of 1920x1080, 2 dispatches

One CPU thread saturated, the GPU never waited on, the graphics driver at 0.1-0.4% of the window.
⚠ **The audio running slow is the same fact, not a second one:** the core is at ~38% of realtime, so
the samples come out at ~38% of the rate. Precaching the disc does not help because the bottleneck is
not I/O. Internal resolution and renderer settings will not move it either - that is now measured
rather than assumed.

The interpreter is the entire cost, and the dynarec is the only lever.

### 3. Lightrec's wall does not exist. All three mechanisms measured on hardware

    mirrored RAM       sceKernelMapDirectMemory called repeatedly with the SAME phys gives EIGHT
                       simultaneous views - the probe's own cap, not the kernel's; it had not
                       refused. Coherent in BOTH directions at two offsets 2 MiB apart, and
                       unmapping one leaves the rest intact.
    fixed placement    ORBIS_MAP_FIXED puts a mapping at an address of our choosing. Not new -
                       ac_orbis_drm.c:6464 does it in production every time a buffer moves bus.
    executable code    map READ|WRITE, then sceKernelMprotect to 0x07, then EXECUTE. Six bytes of
                       x86-64 (b8 ee ff c0 00 c3) were written and CALLED; it returned 0x00c0ffee
                       and the process carried on.

⚠ **THE OBVIOUS FORM OF THE LAST ONE IS REFUSED.** `sceKernelMapDirectMemory` asked for
READ|EXECUTE up front returns `0x8002000d` = EACCES - understood and declined, not malformed. The
policy lives at map time, not at protect time. **Anyone who tries the direct form first will conclude
this is impossible**, which is exactly what the probe concluded one rung before it turned out true.

⚠ **Only the mprotect route was actually EXECUTED.** `sceKernelMapFlexibleMemory` and
`sceKernelMmap` both granted 0x07 as well and neither was called. On this console a granted
protection is not an honoured one - it has charged for that distinction three times now.

⚠ **`MAP_PRIVATE|MAP_ANON` is `0x1002` here, not `0x0022`.** The SDK's `sys/mman.h` is musl's and
carries Linux's value; orbis-compat sits ahead of it in the include path and corrects it to the
FreeBSD one. Passing the wrong value makes the kernel treat the mapping as file-backed, validate
`fd = -1`, and return EBADF - which reads as a refusal of the protection and is nothing of the kind.
`orbis-compat/src/orbis_mmap.cpp:53` has a `static_assert` for this. **Ask the compiler for
constants (`clang -dM`), not a header you found with grep.**

The probe is `orbis_test_mirror_mapping()` in `ac_orbis_drm.c`, behind `ORBIS_TEST_MIRROR=1` for the
safe rungs and `=exec` for the jump. It runs once at device init in any title, takes 2 MiB and gives
it back. If the port hits a wall, that ladder re-establishes the ground truth in one run.

### What this does and does not promise

It removes the reason recorded for not building Lightrec, and that reason was the whole of the case.
**It does not say the port is short.** Lightrec brings its own code emitter, its own build system and
its own assumptions about the host; the platform blockers are gone and the size of the remaining work
is unmeasured.

⚠ And one trap already in this file, worth re-reading before starting: turning `HAVE_LIGHTREC` back
on needs `find -name '*.o' -delete` first. The core's Makefile has the stale-object problem, and
switching the flag the other way already cost a link failure on `lightrec_destroy`.

### ⚠ And the build line, because this file never wrote it down and that cost a package

The frontend needs **three** flags, not one. `Makefile.orbis`'s own comments put only
`HAVE_VULKAN=1` in a command line, and a rebuild made with just that shipped, installed, booted,
drew — and showed **no cores at all**:

    make -f Makefile.orbis HAVE_VULKAN=1 HAVE_STATIC_DUMMY=0 HAVE_DYNAMIC=1 -j$(nproc) pkg

⚠ **Changing any of them needs a `clean` first.** They are `-D` defines and this Makefile has no
header dependencies, so objects built under the other setting are silently reused.

⚠ **Nothing in the artefact says which configuration it is.** The ELF is the same size either way and
the `.pkg` has come out 41680896 bytes for four days running. The grep that answers it:

    sceKernelLoadStartModule    static build 0    dynamic build 3
    libretro_dummy              25 either way, so NOT the marker to look for

The link step now prints the configuration every time (`cores:`, `dummy:`, `vulkan:`, and the driver
archive's build date), so this should not be able to recur silently.

⚠ **A static build also POISONS `/data/retroarch/info/core_info.cache`** — it writes 65 bytes
decompressing to `{"version": "1.2", "items": []}`, and that empty cache keeps the menu empty across
every later install until somebody deletes it by hand. If the core list is empty after a good build,
delete that file first.

Two defects in `Makefile.orbis` were fixed while finding this, both uncommitted for review:

    the eboot.bin recipe did not pass OO_PS4_TOOLCHAIN, which create-fself reads from the
    environment and refuses without - although it is invoked by absolute path out of that very
    toolchain. The `pkg` target passes it; this one did not. The build linked a new .elf, failed at
    status 255, and left the PREVIOUS DAY'S eboot.bin and .pkg beside it, ready to be uploaded as
    "the rebuild". It had only ever worked because the shell that ran make happened to export it.

    the link step now prints what it built, as above.

### How everything here is compiled, so it is not rediscovered

Verified against the trees on 2026-08-23. Every path is a real entry point that was run that day.

**Order matters.** The overlay is what the driver and the frontend both compile against, and the
driver is what the frontend links, so a change low down means rebuilding upward:

    1  orbis-compat   ./build.sh                      -> build/liborbis-compat.a
    2  mesa-ps4       ./ps4/build.sh                  -> build-orbis/src/amd/vulkan/libvulkan_radeon.a
    3  RetroArch      make -f Makefile.orbis ... pkg  -> eboot.bin, IV0000-RTRA00001_*.pkg
    3b cores          ps4/build-core.sh               -> <name>_libretro.prx + .info

⚠ **Nothing rebuilds anything below it automatically.** The frontend links whatever
`libvulkan_radeon.a` is sitting there; it does not check whether the driver sources are newer. The
link step prints the archive's build date for exactly that reason - **read it, and ask whether that
is the driver you meant.**

    orbis-compat/build.sh
        Consumers need exactly two things, and both matter:
          -isystem <orbis-compat>/include   AHEAD of the SDK's include directory
          build/liborbis-compat.a           with --whole-archive
        ⚠ The include order is not a preference. The SDK ships musl's headers behind a FreeBSD
        triple, and the overlay corrects the constants that differ - MAP_ANON among them. Put the
        SDK first and you get Linux values for a FreeBSD kernel, silently.

    mesa-ps4/ps4/build.sh   [--host-too] [--host-orbis] [--sdk <dir>] [--work <dir>]
        no arguments   cross-build the driver for the console. This is the one that matters.
        --host-too     also build a plain Linux RADV, for the drm-shim probes
        --host-orbis   build THIS arm as an ordinary Linux ICD and run its self-tests. Catches
                       anything structural without a console trip.
        ⚠ It prints the driver path and modification time every run because a path that looks
        right pointing at another day's build has cost this workshop an evening.

    RetroArch  make -f Makefile.orbis HAVE_VULKAN=1 HAVE_STATIC_DUMMY=0 HAVE_DYNAMIC=1 -j$(nproc) pkg
        The three flags and the `clean` rule are in the section above. `make ... info` dumps the
        whole flag set if something looks wrong.

    RetroArch  ps4/build-core.sh --core <dir> --out <path> [--prx] [--name <label>] [--common <dir>]
        --prx     a loadable module rather than a static archive
        --name    also writes the minimal <out>.info RetroArch needs to show a readable name
        --common  use the FRONTEND's libretro-common instead of the core's own vendored copy,
                  which for anything predating this platform still has the orbisdev-era ORBIS
                  branch and will not compile

**All four resolve the toolchain and the overlay the same way**, through
`orbis-compat/scripts/ps4/orbis-env.sh`: `ORBIS_COMPAT_DIR` if set, else a sibling directory, else
`~/src-ps4/orbis-compat`; and `OO_PS4_TOOLCHAIN` or `~/.local/opt/openorbis`. A fresh clone of the
`orbis-ports` organisation with the repositories side by side needs no environment at all.

    deploy   orbis-compat/scripts/ps4/deploy.sh --pkg <file> --name <short>
                                                [--also <local>:<remote>]... [--host <ip>]
        Uploads to /data/pkg/<name>-YYYYMMDD.pkg - ⚠ this console installs from /data/pkg and
        nowhere else. Verifies by READING BACK: sizes for packages, byte-for-byte for anything
        under a megabyte. It ends by saying INSTALL + RUN or RUN, no install, and that line is
        the answer to "do I need to reinstall".

    configuration on the console
        /data/tempest-env.txt    read first, by every title
        /data/retroarch-env.txt  read second, ours, and only the log destination belongs in it
        Both are plain KEY=VALUE, applied with setenv() before anything touches Vulkan.

⚠ **The Beetle PSX HW source is NOT in this workshop.** The built `.prx` and its `.info` are on the
console under `/data/retroarch/cores/` and `/data/retroarch/info/`, and the checkout they came from
is not on the machine - a search on 2026-08-23 found nothing. Anyone picking up the Lightrec work
starts by fetching the core again, and the `orbis` platform arm its Makefile grew is not upstream.

### Where the Lightrec work goes: one fork, and probably only one

    orbis-ports/beetle-psx-libretro, branch ps4-support

Upstream is `beetle-psx-libretro`; the binary it produces here is `mednafen_psx_hw_libretro.prx`.
Same shape as the other eight repositories in the organisation.

**Why one fork and not two.** Lightrec is a separate project (pcercuei's) vendored into the core, so
the instinct is that a platform change belongs upstream in Lightrec. ⚠ **The evidence in this file
says otherwise:** the citation for the mirrored-mapping code is `libretro.c:2345-2574`, and that is
**Beetle's own file, not Lightrec's**. The recompiler is handed pointers; arranging the host mappings
is the core's job. If that holds, the whole change is one fork and Lightrec is untouched.

⚠ **UNVERIFIED - the core's source is not in this workshop and this was not checked.** After
cloning, two greps settle it:

    rg -n "memfd_create|MAP_SHM|mmap" --glob '!deps/lightrec/**' libretro.c
    rg -rn "memfd_create|MAP_SHM" deps/lightrec/

Hits only in the first: one fork. Hits in the second as well: Lightrec maps for itself, that is a
general "platform without POSIX shared memory" problem rather than ours, and it belongs upstream in
Lightrec rather than in a PS4 fork of the core.

⚠ **And the port work may not all be recoverable from a fresh clone.** The `orbis` platform arm this
file records the core's Makefile growing is **not upstream**, and the checkout it was written in is
gone. Look for a patch or a stashed tree before starting from zero; if there is none, that arm has to
be written again, and this file's own note that its `libretro-common` is too old to compile here
applies from the first build.

### ⚠ Do not switch cores to get a better recompiler

`pcsx_rearmed` has a mature x86-64 dynarec and is far lighter than Beetle, which makes it look like
the shorter road. **It has no Vulkan hardware renderer.** Beetle PSX HW is the only thing in this
port where foreign graphics code builds its own pipelines on our RADV, and it is what found the
vertex-fetch defect that had been silently corrupting every title. Moving to a software-rendered core
would trade the whole diagnostic value of this arrangement for a frame rate, and would throw away the
port work already spent on Beetle.

The core is right. The recompiler is what is missing from it.

---

## 2026-08-23 (evening) — Lightrec built, and the fork it lives in

The Mesa side's entry above removes the reason this file recorded for not building the dynamic
recompiler. This is the other half: the recompiler is built, it is on by default here, and the
core is on the console. **It has not been run on hardware yet** - what follows is what was
written and why, not what was measured.

### ⚠ First: the checkout this file said was gone is not gone

The entry above records the Beetle PSX HW source as unrecoverable - "a search on 2026-08-23
found nothing" - and tells whoever picks the work up to re-fetch it and rewrite the platform
arm. It was in the previous session's **scratchpad** (`/tmp/claude-1000/.../scratchpad/`), with
the `orbis` arm uncommitted in its working tree, exactly as it had been left.

The search was of `~` and `~/src-ps4`. A scratchpad is where a session is *told* to put
working files, so it is the first place to look for a missing one and it was not looked at.
The tree is now `~/src-ps4/beetle-psx-libretro`, branch `ps4-support`, origin
`git@github.com:orbis-ports/beetle-psx-libretro.git` (not yet pushed - the repository may not
exist yet), upstream `libretro/beetle-psx-libretro`. The recovered arm is its first commit, so
the Lightrec work reads as a diff against it rather than as one lump.

### The fork is one fork, and the grep the entry above asked for has been run

    rg "memfd_create|MAP_SHM|mmap" libretro.c        53 hits
    rg "memfd_create|shm_open" deps/lightrec/         0 hits

Confirmed: arranging host mappings is **Beetle's** job, not Lightrec's. The recompiler is
handed pointers. Nothing goes upstream to pcercuei; the whole change is in this fork.

### What the platform arm actually needed

`libretro.c` builds the maps behind five macros and a descriptor - `MAP`, `MAP_SHM`,
`MAP_CODE`, `UNMAP`, `MFAILED`, and a `MEMFDTYPE`. Every existing arm fills them with POSIX
shared memory, ashmem or Win32 file mappings. The PS4 arm fills them with Sony's allocator, and
the shapes line up one for one:

    memfd_create + ftruncate    ->  sceKernelAllocateDirectMemory   (physical pages, an off_t)
    mmap(MAP_SHARED, memfd)     ->  sceKernelMapDirectMemory        (a view of those pages)
    mmap(MAP_ANON|MAP_PRIVATE)  ->  the same call on its own allocation
    PROT_EXEC at mmap time      ->  REFUSED - map RW, then sceKernelMprotect

`MEMFDTYPE` is `off_t` here and the "memfd" is not a descriptor: it is the offset into the
console's physical memory. New files: `ps4/orbis_lightrec_mem.{c,h}` in the core.

### ⚠ Three things that differ from every other arm, and all three are load-bearing

**MAP_FIXED here has no `_NOREPLACE` form.** The Linux arm asks for an address and checks what
came back; on this kernel the check runs after the frontend's heap has already been replaced.
Every fixed mapping asks whether the range is empty first.

**The range check walks a granule at a time, deliberately.** The natural call is
`sceKernelVirtualQuery(addr, 1, ...)` - "the first mapping at or above" - one call for a whole
range. Nothing in this workshop has ever passed 1. The only established value is 0, from
`ac_orbis_drm.c`, where a nonzero return means nothing is mapped there. ⚠ **The two ways of
being wrong are not the same size:** a guessed flag that makes the call fail reports every
address as free, MAP_FIXED lands on the heap, and the result is a corrupted process rather than
a refusal. Being too conservative only loses the recompiler. ~18,000 queries per content load
buys that, which is nothing against the load itself. If flags=1 is ever established, it
collapses to one call.

**Direct memory is not reclaimed when a mapping goes away**, and `lightrec_init_mmap` is called
*twice* on the way in (hugetlb, then without). Released explicitly on both the failing and the
succeeding path, or it is 2 MiB of unswappable memory per content load.

### The code buffer is mandatory here, unlike everywhere else

Without one, Lightrec lets GNU lightning allocate its own with `mmap(PROT_EXEC)`
(`deps/lightning/lib/lightning.c:2516-2531`). This kernel refuses execute at map time. ⚠ **A
block emitted into non-executable pages does not fail, it ends the process** - so if the
promotion to RWX is refused, `psx_dynarec` is clamped to `DYNAREC_DISABLED` and the interpreter
runs, with a line saying why.

The guard is in two places because the core reads its options *before* it maps anything:
`check_variables` sees "refused", `InitCommon` sees "not available". "Not tried yet" must not
read as "refused", or the recompiler switches itself off on the way in every time.

### The option default is flipped on this platform only

Upstream defaults `beetle_psx_cpu_dynarec` to `disabled` and hides it under **Hacks**. Given
the measurement in the entry above - one CPU thread saturated, the GPU waited on for 0 ms in
every window, the whole Vulkan API at 0.6% of the frame - an interpreter default here is an
unplayable default. `#ifdef __ORBIS__` -> `"execute"`, everywhere else unchanged.

⚠ **A saved `.opt` file wins over a default.** `/data/retroarch/config/Beetle PSX/Beetle
PSX.opt` on the console has no `cpu_dynarec` line, because the core that wrote it had no such
option - so the new default does apply. Delete the line, not the file, if this ever needs
re-testing.

### ⚠ The build is reproducible now, which it was not

The `.prx` that ran for a day could not be rebuilt from the repository: the arm was
uncommitted and the link was a shell command nobody wrote down. `ps4/build.sh` in the fork is
that link. Two things it exists to prevent:

    make platform=orbis           tries to LINK, with $(LD) = $(CXX) = the host driver, and
                                  fails on host libstdc++. The previous route was to run make,
                                  let the link fail, and pick .o files out of the tree.
    make platform=orbis objects   a new target: compile and stop. This is what build.sh uses.

`STATIC_LINKING` is not the escape - it is a `-D` define about whether the core is built INTO a
frontend, not a link mode.

⚠ **`HAVE_LIGHTREC` needs a clean when it changes.** It is a `-D` define with no header
dependency, so objects built the other way are silently reused - which already cost a link
failure on `lightrec_destroy`. `build.sh` keeps a `.ps4-lightrec` stamp and cleans itself.

### On the console now

    /data/retroarch/cores/mednafen_psx_hw_libretro.prx   18903168 bytes, mode 777
    /data/retroarch/info/mednafen_psx_hw_libretro.info   mode 666
    /data/retroarch/info/core_info.cache                 DELETED, as it must be after any change

Nothing on the frontend side changed and no RetroArch rebuild is needed - the whole change is
inside the core.

### What the first run should say

The lines to look for, in order. `[PS4] lightrec:` is the prefix throughout.

    <addr> is taken (...)              one per rejected io_base, with what is there. This is
                                       the only account of where this process's address space
                                       is, and it is worth reading even on a successful run.
    N KiB code buffer at <addr>, writable and executable
                                       the promotion worked. Absent means it did not.
    no executable code buffer ...      the clamp fired; the interpreter is running.

Then the frame rate. The interpreter measured ~38% of realtime on Spyro 3 (40 fps in scene, 23
fps in the worst window). ⚠ **Audio pitch is the same fact as the frame rate here**, not a
second problem: the core runs slow, so samples come out slow.

### Still open, unchanged from the entry above

* The close-hang every title on this Mesa has.
* `orbis_paths.cpp:19` hard-codes `/data/OpenGothic/` as the relative-path anchor.
* `ORBIS_TILE_MODE` has no reader in mesa-ps4 and `tempest-env.example.txt` documents it.
* The temporary ORBIS instrumentation in `vfs_implementation.c` and `core_info.c` - both carry
  "Remove once ps4/HANDOFF.md has the answer", and both answers are in this file now.

### And one upstream defect found on the way, not fixed

`libretro.c`'s generic no-shared-memory arm does not compile, and has not for as long as the
mman deps have been there:

    #define MAP_SHM(addr,size,fd,offset)\
    #define MAP_CODE(addr,size,fd,offset)\
            MAP(addr,size,fd,offset)

The first `#define` ends in a backslash with nothing after it, so it swallows the `#define`
below it and `MAP_SHM` becomes an empty macro that is then called as a function. Any platform
falling into that arm gets four errors. Left alone here - the PS4 arm sits above it and fixing
it invites a conflict with upstream - but it is why nobody has exercised that path.

---

## 2026-08-23 (late) — the recompiler runs, and what was actually slowing it down

Measured on hardware, not predicted. Beetle PSX HW, Spyro 3, Vulkan renderer, 2x internal.

    [PS4] lightrec: calling 0x10800000 to see whether it executes...
    [PS4] lightrec: 0x10800000 returned 0x00c0ffee - the code buffer executes
    [PS4] lightrec: 8192 KiB code buffer at 0x10800000, writable and executable
    Lightrec map addresses: M=0x10000000, P=0x899a9c020, R=0x2fc00000, H=0x2f800000
    [Lightrec]: Using 32-bit LUT

The io_base search took the FIRST 32-bit candidate, 0x10000000, so the mirrors, the BIOS at
+0x1fc00000 and the scratch pad at +0x1f800000 all placed on the first try and the 32-bit LUT
is available. `mirrors_mapped` is true; the map is not "perfect" only because that requires the
guest's RAM at host address 0, which libretro.c deliberately refuses (psx_mem == NULL would be
indistinguishable from failure).

### ⚠ The first run failed, and the defect was in the check rather than the platform

    lightrec: sceKernelMprotect returned 0 for 0x10800000 but the range reads back as
              prot 0x03, without execute. Not using it.       (x16, one per base address)

**`sceKernelQueryMemoryProtection` reports the protection a range was MAPPED with, not the one
`sceKernelMprotect` has just set.** The pages were executable throughout; mesa-ps4's probe had
already established that by CALLING a stub in exactly this arrangement (`MIRROR rung 7 OK`,
same day). It never asked the query API, so nothing had caught it.

⚠ **This is the fifth call on this console that answers, and answers about something else** -
after GB_ADDR_CONFIG, the three tessellation registers, and the readerless env knobs. The rule
this workshop keeps re-learning has a sharper form now: **on this platform, a query API is not
a measurement of the thing it names.** The check was not deleted, it was replaced with the
probe's own: write `b8 ee ff c0 00 c3` into the buffer and call it, expect 0x00c0ffee.

⚠ **And the pair of log lines around that jump are BOTH at RARCH_ERR on purpose.** This
frontend puts everything below ERR on UDP alone (ps4/ps4_log.h). The warning before the jump
was at ERR and the confirmation after it was at INFO, so a klog reader saw "calling %p" and
then nothing - which is exactly what the death that line warns about looks like.

### Where the frame really went, and it was a core option

With the recompiler running the driver's BUDGET instrumentation still said, in twelve
consecutive five-second windows:

    1.00 cores busy    exactly one thread saturated
    GPU waited 0 ms    every window, no exception
    submit path        12-18 ms out of 5000 = 0.3%

The saved options carried **`beetle_psx_pgxp_mode = "memory only"`**. That is not cosmetic:
`mednafen/psx/cpu.c` in PGXP memory mode sets

    lightrec_map[PSX_MAP_KERNEL_USER_RAM].ops = &pgxp_nonhw_regs_ops;
    lightrec_map[PSX_MAP_BIOS].ops            = &pgxp_nonhw_regs_ops;
    lightrec_map[PSX_MAP_SCRATCH_PAD].ops     = &pgxp_nonhw_regs_ops;

Without PGXP those three are NULL and recompiled code reads memory directly. With it, **every
load and store to main RAM becomes a C function call**, which is the most expensive single
thing that can be switched on in a PSX dynarec. The whole benefit is geometry precision.

The set that made it smooth, all live-togglable (cpu.c:2890 watches pgxpMode, invalidate and
spgp and re-inits Lightrec without a reload):

    PGXP Operation Mode                 Memory Only -> Disabled     the big one
    Dynarec SP GP Hit RAM Optimization  Disabled    -> Enabled
    Dynarec Code Invalidation           Full        -> DMA Only
    Dynarec …Event Cycles               128         -> 512
    GTE Overclock                       Enabled     -> Disabled     an overclock costs host time
    Software Framebuffer                Enabled     -> Disabled     the only visual risk

⚠ **Internal resolution is NOT a lever here and raising it is nearly free** - the GPU waited
0 ms in every window measured, both before and after.

### ⚠ A stub that returns -1 is not a neutral stub

    [Lightrec]: Threaded recompiler started with 1 workers.

on six cores. `orbis-compat/include/sys/sysctl.h` answered every sysctl with -1, and **every
caller's fallback for "how many CPUs" is one** - the answer that switches parallelism off
rather than degrading it. Lightrec's `recompiler.c` takes the `__FreeBSD__` arm,
`sysctlbyname("hw.ncpu", ...) ? 1 : count`; Mesa's `u_cpu_detect.c` asks the same question with
a `{CTL_HW, HW_NCPU}` mib, in every title.

Fixed in the overlay, both spellings, six cores, `ORBIS_NCPU` to override. Six and not a guess
at seven: `sceKernelGetCpumode()` exists and reportedly separates the two modes, but this SDK
ships no constants for its return values and six against seven is a rounding error beside six
against one.

⚠ **Mesa does not get this until it is rebuilt**, and whether it should is an open question
rather than an oversight: more util threads in a driver the GPU never waits on could contend
with the emulation thread. That is a measurement somebody should make deliberately.

### Two console-facing traps found while doing this

⚠ **FTP hands back a `.prx` DECRYPTED.** A file uploaded as 18903088 bytes of self reads back
as 19807720 bytes starting `7F 45 4C 46`. md5 can never match, and an upload that worked looks
like an upload that silently failed. Verify by SIZE and MTIME. (`deploy.sh` dodges this only
because its byte-for-byte check is limited to files under a megabyte.)

⚠ **Two UDP log receivers were bound to 18194** - one from that day and one left over from
2026-08-21. `log-receiver.py` sets `SO_REUSEADDR` and not `SO_REUSEPORT`, so Linux delivers a
unicast datagram to exactly one of them and which one is not defined. "The log says nothing"
can mean "it went to a file from two days ago". Check with `ss -ulnp | grep 18194` before
reading silence as evidence.
