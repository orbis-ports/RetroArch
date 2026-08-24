# Core status on the PlayStation 4

One row per core that BUILT. The build says only that it compiled and linked; whether it runs,
draws, sounds right or holds a frame rate is what this table is for, and only the console can
answer it.

Fill in **Status** and **Notes** as you test. Suggested values, and none of them is a guess:

    untested   nobody has launched it
    boots      the core loads and the menu comes back
    plays      content runs and is playable
    slow       runs, below full speed - put the measured figure in Notes
    broken     loads and then fails - say how in Notes
    crash      takes the process down

`Built from` is the upstream commit the `.prx` was made from, so a regression can be bisected
against the same tree. Rebuild one with:

    ps4/build-cores.sh --recipe <libretro-super>/recipes/linux/cores-linux-x64-generic <core>


## ⚠ A prediction, from 17 results with no exception

Every core tested so far that is dominated by C++ has crashed. Every one that is C, or C with a
handful of C++ files, works.

    plays / boots        fceumm 0   snes9x2002 0   snes9x2005 0   snes9x2005_plus 0
                         snes9x2010 0   vba_next 0   cannonball 1   gpsp 2   2048 5
    crash                mednafen_gba 31   quicknes 36   snes9x 37   mesen 86
                         mesen-s 150   vbam 158   bsnes_cplusplus98 201   nestopia 291

(the number is `.cpp`/`.cc` files in the core's tree)

Seventeen for seventeen, and the boundary is sharp - nothing between 5 and 31 has been tested,
but nothing above 30 has survived and nothing below 6 has failed.

⚠ **AND THERE IS ONE COUNTEREXAMPLE THAT MATTERS: Beetle PSX HW.** It is heavily C++ - SPIRV-Cross
and parallel-psx - and it runs fine. The difference is not the language, it is that Beetle was
built through its own `orbis` platform arm while all hundred of these were built with
`platform=unix`. So the hypothesis is not "C++ crashes here"; it is **"something `platform=unix`
does to a C++ core is fatal, and the `orbis` arm did not do it."** Static initialisers,
exceptions, `__cxa_atexit` and the threading model are all candidates and all testable.

### The 24 untested cores this predicts will crash

`bsnes_mercury` `fbalpha2012` `same_cdi` `nekop2` `ecwolf` `nxengine` `puae` `x1` `dice` `desmume2015` `gearsystem` `gearboy` `fbalpha2012_cps1` `gme` `frodo` `neocd` `doublecherrygb` `mednafen_pce` `mednafen_pcfx` `fbalpha2012_neogeo` `gambatte` `mednafen_supergrafx` `reminiscence` `numero`

Testing them one by one confirms a pattern that is already 17 for 17. Two or three spot checks
are worth more than twenty-four, and the time saved is better spent on the one core that would
falsify it - a C++ core that works, or a C core that does not.

### ⚠ The crash column was reset on 2026-08-24, and the reason matters

Seven cores were recorded as `crash` before the cause was known. It was not in any of them: no
`.prx` on this console had ever run a global constructor, so every C++-dominant core met an
unconstructed global the first time it asked one for a size. `libretro-common/dynamic/dylib.c`
carries the full measurement.

All 100 have been relinked with `ps4/orbis-module.ld` and re-uploaded. Those seven are back to
`untested` because their old verdict was about the frontend, not about them - keeping it would
have left seven cores condemned for somebody else's bug.

## Built and on the console — 100 cores

| Core | Name | Size | Built from | Status | Notes |
|------|------|------|-----------|--------|-------|
| `2048` | 2048 | 732K | `39333f7` | plays | menu and game both fine |
| `a5200` | Atari - 5200 (a5200) | 1000K | `40c6f2f` | untested | |
| `atari800` | Atari - 400/800/600XL/800XL/130XE/5200 (Atari800) | 1,8M | `cd72179` | untested | |
| `bk` | Elektronika - BK-0010/BK-0011(M) | 864K | `fe64da4` | untested | |
| `bsnes_cplusplus98` | Nintendo - SNES / SFC (bsnes C++98 (v085)) | 1,9M | `4b97b39` | untested | was crash; relinked with ps4/orbis-module.ld |
| `bsnes_mercury` | bsnes_mercury | 3,2M | `ea22363` | untested | |
| `cannonball` | Cannonball | 1,1M | `0d83575` | untested | OutRun set now on the console at `roms/outrun/` with a dummy `cannonball.game` - load that file |
| `cap32` | Amstrad - CPC/GX4000 (Caprice32) | 2,1M | `4abfb8b` | untested | |
| `clownmdemu` | Sega - MD/CD (ClownMDEmu) | 1,6M | `935d6fc` | untested | |
| `crocods` | Amstrad - CPC (CrocoDS) | 1,3M | `a9c63b2` | untested | |
| `desmume2015` | Nintendo - DS (DeSmuME 2015) | 5,3M | `422b688` | untested | |
| `dice` | Arcade (DICE) | 8,6M | `b11714f` | untested | |
| `dinothawr` | Dinothawr | 1,9M | `601063d` | untested | |
| `doublecherrygb` | doublecherrygb | 2,2M | `1587acd` | untested | |
| `ecwolf` | Wolfenstein 3D (ECWolf) | 2,7M | `0cccd9a` | untested | |
| `fbalpha2012` | Arcade (FB Alpha 2012) | 28M | `0ce3153` | untested | |
| `fbalpha2012_cps1` | Arcade (FB Alpha 2012 CPS-1) | 4,3M | `5542c18` | untested | |
| `fbalpha2012_cps3` | Arcade (FB Alpha 2012 CPS-3) | 1,1M | `57f8015` | untested | |
| `fbalpha2012_neogeo` | Arcade (FB Alpha 2012 Neo Geo) | 3,3M | `e092097` | untested | |
| `fceumm` | Nintendo - NES / Famicom (FCEUmm) | 5,5M | `236ccdf` | plays | loads Zelda |
| `fmsx` | Microsoft - MSX (fMSX) | 908K | `f013e21` | untested | |
| `freechaf` | Fairchild - ChannelF (FreeChaF) | 756K | `76c7a84` | untested | |
| `freeintv` | Mattel - Intellivision (FreeIntv) | 1,4M | `ef3e0fe` | untested | |
| `frodo` | Commodore - C64 (Frodo) | 1,2M | `9fb971b` | untested | |
| `fuse` | Sinclair - ZX Spectrum (Fuse) | 2,4M | `2a5f1d4` | untested | |
| `gambatte` | Nintendo - Game Boy / Color (Gambatte) | 6,3M | `d9d6cd0` | untested | |
| `gearboy` | Nintendo - Game Boy / Color (Gearboy) | 1,7M | `58d7a8e` | untested | |
| `gearsystem` | Sega - MS/GG/SG-1000 (Gearsystem) | 1,8M | `b9c2e96` | untested | |
| `genesis_plus_gx` | Sega - MS/GG/MD/CD (Genesis Plus GX) | 18M | `b7e79b3` | plays |  |
| `genesis_plus_gx_wide` | Sega - MS/GG/MD/CD (Genesis Plus GX Wide) | 17M | `b7ad005` | plays | two images side by side - a widescreen variant that extends the H32/H40 area; unclear whether this is the port or the core's options, and not chased |
| `gme` | Game Music Emu | 1,3M | `1562f62` | untested | |
| `gong` | Gong | 524K | `69ef0b6` | untested | |
| `gpsp` | Nintendo - Game Boy Advance (gpSP) | 1,4M | `6b12231` | plays |  |
| `gw` | Handheld Electronic (GW) | 1,3M | `91d599b` | untested | |
| `handy` | Atari - Lynx (Handy) | 1,6M | `bc55d46` | untested | |
| `hatari` | Atari - ST/STE/TT/Falcon (Hatari) | 4,4M | `24e7bd7` | untested | |
| `jollycv` | ColecoVision/CreatiVision/My Vision (JollyCV) | 784K | `eb14292` | untested | |
| `jumpnbump` | Jump 'n Bump | 712K | `2cc8401` | untested | |
| `lowresnx` | LowRes NX | 664K | `12aeb16` | untested | |
| `lutro` | Lua Engine (Lutro) | 4,4M | `6224157` | untested | |
| `mame2000` | Arcade (MAME 2000) | 20M | `f099ba4` | untested | |
| `mame2003` | Arcade (MAME 2003) | 46M | `259339e` | untested | |
| `mame2003_plus` | Arcade (MAME 2003-Plus) | 50M | `6c514a3` | untested | |
| `mednafen_gba` | Nintendo - Game Boy Advance (Beetle GBA) | 1,1M | `bb9edd1` | untested | was crash; relinked with ps4/orbis-module.ld |
| `mednafen_lynx` | Atari - Lynx (Beetle Lynx) | 852K | `fcdefcf` | untested | |
| `mednafen_ngp` | SNK - Neo Geo Pocket / Color (Beetle NeoPop) | 1,1M | `a50d5ac` | untested | |
| `mednafen_pce` | NEC - PC Engine / SuperGrafx / CD (Beetle PCE) | 6,3M | `ae99235` | untested | |
| `mednafen_pce_fast` | NEC - PC Engine / CD (Beetle PCE FAST) | 5,4M | `bebe2b1` | untested | |
| `mednafen_pcfx` | NEC - PC-FX (Beetle PC-FX) | 2,8M | `0580dee` | untested | |
| `mednafen_psx` | Sony - PlayStation (Beetle PSX) | 14M | `ef51860` | untested | |
| `mednafen_psx_hw` | Sony - PlayStation (Beetle PSX HW) | 14M | `ef51860` | untested | |
| `mednafen_saturn` | Sega - Saturn (Beetle Saturn) | 12M | `ed549bd` | untested | |
| `mednafen_supergrafx` | NEC - PC Engine SuperGrafx (Beetle SuperGrafx) | 4,6M | `3c6fcd3` | untested | |
| `mednafen_vb` | Nintendo - Virtual Boy (Beetle VB) | 844K | `83ed426` | untested | |
| `mednafen_wswan` | Bandai - WonderSwan/Color (Beetle Wonderswan) | 1,9M | `4b01295` | untested | |
| `mesen` | Nintendo - NES / Famicom (Mesen) | 5,2M | `0102910` | plays | was the crash that found the constructor bug |
| `mesen-s` | Nintendo - SNES / SFC / Game Boy / Color (Mesen-S) | 4,2M | `9e4fdeb` | untested | was crash; relinked with ps4/orbis-module.ld |
| `mrboom` | Mr.Boom (Bomberman) | 9,2M | `40ac320` | untested | |
| `mu` | Palm OS (Mu) | 1,4M | `f9d34a0` | untested | |
| `nekop2` | NEC - PC-98 (Neko Project II) | 2,1M | `5fdbb21` | untested | |
| `neocd` | SNK - Neo Geo CD (NeoCD) | 2,9M | `8f2d42c` | untested | |
| `nestopia` | Nintendo - NES / Famicom (Nestopia) | 7,1M | `6c2d242` | untested | was crash; relinked with ps4/orbis-module.ld |
| `numero` | Texas Instruments TI-83 (Numero) | 1,5M | `c0b07a3` | untested | |
| `nxengine` | Cave Story (NXEngine) | 1,7M | `fd1c068` | untested | |
| `o2em` | Magnavox - Odyssey2 / Philips Videopac+ (O2EM) | 1,1M | `679d6fe` | untested | |
| `oberon` | Oberon RISC Emulator | 636K | `5be71d8` | untested | |
| `opera` | The 3DO Company - 3DO (Opera) | 1,3M | `a501a27` | untested | |
| `pocketcdg` | PocketCDG | 852K | `c6fc08d` | untested | |
| `pokemini` | Nintendo - Pokemon Mini (PokeMini) | 1,1M | `132111b` | untested | |
| `potator` | Watara - Supervision (Potator) | 556K | `227c5f6` | untested | |
| `prboom` | Doom (PrBoom) | 4,6M | `861959f` | untested | |
| `prosystem` | Atari - 7800 (ProSystem) | 876K | `8a88014` | untested | |
| `puae` | Commodore - Amiga (PUAE) | 24M | `96ebfcf` | untested | |
| `px68k` | Sharp - X68000 (PX68k) | 1,6M | `0ad84d7` | untested | |
| `quasi88` | NEC - PC-8000 / PC-8800 series (QUASI88) | 3,1M | `b5a0e04` | untested | |
| `quicknes` | Nintendo - NES / Famicom (QuickNES) | 2,3M | `26bb785` | untested | was crash; relinked with ps4/orbis-module.ld |
| `race` | SNK - Neo Geo Pocket / Color (RACE) | 1,3M | `c7810dd` | untested | |
| `reminiscence` | Flashback (REminiscence) | 1,2M | `e6c0b00` | untested | |
| `retro8` | PICO-8 (Retro8) | 1,7M | `ddc06a1` | untested | |
| `same_cdi` | Philips - CDi (SAME CDi) | 15M | `418be50` | untested | |
| `sameboy` | Nintendo - Game Boy / Color (SameBoy) | 1,1M | `aa158a8` | untested | |
| `smsplus` | Sega - MS/GG (SMS Plus GX) | 872K | `8a63f82` | untested | |
| `snes9x` | Nintendo - SNES / SFC (Snes9x) | 4,5M | `890b5d4` | untested | was crash; relinked with ps4/orbis-module.ld |
| `snes9x2002` | Nintendo - SNES / SFC (Snes9x 2002) | 1,5M | `5bd8bd6` | plays |  |
| `snes9x2005` | Nintendo - SNES / SFC (Snes9x 2005) | 1,5M | `deb49d8` | plays |  |
| `snes9x2005_plus` | Nintendo - SNES / SFC (Snes9x 2005 Plus) | 1,5M | `deb49d8` | plays |  |
| `snes9x2010` | Nintendo - SNES / SFC (Snes9x 2010) | 3,6M | `7db129b` | plays |  |
| `stella2014` | Atari - 2600 (Stella 2014) | 5,9M | `4a7da82` | untested | |
| `tamalibretro` | Bandai - Tamagotchi P1 (TamaLIBretro) | 856K | `ea8dd61` | untested | |
| `test` | Test | 524K | `caebf22` | untested | |
| `tgbdual` | Nintendo - Game Boy / Color (TGB Dual) | 732K | `0392c9c` | untested | |
| `theodore` | Thomson - MO/TO (Theodore) | 2,9M | `4d469ce` | untested | |
| `tyrquake` | Quake (TyrQuake) | 1,8M | `e57bb11` | untested | |
| `uzem` | Uzebox (Uzem) | 756K | `d991ee9` | untested | |
| `vba_next` | Nintendo - Game Boy Advance (VBA Next) | 1,6M | `2b96fd3` | plays |  |
| `vbam` | Nintendo - Game Boy Advance (VBA-M) | 2,1M | `115defb` | untested | was crash; relinked with ps4/orbis-module.ld |
| `vemulator` | VeMUlator | 756K | `27a062f` | untested | |
| `virtualjaguar` | Atari - Jaguar (Virtual Jaguar) | 3,6M | `8c758ff` | untested | |
| `x1` | Sharp X1 (X Millennium) | 1,1M | `3106aa5` | untested | |
| `xrick` | Rick Dangerous (XRick) | 1,1M | `fcfde36` | untested | |

## Did not build — 62 cores

These are not on the console. Grouped causes are in `ps4/HANDOFF.md`; the symbol below is
what the link or the compile actually said.

| Core | Stage | Detail |
|------|-------|--------|
| `81` | LINK | GetKeyState |
| `blastem` | COMPILE | no objects; single-shot link? |
| `bluemsx` | LINK | unzOpen2 |
| `boom3` | LINK | glGetError |
| `boom3_xp` | LINK | glDeleteTextures |
| `bsnes` | LINK | hiro::mObject::enabled(bool) const |
| `bsnes2014` | NO-ABI | linked without retro_run |
| `bsnes_hd_beta` | LINK | hiro::mObject::enabled(bool) const |
| `cdi2015` | LINK | link failed |
| `chailove` | LINK | PHYSFS_mountMemory |
| `chimerasnes` | COMPILE | clangclang: : error: error: unsupported argume |
| `citra` | LINK | sk_num |
| `craft` | LINK | glGetError |
| `daphne` | LINK | ace::ace() |
| `desmume` | LINK | glDeleteTextures |
| `dosbox_core` | LINK | link failed |
| `dosbox_svn` | LINK | SDL_GetTicks |
| `dosbox_svn_ce` | CLONE | Nie można sklonować „libretro/deps/sdl” drugi  |
| `emuscv` | LINK | cEmuSCV::RetroSetEnvironment(bool (*)(unsigned |
| `fbalpha2012_cps2` | LINK | BurnDrvCps1944j |
| `fbneo` | LINK | BurnSampleReset() |
| `freej2me` | NO-ABI | linked without retro_run |
| `geolith` | LINK | inflateInit2_ |
| `hbmame` | NO-ABI | linked without retro_run |
| `higan_sfc` | COMPILE | no objects; single-shot link? |
| `higan_sfc_balanced` | COMPILE | no objects; single-shot link? |
| `kronos` | LINK | glBindTexture |
| `mame` | NO-ABI | linked without retro_run |
| `mame2010` | LINK | osd_malloc |
| `mame2015` | LINK | link failed |
| `mame2016` | COMPILE | no objects; single-shot link? |
| `mednafen_supafaust` | COMPILE | mednafen/types.h:10:3: error: "Wrong include o |
| `melonds` | LINK | ARMJIT_Memory::ClassifyAddress9(unsigned int) |
| `meteor` | LINK | AMeteor::Memory::TimeEvent() |
| `mgba` | COMPILE | no objects; single-shot link? |
| `np2kai` | LINK | link failed |
| `openlara` | NO-ABI | linked without retro_run |
| `pcsx_rearmed` | LINK | cdrom_lba_to_msf |
| `picodrive` | LINK | mp3dec_start |
| `puzzlescript` | LINK | JS_ToInt32 |
| `redbook` | LINK | link failed |
| `remotejoy` | LINK | linux_usbfs_backend |
| `rustynes` | COMPILE | no objects; single-shot link? |
| `scummvm` | COMPILE | no objects; single-shot link? |
| `squirreljme` | COMPILE | no objects; single-shot link? |
| `stella` | NO-ABI | linked without retro_run |
| `uw8` | LINK | wasm_rt_trap |
| `vecx` | LINK | glIsEnabled |
| `vice_x128` | LINK | log_cb |
| `vice_x64` | LINK | retro_system_data_directory |
| `vice_x64sc` | LINK | opt_vkbd_alpha |
| `vice_xcbm2` | LINK | pix_bytes |
| `vice_xcbm5x0` | LINK | pix_bytes |
| `vice_xpet` | LINK | log_cb |
| `vice_xplus4` | LINK | input_state_cb |
| `vice_xscpu64` | LINK | opt_vkbd_alpha |
| `vice_xvic` | LINK | log_cb |
| `vitaquake2` | LINK | glGetError |
| `vitaquake3` | LINK | glGetError |
| `vitavoyager` | LINK | inet_htons |
| `yabasanshiro` | LINK | FLAC__SUBFRAME_LPC_QLP_SHIFT_LEN |
| `yabause` | LINK | FLAC__SUBFRAME_LPC_QLP_SHIFT_LEN |
