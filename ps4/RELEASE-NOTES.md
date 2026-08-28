RetroArch for the PlayStation 4, on a jailbroken console under GoldHEN.

Vulkan through RADV, OpenGL ES 3.1 through zink, and a Core Downloader that serves PS4 cores
rather than Linux shared objects.

## Install

Copy the `.pkg` to `/data/pkg` over FTP, then **Settings → Debug Settings → Package Installer**.

Tested on **firmware 11.00** with **GoldHEN v2.4b18.10** (SiSTRo). Other firmware and other
jailbreak builds may work; nobody has checked, and a report either way is useful.

It installs as **RetroArchV**, title id `RTRV00001`. That is a different title id from the
RetroArch builds already circulating, so it sits **beside** an existing install rather than
replacing it — the console decides collisions by title id, not by the name on screen.

## First run

**Online Updater → Update Core Info Files.**

Do this once, before browsing the Core Downloader. Core metadata is not shipped in the package,
so on a fresh install the downloader lists `.prx` filenames instead of core names until those
files arrive. Everything works either way; only the names are missing.

Then **Online Updater → Core Downloader** lists 103 cores. Download one and it is ready to use.

## Quitting

Use RetroArch's own **Quit RetroArch** entry, or the Quit combo on the pad. It returns to the
console's menu with no dialog.

Earlier versions showed `CE-34878-0` here. They ended by returning from `main()`, which takes the
process down outside the path the system expects; this one asks the system to unload it instead.

**Closing it from the console instead** — PS button, *Close Application* — still shows
`CE-34878-0`. The console returns to its menu and nothing needs restarting, but the application is
killed outright there and never gets to shut itself down, so Quit is the better habit.

## New in v0.1.6

- **A USB mouse works.** Move, left and right click, and the wheel, in the menu and in cores that
  ask for one. The console's mouse library ships no usable declarations in the SDK - five entry
  points with no argument types and no data structure - so the layout was established by probing
  the hardware and reading which bytes moved.
- **DOS, through DOSBox Pure.** The first DOS core this port has been able to offer. Use it with
  the USB keyboard and mouse.
- **The mouse pointer is visible at all.** RetroArch hides the cursor unless it believes it is
  running fullscreen, and this console had that recorded as false - a state it cannot actually be
  in, since the application owns the whole screen and there is no desktop to share it with.
- **Three more cores**: `dosbox_pure` (DOS), `arduous` (Arduboy) and `thepowdertoy`. They are
  built through CMake, which this port could not use until now - a whole build type was being
  skipped for want of a toolchain file.

## New in v0.1.5

- **Quitting no longer shows an error.** The application now asks the system to unload it instead
  of returning from `main()`, which took the process down outside the path the console expects.
- **Framebuffer Emulation works on Nintendo 64.** GLideN64 was throwing its depth buffer away on
  every frame over a one-pixel size test, so the picture was ordered by draw order rather than by
  distance. It is on by default upstream and can be left on now.
- **Sound survives switching games.** The console hands out eight audio ports per process and
  never takes one back; the ninth switch was silent. One port is now held for the whole run.
- **Files are no longer written into another application's folder.** Relative paths were anchored
  under `/data/OpenGothic/` by the shared platform overlay.
- **The stock Beetle PSX build is gone from the core list.** It carried none of this port's work
  and ran the interpreter at about a third of full speed, one letter away from the fork that does
  not. Use `mednafen_psx_hw`.

## New in v0.1.4

- **The build no longer reaches into the host's headers.** `-isysroot` never stopped clang
  searching `/usr/include`, so anything the SDK, the platform overlay and Mesa all lacked was
  silently taken from whichever machine did the building. Everything now compiles against this
  platform's headers only.
- **Nintendo 64 is in the core list** for the first time, on GLideN64.

## New in v0.1.3

- **Update Assets no longer crashes.** Large updater downloads stream to disk instead of being
  held whole in memory - assets.zip is 71 MB and killed the process at the end of the transfer.
- **Update Core Info Files and Update Assets exist at all.** Both entries were missing from the
  build while the documentation told you to use them.
- **Vulkan is the default video driver**, rather than OpenGL translating to it.

## What is here

- 103 cores, built for this console, at `cores.prx0.com`
- Nintendo 64 at 60 fps (GLideN64)
- PlayStation at 50 fps (Beetle PSX HW, Lightrec, Vulkan renderer)
- Online Updater: cores, core info, databases, thumbnails, assets
- HTTPS, with certificate validation — the trust anchors ship in the package

## Known limits

- **63 of 164 recipe cores do not build yet**, for reasons recorded per core. The ones that are
  listed are the ones that link and carry `retro_run`.
- **Closing from the console's own menu still shows CE-34878-0** — see above. It is cosmetic; the
  console recovers on its own.
- Cores are cloned from upstream's tip when a build is run, so a given core's version is the
  date it was built, not a pinned release.
