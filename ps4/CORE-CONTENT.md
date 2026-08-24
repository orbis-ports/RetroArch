# Test content, per core

What each of the 100 built cores needs before it can be tested. Two kinds of line, kept apart
on purpose:

* **Requires** - taken from the core's own `.info`: its `supported_extensions` and any firmware it
  declares NOT optional. That is the core telling you what it needs, not me guessing.
* **Suggested** - my recommendation for a *test corpus*. Where a system has free homebrew or a
  freely redistributable shareware release, that is named, because a test set you can re-download
  and share beats one you cannot.

⚠ **A core that declares required firmware will not load content without it.** Those are listed
first; everything else can be tested with any dump you already own.

BIOS/firmware goes in `/data/retroarch/system/` (mode 666). Content anywhere readable -
`/data/retroarch/roms/` is what this port has been using.


## Firmware first — 17 cores will not run without these

| Core | System | Files, into `/data/retroarch/system/` |
|------|--------|----------------------------------------|
| `ecwolf` | Wolfenstein 3D Game Engine | `ecwolf.pk3` |
| `fmsx` | MSX | `MSX.ROM`, `MSX2.ROM`, `MSX2EXT.ROM`, `MSX2P.ROM`, `MSX2PEXT.ROM` |
| `freechaf` | FreeChaF | `sl31253.bin`, `sl31254.bin`, `sl90025.bin` |
| `freeintv` | Intellivision | `exec.bin`, `grom.bin` |
| `gpsp` | Game Boy Advance | `gba_bios.bin` |
| `hatari` | Atari ST/STE/TT/Falcon | `tos.img` |
| `mednafen_lynx` | Lynx | `lynxboot.img` |
| `mednafen_pcfx` | PC-FX | `pcfx.rom` |
| `mednafen_psx` | PlayStation | `scph5500.bin`, `scph5501.bin`, `scph5502.bin` |
| `mednafen_psx_hw` | PlayStation | `scph5500.bin`, `scph5501.bin`, `scph5502.bin` |
| `mednafen_saturn` | Saturn | `sega_101.bin`, `mpr-17933.bin` |
| `mu` | Palm OS | `palmos41-en-m515.rom` |
| `o2em` | Magnavox Odyssey2 / Philips Videopac+ | `o2rom.bin`, `c52.bin`, `g7400.bin`, `jopac.bin` |
| `puae` | Amiga | `kick34005.A500`, `kick40068.A1200`, `kick40060.CD32`, `kick40060.CD32.ext` |
| `px68k` | Sharp X68000 | `keropi/iplrom.dat`, `keropi/cgrom.dat` |
| `quasi88` | PC-8000 / PC-8800 series | `quasi88/n88.rom`, `quasi88/n88_0.rom` |
| `same_cdi` | CD-i | `same_cdi/bios/cdimono1.zip` |

## Everything, by system

| System | Cores | Formats | Content to get |
|--------|-------|---------|----------------|
| (no system name) | `jumpnbump` `test` | `.dat` | **Jump'n'Bump** `.dat` level - the original game data is freeware |
| 2048 Game Clone | `2048` | — | no content |
| 3DO | `opera` | `.iso`, `.bin`, `.chd`, `.cue` | any dump you own |
| Amiga | `puae` | `.adf`, `.adz`, `.dms`, `.fdi`, `.ipf`, `.hdf`, `.hdz`, `.lha` | any dump you own |
| Arcade (various) | `dice` `fbalpha2012` `mame2000` `mame2003` `mame2003_plus` | `.zip`, `.dmy`, `.k1`, `.a1`, `.6c`, `.c6`, `.d2`, `.s1` | any dump you own |
| Atari 2600 | `stella2014` | `.a26`, `.bin` | 2600 homebrew is plentiful and free |
| Atari 5200 | `a5200` | `.a52`, `.bin` | any dump you own |
| Atari 7800 | `prosystem` | `.a78`, `.bin`, `.cdf` | any dump you own |
| Atari 8-bit Family | `atari800` | `.xfd`, `.atr`, `.dcm`, `.cas`, `.bin`, `.a52`, `.zip`, `.atx` | any dump you own |
| Atari ST/STE/TT/Falcon | `hatari` | `.st`, `.msa`, `.zip`, `.stx`, `.dim`, `.ipf`, `.vhd`, `.gem` | any dump you own |
| BK-0010/BK-0011(M) | `bk` | `.bin` | any dump you own |
| C64 | `frodo` | `.d64`, `.t64`, `.x64`, `.p00`, `.lnx`, `.lyx`, `.zip` | any dump you own |
| CD-i | `same_cdi` | `.chd`, `.iso`, `.cue` | any dump you own |
| CP System I | `fbalpha2012_cps1` | `.zip` | any dump you own |
| CP System III | `fbalpha2012_cps3` | `.zip` | any dump you own |
| CPC | `crocods` | `.dsk`, `.sna`, `.kcr` | any dump you own |
| CPC/GX4000 | `cap32` | `.dsk`, `.sna`, `.zip`, `.tap`, `.cdt`, `.voc`, `.cpr`, `.m3u` | any dump you own |
| Cave Story Game Engine | `nxengine` | `.exe` | **Cave Story** freeware data (`Doukutsu.exe` + `data/`) - free from Pixel's original release |
| ColecoVision/CreatiVision/My Vision | `jollycv` | `.col`, `.rom`, `.myv`, `.bin` | any dump you own |
| DOOM Game Engine | `prboom` | `.wad`, `.iwad`, `.pwad`, `.pk3` | **Freedoom** (`freedoom1.wad`/`freedoom2.wad`) - free, drop-in for `doom.wad` |
| Dinothawr Game Engine | `dinothawr` | `.game` | **Dinothawr** game dir - free, ships with the core's own repo |
| Flashback Game Engine | `reminiscence` | `.map`, `.aba`, `.seq`, `.lev` | **Flashback** game data - commercial; the demo also works |
| FreeChaF | `freechaf` | `.bin`, `.chf` | any dump you own |
| Game Boy Advance | `gpsp` `mednafen_gba` `vba_next` | `.gba`, `.bin` | GBA homebrew and AGB test suites are free |
| Game Boy/Game Boy Color | `gambatte` `gearboy` `sameboy` `tgbdual` | `.gb`, `.gbc`, `.dmg` | Blargg's cpu_instrs / dmg-acid2 are free |
| Game Boy/Game Boy Color/Game Boy Advance | `vbam` | `.dmg`, `.gb`, `.gbc`, `.cgb`, `.sgb`, `.gba` | any dump you own |
| Handheld Electronic | `gw` | `.mgw` | **Game & Watch** `.mgw` from MAME sets - commercial |
| Intellivision | `freeintv` | `.int`, `.bin`, `.rom` | any dump you own |
| Jaguar | `virtualjaguar` | `.j64`, `.jag`, `.rom`, `.abs`, `.cof`, `.bin`, `.prg` | any dump you own |
| LowRes NX | `lowresnx` | `.nx` | **LowRes NX** `.nx` programs - free from the LowRes NX gallery |
| Lutro | `lutro` | `.lutro`, `.love`, `.lua` | any **Lua** Lutro game - free samples in the lutro repo |
| Lynx | `handy` `mednafen_lynx` | `.lnx`, `.lyx`, `.o` | any dump you own |
| MSX | `fmsx` | `.rom`, `.mx1`, `.mx2`, `.dsk`, `.fdi`, `.cas`, `.m3u` | any dump you own |
| Magnavox Odyssey2 / Philips Videopac+ | `o2em` | `.bin` | any dump you own |
| Mr.Boom | `mrboom` | `.desktop` | no content |
| Music | `gme` `pocketcdg` | `.ay`, `.gbs`, `.gym`, `.hes`, `.kss`, `.nsf`, `.nsfe`, `.sap` | chiptune files: `.nsf .gbs .spc .vgm .ay .sap` - huge free archives exist |
| Neo Geo | `fbalpha2012_neogeo` | `.zip` | any dump you own |
| Neo Geo Pocket (Color) | `mednafen_ngp` `race` | `.ngp`, `.ngc`, `.ngpc`, `.npc` | any dump you own |
| Nintendo DS | `desmume2015` | `.nds`, `.ids`, `.bin` | any dump you own |
| Nintendo Entertainment System | `fceumm` `mesen` `nestopia` `quicknes` | `.fds`, `.nes`, `.unif`, `.unf` | NES test ROMs (blargg's, nestest) are free and are the *right* first test |
| Oberon RISC machine | `oberon` | `.dsk` | **Project Oberon** disk image - free |
| Outrun Game Engine | `cannonball` | `.game`, `.88` | **OutRun** arcade ROM set (`outrun` MAME set) + `config.xml` - commercial |
| PC Engine SuperGrafx | `mednafen_supergrafx` | `.pce`, `.sgx`, `.cue`, `.ccd`, `.chd` | any dump you own |
| PC Engine/PCE-CD | `mednafen_pce_fast` | `.pce`, `.cue`, `.ccd`, `.chd`, `.toc`, `.m3u` | any dump you own |
| PC Engine/SuperGrafx/CD | `mednafen_pce` | `.pce`, `.sgx`, `.cue`, `.ccd`, `.chd`, `.toc`, `.m3u` | any dump you own |
| PC-8000 / PC-8800 series | `quasi88` | `.d88`, `.u88`, `.m3u` | any dump you own |
| PC-98 | `nekop2` | `.d98`, `.zip`, `.98d`, `.fdi`, `.fdd`, `.2hd`, `.tfd`, `.d88` | any dump you own |
| PC-FX | `mednafen_pcfx` | `.cue`, `.ccd`, `.toc`, `.chd` | any dump you own |
| PICO-8 | `retro8` | `.p8`, `.png` | **PICO-8** `.p8`/`.p8.png` carts - thousands are free on the BBS |
| Palm OS | `mu` | `.prc`, `.pqa`, `.img`, `.pdb`, `.zip` | **Palm OS 4.1** ROM (`palmos41-en-m515.rom`) + `.prc` apps - commercial ROM |
| PlayStation | `mednafen_psx` `mednafen_psx_hw` | `.cue`, `.toc`, `.m3u`, `.ccd`, `.exe`, `.pbp`, `.chd`, `.bin` | any dump you own |
| Pokemon Mini | `pokemini` | `.min` | any dump you own |
| Pong Game Clone | `gong` | — | no content - it is a Pong clone |
| Quake Game Engine | `tyrquake` | `.pak` | **Quake shareware** `pak0.pak` - id's shareware episode is freely redistributable |
| Rick Dangerous Game Engine | `xrick` | `.zip` | **Rick Dangerous** data (`xrick/data.zip` from the xrick project) |
| SEGA Visual Memory Unit | `vemulator` | `.vms`, `.dci`, `.bin` | **Dreamcast VMU** `.vms` minigames - homebrew is free |
| SNK Neo Geo CD | `neocd` | `.cue`, `.chd` | any dump you own |
| Saturn | `mednafen_saturn` | `.ccd`, `.chd`, `.cue`, `.toc`, `.m3u`, `.zip` | any dump you own |
| Sega 8-bit | `smsplus` | `.sms`, `.bin`, `.rom`, `.col`, `.gg`, `.sg` | any dump you own |
| Sega 8-bit (MS/GG/SG-1000) | `gearsystem` | `.sms`, `.gg`, `.sg`, `.mv`, `.bin`, `.rom` | any dump you own |
| Sega 8/16-bit (Various) | `genesis_plus_gx` `genesis_plus_gx_wide` | `.mdx`, `.md`, `.smd`, `.gen`, `.bin`, `.cue`, `.iso`, `.sms` | Genesis homebrew and test ROMs are free |
| Sega Genesis | `clownmdemu` | `.bin`, `.md`, `.gen`, `.cue`, `.iso`, `.chd` | any dump you own |
| Sharp X1 | `x1` | `.dx1`, `.zip`, `.2d`, `.2hd`, `.tfd`, `.d88`, `.88d`, `.hdm` | any dump you own |
| Sharp X68000 | `px68k` | `.dim`, `.img`, `.d88`, `.88d`, `.hdm`, `.dup`, `.2hd`, `.xdf` | any dump you own |
| Super Nintendo Entertainment System | `bsnes_cplusplus98` `snes9x` `snes9x2002` `snes9x2005` `snes9x2005_plus` `snes9x2010` | `.sfc`, `.smc`, `.gb`, `.gbc`, `.st`, `.bs` | SNES test ROMs (blargg's) are free |
| Super Nintendo Entertainment System / Game Boy / Game Boy Color | `mesen-s` | `.sfc`, `.smc`, `.fig`, `.swc`, `.bs`, `.gb`, `.gbc` | any dump you own |
| Supervision | `potator` | `.bin`, `.sv` | any dump you own |
| TI83 | `numero` | `.8xp`, `.8xk`, `.8xg` | **TI-83** apps; boots without content |
| Tamagotchi P1 | `tamalibretro` | `.b`, `.bin`, `.rom` | **Tamagotchi P1** ROM - commercial dump |
| Thomson MO/TO | `theodore` | `.fd`, `.sap`, `.k7`, `.m7`, `.m5`, `.rom` | any dump you own |
| Uzebox | `uzem` | `.uze` | **Uzebox** `.uze` homebrew - free |
| Virtual Boy | `mednafen_vb` | `.vb`, `.vboy`, `.bin` | any dump you own |
| Wolfenstein 3D Game Engine | `ecwolf` | `.wl6`, `.n3d`, `.sod`, `.sdm`, `.wl1`, `.pk3`, `.exe` | `ecwolf.pk3` (ships with ECWolf) **plus** Wolf3D data; the **shareware** `*.wl1` set is free |
| WonderSwan/Color | `mednafen_wswan` | `.ws`, `.wsc`, `.pc2`, `.pcv2` | any dump you own |
| ZX Spectrum (various) | `fuse` | `.tzx`, `.tap`, `.z80`, `.rzx`, `.scl`, `.trd`, `.dsk`, `.dck` | any dump you own |
| bsnes_mercury | `bsnes_mercury` | — | boots with no content |
| doublecherrygb | `doublecherrygb` | — | boots with no content |

## ⚠ Two things this list cannot tell you

**A declared format is not a working format.** `supported_extensions` is what the core accepts on
a working platform. Compressed formats (`.zip`, `.chd`, `.7z`) lean on code paths this port has
never exercised, so if a core refuses a `.zip` try the bare file before concluding the core is
broken.

**Nothing here has been run.** Every core in this list built and linked and that is all that is
known about it - see `ps4/CORE-STATUS.md`, where every row still says `untested`.

