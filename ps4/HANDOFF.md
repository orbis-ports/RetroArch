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
