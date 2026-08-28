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
`git@github.com:orbis-ports/beetle-psx-libretro.git`, upstream `libretro/beetle-psx-libretro`.
(This said "not yet pushed - the repository may not exist yet"; checked 2026-08-28, the remote's
`refs/heads/ps4-support` is `b0b759e`, the same commit as the local branch. The work is not
stranded on one machine.) The recovered arm is its first commit, so
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

### Confirmed on hardware, same evening

    [Lightrec]: Threaded recompiler started with 5 workers.

⚠ **The first attempt at this reported `1 workers` from a correct fix**, and that is its own
entry above: `-MMD` omits `-isystem` headers, so nothing rebuilt and the `.prx` came out
byte-identical in size. `beetle-psx-libretro/ps4/build.sh` now stamps the overlay's newest
mtime alongside HAVE_LIGHTREC and cleans when either moves.

What the driver's BUDGET saw, gameplay windows only:

    before (PGXP on, 1 worker)     1.00 cores flat    152-189 presents / 5 s   ~31-38 fps
    after  (options + 5 workers)   0.80-0.95 cores    197-260 presents / 5 s   ~39-52 fps

⚠ **That delta does NOT separate the core options from the worker count** - both changed
between the two measurements, and the frontend was never run with one without the other. If
the split matters, `ORBIS_NCPU=1` in `/data/retroarch-env.txt` isolates the workers without a
rebuild.

Audio, which is the other thing five compile threads on six cores could have wrecked:

    1 worker    warm-up burst to 1145 underruns, then FLAT - no new underruns for two minutes
    5 workers   warm-up burst to  879 underruns, then FLAT from 40 s after load

So the burst is recompilation warm-up in both cases and it got *shorter*, not longer. No
starvation. One window did show 70 submissions and 61 presents at 0.95 cores - about 12 fps -
which is a genuinely heavy moment rather than a regression.

The GPU still waited 0 ms. Internal resolution remains free, and the remaining cost is
Beetle's own C++ (GPU command translation, SPU) plus the recompiled code itself.

### ⚠ The isolation run first measured nothing, and found something larger

`ORBIS_NCPU=1` was written into `/data/retroarch-env.txt`, the console relaunched, and the core
reported **five** workers. Not a parse error, not a stale build.

**The SDK's `libc.a` is a real static musl archive** - 1481 objects, `getenv` and `setenv` as
defined text rather than stubs into a shared libc module. So the eboot and every `.prx` it
loads link their own copy, each with its own `environ`. `platform_orbis.c`'s reader setenv()s
into the *executable's*; a core calling `getenv()` reads its own, which nothing ever wrote.

⚠ **EVERY ENV KNOB THIS WORKSHOP HAS IS EXPOSED TO THIS, and none of them showed it.**
ORBIS_3D_LINEAR, ORBIS_NO_TESS, MESA_LOG_FILE, RADV_DEBUG, all of tempest-env.example.txt - all
read by Mesa, and Mesa is linked INTO the executable. The first knob that had to reach a
loadable module was the first to fail, and it failed by looking exactly like a knob with no
reader: a clean run that reads as a measurement. That is the shape tempest-env.example.txt
spends half its length warning about, and it had a second cause nobody had named.

Fixed in the overlay: `orbis_env_get()` (`orbis-compat/src/orbis_env.cpp`, `include/orbis_env.h`)
answers from the image's own environment first and falls back to parsing the env files itself.
`sys/sysctl.h` uses it. ⚠ **Anything in a `.prx` that reads a knob must use it rather than
`getenv()`.** The file list naming "retroarch" is a seam, not a design - the honest mechanism is
a loader handing its module what it applied, and libretro has no channel for that.

### What the workers are actually worth

With the fix in, `ORBIS_NCPU=1` took, and the comparison is:

    warm-up underruns before the count goes flat, two runs each
      1 worker    1145, 1124     mean 1134   spread  21  (1.9%)     flat at ~45 s
      5 workers    879,  831     mean  855   spread  48  (5.6%)     flat at ~38 s
    steady state  both FLAT afterwards - zero new underruns, held for two minutes
    frame rate    NOT distinguishable; presents per window overlap and the scenes differ

**24.6% shorter warm-up, and nothing at all afterwards** - which is what the workers should
buy: they compile blocks, and once the blocks are compiled there is no work left. The two
groups do not overlap - the worst 5-worker run (879) is comfortably below the best 1-worker run
(1124), and the 245-underrun gap is five times either group's internal spread.

⚠ **THE SMOOTHNESS WAS THE CORE OPTIONS, PRINCIPALLY PGXP** - not the worker count. Both changed
in the same interval earlier and the entry above said the delta could not separate them. It can
now, and the answer is that the part which felt like the win was the part that was not being
tested.

⚠ **The metric is warm-up underruns, and it is only comparable across runs that BOOT THE DISC.**
Loading a save state skips the code the first run had to compile, which is the whole quantity
being measured.

`ORBIS_NCPU` has been removed from the console's env file. The default of 6 stands, because a
shorter warm-up for free is still worth having.

### Where the frame goes inside the core - and the target it is already hitting

⚠ **The per-thread route is closed.** `sceKernelGetCpuUsage` links and returns `0x8002004e`,
ENOSYS - this kernel does not offer it to this process. `ps4/ps4_threads.c` keeps the attempt
and the return code so nobody spends an evening on it again.

The split therefore comes from inside the core (`ORBIS_CORE_PROFILE=1`, beetle's `retro_run`).
Twelve consecutive five-second windows, Spyro 3, hardware renderer, 2x internal:

    249 frames every window, without exception   = 49.78 fps
    CPU_Run              9.8-12.3 ms/f   49-61%   emulated machine + everything it schedules
    rest of retro_run    7.8-10.3 ms/f   38-51%   parallel-psx building the frame, audio batch
    outside the core                       0.1%   the frontend itself

⚠ **THE DISC IS PAL AND THE CORE IS AT 100% OF TARGET.**

    [Core] Geometry: 320x240, FPS: 50.0000, Sample rate: 44100 Hz
    SET_SYSTEM_AV_INFO: FPS: 49.7610

49.76 requested, 49.78 delivered. There is no deficit left to recover in steady state - but the
thread is 99.9% busy doing it, so there is also no slack. Anything gained from here buys margin
against the dips rather than frames.

⚠ **HALF THE FRAME IS NOT THE RECOMPILER**, which is the answer to "can the JIT be optimised
further": at most half of a frame that is already meeting its target. And `rest` is not the
software framebuffer - that option is already off - nor the driver, whose submit path BUDGET
puts at 0.3% of wall. It is parallel-psx's own command building, above the driver.

⚠ **AND THERE IS A PACING PROBLEM NO AMOUNT OF SPEED FIXES:**

    [Video] Timings deviate too much. Will not adjust. (Target = 59.94 Hz, Game = 50.00 Hz)

A 50 Hz game on a 59.94 Hz output judders by construction, and this console does not output 50
Hz. An NTSC copy of the same title would match the display exactly. Worth knowing before
anyone reads uneven pacing as a performance problem and optimises at it.

`beetle_psx_dynarec_spgp_opt` is still `disabled` - the one recommended knob not applied.

### Scan Directory: "Scanning unsuccessful, no database found"

Two causes, both present, and neither is a defect in this port:

`/data/retroarch/database/rdb/` was **empty**. `tasks/task_database.c:3447` reports exactly that
message when the database list comes back size 0 and the scan is in LOOSE or STRICT mode. The
file wanted is `Sony - PlayStation.rdb` from libretro-database (7709655 bytes).

The core's `.info` declared **no `database` field**, so even with the .rdb present nothing
associates a scanned disc with this core. `ps4/build.sh` in the core's fork now writes
`database = "Sony - PlayStation"`, and the name must match the .rdb's filename exactly.

⚠ **AND THE FILE-MODE RULE IS NOT ONLY ABOUT FILES.** The directories RetroArch creates come out
`drwxr-x---`, which the FTP daemon cannot write into - so the upload "succeeded" and the
directory stayed empty, twice, with no error anywhere. The writable ones (`cores`, `info`,
`assets`, `shaders`, `system`, `roms`) are 777 only because they were chmod'ed by hand at some
point. `chmod 777` the directory before putting anything in a fresh one.

Both were fixed on the console by hand. For a shippable package the `.rdb` belongs in `/app0`
alongside the core and its `.info`.

## 2026-08-23 (late) — how many cores exist, and how many build

`ps4/build-cores.sh` clones cores from a libretro-super recipe, builds them with this toolchain
and links each into a `.prx`. **It does not patch their Makefiles** - Beetle PSX HW needed a
hand-written `orbis` arm and there are 99 more candidates, which is 99 more of those.

The trick that makes it work: the toolchain flags go INSIDE `$(CC)` and `$(CXX)`, not into
`CFLAGS`. A libretro Makefile routinely does `CFLAGS := ...` and throws away what the caller
passed; almost none of them rewrite CC. `platform=unix` then gives the core a sane arm, its own
link fails (host driver, `-shared`), and the objects are collected and linked here.

### The field

    184   cores in libretro-super's cores-linux-x64-generic
    162     plain Makefile (GENERIC)          the tractable class
     17     CMAKE                             needs a toolchain file
      5     GENERIC_GL                        ⚠ IMPOSSIBLE HERE - no OpenGL, only Vulkan
     99   of the 184 also appear in the Vita recipe - already ported to a fixed console once

### First sweep: 20 of 29

    OK       fceumm gambatte snes9x2010 quicknes mednafen_ngp mednafen_wswan mednafen_vb
             mednafen_lynx gme handy prosystem stella2014 tyrquake prboom race pokemini
             snes9x2005 vba_next fmsx freeintv numero
    LINK     genesis_plus_gx mednafen_pce_fast nestopia gearboy gearsystem picodrive vecx
    COMPILE  mgba

### ⚠ Three things the sweep found that no single core would have

**LTO throws the whole libretro API away and the link reports success.** Several cores compile
with `-flto` under `platform=unix`, so their `.o` files are bitcode - `llvm-nm` prints dashes
where the address belongs. ld.lld links bitcode, runs LTO, and internalises everything
unreachable from an entry point; a module has no entry point. `snes9x2010` linked to a 915 KiB
ELF containing **zero** `retro_*` symbols, exit code 0. Fixed by naming all 25 libretro entry
points with `-u`. The harness's "linked without retro_run" guard is what caught it and it earned
its place on the first batch.

**A .prx cannot borrow the frontend's libretro-common.** Cores call `retro_vfs_*_impl`,
`cdrom_*` and friends without building them, because a shared object resolves them lazily
against the frontend at load time. A module has its own symbol table. The harness archives the
frontend's 116 libretro-common objects plus 138 it compiles itself (35 will not build here) and
puts the **archive** after the core's own objects - so a core that did build its own copy keeps
it, and nothing is duplicated.

**FIONREAD was a trap, not an absence.** The SDK defines no `FION*` at all. Taking musl's would
have been the obvious fix and would have been wrong: musl carries LINUX request numbers
(`0x541B`) and this kernel encodes direction, length and group into the number
(`FIONREAD = 0x4004667f`). Same shape as `MAP_ANON` being `0x0020` in the SDK's header and
`0x1002` in the kernel. `orbis-compat/include/sys/ioctl.h` now derives them rather than copying
them. ⚠ **Nothing in this workshop has yet called ioctl() on this console**, so these are
reasoned, not measured.

### The remaining failure classes, each with a shape

    cdrom_lba_to_msf            libretro-common/cdrom/cdrom.c needs HAVE_CDROM, which CHANGES
    (genesis_plus_gx,           the layout of libretro_vfs_implementation_file. Forcing it into
     mednafen_pce_fast)         the shared archive would mix two struct layouts in one link -
                                silent and worse than the missing symbol. Per-core work.

    missing C++ class symbols   Their Makefiles compile straight from sources to the .so in ONE
    (gearboy, gearsystem)       command and never write a .o, so there is nothing to collect.
                                STATIC_LINKING=1 does not change it. Needs a real platform arm.

    nestopia                    12 objects only; its own vendored code fails earlier.
    picodrive                   dr_mp3.h: duplicate case value - a source/compiler disagreement.
    vecx                        glIsEnabled. Correctly impossible: this port has no OpenGL.
    mgba                        no objects and no errors - wrong makefile for the recipe entry.

⚠ **A core that builds is not a core that works.** This harness reports what compiled. Every one
of the twenty is unrun; the console is the only thing that can tell a working core from a
linking one.

### The whole recipe, built: 100 of 162

Every `GENERIC` core in `cores-linux-x64-generic`, one at a time. **453 MB of `.prx` in
`~/.cache/ps4-cores/out`**, with `cores.manifest` recording core, verdict, size and the upstream
commit each was built from.

⚠ **NOT IN /tmp, AND THAT IS NOT A DETAIL.** The first sweep's output lived in the session
scratchpad; a cold reboot cleared tmpfs and took 52 built cores, every clone and the manifest
with it. `build-cores.sh` already defaulted to `~/.cache/ps4-cores` and the default was being
overridden on every call.

    100  OK
     45  LINK       an undefined symbol, named in the manifest
     10  COMPILE    no objects at all
      6  NO-ABI     linked, but without retro_run
      1  CLONE      submodule fetch failed

### Two classes closed during the sweep, worth 7 cores

**`HAVE_CDROM=0`.** It gates passthrough to a *host CD device* - a real drive opened by path -
which this console does not have, so the feature could not work here whatever it linked. It also
adds a member to `libretro_vfs_implementation_file`, so the flag decides a **struct layout shared
between objects**. Satisfying the resulting `cdrom_lba_to_msf` from the shared archive would have
put two layouts of one struct into a single link: silent, and far worse than a missing symbol.

**`ZSTD_trace_*` are undefined WEAK.** ld.lld leaves them undefined, which is what weak means.
create-fself will not: it maps every remaining undefined symbol to a library NID, finds none, and
refuses the module - ⚠ **with exit status 0**, so a script trusting the exit code sees success and
no file. Fixed with real no-op definitions (`ps4/orbis_weak_stubs.c`).

⚠ **And the first attempt at that fix did nothing, instructively:** the stubs went into the
fallback *archive*, and **an archive member is never extracted to satisfy a weak undefined
symbol**. They have to be passed as a plain object.

### What is left, by cause rather than by core

     8  OpenGL / X11        ⚠ NOT FIXABLE HERE and should not be listed as work. boom3, boom3_xp,
                            craft, desmume, kronos, vecx, vitaquake2, vitaquake3. Note desmume2015
                            (same system, software renderer) builds fine - several of these have a
                            sibling that already works.
     8  no .o at all        blastem, higan_sfc, higan_sfc_balanced, mame2016, mgba, rustynes,
                            scummvm, squirreljme. Their Makefiles go from sources to the shared
                            object in ONE command and never write an object. STATIC_LINKING=1 does
                            not change it. Each needs a real platform arm - a patch.
     7  vice_*              562 objects each, then undefined `log_cb`, `pix_bytes`, `opt_vkbd_alpha`.
                            One family, one TU failing, one patch away from seven cores.
     6  NO-ABI              bsnes2014, freej2me, hbmame, mame, openlara, stella. Built something,
                            not a libretro core.
     2  FLAC                yabasanshiro, yabause - vendored libFLAC not in the source list.
     2  hiro::              bsnes, bsnes_hd_beta - byuu's GUI toolkit pulled into a libretro build.
    12  one-offs            SDL_GetTicks, unzOpen2, sk_num, JS_ToInt32, wasm_rt_trap, mp3dec_start,
                            osd_malloc, BurnDrvCps1944j, BurnSampleReset, inet_htons, ace::ace,
                            ARMJIT_Memory, AMeteor, cEmuSCV, GetKeyState, inflateInit2_.

⚠ **NONE OF THE HUNDRED HAS BEEN RUN.** The harness reports what compiled. A core that links and
draws nothing is a pass here and a failure on the console, and only the console knows.

---

## 2026-08-24/25 — running the hundred, and three loader defects underneath them

The sweep of the previous entry produced binaries. This one is about what happened when they
were run, and the answer is that **the first seventeen results were about the frontend, not the
cores**. `ps4/CORE-STATUS.md` carries the per-core table; this is what it cost to get there.

### ⚠ No .prx on this console had ever run a global constructor

Every C++-dominant core crashed on load with

    terminating with uncaught exception of type std::length_error:
    allocator<T>::allocate(size_t n) 'n' exceeds maximum supported size

and every C core was fine - seventeen results, the split exactly along the language. It was
never about C++. Three defects, each found only by running, and **two of them introduced by the
fix for the first**:

    1  crtlib.o marks module_start GLOBAL HIDDEN, so the linker correctly makes it LOCAL, and
       create-fself builds a module's export table from GLOBAL symbols in .symtab. The loader
       can never find it. Constructors never ran, so every C++ global stayed unconstructed and
       the first request for a size returned garbage.

    2  the frontend then ran them from dylib_load - on EVERY load. sceKernelLoadStartModule
       returns the SAME id for an already-loaded module and RetroArch loads a core several
       times, so mednafen_gba constructed its four globals EIGHT times. That presented as
       `failed_to_start_audio_driver` and an assertion deep inside Blip_Buffer.

    3  the once-per-module guard then held ids across unload, and the kernel REUSES them.
       nestopia unloaded, quicknes took its id, its constructors were skipped, and it died on a
       null read at 0x20 - a SIGSEGV that looks exactly like a broken core.

`libretro-common/dynamic/dylib.c` holds the whole chain in a comment; `ps4/orbis-module.ld`
brackets `.init_array` because OpenOrbis's `link.x` collects it and defines nothing around it
while `crtlib.o` carries the bounds as BSS variables eight bytes apart.

⚠ **THE METHOD LESSON IS THE ONE TO KEEP.** Four rounds were spent reading meaning into silence:
a probe that logged through klog printed nothing, which could equally mean "the constructor did
not run" or "klog from a .prx does not reach the host"; then a probe without a priority sat at
the END of the array, so it could say nothing about a module that dies earlier. What settled it
was a **control**: the same probe in `fceumm`, a core known to work. That should have been the
first experiment, not the fourth. And a verdict of `crash` against a core is worth very little
until the loader underneath it is known to be sound - seven cores sat condemned for this.

### The `.info` files under-declare, in three different ways

Every attempt to build a content list from core metadata came out short, and only the console
showed it:

    arcade cores   say their BIOS requirement in `notes` prose, with no firmware fields at all.
                   fbalpha2012_neogeo refused Metal Slug with `NeoGeo BIOS missing` and a list of
                   files nothing had asked for. neogeo.zip was in the repository all along.
    a5200          marks a mandatory BIOS `firmware0_opt = "true"`. It does not start without it.
    nxengine       needs a whole game data directory, described only in `notes`.

⚠ **A core's own metadata is a hint. The only reliable statement about what it needs is the core
refusing to start and saying so.** All 131 optional firmware files the repository carries are now
on the console alongside the required 34; arcade BIOS goes in the ROM directory, not `system/`.

### Two things that are now known and were not

**The archive path works.** `xrick` declares `zip` as its only extension and played straight out
of `data.zip`. The earlier warning that compressed formats were unexercised is answered for
`.zip`; `.chd` and `.7z` still are not. The corpus stays extracted anyway, and
`snes/Super Mario World.zip` still sits beside the `.smc` as a control.

**Arcade is blocked on romset vintage, not on the port.** Checked by CRC against FBA's own DAT
over all 231 sets to hand: 129 match FBA 2012, 98 are the wrong revision, 4 are unknown to it.
⚠ The first run of that check said "1 match" - a merged DAT lists the BIOS chips inside every Neo
Geo game while they live in `neogeo.zip`, so counting them as missing condemned a library that
had just been observed playing. The observation was right and the script was wrong.

### ⚠ The harness will overwrite a hand-ported core, and did

`build-cores.sh` clones UPSTREAM. Its `mednafen_psx_hw_libretro.prx` is plain Beetle - no orbis
arm, no `ps4/orbis_lightrec_mem.c`, no dynarec default - and it lands on the same filename as the
fork in `~/src-ps4/beetle-psx-libretro`. It did, and Spyro got slower for no visible reason. Only
the console copy was affected; the fork's source was never touched.

`PS4_CORE_FORKS` now names such cores. The harness builds them and reports `FORK - built, NOT
written`.

### Where testing stands

    31  plays        including every core that had been recorded as crash
     7  blocked      arcade, waiting on romsets of the right vintage
     1  broken       nestopia - loads, runs, exits cleanly, renders a green screen
    63  untested

Content staged on the console: NES, SNES, GBA, Mega Drive, Neo Geo (with `neogeo.zip`), PSX,
OutRun, Cave Story, Dinothawr, xrick, and Freedoom + `prboom.wad` for PrBoom. Of the untested,
about eight need no content at all; the rest want systems nothing on this machine has.

### ⚠ CORRECTION, 2026-08-25: the GL arm runs, and it is no longer an arm

This entry first recorded GL as *"a gate, not a product - it builds and it LINKS; it does not
run"*, quoting `build-support/orbis/build.sh`. **That comment was false when it was written**, and
mesa-ps4 has since deleted it (`62902e9a229`). GL runs on hardware: a frame, a triangle with
shaders, and Vulkan alive beside it in the same process.

The `--gl` flag is gone with it. `COMMON_OPTS` now carries zink, EGL and GLES2 beside RADV,
everything lands in one `build-orbis`, and the GL link probe runs on every build and fails it if
`libEGL.a` is missing. Two halves compiled separately are two halves that were never compiled
against each other, and this port needs both in one eboot with the core chosen at run time.

    build-orbis/src/amd/vulkan/libvulkan_radeon.a   36 MB
    build-orbis/src/egl/libEGL.a                     5.7 MB
    build-orbis/src/mesa/glapi/es2api/libGLESv2.a
    build-orbis/src/gallium/drivers/zink/libzink.a
    build-orbis/src/gallium/targets/dri/libgallium-*.a

⚠ **Vulkan-only consumers pay nothing** - they link `libvulkan_radeon.a` and the gallium archives
go unreferenced. So this frontend's current link line keeps working untouched.

Two things the GL link probe learned that any consumer will meet, both found on binaries an
earlier probe had passed:

    Mesa's dispatch tables reference their entry points WEAKLY, and a weak undefined reference
    does not extract an archive member - it resolves to zero. Without --whole-archive on the ICD
    the executable linked with vk_common_GetPhysicalDeviceProperties2 still undefined and jumped
    to address 0 on the first dispatch. (Same shape as the ZSTD_trace_* stubs in build-cores.sh:
    a weak undefined symbol never pulls an archive member.)

    .tdata.* is an orphan section under the SDK's own link.x, which cost the RW segment its page
    alignment and made the console refuse the file outright.

### What is still ours to do for GL

⚠ **This frontend has `orbis_vk_ctx.c` and nothing for `RETRO_HW_CONTEXT_OPENGL`.** A GL core asks
the frontend for a context through that mechanism; a working zink underneath changes nothing
until that driver exists. That half was always ours and still is.

What it would unblock, and what it would not: the eight cores the sweep listed as OpenGL-blocked
mostly have working software siblings (`desmume` against `desmume2015`). The real prize is
`parallel_n64`, `mupen64plus_next` and `ppsspp` - which are also the heaviest things this console
would be asked to run, on a port where Beetle PSX already spends a saturated core on the
interpreter. Worth starting now that the driver renders; worth expecting the frame rate to be the
next problem rather than the last one.

## Nintendo 64: what the frame is actually spent on

`mupen64plus_next` runs Shadows of the Empire at 22-25 fps. Everything below was measured on
hardware on 2026-08-25 with `ps4/orbis_profile.c`, which is linked into every core the harness
builds and turns itself on when `/data/retroarch-profile` exists. It reports every five seconds:

    129 frames in 5043 ms = 25.57 fps
      retro_run total  38.65 ms/f  98% of wall
      guest            37.44 ms/f  95%
      rsp-lle          33.94 ms/f  86%     <- rdp-submit is NESTED inside this
      rdp-submit       10.13 ms/f  25%
      rdp-enqueue       5.18 ms/f  13%
      rdram-h2g         0.48 ms/f   1%
      present           1.20 ms/f   3%

Unnested, per frame: **LLE RSP 24 ms, RDP command building 10 ms, R4300 dynarec 4.7 ms, scanout
1.2 ms.** The recompiler this port spent a day giving executable memory to is 12% of the frame.

⚠ **THE GPU IS NOT THE BOTTLENECK AND NEVER WAS.** `present` is one millisecond. A core sold as
"ParaLLEl-RDP on Vulkan" puts only the RASTERISER on the GPU; the RSP is a programmable vector DSP
running the game's own microcode, recompiled by GNU lightning onto the CPU, and it is the largest
single cost by a factor of two. Anyone reading "Vulkan" as "the graphics are free" will optimise
the wrong half.

### The thirteen milliseconds that were handshake

ParaLLEl-RDP processes its command stream on a worker thread, and `CommandRing::enqueue_command`
takes a mutex and signals a condition variable **once per RDP command** - thousands of round trips
a frame. Measured before the change:

    rdp-enqueue   17.08 ms/f    emulation thread blocked in the ring
    rdp-worker     7.09 ms/f    what the worker actually did

The consumer was busy for seven milliseconds and the producer waited seventeen. Setting
`single_threaded_processing` took the frame from 51.9 ms to 38.7 ms - **19 fps to 25.6** - and
`rdp-enqueue` from 17.08 to 5.18.

⚠ **THIS IS A FACT ABOUT THIS MACHINE, NOT ABOUT THE DESIGN.** The ring is right where a spare core
is cheap to reach; here the cores are 1.6 GHz Jaguars and **nothing in this port sets thread
affinity**, so a handoff can be a context switch on the same core. If affinity is ever set up,
re-enable the ring and measure again before believing this entry.

### A direction that was measured and abandoned

parallel-RDP warns at startup:

    VK_EXT_external_memory_host is not supported by this device. Application might run slower
    ... falling back to a slower path.

That message reads like the explanation for everything above, and it is not. The fallback path is
`Renderer::resolve_coherency_host_to_gpu`, page-granular memcpy of dirty RDRAM into a GPU-visible
buffer, and it costs **0.5 ms a frame**. Importing RDRAM as host memory - whether by re-enabling
`has_userptr` in mesa-ps4 or by allocating RDRAM from `vkAllocateMemory` - would buy half a
millisecond out of thirty-nine. Both were planned in detail before anyone measured them.

⚠ The driver's own comment (`ac_gpu_info.c`, "nothing on this console can use it either way") is
still correct in outcome, for a reason it does not give: something does want the extension, and
would gain almost nothing from it.

### What is left, and what it is worth

    LLE RSP           24 ms    only HLE removes this, and HLE needs a graphics plugin that
                               accepts display lists - GLideN64, which needs RETRO_HW_CONTEXT_OPENGL
    RDP commands      10 ms    CPU-side command building; the ring may return here with affinity
    R4300              4.7 ms  the recompiler, working as intended
    scanout            1.2 ms  the GPU

ParaLLEl-RDP implements `ProcessRDPList` and leaves `ProcessDList` empty, so the HLE RSP draws
nothing with it - tried on hardware, the log said `Plugins in use: RDP=ParaLLEl RSP=HLE` and the
screen stayed black. **N64 at full speed on this console is a GL context driver away, not a
tuning exercise away.**

## OpenGL, and Nintendo 64 at full speed

`gfx/drivers_context/orbis_gl_ctx.c` gives this frontend a `RETRO_HW_CONTEXT_OPENGL` context.
Built with `make -f Makefile.orbis HAVE_VULKAN=1 HAVE_OPENGLES=1 ...`; the Vulkan flag is not
optional, because there is no GL hardware path here at all:

    eglSwapBuffers -> kopper -> vkQueuePresentKHR -> VK_EXT_headless_surface -> wsi_orbis -> flip

On hardware, 2026-08-25: `OpenGL ES 3.1 Mesa 26.3.0-devel`, renderer `zink Vulkan 1.3 (RADV
ORBIS)`. mupen64plus-next with GLideN64 and the HLE RSP, Shadows of the Empire:

    303 frames in 5015 ms = 60.41 fps
      retro_run total  16.88 ms/f
      guest             5.18 ms/f  31%    <- all of the emulation
      present          11.68 ms/f  70%    <- idle, waiting for the flip

Five milliseconds of work in a 16.7 ms frame, against 43 ms on ParaLLEl-RDP. The arithmetic in
the section above said full speed was unreachable without this, and it was right about both
halves.

### Three things that each cost a round, and all three were the same mistake

⚠ **A statically linked Mesa does not make `__rglgen_` pointers into direct calls.** gl2.c guards
`rglgen_resolve_symbols()` with `#if !defined(RARCH_CONSOLE)`, on the reasoning that a console
links GL statically. This one does, and it changes nothing: the entry points glsym turns into
POINTERS stay pointers. Unresolved they are null, and the symptom was "Couldn't find any
supported shader backend" followed by SIGSEGV with `rip = 0`. Neither line says "unresolved".

⚠ **A context driver that reports no shader flags is not neutral.** `gl2_get_fallback_shader_type`
asks the CONTEXT driver which shader languages exist - "for gl2, shader support is completely
defined by the context driver shader flags" - and `gl2_shader_init` then logs an error and
returns TRUE. The driver comes up fully initialised with `gl->shader` NULL and presents empty
frames: black screen, no menu, and GoldHEN's counter reading a steady 60 fps.

⚠ **A core's GLES entry points must be THUNKS into the frontend's context, never a second copy of
Mesa's dispatch.** libGLESv2.a is the mapi dispatch table; the context is current in the
frontend's copy, so a copy inside the module would be empty. `ps4/orbis_gl_forward.c` forwards
133 entry points, resolved once from `context_reset` through the frontend's proc address. The
thunks are assembly because a C forwarder needs the exact prototype of all 133 and a wrong one
is not a compile error - it is arguments in the wrong registers, silently.

### ~~Open: framebuffer emulation and depth~~ SOLVED 2026-08-28

⚠ **Read the two entries at the end of this file before anything below.** The cause was GLideN64
throwing the depth attachment away over a one-pixel width difference, on GLES only; it is fixed in
`ps4/core-patches/mupen64plus_next/0008` and confirmed on hardware. The suspects named below were
in the wrong place, and the account of how that was narrowed is worth more than they are.

With `Framebuffer Emulation` ON the depth is wrong. Turning it off fixes it and is the current
recommendation. It is the first time GLideN64 has run over zink on this GPU, so the fault could be
in any of GLideN64, zink, RADV or these thunks.

⚠ **THE SYMPTOM WAS FIRST WRITTEN DOWN HERE AS "geometry behind the camera draws in front" AND
THAT WAS AN INTERPRETATION, NOT AN OBSERVATION.** Re-tested on hardware on 2026-08-28, Shadows of
the Empire: **the player's ship draws behind every other 3D object in the scene** - an enemy far
away appears in front of it, and bases on the map draw over it. Collisions still fire at the right
moment, so the game's own state is untouched and this is the renderer alone. That is a different
statement from the first one and it points somewhere else: "behind the camera draws in front" reads
as a clipping fault, and "the first thing drawn loses to everything drawn after it" reads as **no
depth test at all**.

**Read from the source, three candidates, ranked.** All three were found in the clone at `f275caf`
and none has been measured yet.

⚠ **1. GLideN64 can drop the depth attachment entirely, and only on GLES.**
`FrameBufferList::attachDepthBuffer` (`FrameBuffer.cpp`) will only attach a depth buffer whose
texture matches the colour buffer's width, and the test it uses is chosen by
`Context::WeakBlitFramebuffer`, which **is `isGLESX`**:

    GLES     goodDepthBufferTexture = depthTexture->width == colour->width     exact
    desktop  goodDepthBufferTexture = depth >= colour || |m_width difference| < 2

On a mismatch it sets `pCurrent->m_pDepthBuffer = nullptr` and the FBO is rendered into **with no
depth buffer**: every fragment passes, and the picture is ordered by draw order. And a mismatch is
reachable, because `DepthBuffer::initDepthBufferTexture` creates the texture once, sized to
whichever colour buffer happened to be current, and returns early ever after - so a game pairing
one depth buffer with colour buffers of two widths loses depth on the second. This is the only
candidate that is simultaneously framebuffer-emulation-only, GLES-only, and silent.

**2. The same thing from underneath: a `GL_DEPTH_COMPONENT24` texture attachment that zink or RADV
does not honour.** Indistinguishable from 1 in the picture, distinguishable in one log line -
either GLideN64 dropped the attachment or it did not. With framebuffer emulation off there is one
screen-sized buffer and the widths always agree, which is why that path is unaffected either way.

⚠ **3. A configuration this build splits that upstream treats as a pair.** `enableClipping` is
forced to 1 for every GLES context (`opengl_GLInfo.cpp`), which makes the vertex shader emit
`gl_Position.z /= 8.0` - deliberately, to stop the hardware clipping geometry the software clipper
is about to handle. The only code that scales that back is in `writeDepth()`, and `writeDepth()` is
compiled to `return 0.0;` when `enableFragmentDepthWrite == 0` **and** `N64DepthCompare` is
disabled. That is exactly this build: mupen64plus-next defaults `EnableFragmentDepthWrite` to
`False` under `HAVE_OPENGLES`, and it does not even expose `EnableN64DepthCompare` there. So the
`/8` is never undone.

⚠ **But candidate 3 predicts the wrong direction, which is why it is ranked last rather than first
despite being the one provable defect.** The software clipper runs *only* with framebuffer
emulation ON (`GraphicsDrawer::drawTriangles` calls `renderAndDrawTriangles` in that branch and a
plain `drawTriangles` in the other), so the `/8` leaves near-plane geometry unclipped in the
configuration the user says looks CORRECT. Depth ordering itself survives the `/8` because dividing
by 8 is monotonic. It is worth fixing on its own account; it is probably not this.

**The experiment that costs nothing runs first:** with framebuffer emulation ON, set
`GPU shader depth write` to True in the core options. No rebuild, no upload. It re-enables the
whole `writeDepth()` path and settles candidate 3 by itself.

**And `ps4/core-patches/mupen64plus_next/0006` answers 1 against 2 on the next build.** ⚠ It exists
because **GLideN64 is compiled with `LOG_LEVEL LOG_NONE`**, so every warning it writes - including
`GLInfo`'s own "your GPU does not support the extensions needed for..." lines - is discarded before
it is formatted. On this console that is silence by construction, and it is why none of the above
could be read off a log. The patch reports one line of capabilities at context creation and one
line per dropped depth attachment, through `orbis_report`, bounded at four because klog costs
8-15 ms a line.

## The toolchain's own bugs, and the instruments built to find them

Three defects this week were in the SDK rather than in any core, and all three presented as a core
crashing. They are recorded together because the shape repeats: **a prebuilt library compiled for a
different platform's constants, shipped for this one.**

### ⚠ libc++.a compares against LINUX'S ETIMEDOUT on a FreeBSD target

Observed loading a ROM in mupen64plus-next:

    libc++abi: terminating with uncaught exception of type std::__1::system_error:
               condition_variable timed_wait failed: Operation timed out

"Operation timed out" IS errno 60, ETIMEDOUT, and a timeout is the ordinary outcome of a timed wait -
`wait_for` returns `cv_status::timeout` and does not throw. It threw because the comparison that
filters that case out was compiled against a different number. Disassembling
`condition_variable.cpp.o` out of the SDK's `libc++.a`:

    322: 83 7d 94 6e    cmpl $0x6e, -0x6c(%rbp)     ; 0x6e = 110

110 is Linux's ETIMEDOUT. This console is FreeBSD underneath, its pthread returns 60, and the SDK's
own `bits/errno.h` agrees with 60. **Every timed wait that has ever expired on this platform threw.**

⚠ **AND IT IS NOT LIMITED TO condition_variable.** `std::future::wait_for`, `shared_timed_mutex` and
everything else with a deadline goes through `__do_timed_wait`. The call site that killed the N64
core is `CommandRing::thread_loop`, which does `cond.wait_for(holder, 500us, …)` whenever the RDP
worker has nothing to do - so the core died the first moment it went idle.

`ps4/orbis_cv_fix.cpp` rebuilds `condition_variable`'s four strong symbols from libc++ 11's own
source against this platform's headers. ⚠ All four, not just the broken one: an archive member is
all-or-nothing, so providing `__do_timed_wait` alone would leave `notify_one`, `notify_all` and
`wait` undefined. `notify_all_at_thread_exit` is deliberately absent - it needs libc++'s private
per-thread bookkeeping and no core here has referenced it.

⚠ **Fixing it at `pthread_cond_timedwait` instead would have been wrong**, and the reasoning is worth
keeping: everything compiled in this workshop sees ETIMEDOUT as 60 through the SDK's headers and
tests for 60. Only the prebuilt library expects 110. Translating the return value would fix libc++ by
breaking every honest caller. The mismatch belongs where it was introduced.

The real fix is a libc++ rebuilt against the target's headers, which is an OpenOrbis change.

### A core's dying words now reach a channel someone reads

`assert()`, libc++abi's `abort_message()` and `__cxa_pure_virtual` all say exactly what went wrong and
then call `abort()`. All of it goes to stderr, and on this kernel fd 2 goes nowhere.

⚠ **AND POINTING fd 2 SOMEWHERE IS NOT AVAILABLE:** `dup2` onto 1 or 2 returns **EPERM**. The
descriptor table is not ours to rearrange, so the message has to be caught before it is written
rather than after.

`ps4/orbis_abort_report.c` defines `abort_message`, `__assert_fail` and `abort` itself, overriding the
toolchain's by definition order - each lives in an archive member of its own defining that one symbol,
so a definition reaching the linker first means the member is never pulled. It reports through
`sceKernelDebugOutText` (synchronous; a UDP datagram needs the process to survive its own send) and
appends to `/data/retroarch-abort.log`.

⚠ **The one that matters most is `abort()` itself, and what it must produce is the RETURN ADDRESS.**
Plenty of code aborts without a message - libunwind's `_LIBUNWIND_ABORT`, GNU lightning, paraLLEl-RSP's
allocator. Those arrive as a bare SIGABRT with `abort` as the only frame, because a core is built
`-fomit-frame-pointer` and the console's backtracer cannot walk through it. The return address is on
the stack whether or not there is a frame pointer. It found `RSP::JIT::CPU::init_jit_thunks` in one
step, after two rounds of guessing had found nothing.

⚠ **`-DNDEBUG` IS PER TRANSLATION UNIT.** A core built with it still links libretro-common,
orbis-compat and vendored dependencies that were not. An assertion in one of those is
indistinguishable from a C++ runtime failure from the outside.

### Measuring instead of arguing

`ps4/orbis_profile.c` is linked into every core the harness builds and turns itself on when
`/data/retroarch-profile` exists - a file rather than an environment variable, because each image
carries its own static musl and `setenv` in the frontend is invisible to a `.prx`.

It exists because three performance stories in one day turned out to be wrong before it was written.
The section above on the N64 frame is entirely its output.

## Networking: the socket layer was never missing

⚠ **THIS SECTION REPLACES AN EARLIER ONE THAT WAS WRONG, AND THE MISTAKE IS WORTH KEEPING.** The
previous inventory checked `libc.a`, `libpthread.a` and "`libkernel.a`" at symbol level, found nine
of the twenty-two POSIX socket calls, and sized a rewrite of the whole API over `sceNet*` as an
archive-member override. **There is no `libkernel.a`.** The socket calls arrive through the DYNAMIC
stub `lib/libkernel.so`, which the OpenOrbis stub generator built from retail libkernel's export
list, and `-lkernel` has been on this port's link line since the first build.

    present, from libkernel.so (dynamic)   socket connect bind listen accept send recv sendto
                                           recvfrom shutdown setsockopt getsockopt getpeername
                                           getsockname select poll close fcntl sendmsg recvmsg
    present, from libc.a (static)          getaddrinfo freeaddrinfo getnameinfo gethostbyname
                                           gethostbyname2 inet_pton inet_ntop inet_addr inet_aton
                                           getifaddrs if_nametoindex in6addr_any accept4 signal
    missing                                nothing

Proven, not inferred, three ways:

1. **A link probe.** A translation unit taking the address of all twenty-two calls, compiled with
   this port's exact flags and linked with this port's exact `LIBS` line, links. The only symbol it
   could not resolve on the first attempt was `sceNetGetDnsInfo`, and `-lSceNet` was already there.
2. **The SDK's own libc is built on it.** `getaddrinfo.lo` in `libc.a` has undefined references to
   `socket`, `connect` and `close`; `fcntl.lo` calls libkernel's `_fcntl`; `send.lo` is a tail call
   to `sendto` and `recv.lo` to `recvfrom`; and musl's `socket()` is a wrapper over
   `__sys_socketex(name, domain, type, protocol)` - the same name-taking syscall `sceNetSocket`
   uses, passing `""`. There is ONE descriptor namespace here, and it is the kernel's.
3. **The SDK ships a sample that uses it.** `samples/networking/` opens a TCP listener with plain
   `socket`/`bind`/`listen`/`accept`/`close` over `<sys/socket.h>` and `<netinet/in.h>`.

⚠ **AND `errno` IS SHARED, WHICH IS THE PART THAT WOULD HAVE BEEN HARD TO GET RIGHT BY HAND.**
`libc.a`'s `__errno_location` is a single `jmp` to libkernel's `__error`, and `bits/errno.h` is
FreeBSD's table - the one those syscalls actually set. `EINPROGRESS` is 36, `EAGAIN` 35,
`ECONNREFUSED` 61. So `isagain()` and `isinprogress()` in `net_compat.h` read the right values with
no translation, and a `sceNet*` wrapper would have had to invent a mapping to replace something
that was already correct.

⚠ **SO DO NOT OVERRIDE `socket()`.** The two-namespace hazard the old section warned about is real,
but it is what the override would CREATE, not what it would fix: `sceNetSocket()` returning an
`OrbisNetId` into musl's `getaddrinfo`, which then calls libkernel's `connect()` and `close()` on
it. And `close()` cannot be overridden anyway - it is the file close as well as the socket close, so
an override would have to dispatch on descriptor type. There is nothing to gain and a working layer
to lose.

**What this platform DID need**, all of it landed in phase 5a:

* `network_init()` in `libretro-common/net/net_compat.c` had no ORBIS arm and fell through to the
  generic one, which only ignores `SIGPIPE`. `sceNetInit()` has to run first: musl's resolver reads
  the console's DNS servers through `sceNetGetDnsInfo` (`resolvconf.lo`), which answers with an
  error until the stack is up. Name resolution failing on a console that is plainly online is what
  that omission looks like. The return code is deliberately not fatal - it is negative when the
  stack is ALREADY up, which is the normal case here because `optional/orbis_netlog.cpp` calls
  `sceNetInit()` during early boot and then sends datagrams successfully on hardware.
* `HAVE_SOCKET_LEGACY` had to go from 1 to 0. Inherited from the console Makefiles this port was
  modelled on and inert while networking was off; with it set, the build stops on `redefinition of
  'addrinfo'`, because `net_compat.h` declares its own for platforms that have no `getaddrinfo` and
  this one has a real one in `netdb.h`.
* `DEFAULT_BUILDBOT_SERVER_URL` had no ORBIS arm. ORBIS is not `__linux__` and not any other case in
  that chain, so it fell to the final `""` - a Core Downloader that fetches nothing and explains
  nothing, and an invitation to "fix" it by borrowing the Linux/x86_64 URL, which would fill the
  list with x86-64 ELF objects that install and never load.

⚠ **`-lSceNet` IS NOW LOAD-BEARING FOR libc, NOT JUST FOR THE LOG CHANNEL.** `sceNetGetDnsInfo` is
the single symbol the whole socket API needs from libSceNet; `sceNetInit` is the second, from our
own arm. Removing `-lSceNet` because logging is off in a release build fails at link time.

⚠ **THERE IS NO `HAVE_NETPLAY` SWITCH IN THIS TREE.** `Makefile.common`'s `HAVE_NETWORKING` block
adds `network/natt.o`, `network/netplay/netplay_frontend.o`, `netplay_room_parse.o`, the three
`tasks/task_netplay_*.o` and `-DHAVE_NETWORK_CMD` unconditionally, and `runloop.c` defines
`core_set_netplay_callbacks` behind `HAVE_NETWORKING` alone. All seven compile clean for this
target and are dead code unless a session is started, so they are carried. Removing them means an
ORBIS `#ifdef` in `retroarch.c`.

**Timeouts need no work.** `socket_connect_with_timeout()` is non-blocking `connect` + `poll` +
`getsockopt(SO_ERROR)`, and `NETWORK_HAVE_POLL` is set for this platform because it takes the
generic POSIX branch. `poll` is a real libkernel export. There is no `sceNetSelect` and the
`sceNetEpoll*` family is declared in this SDK as `void sceNetEpollWait();` - names without
signatures - and neither fact matters, because nothing calls them.

**For TLS (5b):** `time()`, `getrandom` and `getentropy` are all present in libc. BearSSL and
mbedTLS are vendored under `deps/`; `HAVE_BUILTINBEARSSL := 1` in `Makefile.orbis` is the whole
switch, and it sets `HAVE_SSL` itself. Sony's `libSceSsl.so` and `libSceHttp.so` are present as
stubs and `orbis/Ssl.h` declares `void sceSslConnect();` - names without signatures - but
`samples/net_http/` shows the working call sequence for `sceSslInit`/`sceHttpInit` and could be
used to recover them if BearSSL's certificate story turns out worse than it looks.

## Distribution: mesa as a release, and where orbis-compat sits

The full plan for building and publishing all of this from GitHub Actions lives outside this file.
Two findings from it belong here because they are facts about the tree.

**A Mesa bundle is 87 MB and 15 MB compressed** - `libvulkan_radeon.a` 35 MB, `libgallium-*.a` 41 MB,
`libEGL.a` 5.5 MB, the GLES dispatch 228 KB, `include/` 5.5 MB. Building Mesa in every consumer's
pipeline pays for the same work repeatedly; it is the one input both expensive and slow-changing.

⚠ **AND MESA IS LINK-TIME COUPLED TO orbis-compat, not merely compile-time.** Its archives carry
undefined references only the overlay satisfies:

    libvulkan_radeon.a  ->  clock_gettime fstat open unlink pthread_create
                            orbis_sysconf  _Znam _Znwm _ZnwmRKSt9nothrow_t

`orbis_sysconf` is the tell: orbis-compat's `unistd.h` defines `sysconf` as a macro renaming it, so
every Mesa object that asks how many CPUs the machine has imports a symbol that exists nowhere else.

That settles two questions that will be asked again. **The direction cannot invert** - orbis-compat is
the base layer and Mesa its consumer, so publishing Mesa out of orbis-compat's repository would have
the base release the thing built on top of it, and every overlay change would drag a full Mesa build
behind it. **And the overlay does not belong inside the Mesa tarball** - those references resolve at
the FINAL link where `-lorbis-compat` is already on the line, and bundling a copy would freeze the
layer that changes most often inside the artifact meant to change least.

The proportionate check is the import list itself: assert that every symbol Mesa's archives import
from orbis-compat is still defined by the orbis-compat about to be linked. ⚠ It catches presence, not
meaning - a changed return value or a constant inlined at Mesa's compile time leaves nothing to check.

## The release pipeline, and the three things the plan got wrong about it

Phases 01-04 of the release plan are now in the tree: `.github/actions/orbis-toolchain/action.yml`,
`.github/workflows/frontend.yml`, `.github/workflows/cores.yml`, `ps4/shard-cores.sh`,
`ps4/make-index.sh`, disk hygiene in `ps4/build-cores.sh`, and a release workflow in mesa-ps4.
Nothing has run on a runner. What follows is only the part that was measured rather than written.

**The index CRC is of the `.prx`, not of the `.zip`.** The plan said to hash the archive that gets
uploaded. `tasks/task_core_updater.c:832-853` hashes `download_handle->local_core_path` and compares
it to `entry->crc`, and `local_core_path` is the *extracted* module -
`core_updater_list.c:466-471` strips the archive extension. Publish the archive's CRC and the
comparison never matches: nothing errors, and every core re-downloads on every visit to the
downloader, forever. The plan's *reasoning* survives intact and is why the index is cut inside the
shard that built the core - `create-fself` is not byte-reproducible, so a CRC computed from a later
rebuild of identical objects is a different number.

Two more parser facts worth not re-deriving. The CRC goes through `string_hex_to_unsigned`
(`libretro-common/string/stdstring.c:624-646`), which returns **0 on any parse failure**, and
`core_updater_list.c:367-379` treats 0 as a rejected line - a malformed CRC silently removes a core
from the list rather than reporting anything. And the date is `strtoul`-walked and must end at NUL
(`core_updater_list.c:331-362`), so a trailing space drops the line too.

**The `.info` files cannot ship inside the package the way phase 02 wanted.** Package contents mount
at `/app0`, which `frontend/drivers/platform_orbis.c:72-77` already documents as read-only, and the
same file sets `DEFAULT_DIR_CORE_INFO` to `/data/retroarch/info`. RetroArch will not look in
`/app0`. `make-pkg.sh` does have `--extra <src>:<targ>`, but `Makefile.orbis`'s `pkg` target passes
none. Making a fresh install legible therefore needs a first-boot copy out of `/app0/info`, or
`CORE_INFO_PATH` moved to `EBOOT_PATH` - not a packaging step. Left as a comment in `frontend.yml`
rather than a step that silently copies nothing.

**Two runner-environment dependencies nobody had written down.** `PkgTool.Core` dlopens
`libssl.so.1.1` (confirmed in its `strings`) and a GitHub runner has OpenSSL 3, so the frontend
workflow fetches the focal `libssl1.1` deb. And mesa-ps4's `build-support/orbis/build.sh` refuses to
run without `nix` and wraps every meson/ninja call in `nix develop nixpkgs#mesa`; it also runs
`ninja -k 0 ... || true`, so it exits 0 on a failed build and the release workflow has to assert the
five archives itself rather than trust the exit status.

**Still open, and both are cheap.** ⚠ Spike S1 - whether a Release asset keeps a leading dot - is
unanswered, and it decides Releases against Pages as the host. `cores.yml` answers it on its first
real run by re-fetching `.index-extended` from the release it just cut. ⚠ And the OpenOrbis SDK
release asset name could not be established: the copy at `~/.local/opt/openorbis` has a changelog
topping out at v0.5.2 (2021) but a `create-fself` dated 2026-01-04, so it is *not* that release
asset, and pinning `v0.5.2` would build against a 2021 SDK. Publishing a known-good copy under
`orbis-ports` is the honest fix.

Measured: 101 cores, 461 MB, 109 MiB zipped; 101/101 modules carry `retro_run`; all 101 index CRCs
cross-checked against `7z h` and Info-ZIP.

## Where the cores are hosted, and why not GitHub

Measured, not chosen on taste. **A GitHub Release asset cannot be named `.index-extended`.** Uploading
that exact filename to a throwaway release stored it as `default.index-extended`; the dotted name
returns 404 and the renamed one returns 200. The filename is a hardcoded string literal at
`tasks/task_core_updater.c:389`, joined onto the base URL - so hosting on Releases would have meant
patching the client with an `#ifdef` and diverging from upstream over a hosting quirk.

**Cloudflare R2 keeps the key verbatim.** Same test against the bucket: `PUT .index-extended`,
`GET /.index-extended`, 200, exact name. No client patch. The bucket is `orbis-cores` on account
`dde701a9fad0ed4ac032e5bfbbae1b56`.

**The domain is what makes it usable before TLS exists.** The bucket's own `pub-*.r2.dev` hostname
answers plain http with a 301 to https. `net_http.c:2329` follows redirects, so it would walk
straight into a handshake that `HAVE_SSL=0` cannot complete - a silently empty Core Downloader. A
custom domain does not redirect: `prx0.com` was registered, `cores.prx0.com` attached to the bucket,
and plain `http://cores.prx0.com/.index-extended` returns 200 with no redirect. ⚠ That is the only
reason the domain exists, and it is what takes phase 5b off the critical path - BearSSL is still
wanted, but nothing waits on it now. `config.def.h`'s ORBIS arm points there.

Ownership verification takes a few minutes and reports `error code: 1014` on http until it clears;
`wrangler r2 bucket domain list orbis-cores` shows `ownership_status` going pending → active.

**CI credentials, and why these names.** `wrangler` rather than rclone/aws-cli, so no S3 access keys
exist to leak; the R2 API token is scoped `Workers R2 Storage: Edit`. `CLOUDFLARE_API_TOKEN` and
`CLOUDFLARE_ACCOUNT_ID` are org secrets, the token restricted to this repository - the org is mostly
upstream forks, and a fork with an enabled workflow is the cheapest way for a secret to reach a log.
`R2_CORES_BUCKET` and `CORES_BASE_URL` are org *variables*, not secrets: they are not sensitive, and
as variables they appear in the log, which shortens "where did that URL come from" to one glance.

**Upload ordering is load-bearing.** `.index-extended` goes last, after every zip it names - a
console reading the index between the two states downloads a core that is not there yet. Pruning
runs after the new index is live, and the only inventory available is the *previous* index, because
wrangler has no `r2 object list` and no S3 keys exist. An object no index ever named is invisible to
the job and needs a manual sweep.

## The pipeline runs: what four red runs taught, and what is live

`http://cores.prx0.com/.index-extended` serves 101 lines over plain HTTP, no redirect. All 101
archives return 200. The first core's `.prx`, unzipped, hashes to exactly the CRC its index line
claims. That is every step the console performs except the console.

**Coverage is honest and it is not 163.** The recipe has 164 cores, 163 were attempted, **101 are in
the index**. Those 101 are the same set this machine has built by hand - CI introduces no difference
of its own, which is the most useful thing the first green run said. The other 63 now carry a
recorded reason: `bsnes` `fbneo` `desmume` `dosbox_svn` `bluemsx` fail to LINK on missing symbols;
`bsnes_hd_beta` wants `GOMP_parallel`, so OpenMP; `chailove` `geolith` `daphne` want PHYSFS, zlib and
SDL; `bsnes2014` and `freej2me` link *without* `retro_run` and are quarantined as NO-ABI; `citra` and
`blastem` produce no objects at all, so they build differently than the harness assumes.

**Four failures, and only one was environmental.**

⚠ *The runner is reclaimed sometimes.* Shard 0 ran 59 minutes and died with `The runner has received
a shutdown signal`. `kronos` had spent 57 of those minutes and its recorded `COMPILE` verdict is an
artifact - `trap ... EXIT INT TERM` does not stop bash, so the handler ran, deleted the clone, and
execution *resumed* to write a row about a core that had already been killed. Anyone chasing
`glsym/rglgen_private` should get a clean run first. The fix is a 25-minute per-core cap, chosen
from measurement: the slowest *successful* core anywhere is `mednafen_saturn` at 831 s. It must not
use `timeout --foreground` - tested, that leaves three orphaned `clang++` behind; without it, none.

⚠ *One core failing must never block 162.* `publish` has `needs: shard`, so a red shard sank the
whole release. Per-core outcomes are data now; a shard fails only if *zero* cores build.

⚠ *The leading dot bites a third time.* `actions/upload-artifact` has excluded hidden files by
default since v4.4, so `path: dist/.index-extended` matched NOTHING - on a file verified one step
earlier. `include-hidden-files: true`. Releases rename it, upload-artifact hides it, R2 keeps it.

⚠ *A non-empty secret is not a working one.* The first R2 attempt produced 101 consecutive
`"code":10000, "Authentication error"` with zero successes, after nine minutes of uploading, because
the credentials step only checked that the variables were set. ⚠ **AND THE CAUSE WAS THE BUCKET
SCOPING.** An R2-page token with *Apply to specific buckets only* authenticates the S3 endpoint, not
the REST path `wrangler r2 object put` uses. What works: a Custom Token with
`Account · Workers R2 Storage · Edit`, account-scoped, no bucket restriction. A one-object probe now
proves the token in three seconds before anything else runs.

And one of ours: the probe's own `echo` was committed with an unclosed quote, which YAML validation
cannot see because to YAML it is a perfectly good string. Every `run:` block now goes through
`bash -n` before commit.

Live: `orbis-mesa-c42aa135f234` (16.4 MB, past the link probe), the `cores` Release as the archival
copy (103 assets, where GitHub stores the index as `default.index-extended`), and the R2 bucket the
console actually reads. The frontend `.pkg` is still an artifact, not a Release - that needs a tag.

## RetroArchV, and why the title id had to move with the name

The package is now `RetroArchV`, title id `RTRV00001`, content label `RETROARCHV000000`.

⚠ The console decides collisions by **title id**, never by the name on screen. A second package
carrying `RTRA00001` would not have appeared beside an existing RetroArch install - the installer
would have treated it as an update and replaced it, silently, with something built by strangers.
Renaming the title alone would have left that exactly as dangerous while looking solved. Both moved
together, so the two are different applications as far as the system is concerned and either can be
removed without touching the other.

`ps4/icon0.png` is 512x512 and opaque on purpose: the system draws icon0 as a square, so the rounded
corners in `media/ico_src/icon.svg` would have appeared as transparent notches. It is the invader
from `media/retroarch-vector_invader-only.svg` - cropped to its alpha bounding box, because the
source has ~30% padding inside its viewBox and scaling the box rather than the glyph produced a
small mark floating in a large field - recoloured white over our own gradient, with a chevron mark.
Verified by content rather than by build success: the icon's bytes appear verbatim inside the
`.pkg`, alongside one `RetroArchV` and four `RTRV00001`.

Both workflow steps that used to spell `IV0000-RTRA00001_00-RETROARCH0000000.pkg` now glob
`IV0000-*.pkg` and assert exactly one. They were already stale when this landed - a hardcoded name
in CI for a value Makefile.orbis owns is a green run that copies a file which no longer exists.

## Networking works, and three defects of one family stood in the way

The Core Downloader lists 101 cores from `http://cores.prx0.com/`, downloads one, extracts it and
runs it. Databases update too - a 52 MB transfer. All of it measured on hardware.

⚠ **Every step of the diagnosis that reasoned from symbol tables was wrong, and every step that
measured was right.** Phase 5a concluded no wrapper was needed because `libkernel.so` exports all
22 POSIX socket calls. That is true and it is not the question. Presence is not permission, and the
two are indistinguishable at link time.

**1. The socket cannot be made non-blocking by any POSIX route.** Probed:

    fcntl(F_GETFL)  =  2   errno=0     reading flags is allowed
    fcntl(F_SETFL)  = -1   errno=13    EACCES
    ioctl(FIONBIO)  = -1   errno=13    EACCES
    connect()       =  0   errno=0     a blocking connect succeeds

`socket_connect_with_timeout()` calls `socket_nonblock()` FIRST and returns on failure without
reaching `connect()`, so `net_http` reported `socket_connect_failed` on a connect that never
happened. ⚠ That misattribution cost most of an afternoon: it reads as a host problem, then as a
process with no network authority, and it is neither. `sceNetSetsockopt(SO_NBIO)` works - **applied
to the descriptor musl's `socket()` returned**, which is also the experimental proof that the two
APIs share one descriptor namespace. The fix is an ORBIS arm in `socket_set_block()`.

**2. A downloaded core arrives without the execute bit.** The zip extracts to 0666, every `.prx`
that has ever loaded here is 0777, and `sceKernelLoadStartModule` will not open it. The download
reported success, the CRC matched, the file was byte-correct, and the core would not load. Nothing
server-side was wrong, so no assertion in the pipeline could have caught it. `chmod(0777)` in
`CORE_UPDATER_DOWNLOAD_END` - 0777 and not 0755 because that is the mode of the cores that already
work, under a different uid than the downloader writes as.

**3. musl's resolver cannot run here, and repairing its flags would not fix it.**
`sceNetGetDnsInfo()` reports a working nameserver and `getaddrinfo()` still answers -11 with EACCES:
`__res_msend` opens its query socket as `SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK`, and this SDK
defines those as 02000000 and 04000 - **Linux's values on a FreeBSD kernel**, which did not accept
them in `socket()` until FreeBSD 10. ⚠ And overriding `socket()` would not be enough: probed, the
flagged call returns a valid descriptor AND leaves errno at 13, because musl retries plain, tries
`fcntl(O_NONBLOCK)`, is refused again, and hands the descriptor back anyway - its resolver then runs
on a blocking socket it believes is non-blocking. `sceNetPoolCreate()` + `sceNetResolverStartNtoa()`
resolved in 73 ms first try. ⚠ The pool is the part the first attempt missed;
`sceNetResolverCreate(memid=0)` fails with 0x80410109.

That is three defects of one family in one day, with the libc++ `ETIMEDOUT` and the `LINUX_FIONBIO`
in `bits/ioctl.h`: **a musl and a libc++ built against Linux constants, shipped for a FreeBSD
target.** All of them compile, none of them warn.

**Instruments, kept.** `ps4/orbis_net_probe.c` runs from `frontend_orbis_get_env()` under
`-DORBIS_NET_TRACE` and answers "which API may this process actually use" on hardware. The same flag
un-gates `net_http_log_transport_state()`, whose stage names (`dns_lookup_failed`,
`socket_connect_failed`, `socket_send_failed`) are what turned each of these from a guess into a
measurement. ⚠ It logs through `ps4_log()`, not `RARCH_LOG` - the probe runs before RetroArch's
logger exists, and one build printed nothing at all for that reason.

⚠ **And `ps4_log()` writes klog ONLY while netlog is down** (`klogWanted()` in ps4_app.cpp is
`s_frameKlog || orbis_netlog_ready()==0`). A klog capture alone shows the first few lines of a boot
and then goes quiet. Capture UDP.

⚠ **This console's FTP does not return binaries faithfully.** The same 749,552-byte `.prx` came back
as 1,104,568 bytes through both lftp and curl, and it is not LF-to-CRLF expansion - the file holds
907 bytes of 0x0A. Uploads are fine; a 63 MB package installs and runs. Any forensic based on a file
pulled off this console is based on a corrupted copy.

## TLS, and the one line that made it impossible

HTTPS works, with certificate validation, verified on hardware against the same R2 bucket over
both schemes so TLS was the only variable.

⚠ **The blocker was never the crypto.** BearSSL is vendored at `deps/bearssl-0.6`, the backend is
written, and both things TLS needs from the platform - `time()` for validity windows,
`getrandom`/`getentropy` to seed - were present all along. What stopped it was
`net_socket_ssl_bear.c` reading its trust anchors from one hardcoded path,
`/etc/ssl/certs/ca-certificates.crt`, which is a Linux distribution's layout. On a console that
file does not exist, so BearSSL initialised with **zero** anchors and every handshake failed
validation.

⚠ And worse than "no TLS": `filestream_read_file` leaves its out-pointer NULL on failure and the
old code handed that straight to `append_certs_pem_x509()`, whose first act is `strstr(NULL, ...)`.
Turning on `HAVE_SSL` without fixing that would have made every https URL a crash rather than a
failed connection. A missing END marker in a truncated bundle had the same shape.

The path list is ordered `/data/retroarch/cacert.pem` then `/app0/cacert.pem`: the writable copy
wins so roots can be refreshed without a new package - a CA expiring is not a reason to reinstall -
and `/app0` is read-only but perfectly *readable*, so the shipped bundle needs no first-boot copy.
`ps4/cacert.pem` is 121 anchors, 185 KB, checked with `openssl s_client` against all three hosts
this port uses. It reaches the package through the first `--extra` this project has ever passed;
confirmed on hardware at `/mnt/sandbox/RTRV00001_000/app0/cacert.pem`, 185311 bytes.

`net_socket_ssl.h` also used `ssize_t` without declaring the dependency - transitive everywhere
else, absent here.

⚠ **Do not enable "Always Use HTTPS" on cores.prx0.com.** v0.1.1 and earlier have no TLS, and a
redirect they cannot follow turns their Core Downloader into a silent empty list. Plain http must
keep answering until those packages are gone. This is also why the CRC in `.index-extended` never
authenticated anything by itself: it arrives over the same channel as the file it describes.

## The Online Updater, and a 71 MB download that killed the process

Update Assets crashed the console at the end of the transfer - black screen, no abort report,
Mesa's log ending mid-frame-statistics with no error. Update Databases, 52 MB, had worked minutes
earlier.

⚠ **The first explanation was wrong and worth recording as such.** `net_http` grows its response
buffer by doubling, so a 71 MB body looked like it must pass through a 64 MB → 128 MB realloc
with both alive at once. It does not: `net_http.c:1556` sizes the buffer from `Content-Length`
in one allocation as soon as the headers land. The doubling only applies to a chunked response.
The theory was tidy, matched "at 99%", and was false.

What is true is simpler. The whole body is held in RAM until the transfer completes, then
`cb_generic_download()` writes it out and starts a decompress task - so the peak is 71 MB of
buffer, plus the write, plus inflating **7040 files and 85 MB** while the buffer is still alive.
52 MB survived that and 71 MB did not.

**The fix had been sitting in the tree with no caller.** `task_push_http_download_file()` streams
the body to a path as it arrives, so the peak is the receive window rather than the payload -
`task_push_http_transfer_file()`, which every updater entry used, passes `sink_path = NULL`.
Six downloads now take the streaming path: assets, core info, databases, overlays, cheats and
core system files. Confirmed on hardware: 6995 files extracted, all nine XMB themes, no crash.

⚠ **Only the enums whose destination is a plain settings directory.** The sink path must equal the
`output_path` the callback would have computed, and thumbnails, Content Downloader items,
autoconfig profiles and shader packs all derive a subdirectory that does not exist at push time -
from a playlist, a category, the joypad driver name. Those keep the in-memory path.
`download_stream_dir()` returns NULL for them and the old push runs, and a directory that cannot
be created also falls back rather than failing.

**Two other updater findings from the same session.** ⚠ `HAVE_UPDATE_CORE_INFO` and
`HAVE_UPDATE_ASSETS` were absent from Makefile.orbis on a reason that had expired - the comment
said the package ships the .info files, which it never did and cannot, because package contents
mount read-only at /app0 while RetroArch reads /data/retroarch/info. Measured: `/app0/assets` in
the installed package holds ZERO files. So the release notes and cores.prx0.com were telling a new
user to click a menu entry the build did not contain.

⚠ And the video driver defaulted to `gl`. `configuration.c` tests `HAVE_OPENGL || HAVE_OPENGLES`
before Vulkan, and this port builds with GLES for the sake of cores that need a GL context - so a
fresh install came up on zink, translating to the Vulkan that RADV was going to be handed anyway.
An ORBIS arm now selects Vulkan. It costs GL cores nothing: `video_driver_find_driver()` forces
the driver to match a core's hardware context and remembers the previous one.

## Why mupen64plus_next was never in a release, and what it says about every other core

It builds here and it has never once built in CI. Two causes, both of them a dependency this
machine happened to have and nobody had written down.

⚠ **clang falls back to /usr/include, and -isysroot does not stop it.** GLES3/gl3.h and its EGL
neighbours live in the Mesa tree - `$(ORBIS_MESA_SRC)/include`, the path Makefile.orbis:392 gives
the frontend - and `ps4/build-cores.sh` never passed it to cores. Measured directly:

    clang --target=x86_64-pc-freebsd12-elf -isysroot $TOOLCHAIN ... -E
      -> # 1 "/usr/include/GLES3/gl3.h"

So every GL core built on this machine has been compiled against **this Linux desktop's** GLES
headers. It works, because they are Khronos's and near-identical - an accident that holds. A
runner has no libgles-dev, nothing to fall back to, and the core fails with
`'GLES3/gl3.h' file not found`. ⚠ CI was right and the development machine was wrong, which is the
opposite of how this reads at first: seven objects short, then `undefined symbol: glsm_ctl` at the
link, and glsm.o was simply one of the files that never compiled.

⚠ **And `nasm` is not on a GitHub runner.** mupen's x86_64 dynarec assembles
`new_dynarec/x64/linkage_x64.o` with it - `make: nasm: No such file or directory`, Error 127.
`make` runs with `-k`, so both failures surfaced only as a missing symbol much later.

**The instruments this took, and why they were worth more than the fix.** Four attempts learned
nothing because the evidence kept being thrown away: a diagnostic step that reddened healthy
shards (GitHub runs `run:` under `bash -e`, and testing for a file that is absent *because nothing
failed* returns 1); an artifact with `if-no-files-found: error`, which discards the logs in the one
case where the logs are all there is; a selection keyed on a manifest that `build-cores.sh` never
writes when zero cores build - the single-core case exactly; and `WORK`, which is a step-level
variable, not a job-level one. Each was correct for the path its author had in mind and silent on
the path being walked, which is the same shape as every defect in this port.

⚠ **`cores.yml` now takes a `cores` input** - space-separated names, one shard, publishes nothing.
Chasing this cost two full 163-core runs before that existed. Publishing is blocked for a subset on
purpose: the prune step deletes bucket objects the new index does not name, so a two-core
diagnostic run would have taken the other ninety-nine down with it.

**Left open.** The host-header fallback is not specific to mupen: any core reaching for a header
the SDK and overlay lack will silently take this machine's. `-nostdsysteminc` would close it and
has not been tried.

## Open on hardware after v0.1.4: a save state, an audio port, and two PSX cores

**PrBoom crashes seconds after Load State, and the fault is inside the core.** Symbolized from a
console klog dump against a local build of the same pinned commit:

    signal 10 (SIGBUS), general protection fault
    rip 0x8009acf98 -> /data/retroarch/cores/prboom_libretro.prx +0x154f98
                    -> Z_Malloc +0x1d8

The zone allocator walking a corrupted free list. Not the frontend, not Mesa, not the platform.
⚠ The state loaded at 09:55:44 and the fault came at 09:55:47 - the load "succeeded" and the heap
was already wrong. The state file was **351640 bytes while the core's current
retro_serialize_size() reported 198200**: prboom sizes its state from live thinker, sector and
line counts, so it changes with the situation. ⚠ And the core declares no
RETRO_SERIALIZATION_QUIRK_CORE_VARIABLE_SIZE anywhere, so the frontend is never told - which also
means rewind, run-ahead and netplay are being driven on an assumption that does not hold.
retro_unserialize does bound its read by `size`, so it is not a naive overrun. **Not yet
established** whether this reproduces off this platform; the A/B test that settles it is save and
load at the same moment of play, where the two sizes agree.

⚠ **Audio died on core switches, and the driver was not leaking - the system keeps the port.**
FIXED. `sceAudioOutOpen failed: 0x80260005` is ORBIS_AUDIO_OUT_ERROR_PORT_FULL. The obvious
reading is a missing close, and it was wrong. Instrumented and measured across eight switches:

    eight opens, eight closes, EVERY close returning rc 0x00000000
    handles counted down 0x20000007, 0x20000006 ... 0x20000000, never reissued
    the ninth open failed

So `sceAudioOutClose` reports success and the port stays spent. Eight per process, then silence.
⚠ The fix is therefore not to close better but to stop reopening: `ps4_audio_init()` opens the
MAIN port ONCE per process and hands the same handle to every later init, and `ps4_audio_free()`
does not close it. Nothing is lost by holding it - the parameters are compile-time constants
(PS4_AUDIO_RATE, PS4_AUDIO_GRAIN, S16 stereo) and RetroArch is told the rate through *new_rate
and resamples - and a port that cannot be reused is worth nothing returned. Confirmed on hardware
over a dozen-plus core switches with sound throughout.

⚠ **The measurement overturned the hypothesis rather than confirming it.** Had the close been
"fixed" without instrumenting first, the change would have added a close that was already there
and the search would have gone on.

⚠ **Two Beetle PSX cores shipped and only one was ours. The stock one is now withheld.**
`mednafen_psx_hw` is the fork at `b0b759e`, ten commits ahead of upstream, with the ORBIS platform
arm, `orbis_lightrec_mem.c` and the Vulkan renderer - PS4_CORE_FORKS protects it from being
overwritten. `mednafen_psx` was built by an ordinary shard straight from upstream at `ef51860`:
`HAVE_LIGHTREC=1` in its Makefile and none of the port's work behind it, so it has no executable
code buffer, falls back to the MIPS interpreter, and runs at the ~38% of realtime this file
measured on Spyro 3 - beside a fork that holds full speed, under a name one letter apart from it.

The fix is a withdrawal, not a port. **Folding it into the fork was the other option and it buys
nothing**: the fork already IS upstream plus the platform work, and `_hw` is the same core with
the hardware renderer available. A second entry can only be the same emulator configured worse.

`PS4_CORE_DROP` in `ps4/build-cores.sh` withholds it. ⚠ **It is a separate mechanism from
`PS4_CORE_FORKS` because it answers a different question.** FORKS is about not overwriting a file;
DROP is about what the Core Downloader offers. A core reaches the menu by being in the index and a
user picks it by name, so every name in that list reads as a recommendation - and this is the only
list a console owner sees. Everything else missing from the index failed to build; this one builds
and is held back, which is why the verdict is `DROP` rather than a failure class.

⚠ **Withholding does not remove what is already published.** The publish job prunes bucket objects
the new index does not name, so `mednafen_psx_libretro.zip` goes on the next FULL run - a subset
run publishes nothing and prunes nothing. Until then the object is still fetchable by URL; it is
simply no longer named by anything a console reads.

**The other PlayStation options, measured rather than assumed.** `pcsx_rearmed` fails at
LINK on `lightrec_init_mmap` - the same executable-memory problem this port already solved for
Beetle, so it is the cheapest of these to try. `duckstation` and `swanstation` are CMAKE in the
recipe and the harness skips that build type for want of a toolchain file; they have never been
attempted. PS2 is `play` and `pcsx2`, also CMAKE - and beyond the missing infrastructure, PCSX2
wants an order of magnitude more CPU than this machine has, so treat it as arithmetic rather than
porting.

## 2026-08-28 — one core option, and the console had to be recovered from outside

`GPU shader depth write` (`EnableFragmentDepthWrite`) was suggested in the entry above as the
free experiment that would settle candidate 3 without a rebuild. **It is not free. Turning it on
hangs the core before its first frame and then takes the whole console down**, and it is now
clamped off on this platform and removed from the menu
(`ps4/core-patches/mupen64plus_next/0007`).

⚠ **THE SUGGESTION WAS MINE AND THE COST LANDED ON THE USER'S CONSOLE.** The reasoning behind it
was sound - the option is exactly the other half of a pair this build splits - and the reasoning
said nothing about what the option costs to *try*. A core option is not automatically a cheap
experiment on a machine with no way to interrupt a wedged process.

### What the log says, and it is not "lag"

Same ROM, same build, same session, thirty minutes apart. `ps4-klog-20260828-092942.log`:

    11:35:58.626  mupen64plus: Init new dynarec          option False
    11:36:00.426  [Audio] sinc resampler active path: float
    11:36:07.626  profile: 650 frames in 8972 ms = 72.44 fps

    12:06:45.669  mupen64plus: Init new dynarec          option True
    12:06:55.469  [WARN] [PS4] audio: 1772 underruns in 1875 grains
    12:07:05..35  1875 underruns per 1875 grains, four windows running
    12:07:41.070  [ScePthread/System] Internal Memory is running out.   x10851

**Nothing at all between `Init new dynarec` and the pthread pool giving out** - no first frame, no
profile window (and the profiler was on, `/data/retroarch-profile` exists and it reports every five
seconds *of frames*), 100% audio underruns throughout. Every line before that point is identical
between the two runs, down to EGL, the zink renderer string and `133 entry points resolved, 0
missing`. So this is not a slow frame: **the core never completes one**, and something between
dynarec init and the first `retro_run` allocates pthread objects without bound.

⚠ **THE EXHAUSTION IS SYSTEM-WIDE, WHICH IS WHY THE CONSOLE DID NOT COME BACK.**
`[ScePthread/System]` is the system's pool, not this process's heap - technote 235. Once it is
gone the shell cannot get itself back either: the PS button did not return to Orbis, and Close
Application from outside wedged the machine rather than freeing it. **A core option that can do
that must not be reachable from a menu**, whatever it is worth when it works.

### Where the clamp is, and why not at the option

`custom/GLideN64/mupenplus/Config_mupenplus.cpp`, immediately before `config.validate()`, which is
after every writer. ⚠ **Removing the core option alone would not have been enough:**
`LoadCustomSettings()` parses `generalEmulation\enableFragmentDepthWrite` out of GLideN64's
per-game ini, and with `GLideN64IniBehaviour == 0` that runs *last*. The core option is removed as
well, so nothing offers the switch, but the clamp is what makes it safe.

⚠ The removal produces one new line per content load, and it is not a fault:

    [ERROR] [Environ] GET_VARIABLE: mupen64plus-EnableFragmentDepthWrite - Invalid value.

`EnableN64DepthCompare` and `EnableShadersStorage` have printed exactly that on every GLES build
since the port began - upstream compiles those two out under `HAVE_OPENGLES` and asks for them
anyway. This is the third.

### What it does NOT settle

**Candidate 3 from the entry above is still open**, and now it cannot be tested the cheap way. The
`/8` that `enableClipping` puts in the vertex shader is still never scaled back on this platform;
the option that would scale it back is the one that hangs. If that pairing has to be tested, the
route is a build with `enableClipping` forced to 0 instead - which costs a core build and leaves
the software clipper doing the work it was always doing with framebuffer emulation on.

**And the cause of the hang is unknown.** Writing `gl_FragDepth` defeats early-Z, which is a frame
rate story, not a hang. The shape to chase is what creates pthread objects per shader variant when
every fragment program suddenly writes depth - zink compiling a program set it has never built, on
a driver where `orbis-compat` reports six CPUs to Mesa's `util_queue`. Nobody has looked.

⚠ **AND THE PUBLISHED CORE STILL HAS THE OPTION.** `mupen64plus_next` in the index was built before
this patch, so every installed copy can still be walked into this. It is fixed by the next cores
run, not by anything already shipped.

## 2026-08-28 — the depth bug reproduced on a laptop, and it is not this port's driver

The framebuffer-emulation depth fault is **confirmed, off the console, in one run**, and the cause
is candidate 1 from two entries above. `ps4/core-patches/mupen64plus_next/0008` fixes it.

### What the host says

mupen64plus-next built for Linux at the same commit `f275caf`, `FORCE_GLES3=1` so the flags are the
console's exactly (`-DEGL -DHAVE_OPENGLES -DHAVE_OPENGLES3 -DGLES3`), RetroArch from this branch
built `--enable-egl --enable-opengles --enable-opengles3`, and the same ROM - fetched over FTP and
checked against the md5 the console's own log printed. GLideN64 announces the same state it
announces on the console:

    GL 3.1 ES | depthTexture 1 weakBlit(GLES) 1 noPerspective 0 imageTextures 1
              | fbEmulation 1 fragDepthWrite 0 clipping 1 n64DepthCompare 0 copyDepthToRDRAM 2

and then, five thousand six hundred and seventy-two times in twenty-five seconds - **every frame**:

    depth attachment DROPPED for colour buffer 0000027f:
    depth texture 640 wide, colour 639 (m_width 320 vs 320)

⚠ **ONE PIXEL.** The colour texture is `m_width * m_scale` truncated, 639. The depth texture was
created before its colour buffer existed, took the *window* width instead, and is 640. GLideN64's
GLES branch tests `==`, so the depth buffer is thrown away and the FBO is rendered into with no
depth attachment at all. Every triangle passes, and the picture is ordered by draw order - which is
exactly "my ship draws behind everything, and the collisions are still right".

### ⚠ IT IS NOT ZINK, NOT RADV, NOT LIVERPOOL AND NOT OUR THUNKS

The same build on the host's **native radeonsi** - no zink in the process at all - drops just as
often:

    zink over RADV      26 000-32 000 drops / 25 s
    native radeonsi     26 000 drops / 25 s
    desktop-GL test     0 drops

So this is upstream GLideN64 on any GLES3 machine with framebuffer emulation on. Desktop GL never
sees it because the branch one line below tolerates a larger depth texture *or* an `m_width`
difference under two pixels - and here **both** clauses pass. That branch is the fix; `0008` takes
it on this platform and `depthWidthLoose=0` in the knob file puts upstream's back.

⚠ **AND THE FIX INTRODUCES NO NEW GL ERROR.** The worry was that a lenient size test would make
GLideN64's depth *blits* illegal, since ES3 will not scale a depth blit. Checked with `MESA_DEBUG=1`
over both variants: the same two error classes appear either way and no new one -
`GL_INVALID_FRAMEBUFFER_OPERATION` in `glReadPixels` and `glBlitFramebuffer`, plus three
`GL_INVALID_ENUM in glFramebufferTexture2D(unknown textarget 0x8d65)`. **0x8d65 is
`GL_TEXTURE_EXTERNAL_OES`** and those are a separate, pre-existing defect this port has never
noticed, present with and without the fix. Worth its own look; not this.

### The hang did NOT reproduce, and that was the right thing to doubt

`EnableFragmentDepthWrite=True` on the host runs. Sixty seconds, frames throughout, no thread
storm - where on the console the same option gives no first frame and then exhausts the system
pthread pool. So that fault is genuinely console-side: Liverpool, our Mesa build, or the console's
pthread pool, and the laptop cannot say which. **The user said so before the run and was right;
the run was still worth it, because it is what turned the depth fault into a fixed bug.**

### ⚠ Building this branch for the host found a defect of ours first

`libretro-common/dynamic/dylib.c`'s ORBIS constructor walker - `dylib_orbis_run_init_array` and
`dylib_orbis_forget` - was **outside every `#ifdef ORBIS`**, so it compiled on every platform and
the Linux build stopped on `sceKernelDlsym`. It has been that way since 2026-08-24 and only a
non-ORBIS build could see it. Fixed in `8cbea55517`. ⚠ Anything else this branch has added without a
guard is in the same position, and the host build is now the thing that would say so.

### ⚠ AND FTP DOES RETURN A DATA FILE FAITHFULLY

This file says "any forensic based on a file pulled off this console is based on a corrupted copy",
from a `.prx` that came back 900 KB larger. That reading was too broad: **a `.prx` is a signed
module and FTP hands it back decrypted**, which is a fact about self files, not about the transfer.
A 12 MB `.z64` pulled the same way came back at `c7b40352aad8d863d88d51672f9a0087`, the md5
mupen64plus itself printed on the console. Verify by content and the question does not arise.

### Where the workshop is

    ~/.cache/ps4-hostrepro/RetroArch      git worktree of this branch, configured for GLES3 + EGL
    ~/.cache/ps4-hostrepro/mupen-host     f275caf, FORCE_GLES3=1, with the instrumentation below
    ~/.cache/ps4-hostrepro/roms/sote.z64  md5-checked against the console
    ~/.cache/ps4-hostrepro/cfg/retroarch.cfg   video_driver gl, context x-egl, rgui, no audio

    MESA_GLES_VERSION_OVERRIDE=3.1   the console reports ES 3.1; without this the host offers 3.2
    MESA_LOADER_DRIVER_OVERRIDE=zink the console's stack; leave it out for the radeonsi control
    MESA_DEBUG=1                     GL errors on stderr, with no code change

⚠ **NOT in the session scratchpad and not in /tmp.** A cold reboot cleared tmpfs once and took
fifty-two built cores with it; `/tmp` here is tmpfs with the whole build in RAM besides.

The host clone carries three edits that are deliberately **not** patches in this repository, because
they are an instrument rather than a port: `LOG_LEVEL LOG_WARNING` in `GLideN64/src/Log.h`,
an `fprintf` in `attachDepthBuffer`'s drop branch, and one in `GLInfo::init` - the same two lines
`0006` reports through `orbis_report` on the console. `HOSTREPRO_DEPTH_LOOSE=1` selects the fix at
run time there.

### Confirmed on hardware, same day

The console now reports what the laptop reported, and reports it about a working picture:

    [orbis] gliden64: GL 3.1 ES | depthTexture 1 weakBlit(GLES) 1 noPerspective 0 imageTextures 1
                    | fbEmulation 0 fragDepthWrite 0 clipping 1 n64DepthCompare 0 copyDepthToRDRAM 2
    [orbis] gliden64: GL 3.1 ES | ... | fbEmulation 1 ...

    depth attachment DROPPED     ZERO, with framebuffer emulation on

Two lines because the first load still had the old workaround in `Mupen64Plus-Next.opt`
(`EnableFBEmulation = "False"`) and the second is after turning it back on. Shadows of the Empire
draws correctly with framebuffer emulation enabled - the ship in front of the bases, which is what
started this.

⚠ **The capability line is IDENTICAL on the two machines**, field for field, which is the retrospective
justification for the host reproduction: `depthTexture 1 weakBlit(GLES) 1 noPerspective 0
imageTextures 1 | fragDepthWrite 0 clipping 1 n64DepthCompare 0 copyDepthToRDRAM 2` on a GFX7
Liverpool under our Mesa and on a GFX11 Phoenix under Arch's. The fault was never in the half that
differs.

⚠ **AND THE PROOF CAME OFF /data/retroarch-abort.log, NOT OFF THE UDP OR klog CAPTURE.** The klog
receiver's connection died at 12:10 when the console wedged and nothing reconnected it, so the log
files on the workshop machine end there and read as if the console had stopped talking. `orbis_report`
writes klog *and* appends to that file, which is why the measurement survived a dead receiver. Check
the file before concluding a run produced nothing.

**Still to ship.** The core in the index was built before any of this. Nothing a user has installed
carries the fix until the next full cores run.

## 2026-08-28 — /data/OpenGothic is no longer every title's junk drawer

`orbis_paths.cpp:19` has been on the open list since 2026-08-22, first as "harmless" and then as
"it is not". Closed.

This process has no working directory - `getcwd` is ENOSYS and a relative `open` returns EINVAL,
not ENOENT - so orbis-compat interposes `open`, `stat`, `unlink` and `rename` and rewrites every
relative path under one root. That root was the string `/data/OpenGothic/` compiled into an overlay
that four titles now link. RetroArch has been creating that directory on every boot and opening
files inside it, because it does open relative paths - `Main Menu.png` among them.

**The root is now decided in three steps, and the choice is logged once:**

    1  orbis_set_anchor_root()          the application knows where its own data lives
    2  /data/<TITLEID>/                 from sceKernelGetAppInfo
    3  /data/orbis-compat/              neither of the above answered

⚠ **STEP 2 IS UNPROVEN ON THIS FIRMWARE AND IS WRITTEN TO SAY SO.** `sceKernelGetAppInfo` is
declared by the SDK, and this workshop has now been caught five times by a call that exists, links
and is refused at run time - `sceKernelGetCpuUsage` (ENOSYS), `fcntl(F_SETFL)` (EACCES),
`ioctl(FIONBIO)` (EACCES), `dup2` onto fd 2 (EPERM), `sceKernelQueryMemoryProtection` (answers about
a different thing). So the log line reports what it returned **whichever route wins**, and one boot
settles it without anyone arranging an experiment.

⚠ **AND THE TITLE ID IS VALIDATED BEFORE IT BECOMES A DIRECTORY NAME.** `OrbisAppInfo::TitleId` is a
fixed ten-byte field with no promise of a terminator. It is copied bounded, terminated here, and
rejected unless it is one to nine characters of A-Z0-9 - a field of garbage would otherwise become a
directory of garbage, and one containing '/' would become a directory somewhere else entirely.

**Both existing consumers name their own anchor**, which is why nothing moves underneath them:
RetroArch calls `orbis_set_anchor_root("/data/retroarch/")` in `frontend_orbis_init`, immediately
after the stderr capture and before the first file is opened; OpenGothic calls it with
`/data/OpenGothic/` as the first thing after `ps4_app_init`, so `save_slot_N.sav` stays where the
saves already are. ⚠ A call that arrives after the anchor has been decided **cannot** take effect -
the decision is made once because it sits inside `stat()` - and it logs that it was ignored rather
than pretending.

Confirmed by content, not by the build succeeding: `/data/OpenGothic` no longer appears anywhere in
`retroarch_orbis.elf`, and `/data/orbis-compat/`, `paths: title id is` and `orbis_set_anchor_root`
all do.

⚠ **The overlay's header had to grow a C guard.** `orbis_paths.h` included `<string>` at the top,
so a C consumer could not include it at all - and a frontend written in C is exactly the consumer
that needs to name its own anchor. The C++ half is behind `#ifdef __cplusplus` now; the setter is
`extern "C"` and was compiled from both C and C++ with the console toolchain before being believed.

orbis-compat `5f1e4e6`, OpenGothic `c0c202e2`.

## 2026-08-28 — the close-hang: two things believed about it are wrong

Not fixed. But it is narrowed further than it has ever been, and the narrowing came from the
maintainer's own account plus one log fetch - no experiment was needed to overturn either belief.

### ⚠ IT DOES NOT NEED A CORE LOADED

This file has said since 2026-08-22 that "Close Content followed by Quit exits properly, so the
condition involves a loaded core". **It does not.** Quit from the menu, dummy core, nothing loaded -
CE-34878-0 all the same, twice in the capture below. The "Close Content then Quit exits cleanly"
observation that produced that conclusion was made once and has not held up.

### ⚠ AND THE DRIVER TEARS ITSELF DOWN CLEANLY BEFORE IT HAPPENS

Mesa's log at exit - `/data/retroarch-mesa.log`, which is where `MESA_LOG_FILE` in
`/data/retroarch-env.txt` sends it - ends:

    wsi/orbis: scan-out down after 4703 flip(s)
    orbis-drm: at teardown 0 BO(s), 0 syncobj(s), 0 context(s), 0 VA range(s), 0 KiB held
               - winsys-lifetime, and it did not grow

Nothing leaked and the scan-out came down. The process dies **after** that.

⚠ **AND THIS FILE'S CLAIM THAT "NO CAPTURE FROM THIS CONSOLE HAD EVER CONTAINED `scan-out down`"
WAS ABOUT THE WRONG FILE.** Mesa writes to `MESA_LOG_FILE`, not to the UDP channel, so grepping a
UDP capture for `wsi/orbis` finds nothing however healthy the driver is - there are 1575 `wsi/orbis`
lines in the file and 0 in the datagram log of the same session. Two days of "the teardown path has
never run" rested on that.

### What is actually left, and it is a short list

The maintainer's account, which no log here contradicts:

* every title built against this workshop's Mesa ends this way, OpenGothic included;
* **a RetroArch built WITHOUT that driver exits with no dialog at all.**

⚠ **THAT SECOND POINT CONTRADICTS `ps4_app.h`.** Its termination note says returning from `main()`
on a retail console is outside the system's expected path and pops CE-34878-0 by itself - and if
that were the whole story, linking a graphics driver could not change it. The same code returning
from the same `main()` is silent without Mesa. So the dialog is not the price of returning; it is
the price of something Mesa leaves behind.

Which points at what linking Mesa adds AFTER the driver has already destroyed itself: **atexit
handlers and static destructors**. `src/util/u_queue.c:83` registers one that walks every
`util_queue` still on its list and joins its worker threads, and this console has already spent a
day proving how little it forgives around threads (`ORBIS_NCPU`, the pthread pool exhaustion of the
same date, `sceKernelGetCpuUsage` refusing outright).

### The instrument, in the build now on the console

Three markers, and the ordering is what makes them an answer rather than three log lines - `atexit`
runs handlers in REVERSE registration order:

    registered in frontend_orbis_init, before anything creates a util_queue   ->  runs LAST
    registered in frontend_orbis_shutdown, at the end of main_exit            ->  runs FIRST

With `ps4_log("exit: main_exit returned...")` at the end of `rarch_main`, four outcomes are
distinct and the next Quit picks one:

    no "main_exit returned"      died in the rest of main_exit, after frontend_orbis_shutdown
    no "atexit has begun"        died between main() returning and the first handler
    only "atexit has begun"      died inside a handler registered after startup - Mesa's
    both markers                 died after every handler, in the runtime's final teardown

⚠ **They go through `ps4_log`, which is klog AND UDP.** A process three instructions from death
cannot rely on a datagram leaving the machine; klog has already written by the time the call
returns. 8-15 ms a line, twice, once per process.

### And the anchor bug was confirmed in the field on the way past

The same capture, from the build that predates this morning's fix:

    paths: anchor '/data/OpenGothic/'
    paths: relative paths are anchored - 'Main Menu.png' -> '/data/OpenGothic/Main Menu.png'

Exactly the file this file guessed at on 2026-08-23, named by the console itself.

## 2026-08-28 — the exit markers answered, and the answer was not the guess

All three fired:

    14:23:27.968  shutdown requested - ending the process rather than idling; expect CE-34878-0
    14:23:27.968  exit: main_exit returned, main() is about to return
    14:23:27.968  exit: atexit has begun
    14:23:27.968  exit: every atexit handler returned, Mesa's included

⚠ **SO IT IS NOT THE atexit HANDLERS, AND util_queue's THREAD JOIN WAS THE WRONG SUSPECT.** The
entry above named `src/util/u_queue.c:83` as the thing to look at, on the reasoning that joining
worker threads at exit is where this console is least forgiving. That handler ran and returned. The
process survived the rest of `main_exit`, the return from `main()`, and every handler in the list -
and died after all of it, in under a millisecond.

**And the list that just completed is bigger than it looks**, which narrows this further than the
four outcomes did. Clang registers a C++ static object's destructor with `__cxa_atexit` from a
constructor in `.init_array`, so **static destructors are IN the atexit list** - Mesa's, ACO's, all
of them. They ran. What musl's `exit()` has left after `__funcs_on_exit` is exactly:

    __libc_exit_fini()   .fini_array - and by the above, largely empty here
    __stdio_exit()       flush and close every open FILE
    _Exit(code)          the kernel

⚠ **WHICH MAKES `MESA_LOG_FILE` THE SHARP SUSPECT, AND THE TEST FREE.** It is the one stdio object
that linking Mesa adds: a `FILE*` opened with `fopen` and never closed. A RetroArch built without
the driver has no such file, and exits silently - which fits. `/data/retroarch-env.txt` now has both
its lines commented out, with the reasoning in the file; one launch and one Quit settles it. **Put
them back afterwards** - without them Mesa's log goes to a stderr that goes nowhere on this console.

### Two stale things found on the way, both in this port's own record

⚠ **`/data/tempest-env.txt` DOES NOT EXIST ON THE CONSOLE.** This file, and a comment in
`platform_orbis.c`, both say `ORBIS_3D_LINEAR=1` and `ORBIS_NO_TESS=1` are "not options" and are off
in the driver unless that file turns them on. Checked in mesa-ps4 today: the driver flipped both
defaults, and the knobs now read `=0` to turn the behaviour OFF (`ac_surface.c:1691`,
`radv_physical_device.c:1081`). The frontend applies two lines from its own file, none from the
shared one, and renders correctly. The comment is corrected; the reader stays, because a file that
is not there costs one failed open and it is still how any knob is turned on.

## 2026-08-28 — the close-hang is not this frontend's, and now that is proven rather than assumed

Four runs, one afternoon, each one eliminating a step. The conclusion is negative and it is worth
as much as a fix: **nothing RetroArch can do changes this exit**, and the question moves to
mesa-ps4 with a boundary drawn around it.

### What the process survives

    shutdown requested                            frontend_driver_shutdown, late in main_exit
    exit: main_exit returned                      the rest of main_exit, and main() about to return
    exit: atexit has begun                        __funcs_on_exit entered
    exit: every atexit handler returned           ALL of them - Mesa's util_queue join included
    exit: .fini_array is running                  __libc_exit_fini entered

All five, every run, inside two milliseconds. And then CE-34878-0.

⚠ **THE atexit LIST IS BIGGER THAN IT LOOKS, WHICH IS WHY THE FOURTH LINE MATTERS SO MUCH.** Clang
registers a C++ static object's destructor with `__cxa_atexit` from a constructor in `.init_array`,
so **static destructors run in that list** - Mesa's, ACO's, every one. They all returned.

### The three eliminations, in order

⚠ **util_queue's thread join was the first guess and it was wrong.** The entry above named
`src/util/u_queue.c:83` on the reasoning that joining worker threads at exit is where this console
is least forgiving. That handler ran and returned.

⚠ **`MESA_LOG_FILE` was the second and it was wrong.** It is the one stdio object linking Mesa
adds - a `FILE*` opened with `fopen` and never closed - and a build without the driver has no such
file and exits silently. Both lines commented out of `/data/retroarch-env.txt`, confirmed by
`env: 0 line(s) applied`, same crash.

⚠ **AND THE THIRD ELIMINATION TOOK THE WHOLE REMAINING SPACE AT ONCE.** A build whose `main()`
called `_Exit(0)` instead of returning - skipping `.fini_array`, `__stdio_exit` and every step
musl's `exit()` has left, going straight to the kernel - **died exactly the same way.**

So the fault is in process termination itself, with this driver's state in the process. There is no
step left between the last line RetroArch can write and the dialog. **It belongs to mesa-ps4.**

### What mesa-ps4 has to go and look at

The driver's own teardown reports itself clean:

    wsi/orbis: scan-out down after 4703 flip(s)
    orbis-drm: at teardown 0 BO(s), 0 syncobj(s), 0 context(s), 0 VA range(s), 0 KiB held
               - winsys-lifetime, and it did not grow

⚠ **"winsys-lifetime" IS THE QUALIFIER TO READ, NOT TO SKIP.** It says nothing about allocations of
other lifetimes, and this workshop already knows that **direct memory is not reclaimed when a
mapping goes away** - recorded here on 2026-08-23, from the Lightrec work. Physical pages taken with
`sceKernelAllocateDirectMemory` and never released are the shape that would make a process's death
the system's problem rather than the process's, and that also fits the other half of the symptom:
Close Application from outside wedges the console rather than freeing it.

`wsi_orbis_release` does call `sceVideoOutClose` and does release its own direct memory when it owns
the buffers, so the scan-out path is not the obvious offender. The audit is of everything else the
driver takes from the kernel and of what is still held at `_Exit`.

### ⚠ access() IS REFUSED ON THIS CONSOLE, WITH EPERM

Found while building the third experiment, and measured in one line beside its own control:

    exit: fastexit switch - access()=-1 errno=1, stat()=0 errno=0

Same path, same instant. `stat` finds the file; `access` says EPERM. It is a real `libkernel.so`
export (`T access`), while the SDK's own `libc.a` ships `access.lo` as an **empty object** - no
`.text` at all - and `faccessat.lo` as a stub that sets `0x4e` (ENOSYS) and returns -1.

⚠ **THIS IS THE SIXTH CALL IN THAT FAMILY** - after `fcntl(F_SETFL)`, `ioctl(FIONBIO)`, `dup2` onto
fd 2, `sceKernelGetCpuUsage` and `sceKernelQueryMemoryProtection`. **Presence is not permission**,
and the first version of that experiment used `access()` alone, got a silent no, and was
indistinguishable from "the file was not there yet". A check that cannot report what it saw is not
a check.

Nothing in this tree relies on it: the only `access()` callers are `linuxraw_joypad.c`,
`parport_joypad.c` - neither built here - and a Wii U shim. A mine defused rather than a live bug,
but nobody should reach for it on this platform.

### What is left in the tree

The five markers stay; they are the record of how far the process gets, and the next thing to move
that boundary will be a driver change. The `_Exit` switch and its `access()` call are gone - a
switch that changes nothing except losing buffered writes is a hazard with no benefit.

## 2026-08-28 — linking the driver is enough; the frontend never has to call it

The zero-cost split, run before touching mesa-ps4: `/data/retroarch/retroarch.cfg` set to
`video_driver = "ps4"` and `menu_driver = "rgui"`, same eboot. The whole driver is still inside the
binary - its static constructors run at start and its destructors at exit - but nothing calls
`vkCreateInstance`. Confirmed from the log rather than from the intent:

    [PS4] video-out up: 1920x1080, 2 buffers, 16 MiB direct     <- the software driver
    (no "Vulkan up", no "vulkan: destroying the context")

**Same crash.**

⚠ **SO IT IS NOT GPU STATE, AND IT IS NOT ANYTHING THE DRIVER TOOK FROM THE KERNEL WHILE RUNNING.**
The software path opens video-out and takes 16 MiB of direct memory itself, and a build without the
driver has always done that and exited silently - so video-out and direct memory are not the
variable either. What is left is what the ARCHIVE contributes to the process: its static
constructors and destructors, its TLS, its data, its size.

### ⚠ AND THE CONTROL FOR THAT IS FROM MEMORY, WHICH IS NOT GOOD ENOUGH

"RetroArch built without our Mesa exits cleanly" is the maintainer's recollection of a build from
weeks ago, and everything under it has changed since - the frontend, orbis-compat, the exit path
itself. It is the whole premise of the hunt and it has never been re-measured on this tree.

So: `make -f Makefile.orbis clean` and a build with no `HAVE_VULKAN` and no `HAVE_OPENGLES`. The
link step prints `vulkan: OFF - software rendering only`, the package drops from 60 MB to 12 MB,
the eboot from 58 MB to 9.6 MB, and `radv`, `wsi/orbis` and `vk_icdGetInstanceProcAddr` are all
absent from the ELF. `/data/pkg/retroarchv-nomesa-20260828.pkg`.

    exits cleanly    the control holds, the archive is the variable, and the bisect is worth doing
    still crashes   the premise is stale and this hunt has been chasing the wrong difference
                    for a day - which is worth finding out in one install rather than in ten

⚠ **THE CLEAN WAS NOT OPTIONAL.** `HAVE_VULKAN` and `HAVE_OPENGLES` are `-D` defines and this
Makefile has no header dependencies, so objects built the other way are silently reused - recorded
here on 2026-08-23 as the trap that shipped a package with no cores in it.

## 2026-08-28 — ⚠ THE PREMISE WAS STALE, AND A DAY WENT INTO THE WRONG DIFFERENCE

The control was run and **it does not hold**. A frontend built with no `HAVE_VULKAN` and no
`HAVE_OPENGLES` - `vulkan: OFF - software rendering only`, 9.6 MB of eboot against 58, `radv`,
`wsi/orbis` and `vk_icdGetInstanceProcAddr` all absent from the ELF - ends **exactly the same way**:

    14:54:01.273  shutdown requested
    14:54:01.273  exit: main_exit returned, main() is about to return
    14:54:01.273  exit: atexit has begun
    14:54:01.273  exit: every atexit handler returned
    14:54:01.273  exit: .fini_array is running

then CE-34878-0.

**So Mesa is not the variable and never was.** Everything above in today's entries - util_queue's
atexit handler, `MESA_LOG_FILE`, `_Exit(0)`, the driver's teardown accounting, the hand-over to
mesa-ps4 - was measuring a difference that is not there. Each measurement is still true; the frame
around them was wrong.

⚠ **AND THE FRAME CAME FROM A RECOLLECTION, WHICH IS THE PART TO KEEP.** "A RetroArch built without
our Mesa exits with no error" was remembered from a build made weeks ago - before this port stopped
idling at Quit (`frontend_orbis_shutdown` used to hold the process on a heartbeat forever, and the
entry that changed it is in this file). So the clean exit being remembered was of a build that
**never returned from main() at all**. It was about a different exit path, not a different link
line, and nothing distinguished those two readings until the control was actually built. A control
that is remembered rather than run is not a control.

### Which leaves ps4_app.h's own sentence standing, and it was right from the start

    Termination: returning from main() on a retail console tears the process down outside the
    system's expected path and pops the error dialog, which reads as a crash (CE-34878-0).

That is the whole explanation, and it fits every measurement made today: the process survives
`main_exit`, every atexit handler, `.fini_array`, and `_Exit(0)` - because none of those is what is
wrong. **Ending without asking the system is.**

`sceSystemServiceLoadExec("exit", NULL)` is the path that asks. "exit" is the reserved argument
meaning hand control back to the system rather than replace this process with another title; the
SDK declares it with a real signature (`SystemService.h:63`) and `-lSceSystemService` has been on
this port's link line since the beginning. It is called from `frontend_orbis_shutdown`, after
`driver_uninit` has already torn everything down.

⚠ **IT IS NOT EXPECTED TO RETURN, AND THE FALLBACK IS THE OLD BEHAVIOUR RATHER THAN IDLING.** A
frontend that will not close is worse than one that closes with a dialog - that is the trade this
port already made once and it stands. The return code is logged either way, because "it refused"
and "it was never reached" must not look alike in a log.

⚠ **AND THE HAND-OVER TO mesa-ps4 HAS TO BE WITHDRAWN.** `ps4-mesa-docs` was given an entry today
saying the close-hang belongs to the driver. It does not. Correcting a record in another workshop
is part of the same job as writing it.

### Confirmed on hardware, and what the record now says

    15:02:41.450  shutdown requested

and nothing after it. No `sceSystemServiceLoadExec ... returned` line, none of the exit markers, no
dialog - the call did not return and the system took the process back. `("exit", NULL)` is the form
this firmware accepts; the second candidate the build carried was never reached and is gone.

**The markers stay.** They are now unreachable on a healthy exit and only appear if
`sceSystemServiceLoadExec` ever returns - which makes them free in the normal case and exactly the
record wanted in the abnormal one.

⚠ **AND THE ENTRY THIS PORT PUT IN ps4-mesa-docs HAS BEEN REVERTED** (`5a1021b`). It told that
workshop the close-hang was theirs to audit, and it was wrong. Leaving it would have cost somebody
a day looking for a leak that is not there. **Correcting a record in another workshop is part of
the same job as writing it.**

`ps4/RELEASE-NOTES.md` and the page `make-site.py` generates both said the dialog was expected and
explained it by a graphics teardown that never happened. Both now say Quit returns to the menu.
⚠ They still warn against *Close Application* from the console - that route has been seen to wedge
the machine and **has not been re-tested since the exit changed**, so it is written as unknown
rather than as fixed. It is the obvious next thing to measure and it costs one Quit's worth of
effort.

## 2026-08-28 — Close Application: a second failure, and this time the control was run first

Quit from inside the application is clean. **Closing from the console's own menu still is not**, and
it is a different fault with a different owner.

### The process is killed outright - nothing of this port runs

Two channels, same session, and both stop mid-sentence:

    UDP    the last line is an ordinary startup line. No "shutdown requested", no
           sceSystemServiceLoadExec, none of the exit markers.
    Mesa   /data/retroarch-mesa.log ends in the middle of a session at frame 768,
           with ZERO "scan-out down".

So `frontend_orbis_shutdown` is never called, the driver never tears down, video-out is never
closed and the scan-out buffers are never unregistered. ⚠ **The exit fix cannot apply here, because
none of this port's code runs.** The application is not told it is being closed.

### ⚠ AND THE CONTROL SEPARATES IT CLEANLY - SAME BINARY, ONE CONFIG LINE

    video_driver = "ps4"       software: video-out, 2 buffers, 16 MiB direct, no GPU   CLEAN
    video_driver = "vulkan"    RADV: video-out, swapchain images, GPU submissions      CRASH

Same eboot, Mesa linked either way, same `sceVideoOut*` API, same `sceKernelAllocateDirectMemory`.
**The only difference is whether RADV was running.** This time the control was measured before the
conclusion was written, which is the lesson the earlier half of today paid for.

⚠ **AND IT RULES OUT THE OBVIOUS READING.** "The display is scanning out of memory that vanishes"
cannot be the whole story: the software driver registers its own direct memory as scan-out buffers
and is killed exactly as abruptly, and the console comes back. What Vulkan adds on top is a live
GPU context with work in flight and swapchain images the GPU renders into.

### What the next experiment is, and what it costs

`wsi_orbis` registers the swapchain's own images as scan-out buffers when it can - zero copy,
`owns_buffers == false` - and falls back to allocating GARLIC buffers of its own with a memcpy per
frame. That fallback is much closer in shape to the software driver that survives.

    zero copy survives too    the display is not the variable; a live GPU context is
    zero copy is the one      the display scanning out RADV's render targets is what the
                              system cannot survive losing, and the copy path is a fallback
                              that could be selected when a title wants to be closable

⚠ **THERE IS NO ENV KNOB FOR IT.** `wsi_common_headless.c:862-893` takes the direct path whenever
every image is CPU-mapped and shares a pitch; `owns_buffers` follows from `addrs == NULL` at
`wsi_orbis.c:297`. Adding the knob is a few lines, but it costs a Mesa rebuild and a frontend
relink, so it is a decision rather than a try.

### ⚠ AND IT IS A DIALOG, NOT A WEDGE - WHICH SETTLES WHAT IT IS WORTH

Measured, not assumed: on the current build, Close Application on the Vulkan driver shows the error
dialog and **the console returns to its menu with no restart needed**.

That contradicts what this port has been telling users since v0.1.x - "that can leave the system
hung and needing a restart" - and the reason it was believed is worth keeping. The wedges that were
actually seen happened to a process that was ALREADY in a bad state: once when the system pthread
pool had been exhausted by the fragment-depth-write option, and once when the frontend still idled
forever at Quit so nothing ever ended. Neither was Close Application's own doing, and both got
written down as if they were.

So this is a cosmetic defect with a working alternative, not a hazard. **It does not justify a Mesa
rebuild on its own.** The zero-copy experiment above stays written down for whoever is in that tree
for another reason.

**Nothing here is a RetroArch change.** Quit is the route that works and the release notes say so.
