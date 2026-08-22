# RetroArch on the PlayStation 4 — plan

Written 2026-08-22, against RetroArch master at **c59b1833f7**, tagged `ps4-base` — clean upstream
plus local upstream-style fixes, and no PS4 work of ours in the tree. Everything this plan describes
is a diff against that tag, on the branch `ps4-support`.

The target is a RetroArch that boots on a retail console under GoldHEN, draws with the CPU, takes
pad input and plays sound — and whose video driver is later replaced by the RADV port in
`~/src-ps4/mesa-ps4` without any of the rest moving.

---

## 1. What is already in the tree, and why almost none of it is usable

`git ls-files | grep -i orbis` finds a complete-looking port:

    Makefile.orbis, Makefile.orbis.salamander
    frontend/drivers/platform_orbis.c
    gfx/drivers_context/orbis_ctx.c
    input/drivers/ps4_input.c, input/drivers_joypad/ps4_joypad.c
    libretro-common/include/defines/ps4_defines.h
    .github/workflows/PS4-ORBIS.yml

Four facts about it decide the whole plan:

**1.1 It targets a different SDK.** `Makefile.orbis` requires `$ORBISDEV`, compiles
`--target=x86_64-scei-ps4`, links `$(ORBISDEV)/usr/lib/crt0.o` against `linker.x`, and packages with
`orbis-elf-create` + `make_fself.py`. Its libraries — `-lorbisPad -lorbisLink -lorbisNfs
-ldebugnet -lkernelUtil` — are orbisdev's, not the SDK's. Everything else in `~/src-ps4` is
OpenOrbis: `--target=x86_64-pc-freebsd12-elf`, `link.x`, `crt1.o`, `create-fself`, `-lScePad`.
The two are not mixable, and only one of them has `orbis-compat` behind it.

**1.2 Its object list is empty.** The file reads

    OBJ += deps/xxHash/xxhash.o \
    # 	input/drivers/ps4_input.o \
    ...

A backslash continuation into a `#` line: make joins them and the comment eats the rest, so the
platform, input, audio and context objects are *not compiled*. Whatever the CI is green on, it is
not this port.

**1.3 The CI never links a working binary either.** `.github/workflows/PS4-ORBIS.yml` builds
`HAVE_STATIC_DUMMY=1` only — no core, no drivers — inside a container of stub headers
(`.github/workflows/scripts/stubs/orbis/`). It is a compile smoke test.

**1.4 Its graphics path is Piglet.** `orbis_ctx.c` includes `<piglet.h>`, links
`-lScePigletv2VSH_stub`, and calls `scePigletSetConfigurationVSH` — Sony's GLES2 in the VSH
process. That is the thing this port exists to not need.

**Decision D0.** The orbisdev leg is not resurrected. The files above are *reference*: they say what
RetroArch expects a console port to provide. New code is written against OpenOrbis + orbis-compat.

---

## 2. Decisions

**D1 — Toolchain: OpenOrbis + `orbis-compat`.** Not just for the packaging tools. `orbis-compat`
carries corrections that a build without it gets silently wrong, all of them applicable here:
four pthread types musl declares 4 bytes where Sony writes 8 (measured on hardware; the mutexattr
case corrupted a heap block 4704 bytes away in dEQP), the libc++/SDK include order that decides
whether `std::abs(float)` truncates to int, `orbis_prefix.h` in place of `-include stdlib.h`, and
implementations for `stat`, `mmap`, futex, `backtrace`, thread/clock/timer, that RetroArch will hit
immediately. RetroArch is mostly C, so the `std::abs` trap is a smaller risk than it was for
OpenGothic — but `deps/` has C++ in it and cores certainly do.

The toolchain description lives in `orbis-compat/cmake/ps4-openorbis.cmake`. RetroArch is
Makefile-driven, so the flags must be *transcribed*, not included. `~/src/ps4doom/Makefile` is the
existing transcription and the template to copy: same `--target`, `-isysroot`, `link.x`,
`--no-rosegment`, `crt1.o` last.

**D2 — Software rendering first, RADV second, Piglet never.** The software driver is not a
throwaway: it is the leg that proves boot, paths, input, audio, menu and packaging *without* the
driver in the picture. When Vulkan lands, a regression has two legs to bisect between.

**D3 — Static cores first, dynamic cores as the real goal.** `HAVE_DYNAMIC=0` to begin with, one
`eboot.bin` per core, as Vita/3DS/Switch do; `-lretro_orbis` is already the shape `Makefile.orbis`
assumes. That is a sequencing decision, not a limit of the platform — the `.prx` route is supported
end to end and is where this should land.

What the pieces already are:

* **The SDK builds PRXs.** `samples/library_example/` — same `--target`, same `link.x`, `-pie`, but
  `crtlib.o` instead of `crt1.o`, and `create-fself --lib=libExample.prx` instead of `--eboot`.
  It also emits a PC-side stub `.so` for build-time linking, which a libretro core does not need
  (see below).
* **The console loads them and resolves by name.** `samples/using_library/using_library/main.cpp`:
  `sceKernelLoadStartModule("/app0/sce_module/libExample.prx", …)` then
  `sceKernelDlsym(handle, "name", &fnptr)`. The module ships inside the pkg under `sce_module/`,
  which `create-gp4` picks up (`LIBMODULES := $(wildcard sce_module/*)`).
* **RetroArch already speaks exactly that.** `libretro-common/dynamic/dylib.c:130` implements
  `dylib_load` as `sceKernelLoadStartModule` and `dylib_proc` as `sceKernelDlsym`. Nothing has to
  be written for the loader; it has to be turned on and pointed at the right path.
* **libretro's ABI fits a PRX better than most.** A core exports `retro_init`, `retro_run`, … as
  plain C — no name mangling to work around, unlike the sample's `_Z19testLibraryFunctionPcmi` —
  and it calls back into the frontend *only* through the function pointers handed to
  `retro_set_environment`/`retro_set_video_refresh`. So the module needs **zero imports from the
  eboot**, which is the case the PS4 module loader handles and the hard case never arises.

⚠ **ALL THREE OF THE UNKNOWNS BELOW CAME BACK ANSWERED ON 2026-08-22, and the answer was yes.**
A core builds and loads as a PRX, the two libcs did not collide, and `crtlib.o`'s init was enough.
See `ps4/HANDOFF.md`. The scale question is the only part still open: 185 KB is not tens of
megabytes.

What was unknown, and what Phase 6b was for:

* whether a core-sized PRX (tens of MB, heavy relocation) loads as readily as a 300-byte sample;
* whether the PRX and the eboot end up sharing one libc/heap, or two — the consumer sample ships
  `libc.prx` and `libSceFios2.prx` in `sce_module/`, so the system libs are modules, but a core
  linking `-lc -lc++` itself needs checking against `orbis-compat`'s allocator;
* `crtlib.o`'s init path versus what a core expects to have run before `retro_init`.

Static first because one binary removes all three questions at once, and Phase 0-5 have enough
unknowns of their own. Revisit immediately after Phase 6.

**D4 — Video via `sceVideoOut` directly.** New `gfx/drivers/ps4_gfx.c`. Blueprint for the
sequence: `~/src/ps4doom/platform/doomgeneric_ps4.c` (proven on hardware). Blueprint for the
RetroArch driver shape: `gfx/drivers/switch_nx_gfx.c` — software, `memcpy`+scale into a display
buffer, RGUI composite, no context driver.

**D5 — RGUI only, at first.** XMB/Ozone/gfx-widgets want a GL or Vulkan driver. RGUI is software
and draws its own bitmap font.

**D6 — Log channel from day one.** `orbis-compat/optional/ps4_app.cpp`: UDP netlog primary, klog for
the bounded lines. RetroArch's own `HAVE_NETLOGGER` is a separate mechanism and needs a working
socket stack; `ps4_app` is what already works.

---

## 3. Phases

Each phase ends in something that runs — under `unemups4` first, then on the console.

### Phase 0 — the link, and nothing else
New `Makefile.orbis` (OpenOrbis flavour; the old one is deleted, not kept beside it — two Makefiles
named for one console is how 1.2 happened). `HAVE_STATIC_DUMMY=1`, all drivers null, `main()` calls
`ps4_app_init()` and `ps4_idle_forever()`.

Deliverable: `eboot.bin` that boots and prints one netlog line.
Proves: flags, `-Werror` policy (upstream `Makefile.orbis` sets it; expect to relax it and say so
in the file), `crt1.o`/`link.x`/`--no-rosegment`, `create-fself`.

### Phase 1 — the libc gap list
Turn on the real object list (`Makefile.common`), compile everything, read the link errors. This is
a *survey*, deliberately before any driver work: it is cheaper to learn what is missing from one
link than from six.

Expected gaps, from what `orbis-compat` already had to add for other consumers: `dirent`/`readdir`
behaviour, `realpath`, `getcwd`, `sysconf`, `gettimeofday`, `mmap` semantics, `pthread` attribute
types, `backtrace`. Anything general goes *into* `orbis-compat` (it is the shared overlay, and
RetroArch is its fourth consumer); anything RetroArch-shaped goes in `ps4/`.

Deliverable: a linking `retroarch_orbis.elf` with the dummy core, and a list of what had to be
written.

### Phase 2 — frontend driver
Rewrite `frontend/drivers/platform_orbis.c` against OpenOrbis. What it owes RetroArch:
- path layout: assets read-only under `/app0`, writable state under `/data/retroarch` (config,
  savefiles, savestates, system, playlists). `orbis_paths.h` in the overlay already answers where
  writable storage is.
- `sceSystemServiceHideSplashScreen()`, `sceUserServiceInitialize()` at start.
- exit: **do not return from `main()`** — a retail console pops CE-34878-0. `ps4_idle_forever()`.
- `frontend_driver_get_core_extension` = `"a"`/static, `environment_get`, CPU count.

### Phase 3 — `gfx/drivers/ps4_gfx.c`, software
The measured sequence (ps4doom, hardware-proven):
1. `sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, 0)`
2. framebuffers **must be direct memory** — `sceKernelAllocateDirectMemory(… 3 /*WB_ONION*/ …)` +
   `sceKernelMapDirectMemory(… 0x33 …)`, 2 MiB aligned, carved into N buffers.
   `malloc`'d memory is rejected on hardware with `0x80290013` and only ever worked under the
   emulator.
3. `sceVideoOutSetBufferAttribute(A8B8G8R8_SRGB, LINEAR, 16:9, w, h, pitch=w)` +
   `sceVideoOutRegisterBuffers`
4. `sceKernelCreateEqueue` + `sceVideoOutAddFlipEvent` — **required**: without the event queue,
   submitted flips are not processed, `flipArg` never advances, and a frame-wait loop crawls at
   minutes per frame.
5. `sceVideoOutSubmitFlip(…, ORBIS_VIDEO_OUT_FLIP_VSYNC, …)`

RetroArch side: `libretro-common/gfx/scaler` for core-pixfmt → XRGB8888 scale-and-letterbox, RGUI's
RGBA4444 menu framebuffer composited over it, `video_driver_t` with `frame`/`set_nonblock_state`/
`viewport_info`, no `gfx_ctx`. Register `&video_ps4` in `gfx/video_driver.c` under `#ifdef ORBIS`.

⚠ **SETTLED 2026-08-22, BY MEASUREMENT: 1080p, and it holds 60 Hz.** The scale costs 6.1 ms of a
16.67 ms frame - 37% - and the achieved interval is 16683 us, which is the display pacing us
rather than the other way round. 720p remains the lever if a core ever needs more than the 10.5 ms
that leaves, and it should be a runtime option rather than a new default. See `ps4/HANDOFF.md`.

### Phase 4 — input
`libScePad`. The existing `ps4_joypad.c` is orbisdev's `orbisPad` and gets rewritten.
⚠ `scePadOpen` needs the **real** user id from `sceUserServiceGetInitialUser()` — the `0xFF`
"main user" constant is accepted by the emulator and rejected on hardware with `0x809b0001`.
Analog sticks, touchpad as pointer (RGUI likes one), rumble later.

### Phase 5 — audio
`sceAudioOut`, 48 kHz stereo s16. Blueprint: `~/src/ps4doom/platform/doom_sound_ps4.c`.
`audio/drivers/ps4_audio.c`, threaded write with RetroArch's own resampler ahead of it.

### Phase 6 — a core, and a package
Build one libretro core static with the same toolchain (a small pure-C one first — 2048 or
gambatte), link `retroarch_orbis`, package. Packaging: `orbis-compat/cmake/ps4-package.cmake` is
CMake-shaped, so either drive its `create-gp4`/`PkgTool` steps from the Makefile or copy ps4doom's
`pkg.gp4` route. RetroArch's asset bundle is large — check what `/app0` costs before shipping all
of it.

### Phase 6b — a core as a PRX
The D3 route, once one core runs statically and there is a known-good binary to compare against.
Build the same core with `crtlib.o` + `create-fself --lib=`, ship it in `sce_module/`, set
`HAVE_DYNAMIC=1`, and let `dylib.c`'s existing ORBIS path load it. Answers the three unknowns in D3
against a core whose static build already works, so any difference is the PRX and nothing else.

Unlocks: one eboot for all cores, the core downloader, and playlists that are not per-binary.

### Phase 7 — RADV  ✅ 2026-08-22
Only now, and in this order:
1. `vkloader`: run `orbis-compat/vkloader/gen.py` over RetroArch's own objects to emit
   `ps4/vkthunks.c` (per-consumer by design — Tempest's list is not ours).
2. `gfx/common/vulkan_common.c` loads Vulkan with `dylib_load` (`libvulkan.so.1`, `vulkan-1.dll`).
   Add an `#ifdef ORBIS` branch that takes `vkGetInstanceProcAddr` from the static ICD instead —
   the symbol exists because vkloader defines it.
3. Surface: `wsi_orbis` hangs off `VK_EXT_headless_surface`, so RetroArch needs a
   `VULKAN_WSI_HEADLESS` in `gfx/common/vulkan_common.h` and a context driver that calls
   `vkCreateHeadlessSurfaceEXT`. The flip is inside Mesa; RetroArch only presents.
4. Then XMB/Ozone, slang shaders, and cores with a hardware-render callback become possible.

---

## 4. Risks

- **`-Werror` upstream in `Makefile.orbis`.** Fine for a stub build, hostile for a real one.
  Relax deliberately, with the list of what is suppressed written down.
- **RetroArch's POSIX surface is wide.** Phase 1 exists because of this; the size of that gap is
  the single largest unknown in the plan.
- **Memory.** RetroArch + a core + assets on a console whose allocator is not Linux's.
  `orbis-compat/src/orbis_bigheap.c` exists for exactly this and should be wired in early.
- **Software scaling cost** (see Phase 3).
- **`/app0` is read-only**; every write path must resolve to `/data`. Cores assume otherwise.
- **The static build has no dynamic core loading**, which makes the online updater and core
  downloader dead weight — turn them off rather than let them fail at runtime. They come back with
  D3's PRX route.

## 5. What it costs

Calibrated against this workshop's own two ports, not guessed:

| project | size | elapsed |
|---|---|---|
| **ps4doom** — a whole software port: video-out, pad, audio, OPL synth, pkg | 10 commits | **2 days** |
| **RADV/orbis** — `git diff --cached --stat` in `mesa-ps4` | 17 791 insertions / 76 files | **~4 weeks** (HANDOFF, 2026-07-26 → 08-21) |

RetroArch's compile surface here is measured, not estimated: `make -f Makefile.orbis info` reports
**261 objects**.

| phase | sessions | what decides it |
|---|---|---|
| 0 — Makefile, link a stub | 1 | the template exists (`~/src/ps4doom/Makefile`); no new ideas in it |
| 1 — 261 objects, the libc gap | **2-6** | ← the whole variance lives here |
| 2 — frontend driver | 1-2 | |
| 3 — `ps4_gfx.c`, software | 2-3 | sequence proven; RetroArch plumbing, scaler, RGUI composite and hunting the first pixel are not |
| 4 — input | 1 | |
| 5 — audio | 1 | `doom_sound_ps4.c` is the model |
| 6 — a core, a package | 1-2 | |
| **to "a game runs on the console"** | **9-16, realistically ~12** | |
| 6b — a core as a PRX | 1-3 | three unknowns, any of which can be a wall |
| 7 — RADV | 3-6 | thunks are mechanical; `VULKAN_WSI_HEADLESS` is moderate; the long tail is RetroArch's own Vulkan assumptions (slang → glslang → C++) |
| **all of it** | **15-25, median ~18** | |

⚠ **MEASURED 2026-08-22, AND THE TABLE ABOVE IS NOW WRONG WHERE IT MATTERS MOST.** Phases 0 and 1
cost one sitting between them, not the 3-7 sessions budgeted, because the libc gap that the whole
spread hung on turned out to be **empty**: 239 objects compile with zero warnings and not one shim
had to be written. `ps4/HANDOFF.md` is the account. The rows below Phase 1 have not been re-derived
- they were always conditioned on Phase 1's answer, and the answer was the good one.

⚠ **Phase 1 was the only real unknown, and this is why nothing after it was worth estimating first.** ps4doom took two days because doomgeneric barely touches POSIX. RetroArch touches all of it:
VFS, dirent, threading, mmap, sockets, sysconf. `orbis-compat` has three consumers already and may
cover the lot in one session — or `vfs_implementation.c` and `readdir` may eat six. Phases 0+1 are
~3 sessions and collapse the spread on everything after them from ±10 to ±3. Any other order is
estimating blind.

⚠ **The emulator shortens the loop; it does not replace the console.** ps4doom's `0x80290013`:
`malloc`'d video-out buffers worked under unemups4 and were refused by hardware. Every phase needs
a hardware confirmation, and that is already priced in above.

## 6. What is deliberately not in this plan

Salamander (`Makefile.orbis.salamander`) — it exists to swap cores at runtime, which without
`dlopen` means chain-loading another `.self`. Worth doing eventually, not before one core runs.
Networking, cheevos, netplay: off until the socket stack is known good.
