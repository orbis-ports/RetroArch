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

Then **Online Updater → Core Downloader** lists 101 cores. Download one and it is ready to use.

## Quitting

Use RetroArch's own **Quit RetroArch** entry, or the Quit combo on the pad. It returns to the
console's menu with no dialog.

Earlier versions showed `CE-34878-0` here. They ended by returning from `main()`, which takes the
process down outside the path the system expects; this one asks the system to unload it instead.

**Closing it from the console instead** — PS button, *Close Application* — still shows
`CE-34878-0`. The console returns to its menu and nothing needs restarting, but the application is
killed outright there and never gets to shut itself down, so Quit is the better habit.

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

- 101 cores, built for this console, at `cores.prx0.com`
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
