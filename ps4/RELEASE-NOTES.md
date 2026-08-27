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

Use RetroArch's own **Quit RetroArch** entry, or the Quit combo on the pad. It ends with
`CE-34878-0` on screen; that dialog is expected here and nothing is wrong.

**Do not close it from the console** — PS button, *Close Application*. That can leave the system
hung and needing a restart: the application tears its graphics context down on the way out, and
taken away mid-frame it sometimes does not get to.

## What is here

- 101 cores, built for this console, at `cores.prx0.com`
- Nintendo 64 at 60 fps (GLideN64)
- PlayStation at 50 fps (Beetle PSX HW, Lightrec, Vulkan renderer)
- Online Updater: cores, core info, databases, thumbnails, assets
- HTTPS, with certificate validation — the trust anchors ship in the package

## Known limits

- **63 of 164 recipe cores do not build yet**, for reasons recorded per core. The ones that are
  listed are the ones that link and carry `retro_run`.
- **Quitting shows CE-34878-0** — see above. The process ends rather than idling forever, and
  the alternative was a hang that only a console restart cleared.
- Cores are cloned from upstream's tip when a build is run, so a given core's version is the
  date it was built, not a pinned release.
