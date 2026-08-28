#!/usr/bin/env bash
# Build libretro cores for the PlayStation 4 - one, some, or all of them.
#
#   ps4/build-cores.sh --recipe <file> [options] [core]...
#
#   --recipe <file>   a libretro-super recipe (recipes/linux/cores-linux-x64-generic)
#   --work <dir>      clones, logs and intermediates      (default ~/.cache/ps4-cores)
#   --out <dir>       .prx and .info go here              (default <work>/out)
#   --patches <dir>   per-core patch tree                 (default ps4/core-patches)
#   --all             every GENERIC core in the recipe
#   --list            print what the recipe offers, and exit
#   --update          fetch and reset to the recipe's branch before building
#   --keep            incremental: do not reset, do not delete objects
#   --drop-clones     delete each core's clone once it is done with (default under CI)
#   --keep-clones     keep every clone (default interactively)
#   --jobs N          parallelism for each core's make
#   --core-timeout S  wall-clock cap per core in seconds; 0 disables   (default 1500)
#
# ⚠ ONE CORE IS THE NORMAL CASE, NOT THE SPECIAL ONE. `--all` exists for a sweep, but the thing
# this is used for afterwards is "upstream moved, rebuild gambatte" and "I have a fix for
# picodrive, try it". So a bare core name rebuilds exactly that core, in place, and the patch
# tree is where a fix lives - in THIS repository, versioned, rather than in a clone under /tmp
# that the next --update throws away.
#
#   ps4/build-cores.sh --recipe <r> gambatte              rebuild one, patches reapplied
#   ps4/build-cores.sh --recipe <r> --update gambatte      take upstream's newest first
#   ps4/build-cores.sh --recipe <r> --keep gambatte        incremental, patches left alone
#
# ⚠ AND IT DOES NOT PATCH MAKEFILES BEHIND YOUR BACK. The toolchain flags go INSIDE $(CC) and
# $(CXX) rather than into CFLAGS, because a libretro Makefile routinely does `CFLAGS := ...` and
# discards what the caller passed, while almost none of them rewrite CC. `platform=unix` then
# gives the core a sane arm to start from, its own link fails (host driver, -shared), and the
# objects are collected and linked here. A core that needs more than that gets a patch.
#
# ⚠ THE CLONES ARE HYGIENE, NOT SURVIVAL - THE PLAN'S DISK FIGURE IS WRONG. This work directory
# reaches 15 GB across the recipe, and the plan (and the text that stood here) said a
# GitHub-hosted runner has "about 14 GB free" - which would have made dropping the clones the
# difference between finishing the sweep and failing it. MEASURED, on every ubuntu-latest runner
# in run 32944175297 (2026-08-26), `df -h` printed:
#
#     /dev/root  145G  59G  86G  41% /          - and /mnt sits on that same filesystem
#
# 86 GB free against a 15 GB peak. So --drop-clones is HYGIENE: it keeps the tree small, keeps a
# rerun from tripping over a half-fetched clone, and keeps `du` legible. It is NOT what stands
# between a sweep and a full disk, and it stays on in CI anyway because nothing worth keeping
# lives in a clone - the logs, the .elf, the .prx and the manifest are all in $WORK itself.
#
# What actually held shard 0 open for 59 minutes was TIME: kronos sat in one `make` from 07:45:37
# to 08:43:01 and the runner was reclaimed underneath it. See --core-timeout below.
#
# ⚠ THE DEFAULT IS DECIDED BY $CI, NOT CHOSEN. Dropping cannot be the unconditional default -
# it would break the workflow this script is named after. `build-cores.sh --recipe <r> gambatte`
# is the normal case, --update fetches into an existing clone, and the patch loop re-applies
# against one; deleting it means every one-core rebuild re-clones, and the 15 GB that is a
# liability on a runner is the thing that makes iteration here quick. Nor can it be an opt-in
# flag alone, because the flag CI forgets is the flag CI does not have. GitHub Actions sets
# CI=true in every job, so the environment that needs the sweep gets it and this machine does
# not, and --drop-clones / --keep-clones override either way.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

for _c in "${ORBIS_COMPAT_DIR:-}" "$ROOT/../orbis-compat" "$HOME/src-ps4/orbis-compat"; do
  [[ -n "$_c" && -f "$_c/scripts/ps4/orbis-env.sh" ]] && { ORBIS_COMPAT_DIR="$_c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || { echo "build-cores: orbis-compat not found" >&2; exit 1; }
. "$ORBIS_COMPAT_DIR/scripts/ps4/orbis-env.sh"
TOOLCHAIN="$OO_PS4_TOOLCHAIN"

WORK="${HOME}/.cache/ps4-cores"; OUT=""; RECIPE=""
PATCHES="$HERE/core-patches"
JOBS="$(nproc)"; ALL=0; LIST=0; UPDATE=0; KEEP=0
# 1500s = 25 min. Chosen from measured data, not from taste - see the block above capped().
CORE_TIMEOUT="${PS4_CORE_TIMEOUT:-1500}"
CORE_TIMEOUT_KILL=30      # grace between the TERM and the KILL that follows it
DROP_CLONES=-1        # -1: decide from $CI below.  0: keep.  1: drop.
CORES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --recipe)  RECIPE="$2";  shift 2 ;;
    --work)    WORK="$2";    shift 2 ;;
    --out)     OUT="$2";     shift 2 ;;
    --patches) PATCHES="$2"; shift 2 ;;
    --jobs|-j) JOBS="$2";    shift 2 ;;
    --core-timeout) CORE_TIMEOUT="$2"; shift 2 ;;
    --all)     ALL=1;        shift ;;
    --list)    LIST=1;       shift ;;
    --update)  UPDATE=1;     shift ;;
    --keep)    KEEP=1;       shift ;;
    --drop-clones) DROP_CLONES=1; shift ;;
    --keep-clones) DROP_CLONES=0; shift ;;
    -*) echo "build-cores: unknown argument: $1" >&2; exit 2 ;;
    *) CORES+=("$1"); shift ;;
  esac
done
[[ -n "$RECIPE" && -f "$RECIPE" ]] || { echo "build-cores: --recipe <file> is required" >&2; exit 2; }
case "$CORE_TIMEOUT" in
  ''|*[!0-9]*) echo "build-cores: --core-timeout wants whole seconds, got '$CORE_TIMEOUT'" >&2; exit 2 ;;
esac
if [[ "$CORE_TIMEOUT" -ne 0 ]] && ! command -v timeout >/dev/null; then
  echo "build-cores: coreutils timeout is missing; the per-core cap is OFF" >&2
  CORE_TIMEOUT=0
fi
OUT="${OUT:-$WORK/out}"

if [[ $DROP_CLONES -eq -1 ]]; then
  case "${CI:-}" in true|TRUE|1) DROP_CLONES=1 ;; *) DROP_CLONES=0 ;; esac
fi
# --keep is the incremental edit-compile loop; deleting the tree it is incremental against is
# not a combination that means anything. It wins.
[[ $KEEP -eq 1 ]] && DROP_CLONES=0

# ⚠ THE RECIPE IS EXTENDED BY ps4/core-recipe-extra, AND IT IS CHECKED FIRST.
#
# The upstream recipe is fetched from libretro-super, so a core it omits is never attempted here -
# not tried and rejected, never tried at all. dosbox_pure is the case that made this necessary: it
# is absent from the recipe, builds first try with no patches, and is the only working DOS core
# for this port, while all three the recipe DOES list fail. The extra file is searched before the
# recipe so it can also correct a line that has gone stale.
RECIPE_EXTRA="$HERE/core-recipe-extra"
recipe_line() { # core -> its line, ours first
  local line=""
  [[ -f "$RECIPE_EXTRA" ]] \
    && line="$(awk -v c="$1" '!/^[[:space:]]*(#|$)/ && $1==c {print; exit}' "$RECIPE_EXTRA")"
  [[ -n "$line" ]] && { printf '%s\n' "$line"; return; }
  awk -v c="$1" '!/^[[:space:]]*(#|$)/ && $1==c {print; exit}' "$RECIPE"
}

if [[ $LIST -eq 1 ]]; then
  awk '!/^[[:space:]]*(#|$)/ {printf "%-26s %s\n", $1, $6}' "$RECIPE" ${RECIPE_EXTRA:+"$RECIPE_EXTRA"} | sort
  exit 0
fi
# ⚠ --all TAKES CMAKE TOO, AND LEAVING IT OUT COST A RELEASE. The exclusion was deliberate while
# this port had no CMake toolchain file - it has one now - but it outlived its reason by exactly
# one release: run 33211929629 swept, went green, and published the same 101 cores as before,
# silently omitting swanstation, arduous and thepowdertoy, all three of which had built by hand
# that day. A core that builds and reaches no index is indistinguishable from one that does not
# build at all.
#
# The cost this used to worry about is real but bounded: every core, CMAKE included, is capped by
# --core-timeout, and one that fails records a verdict, which is data rather than damage.
# ⚠ ps4/shard-cores.sh MUST DRAW FROM THE SAME TWO FILES AND THE SAME TWO BUILD TYPES. Where the
# sweep and the sharder disagree, the difference is a core nobody ever sees.
if [[ $ALL -eq 1 ]]; then
  mapfile -t CORES < <(awk '!/^[[:space:]]*(#|$)/ && ($6=="GENERIC" || $6=="CMAKE") {print $1}' \
      "$RECIPE" ${RECIPE_EXTRA:+"$RECIPE_EXTRA"} | sort -u)
fi
[[ ${#CORES[@]} -gt 0 ]] || { echo "build-cores: name a core, or pass --all" >&2; exit 2; }

mkdir -p "$WORK" "$OUT"

# ⚠ -nostdsysteminc: THE BUILD HOST'S HEADERS ARE NOT PART OF THIS PLATFORM.
# -isysroot does NOT stop clang searching /usr/include. Measured directly:
#
#     clang --target=x86_64-pc-freebsd12-elf -isysroot $TOOLCHAIN ... -E
#       -> # 1 "/usr/include/GLES3/gl3.h"
#
# So a header the SDK, the overlay and Mesa all lack is silently taken from the machine doing
# the building - this Linux desktop - and the result compiles, links and runs. That is how
# mupen64plus_next built here for weeks and never once in CI, where there is no libgles-dev to
# fall back to. The accident held only because those particular headers are Khronos's and
# near-identical; nothing guarantees the next one will be.
#
# ⚠ -nostdinc, NOT -nostdsysteminc. The latter is a -cc1 flag the driver rejects, and
# `-Xclang -nostdsysteminc` is accepted while doing nothing - measured, the host GLES header
# still resolved through it. -nostdinc also drops clang's own builtin directory, so that one is
# added back explicitly, and LAST: ahead of the SDK it would answer <stddef.h> from clang's copy
# instead of the SDK's and quietly change what this port is built against.
ORBIS_ARCH=(--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables
            -isysroot "$TOOLCHAIN" -nostdinc
            -DORBIS -D__ORBIS__ -D__PS4__ -DPS4 -D_BSD_SOURCE=1)

# ⚠ THE INCLUDE ORDER IS NOT A PREFERENCE, AND C++ NEEDS A DIFFERENT ONE FROM C.
#
# C: the overlay ahead of the SDK, so its corrected constants win - MAP_ANON is 0x1002 on this
# kernel and the SDK's musl header says 0x0020.
#
# C++: libc++ FIRST, then the overlay, then the SDK. Getting this wrong is not a link error, it
# is `cmath:341: no member named 'abs' in the global namespace` - libc++'s <cmath> hoists the C
# library's abs into std:: and needs the C headers underneath it. This file had c++/v1 appended
# LAST on its first draft, which is the trap Makefile.orbis and ps4-mesa-docs docs/retroarch/HANDOFF.md both already
# describe. Writing a new tool next to a documented trap is not protection from it.
# ⚠ THE GL HEADERS COME FROM MESA, AND WITHOUT THEM clang QUIETLY USES THE BUILD HOST'S.
#
# GLES3/gl3.h and its EGL neighbours live in the Mesa tree - $(ORBIS_MESA_SRC)/include, the same
# path Makefile.orbis:392 gives the frontend. They are NOT in the SDK and NOT in the overlay.
#
# Leaving them out does not fail on a developer machine, which is the whole problem. Measured:
#
#     clang --target=x86_64-pc-freebsd12-elf -isysroot $TOOLCHAIN ... -E
#       -> # 1 "/usr/include/GLES3/gl3.h"
#
# -isysroot does not stop clang falling back to /usr/include, so every GL core built here has
# been compiling against this Linux desktop's GLES headers. It works, because those headers are
# Khronos's and largely identical - it is an accident that happens to hold. On a runner with no
# libgles-dev there is nothing to fall back to and the core fails with 'GLES3/gl3.h' file not
# found, which is how mupen64plus_next has never once been in a release while building fine
# on this machine.
MESA_INCLUDES=()
if [[ -n "${ORBIS_MESA_SRC:-}" && -f "$ORBIS_MESA_SRC/include/GLES3/gl3.h" ]]; then
  MESA_INCLUDES=(-isystem "$ORBIS_MESA_SRC/include")
else
  echo "build-cores: ⚠ ORBIS_MESA_SRC is unset or has no GLES headers - GL cores will fall back" >&2
  echo "             to whatever this machine has in /usr/include, or fail on a machine that" >&2
  echo "             has none. Point it at a Mesa tree or bundle." >&2
fi

CLANG_RESOURCE_INC="$(clang -print-resource-dir)/include"

C_INCLUDES=(-isystem "$ORBIS_COMPAT_DIR/include" "${MESA_INCLUDES[@]}"
            -isystem "$TOOLCHAIN/include" -isystem "$CLANG_RESOURCE_INC")
CXX_INCLUDES=(-isystem "$TOOLCHAIN/include/c++/v1"
              -isystem "$ORBIS_COMPAT_DIR/include" "${MESA_INCLUDES[@]}"
              -isystem "$TOOLCHAIN/include" -isystem "$CLANG_RESOURCE_INC"
              -include orbis_prefix.h)
CC_ORBIS="clang ${ORBIS_ARCH[*]} ${C_INCLUDES[*]}"
CXX_ORBIS="clang++ ${ORBIS_ARCH[*]} ${CXX_INCLUDES[*]}"

# ⚠ THE CMake TOOLCHAIN FILE IS GENERATED HERE RATHER THAN CHECKED IN, AND THAT IS THE POINT.
#
# A core built through CMake must see the SAME target, the SAME sysroot and the SAME include order
# as one built through make - otherwise this harness has two definitions of what this platform is,
# and the second one will drift out of date silently, which is the failure mode every ⚠ above is
# about. So the file is written from $ORBIS_ARCH and $C_INCLUDES/$CXX_INCLUDES, the arrays that
# already decided it, and there is nothing to keep in step.
#
# ⚠ IT IS NOT orbis-compat/cmake/ps4-openorbis.cmake AND MUST NOT BE. That file is for
# EXECUTABLES - Tempest, OpenGothic, VK-GL-CTS. It links crt1.o, puts the overlay on the link line
# with --whole-archive, and forces BUILD_SHARED_LIBS OFF. A libretro core is none of those things:
# its own link is EXPECTED to fail, the objects are collected and linked below against the module
# script, and crt1.o in the middle of that would be an entry point in a thing that has none.
# It also does not pass -nostdinc, so it reaches this desktop's /usr/include - see the block above.
#
# ⚠ FreeBSD, NOT Generic, AS THE SYSTEM NAME. Generic leaves UNIX unset, and a libretro
# CMakeLists.txt routinely branches on if(UNIX) for its threading, its dynamic loader and its
# endianness - taking the Windows arm instead is not a build failure, it is a wrong build. The
# triple is x86_64-pc-freebsd12-elf, so FreeBSD is also the truthful answer.
#
# ⚠ try_compile LINKS A REAL EXECUTABLE HERE, AND THE SHORTCUT THAT AVOIDS THAT IS A TRAP.
#
# CMake proves a compiler works by linking a little executable, and the documented escape for a
# target with no runnable link is CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY. It configures, and
# it is wrong twice over:
#
#   * NOTHING LINKS, SO EVERY check_function_exists() SAYS YES. A core asking whether shm_open or
#     posix_memalign exists gets told yes because the reference compiled, and finds out at the real
#     link - or does not, and quietly configures a code path this platform has no symbol for.
#   * CMake FEEDS THE EXECUTABLE LINKER FLAGS TO THE ARCHIVER. The static library rule is
#     "<CMAKE_AR> qc <TARGET> <LINK_FLAGS> <OBJECTS>" and llvm-ar reads a leading word as operation
#     LETTERS, so yaps2's own compiler-flag probes came out as
#         /usr/bin/llvm-ar: error: unknown option n      <- a -n... flag
#         /usr/bin/llvm-ar: error: unknown option f      <- a -f... flag
#     reported as a COMPILE failure in a try_compile, about as far from the cause as a message gets.
#
# So the link line below is the real one - the SDK's crt1.o, its libraries, and this overlay's
# corrected linker script - and CMake's checks answer truthfully. It is the same recipe as
# orbis-compat/cmake/ps4-openorbis.cmake, which is the file this port uses for EXECUTABLES; a core
# is not one, and its own link is still discarded, but a try_compile IS an executable and wants an
# executable's link line.
CMAKE_TOOLCHAIN="$WORK/orbis-core.cmake"
# Only if it has been built - a missing archive would fail every check for a reason none of them
# are about, and the overlay carries interposers rather than symbols the checks need.
ORBIS_COMPAT_LINK=""
[[ -f "$ORBIS_COMPAT_DIR/build/liborbis-compat.a" ]] \
  && ORBIS_COMPAT_LINK="-L$ORBIS_COMPAT_DIR/build -lorbis-compat"
cat > "$CMAKE_TOOLCHAIN" <<EOF
# GENERATED by ps4/build-cores.sh on each run - edit that script, not this file.
set(CMAKE_SYSTEM_NAME      FreeBSD)
set(CMAKE_SYSTEM_VERSION   12)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_AR       $(command -v llvm-ar)       CACHE FILEPATH "")
set(CMAKE_RANLIB   $(command -v llvm-ranlib)   CACHE FILEPATH "")
set(CMAKE_NM       $(command -v llvm-nm)       CACHE FILEPATH "")
set(CMAKE_OBJDUMP  $(command -v llvm-objdump)  CACHE FILEPATH "")

set(CMAKE_C_FLAGS_INIT   "${ORBIS_ARCH[*]} ${C_INCLUDES[*]}")
set(CMAKE_CXX_FLAGS_INIT "${ORBIS_ARCH[*]} ${CXX_INCLUDES[*]}")
set(CMAKE_ASM_FLAGS_INIT "${ORBIS_ARCH[*]} ${C_INCLUDES[*]}")

# The executable link line, which is what CMake's own checks use. --script is this overlay's
# corrected copy of the SDK's link.x and --no-rosegment keeps the loader from rejecting the image;
# orbis-compat/cmake/ps4-openorbis.cmake carries both reasons in full. crt1.o goes LAST, the order
# the SDK's own link rule uses.
#
# ⚠ THE CORE'S OWN SHARED/MODULE LINK IS STILL EXPECTED TO FAIL and is left alone deliberately -
# build-cores.sh collects the objects and links them against ps4/orbis-module.ld itself.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 -Wl,--script=$ORBIS_COMPAT_DIR/cmake/orbis-tls.ld -Wl,--eh-frame-hdr -Wl,--no-rosegment -L$TOOLCHAIN/lib $ORBIS_COMPAT_LINK")
set(CMAKE_C_STANDARD_LIBRARIES   "-lc -lkernel -lc++ $TOOLCHAIN/lib/crt1.o")
set(CMAKE_CXX_STANDARD_LIBRARIES "-lc -lkernel -lc++ $TOOLCHAIN/lib/crt1.o")

# NEVER for programs: a core's build may want pkg-config, python or a shader compiler, and those
# are the HOST's. ONLY for libraries and headers: anything found outside the sysroot would be this
# desktop's, which is the whole reason -nostdinc is on the compile line.
set(CMAKE_FIND_ROOT_PATH "$TOOLCHAIN;$ORBIS_COMPAT_DIR")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

# ⚠ CHECK THE FILE THAT WAS JUST WRITTEN, BECAUSE AN EMPTY ONE IS NOT AN ERROR ANYWHERE ELSE.
#
# The heredoc above is UNQUOTED - it has to be, that is how the flag arrays reach it - so every
# backtick and every $ in it is live. A pair of backticks in one of its comments turned the whole
# body into a command substitution and left this file ZERO BYTES, and nothing said so: CMake reads
# an empty toolchain file happily, falls back to /usr/bin/c++ and the host's headers, and builds a
# core for THIS DESKTOP that ld.lld then links and create-fself then accepts, because the host and
# the console are both x86-64. The verdict was OK. The tell was in the link of the NEXT core:
#
#     undefined symbol: std::cerr        _ZSt4cerr, which is libstdc++'s mangling
#                                        libc++.a has _ZNSt3__14cerrE
#
# So a green core built against the wrong C++ library entirely. Anything written by an unquoted
# heredoc has to be looked at afterwards.
if ! grep -q CMAKE_CXX_FLAGS_INIT "$CMAKE_TOOLCHAIN"; then
  echo "build-cores: the generated $CMAKE_TOOLCHAIN is empty or truncated." >&2
  echo "   An unquoted heredoc wrote it - check it for backticks and for a stray \$." >&2
  exit 1
fi

# ⚠ THE libretro ABI HAS TO BE NAMED OR LTO THROWS IT AWAY, AND THE BUILD STILL SUCCEEDS.
#
# Several cores compile with -flto under platform=unix, so their .o files are LLVM bitcode -
# llvm-nm prints dashes where the address would be. ld.lld links bitcode, runs LTO, and
# internalises everything unreachable from an entry point. A module has no entry point, so
# "everything" is everything: snes9x2010 linked to a 915 KiB ELF with ZERO retro_* symbols and
# ld.lld reported success. Naming them with -u makes them GC roots.
LIBRETRO_ABI=(retro_init retro_deinit retro_api_version
              retro_get_system_info retro_get_system_av_info
              retro_set_environment retro_set_video_refresh
              retro_set_audio_sample retro_set_audio_sample_batch
              retro_set_input_poll retro_set_input_state
              retro_set_controller_port_device retro_reset retro_run
              retro_serialize_size retro_serialize retro_unserialize
              retro_cheat_reset retro_cheat_set
              retro_load_game retro_load_game_special retro_unload_game
              retro_get_region retro_get_memory_data retro_get_memory_size)
KEEP_SYMS=()
for _s in "${LIBRETRO_ABI[@]}"; do KEEP_SYMS+=(-u "$_s"); done

# ⚠ THE FRONTEND'S libretro-common, AS AN ARCHIVE, AFTER the core's own objects.
#
# Cores call retro_vfs_*_impl and cdrom_* without building them, because on a normal platform the
# shared object resolves them lazily against the frontend at load time. A .prx has its own symbol
# table and cannot. An ARCHIVE and not a list of objects, deliberately: members are pulled only
# for symbols still undefined, so a core that DID build its own copy keeps it and nothing is
# duplicated. The frontend's copy is also the one fixed for this console.
COMMON_LIB="$WORK/liborbis-retro-common.a"
if [[ ! -f "$COMMON_LIB" ]]; then
  mapfile -t _common < <(find "$ROOT/libretro-common" -name '*.o' -type f | sort)
  if [[ ${#_common[@]} -eq 0 ]]; then
    echo "build-cores: no libretro-common objects in $ROOT - build the frontend first" >&2
    exit 1
  fi
  _extra="$WORK/common-extra"; mkdir -p "$_extra"
  _built=0; _failed=0
  while IFS= read -r csrc; do
    [[ -f "${csrc%.c}.o" ]] && continue
    _obj="$_extra/$(echo "${csrc#$ROOT/libretro-common/}" | tr '/' '_')"; _obj="${_obj%.c}.o"
    if [[ ! -f "$_obj" ]]; then
      if $CC_ORBIS -O2 -I"$ROOT/libretro-common/include" -c -o "$_obj" "$csrc" >/dev/null 2>&1
      then _built=$((_built+1)); else rm -f "$_obj"; _failed=$((_failed+1)); continue; fi
    fi
    _common+=("$_obj")
  done < <(find "$ROOT/libretro-common" -name '*.c' -type f | sort)
  llvm-ar rcs "$COMMON_LIB" "${_common[@]}"
  echo "== libretro-common fallback: ${#_common[@]} objects ($_built built here, $_failed would not)"
fi

# ⚠ A PLAIN OBJECT, NOT AN ARCHIVE MEMBER, AND THE DIFFERENCE IS THE WHOLE POINT. An UNDEFINED
# WEAK symbol does not trigger archive extraction - that is what weak means, so putting these
# stubs in liborbis-retro-common.a left every reference exactly as undefined as before and
# create-fself went on refusing the module. Passed directly, the definitions are always present.
# See ps4/orbis_weak_stubs.c for why create-fself cares when the linker does not.
# ⚠ THE LINKER SCRIPT IS OURS, NOT THE TOOLCHAIN'S, AND ps4/orbis-module.ld SAYS WHY IN FULL.
# The short version: OpenOrbis's link.x collects .init_array and defines no symbols around it,
# while crtlib.o carries __init_array_start/__init_array_end as BSS VARIABLES eight bytes apart.
# A module linked with the stock script therefore reports an empty constructor list.
WEAK_STUBS="$WORK/orbis_weak_stubs.o"
if [[ ! -f "$WEAK_STUBS" ]]; then
  $CC_ORBIS -O2 -c -o "$WEAK_STUBS" "$HERE/orbis_weak_stubs.c" \
    || { echo "build-cores: could not build the weak stubs" >&2; exit 1; }
fi

# ⚠ AN ARCHIVE, AND FOR THE OPPOSITE REASON TO THE ONE ABOVE. The weak stubs must be present
# unconditionally; these must NOT be. orbis_gl_forward.c defines a hundred and thirty-three GLES and EGL
# entry points and orbis_exec_mem.c defines three, and a core that references neither has no
# business carrying them. Archive members are pulled only for symbols still undefined, so each core
# takes exactly what it asked for and nothing else.
#
# The extraction works here where it failed for the weak stubs because these references are STRONG:
# glsm calls glBindFramebuffer because it means to. See ps4/orbis_gl_forward.c, which forwards to
# the frontend's context rather than answering itself, and ps4/orbis_exec_mem.c.
#
# ⚠ AND ITS POSITION ON THE LINK LINE IS LEAD, NOT DECORATION. orbis_abort_report.c does not add a
# missing symbol - it REPLACES two the toolchain has, abort_message from libc++.a and __assert_fail
# from libc.a, so that a core's dying words reach a channel this console actually reads. That works
# only while this archive is searched before -lc and -lc++, which is how the ld.lld line below is
# ordered. Move it after them and the override silently stops happening.
CORE_SUPPORT_LIB="$WORK/liborbis-core-support.a"
if [[ ! -f "$CORE_SUPPORT_LIB" ]]; then
  _sup=()
  for _s in orbis_gl_forward.c orbis_exec_mem.c orbis_abort_report.c orbis_profile.c orbis_cv_fix.cpp; do
    # ⚠ orbis_cv_fix REPLACES A MEMBER OF libc++.a AND SO MUST BE COMPILED THE SAME WAY THE CORES
    # ARE - libc++'s own headers first. It defines std::condition_variable's members, so a
    # different <condition_variable> than the cores see would be a different class.
    case "$_s" in
      *.cpp) _cc="$CXX_ORBIS -std=gnu++11" ;;
      *)     _cc="$CC_ORBIS" ;;
    esac
    $_cc -O2 -c -o "$WORK/${_s%.*}.o" "$HERE/$_s" \
      || { echo "build-cores: could not build $_s" >&2; exit 1; }
    _sup+=("$WORK/${_s%.*}.o")
  done
  llvm-ar rcs "$CORE_SUPPORT_LIB" "${_sup[@]}"
fi

# ⚠ SOME CORES NEED FLAGS THE RECIPE DOES NOT CARRY, and one needs its build type overruled.
#
# The recipe's build type is about the machine libretro-super was written for, not about this one.
# mupen64plus_next is GENERIC_GL there, and that turned out to be right for this console too -
# but only after the frontend grew an OpenGL context driver. Until then the flags below asked for
# paraLLEl-RDP over Vulkan, which was the only renderer that could run at all.
#
# ⚠ THAT IS NO LONGER TRUE AND THE FLAGS OUTLIVED IT. Measured on hardware, Shadows of the
# Empire: GLideN64 with the HLE RSP is 60.41 fps and 5.2 ms of work in a 16.7 ms frame;
# paraLLEl-RDP is 43 ms a frame. And the paraLLEl configuration does not even link here -
# HAVE_PARALLEL_RDP=1 drops libretro-common/glsm from the build while GLideN64's sources, which
# are compiled either way, still call glsm_ctl. It built on this machine only because a clone
# from an earlier GLideN64 run still had glsm.o lying in it; a fresh clone in CI reported
# "undefined symbol: glsm_ctl" and mupen64plus_next has never been in a release.
#
# So: no HAVE_PARALLEL_RDP, which is also what makes the patch tree's fallback choose
# RDP_PLUGIN_GLIDEN64 (see 0002, guarded on that same define), and no LLE, because the HLE RSP is
# what GLideN64 draws from - paraLLEl-RDP implements only the low-level entry point and pairs
# with an HLE RSP as a black screen.
#
# Anything listed here is built whatever its build type says. The flags are the recipe's own for
# that core, minus the ones platform=unix already sets.
core_make_flags() {
  case "$1" in
    mupen64plus_next) echo "HAVE_THR_AL=1 WITH_DYNAREC=x86_64 FORCE_GLES3=1" ;;
    parallel_n64)     echo "HAVE_PARALLEL=1 HAVE_PARALLEL_RSP=1 HAVE_THR_AL=1 WITH_DYNAREC=x86_64" ;;
    *)                echo "" ;;
  esac
}

# ⚠ AND THE SAME FOR CMake CORES, WHERE THE RECIPE'S ARGUMENTS ARE WRITTEN FOR A LINUX DESKTOP.
#
# Appended after the recipe's own -D arguments, so these win.
#
# play: USE_GLES=ON. Left to its default, deps/Framework/build_cmake/FrameworkOpenGl decides GLES
# by platform name - Android, iOS, ARM, Emscripten - and this console is none of them, so it takes
# the desktop arm: find_package(GLEW), then a bundled glew-2.0.0 whose CMakeLists does
# find_package(OpenGL REQUIRED), which wants GLX. That is what stopped the core configuring:
#
#     Could NOT find OpenGL (missing: OPENGL_opengl_LIBRARY OPENGL_glx_LIBRARY OPENGL_INCLUDE_DIR)
#     deps/Dependencies/glew-2.0.0/CMakeLists.txt:20 (find_package)
#
# ⚠ IT IS NOT A WORKAROUND, IT IS THE TRUTHFUL ANSWER. This port has no desktop GL and no GLX; it
# has GLES 3.1 through zink, which is exactly the arm USE_GLES selects - no glew, no
# find_package(OpenGL), and GLESv2 on the link line. The variable is a CACHE BOOL, so a -D wins.
# ⚠ BUILD THE CORE'S TARGET, NOT `all` - A CMake TREE SHIPS PROGRAMS AS WELL AS THE CORE.
#
# Everything under the build directory is swept up for the link, and a source tree that also
# builds tools and test suites contributes their entry points. play, from `all`:
#
#     ld.lld: error: duplicate symbol: main
#       >>> .../CodeGen/CMakeFiles/CodeGenTestSuite.dir/tests/Main.cpp.o
#       >>> .../tools/NamcoSys147NANDTools/CMakeFiles/NamcoSys147NANDTools.dir/Main.cpp.o
#
# and the recipe already passes -DBUILD_TESTS=no, which those two did not honour. libretro CMake
# cores name their target <core>_libretro - the same convention this file already relies on for
# the .prx filename - so when that target exists it is the one to build, and `all` is the fallback
# for a core that names its target something else.
#
# ⚠ AND NO `grep -q` IN THE PIPELINE, for the reason this file already records at the retro_run
# check: grep -q exits on its first match, cmake gets SIGPIPE, and under `set -o pipefail` the
# PIPELINE fails even though the target was found. Written that way first, it answered `all` every
# time and the duplicate `main` above never went away.
cmake_build_target() { # -> the target to build, from $core and $cbuild in scope
  local targets
  targets="$(cmake --build "$cbuild" --target help 2>/dev/null)"
  if [[ "$targets" == *"... ${core}_libretro"$'\n'* ]]; then
    echo "${core}_libretro"
  else
    echo all
  fi
}

core_cmake_flags() { # core -> extra -D arguments, appended after the recipe's
  case "$1" in
    play) echo "-DUSE_GLES=ON" ;;
    *)    echo "" ;;
  esac
}

# ⚠ A PER-CORE WALL-CLOCK CAP, BECAUSE ONE CORE MUST NOT SPEND THE WHOLE SHARD'S BUDGET.
#
# Run 32944175297, shard 0: frodo finished at 07:45:37 and the NEXT line in the log is at
# 08:43:01. kronos held one `make` open for 57 minutes, the runner was reclaimed underneath the
# job ("The runner has received a shutdown signal"), and fourteen other cores in that shard - and
# the publish job behind them - were lost to one core that would not stop.
#
# ⚠ THE NUMBER COMES FROM THE MEASUREMENT, NOT FROM TASTE. Timing every core in all eight shards
# of that run (the gaps between consecutive report lines) puts the slowest SUCCESSFUL core at
# mednafen_saturn, 831s. Next behind it: same_cdi 204s, mame2003 105s, puae 99s. The slowest
# FAILING core that failed honestly was mame2015 at 245s. So the whole recipe fits under ~14
# minutes per core, with one core defining that ceiling on its own.
#
#     1500s = 25 min = 1.8x mednafen_saturn.
#
# The headroom is for a slower runner and a cold clone, not for a second mednafen_saturn - and it
# is still 1/2.3 of what kronos spent. --core-timeout 0 turns it off for a hand-run build.
#
# ⚠ AND IT KILLS THE PROCESS GROUP, NOT ONLY THE CHILD. `make -j4` leaves clang++ processes
# behind: the same log ends with the runner reaping "orphan process: pid (6929) (make)" and three
# stray clang++. GNU timeout WITHOUT --foreground puts itself and the command in a fresh process
# group and signals the whole group on expiry; with --foreground it signals the command alone.
# Measured here against a fake Makefile whose recipe backgrounds a `sh -c sleep 300`:
#
#     plain timeout        -> rc=124, 0 survivors
#     timeout --foreground -> rc=124, 3 survivors
#
# So: no --foreground, ever, and -k to follow the TERM with a KILL for anything that ignores it.
#
# ⚠ AND THE SAME NEW PROCESS GROUP HAS TO BE KILLED BY HAND WHEN THE *RUNNER* CANCELS US. That
# group is what insulates make from a signal aimed at this script's group - which is the whole
# point on expiry and exactly wrong on a cancel, where make would be left running until the
# runner's own orphan reaper found it. So the pid is written down while the command runs and
# on_signal() below kills its group. Backgrounding it and waiting is not decoration either: bash
# defers a trap until the current FOREGROUND command returns, so a script sitting in a 25-minute
# `make` would not run its handler until that make was done.
CORE_DEADLINE=0
CAPPED_PID_FILE=""
capped() { # capped <cmd>... - run it inside what is left of this core's budget
  if [[ "$CORE_TIMEOUT" -eq 0 ]]; then "$@"; return; fi
  local left=$(( CORE_DEADLINE - SECONDS ))
  [[ "$left" -gt 0 ]] || return 124
  timeout -k "$CORE_TIMEOUT_KILL" -s TERM "$left" "$@" &
  local tpid=$! rc
  printf '%s\n' "$tpid" > "$CAPPED_PID_FILE"
  wait "$tpid"; rc=$?
  : > "$CAPPED_PID_FILE"
  return "$rc"
}
# ⚠ THE GUARD IS pgid == pid, AND IT IS NOT PARANOIA - THIS IS `kill` ON A NEGATIVE NUMBER. GNU
# timeout makes itself the LEADER of the group it creates, so its pgid equals its pid; a pid that
# has been recycled into something else almost never satisfies that, and the file is emptied the
# moment each capped command returns.
# shellcheck disable=SC2329  # run from the traps below, which shellcheck cannot follow
kill_capped() {
  local p g
  p="$(cat "$CAPPED_PID_FILE" 2>/dev/null)" || return 0
  [[ "$p" =~ ^[0-9]+$ ]] || return 0
  g="$(ps -o pgid= -p "$p" 2>/dev/null | tr -d ' ')"
  [[ "$g" == "$p" ]] || return 0
  kill -TERM "-$g" 2>/dev/null
  sleep 2
  kill -KILL "-$g" 2>/dev/null
  : > "$CAPPED_PID_FILE"
}
# 124 is timeout's own verdict; 128+9 is what it reports when -k had to escalate to SIGKILL.
timed_out() { [[ "$1" -eq 124 || "$1" -eq 137 ]]; }

MANIFEST="$OUT/cores.manifest"
: > "$MANIFEST.new"
CAPPED_PID_FILE="$WORK/.capped.pid"
: > "$CAPPED_PID_FILE"

# ⚠ MERGE ON THE WAY OUT, NOT AT THE END OF THE LOOP. A sweep of 162 cores takes hours and the
# first one was stopped partway through: 64 cores built, every one of them missing from the
# manifest, because the merge was the last statement in the file. Work that is done but unrecorded
# gets done again. A trap records it however the run ends.
#
# MERGE and not replace, too: building one core must not delete the record of the other hundred.
# shellcheck disable=SC2329  # run from the traps below, which shellcheck cannot follow
merge_manifest() {
  [[ -s "$MANIFEST.new" ]] || { rm -f "$MANIFEST.new"; return; }
  if [[ -f "$MANIFEST" ]]; then
    awk -F'\t' 'NR==FNR{seen[$1]=1; print; next} !($1 in seen)' "$MANIFEST.new" "$MANIFEST" \
        | sort > "$MANIFEST.merged"
    mv "$MANIFEST.merged" "$MANIFEST"
  else
    sort "$MANIFEST.new" > "$MANIFEST"
  fi
  rm -f "$MANIFEST.new"
  # ⚠ THE COUNT IS PRINTED HERE AND NOWHERE ELSE. It used to be the last line of the file, which
  # runs BEFORE an EXIT trap - so it read a manifest that had not been written yet and reported
  # zero on a run that had just built a core.
  echo "== manifest: $MANIFEST ($(wc -l < "$MANIFEST" 2>/dev/null || echo 0) cores recorded)"
}
# ⚠ THE GUARD IS NOT PARANOIA, IT IS THE COST OF BEING WRONG. This is an `rm -rf` whose path
# comes from field two of a recipe file. A blank or `.` there would make $src the work directory
# itself - the clones, the manifest, liborbis-retro-common.a, and $OUT with every .prx already
# built in this run. So it deletes only a non-empty child of $WORK, never $WORK, never $OUT.
CLONE_TO_DROP=""
drop_clone() {
  local victim="$CLONE_TO_DROP"
  CLONE_TO_DROP=""
  [[ -n "$victim" && -d "$victim" ]] || return 0
  [[ "$victim" == "$WORK/"?* ]] || return 0
  [[ "$victim" != *"/."* ]] || return 0
  [[ "$victim" != "$OUT" && "$OUT" != "$victim/"* ]] || return 0
  rm -rf "$victim"
}

# shellcheck disable=SC2329  # run from the traps below, which shellcheck cannot follow
on_exit() { drop_clone; merge_manifest; }

# ⚠ A SIGNAL HAS TO END THE RUN, NOT JUST RUN THE HANDLER AND CARRY ON. `trap on_exit TERM` does
# NOT stop the script: bash runs the handler and resumes at the next statement. Run 32944175297
# shard 0 shows exactly what that costs. The runner's SIGTERM killed kronos's make, on_exit
# deleted the kronos clone and merged the manifest - and then the loop RESUMED, found no objects
# in a directory that had just been removed ("find: '/mnt/ps4-cores/libretro-kronos': No such
# file or directory"), and recorded `kronos COMPILE` for a core it never finished compiling.
# Then it went on to clone mame2003_plus and recorded `mame2003_plus CLONE` for a clone the
# cancel interrupted. Two fabricated verdicts, both of which read as broken cores.
# shellcheck disable=SC2329  # run from the traps below, which shellcheck cannot follow
on_signal() { # on_signal <name> <exit status>
  trap - EXIT INT TERM
  echo "build-cores: caught SIG$1 - stopping, the results so far stand" >&2
  kill_capped
  on_exit
  exit "$2"
}
trap on_exit EXIT
trap 'on_signal INT 130' INT
trap 'on_signal TERM 143' TERM

printf '%-24s %-9s %-9s %-8s %s\n' CORE RESULT SIZE COMMIT NOTE
printf '%-24s %-9s %-9s %-8s %s\n' ------------------------ --------- --------- -------- ----

N_OK=0; N_FORK=0; N_BAD=0; N_SKIP=0; FAILED=()
report() { # core result size commit note
  printf '%-24s %-9s %-9s %-8s %s\n' "$1" "$2" "$3" "$4" "$5"
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$MANIFEST.new"
  case "$2" in
    OK)   N_OK=$((N_OK + 1)) ;;
    FORK) N_FORK=$((N_FORK + 1)) ;;   # built on purpose and deliberately not written
    SKIP) N_SKIP=$((N_SKIP + 1)) ;;   # not in the recipe, or a build type this port cannot use
    DROP) N_SKIP=$((N_SKIP + 1)) ;;   # deliberately not offered - see PS4_CORE_DROP
    *)    N_BAD=$((N_BAD + 1)); FAILED+=("$1($2)") ;;
  esac
}

# ⚠ PS4_CORE_DROP IS ABOUT THE MENU, NOT ABOUT THE BUILD.
#
# Everything else that does not reach the index failed to get there. These built fine and are
# withheld anyway, because a core list is a recommendation: every name in it reads as a choice
# somebody vouched for, and a new user picks by name.
#
# mednafen_psx is upstream Beetle PSX with none of this port's work - no ORBIS platform arm, no
# ps4/orbis_lightrec_mem.c, so no executable code buffer and no recompiler. It runs the MIPS
# interpreter, which was measured at ~38% of realtime on Spyro 3 while the fork it sits beside
# holds full speed. Two entries a letter apart, one of them slow for a reason no menu can show.
# PS4_CORE_FORKS already stops it OVERWRITING mednafen_psx_hw; this stops it being offered.
#
# swanstation BUILDS AND DOES NOT RUN, which is the worst thing a core list can contain: a name
# somebody picks, that fails on the first game. Four separate faults were fixed in it and a fifth
# is open - see ps4-mesa-docs for the account. It is withheld rather than patched out of the tree,
# because the work done on it is real and the patches should keep applying; what must not happen
# is a user choosing it over mednafen_psx_hw, which works, holds 50 fps, and is already the
# PlayStation core this port offers. Remove the name here the day it boots a game.
#
# ⚠ Dropping a core does not remove what is already published. The publish job prunes bucket
# objects the new index does not name, so the removal happens on the next full run - and only a
# full run, because a subset run publishes nothing.
core_dropped() { # core -> 0 if this core is deliberately withheld
  case " ${PS4_CORE_DROP-mednafen_psx swanstation} " in *" $1 "*) return 0 ;; esac
  return 1
}

for core in "${CORES[@]}"; do
  CORE_DEADLINE=$(( SECONDS + CORE_TIMEOUT ))
  if core_dropped "$core"; then
    report "$core" DROP - - "withheld on purpose - PS4_CORE_DROP"
    continue
  fi
  line="$(recipe_line "$core")"
  [[ -n "$line" ]] || { report "$core" SKIP - - "not in the recipe"; continue; }
  read -r _name dir url branch _fetch buildtype makefile subdir _rest <<<"$line"

  # GENERIC_GL wants an OpenGL context this port does not have. CMAKE is driven through the
  # generated toolchain file above. Unless core_make_flags names the core - see the comment there
  # for why the recipe's build type is not the last word on whether a core can render here.
  makeflags="$(core_make_flags "$core")"
  [[ "$buildtype" == GENERIC || "$buildtype" == CMAKE || -n "$makeflags" ]] \
    || { report "$core" SKIP - - "build type $buildtype"; continue; }

  # Whatever the previous core left behind goes now, before this one's clone lands beside it.
  drop_clone

  src="$WORK/$dir"
  [[ $DROP_CLONES -eq 1 ]] && CLONE_TO_DROP="$src"
  if [[ ! -d "$src/.git" ]]; then
    rm -rf "$src"
    # Capped too, and not only the make: the log gives no way to tell which of the two ate
    # kronos's 57 minutes, because a quiet clone prints nothing until it is done.
    capped git clone -q --depth 1 --recursive -b "$branch" "$url" "$src" 2>"$WORK/$core.clone"
    rc=$?
    if timed_out "$rc"; then
      report "$core" TIMEOUT - - "clone ran past ${CORE_TIMEOUT}s"
      continue
    elif [[ "$rc" -ne 0 ]]; then
      report "$core" CLONE - - "$(tail -1 "$WORK/$core.clone" | cut -c1-46)"
      continue
    fi
  elif [[ $UPDATE -eq 1 ]]; then
    git -C "$src" fetch -q --depth 1 origin "$branch" 2>>"$WORK/$core.clone" || true
  fi

  # ⚠ RESET THEN PATCH, EVERY TIME, so a build is a function of (upstream ref, patch tree) and
  # nothing else. --keep opts out for the edit-compile loop, where re-applying a patch that is
  # already applied would fail and reverting local edits would be infuriating.
  npatch=0
  if [[ $KEEP -eq 0 ]]; then
    git -C "$src" reset -q --hard "origin/$branch" 2>/dev/null \
      || git -C "$src" reset -q --hard 2>/dev/null || true
    git -C "$src" clean -qfd 2>/dev/null || true
    # ⚠ NEITHER reset NOR clean REACHES INTO A SUBMODULE, AND THAT MAKES A BUILD UNREPEATABLE.
    #
    # `git reset --hard` in a superproject restores the RECORDED COMMIT of each submodule, not the
    # files inside one, and `git clean -fd` skips them entirely. Play! keeps deps/Framework and
    # deps/Dependencies as submodules, so an edit made inside either survives every reset - which
    # is how a patch that contained no changes at all appeared to work: the file it was supposed to
    # change had been edited by hand and nothing ever put it back. `git diff` in the superproject
    # records such an edit as `-Subproject commit <sha>` / `+Subproject commit <sha>-dirty` and
    # NOTHING ELSE, so the patch was a one-line no-op that git apply later refused outright.
    #
    # ⚠ A PATCH THAT TOUCHES A SUBMODULE HAS TO BE GENERATED FROM INSIDE IT:
    #     git -C deps/<sub> diff --src-prefix=a/deps/<sub>/ --dst-prefix=b/deps/<sub>/
    # git apply from the clone root then finds the file by path, which is all it needs.
    git -C "$src" submodule foreach -q --recursive \
        'git reset -q --hard 2>/dev/null; git clean -qfd 2>/dev/null' 2>/dev/null || true
    if [[ -d "$PATCHES/$core" ]]; then
      for p in "$PATCHES/$core"/*.patch; do
        [[ -e "$p" ]] || continue
        if git -C "$src" apply --whitespace=nowarn "$p" 2>>"$WORK/$core.patch"; then
          npatch=$((npatch+1))
        else
          report "$core" PATCH - "$(git -C "$src" rev-parse --short HEAD)" "$(basename "$p") would not apply"
          npatch=-1; break
        fi
      done
    fi
  fi
  [[ $npatch -lt 0 ]] && continue
  commit="$(git -C "$src" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  [[ $npatch -gt 0 ]] && commit="$commit+$npatch"

  bdir="$src/${subdir:-.}"; [[ -d "$bdir" ]] || bdir="$src"

  # ⚠ THE OBJECTS ARE NOT NECESSARILY UNDER THE BUILD DIRECTORY, and assuming they were cost
  # three cores a wrong verdict. gearboy builds from platforms/libretro with sources named
  # ../../src/*.cpp, so `$(SOURCES_CXX:.cpp=.o)` puts every object in src/ - one level ABOVE the
  # directory make ran in. Searching only there found one stray object, the link failed on the
  # core's own classes, and it read as a core that will not compile. It compiles fine.
  #
  # So the clone root is the search root, both for the clean and for the collection.
  [[ $KEEP -eq 0 ]] && find "$src" -name '*.o' -type f -delete 2>/dev/null

  # ⚠ HAVE_CDROM=0, AND IT IS A CORRECTNESS ARGUMENT RATHER THAN A CONVENIENCE.
  #
  # It gates passthrough to a HOST CD device - a real drive the frontend opens by path. This
  # console has none, so the feature could not work here whatever it linked. Left on, it also
  # adds a `cdrom` member to libretro_vfs_implementation_file, so the flag decides a STRUCT
  # LAYOUT shared between the core's objects and anything else handling that type. Seven cores
  # so far fail with `undefined symbol: cdrom_lba_to_msf` - their vfs is built expecting the
  # member while cdrom.c never joins the source list - and satisfying that from a shared archive
  # would put two layouts of one struct in a single link. That is silent and much worse than a
  # missing symbol. Turning it off makes both sides agree and loses nothing that exists here.
  if [[ "$buildtype" == CMAKE ]]; then
    # ⚠ FIELD 8 MEANS SOMETHING ELSE HERE. For a GENERIC core it is a SOURCE subdirectory to run
    # make in; for a CMAKE core it is a BUILD directory that does not exist yet - "build" for all
    # of them but tic80, which says "builddir". So $bdir above is not the answer and is not used.
    cbuild="$src/${subdir:-build}"

    # ⚠ A STALE CMakeCache.txt SURVIVES `git clean -fd` AND SILENTLY PINS THE OLD FLAGS.
    # Every core here gitignores its build directory, and clean does not touch ignored paths
    # without -x. CMAKE_CXX_FLAGS_INIT is only consulted on the FIRST configure, so a cache left
    # from a previous run keeps the flags that run was given: correcting the toolchain file and
    # rebuilding changed nothing, twice, and the compiler errors were identical each time -
    # which reads as a fix that does not work rather than a fix that was never applied.
    # RESET THEN CONFIGURE, for the same reason the patch loop resets before it patches.
    [[ $KEEP -eq 0 ]] && rm -rf "$cbuild"

    # ⚠ THE RECIPE'S ARGUMENTS ARE NOT ALL CMake ARGUMENTS, AND TWO OF THEM WOULD UNDO THIS FILE.
    #
    #   ishiiruka  -DCMAKE_CXX_COMPILER=g++-7 -DCMAKE_C_COMPILER=gcc-7
    #   flycast    HAVE_OIT=1
    #
    # The first pair names a HOST compiler and would build a Linux core with a PS4 toolchain file
    # attached, which is not a failure until the link. The second is a make variable that wandered
    # into a CMake line: cmake reads a bare word as a source directory, so it would silently
    # configure the wrong tree. Only -D arguments are passed, and not those two.
    cargs=()
    # shellcheck disable=SC2086  # the recipe's tail is a list of arguments and must word-split
    for a in $_rest; do
      a="${a//\"/}"
      case "$a" in
        -DCMAKE_C_COMPILER=*|-DCMAKE_CXX_COMPILER=*|-DCMAKE_TOOLCHAIN_FILE=*) ;;
        -D*) cargs+=("$a") ;;
        *) ;;
      esac
    done
    # shellcheck disable=SC2086  # a list of -D arguments, and must word-split
    for a in $(core_cmake_flags "$core"); do cargs+=("$a"); done

    # ⚠ Unix Makefiles EXPLICITLY, EVEN WHERE ninja IS INSTALLED. Two reasons, both about this
    # harness rather than about taste: `-k` past a failure is what leaves a partial build's objects
    # on disk for the collection below, and the object layout under CMakeFiles/<target>.dir is what
    # the exclusions there are written against. A generator chosen by whatever the runner happens
    # to have installed is a build that differs between machines for no stated reason.
    # ⚠ pkg-config IS A HOST PROGRAM AND IT ANSWERS WITH HOST PATHS. CMAKE_FIND_ROOT_PATH keeps
    # find_library and find_path inside the sysroot, and has no say over what pkg-config reports:
    # yaps2's configure printed `Found Freetype: /usr/lib/libfreetype.so` and `Found WebP:
    # /usr/include` - this desktop's, for a console build. That is the same accident as the GLES
    # headers above, one layer out, and it ends in a core that links a host shared object.
    # PKG_CONFIG_LIBDIR REPLACES the default search path rather than adding to it, so pointing it
    # at the SDK means pkg-config finds what the SDK ships and otherwise finds nothing.
    ( export PKG_CONFIG_LIBDIR="$TOOLCHAIN/lib/pkgconfig" \
             PKG_CONFIG_SYSROOT_DIR="$TOOLCHAIN" \
      && capped cmake -S "$src" -B "$cbuild" -G "Unix Makefiles" \
          -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN" -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
          -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "${cargs[@]}" \
      && capped cmake --build "$cbuild" -j"$JOBS" --target "$(cmake_build_target)" -- -k \
      ) >"$WORK/$core.log" 2>&1
    rc=$?
  else
    # $makeflags is a list of make variables and must word-split - hence the disable below.
    # shellcheck disable=SC2086
    ( cd "$bdir" && capped make -f "${makefile:-Makefile}" platform=unix HAVE_CDROM=0 $makeflags \
          CC="$CC_ORBIS" CXX="$CXX_ORBIS" AR=llvm-ar -k -j"$JOBS" ) >"$WORK/$core.log" 2>&1
    rc=$?
  fi
  # ⚠ TIMEOUT IS ITS OWN VERDICT AND NOT "COMPILE". A core that ran out of clock has objects
  # half-written and a log that ends mid-sentence; calling that a compile failure sends whoever
  # reads the manifest looking for an error message that was never printed. The two want
  # different follow-up - one wants a patch, the other wants a profile or a bigger cap.
  if timed_out "$rc"; then
    report "$core" TIMEOUT - "$commit" "make ran past ${CORE_TIMEOUT}s, killed with its children"
    continue
  fi

  # ⚠ NOT EVERY .o UNDER A CMake TREE BELONGS TO THE CORE, AND THE STRAYS DEFINE main().
  #
  # Three kinds, and it took a link failure to find the third. CMake proves the compiler works by
  # building CMakeFiles/<version>/CompilerId*/CMakeC*CompilerId.o and leaves it there; its feature
  # checks leave objects in CMakeScratch/ and CMakeTmp/; and check_ipo_supported() configures and
  # builds AN ENTIRE SUB-PROJECT under CMakeFiles/_CMakeLTOTest-C/, whose objects sit in a
  # perfectly ordinary-looking boo.dir/. swanstation:
  #
  #     ld.lld: error: duplicate symbol: main
  #       >>> .../CMakeFiles/_CMakeLTOTest-C/bin/CMakeFiles/boo.dir/main.c.o
  #       >>> .../CMakeFiles/_CMakeLTOTest-CXX/bin/CMakeFiles/boo.dir/main.cpp.o
  #
  # ⚠ SO "IT IS IN A <target>.dir" IS NOT THE TEST, and that was the first rule written here. What
  # every one of these has in common is a component under CMakeFiles/ that is not a target: a
  # version number, or a name CMake prefixes with an underscore for exactly this reason.
  mapfile -t objs < <(find "$src" -name '*.o' -type f \
      -not -path '*/CMakeFiles/[0-9]*.[0-9]*/*' -not -path '*/CMakeFiles/_*/*' \
      -not -path '*/CMakeScratch/*' -not -path '*/CMakeTmp/*' | sort)
  if [[ ${#objs[@]} -eq 0 ]]; then
    err="$(grep -m1 -E 'error:|CMake Error|No such file|No rule' "$WORK/$core.log" | cut -c1-46)"
    report "$core" COMPILE - "$commit" "${err:-no objects; single-shot link?}"
    continue
  fi

  # ⚠ --error-limit=0, AND IT CHANGES WHAT THE VERDICT MEANS. ld.lld stops REPORTING after twenty
  # errors and says so in a line easy to miss. mupen64plus_next's first link here printed twenty
  # undefined glXxx symbols and not one of the twenty-two its recompiler was missing, which reads
  # as "a GL core, otherwise complete" - the opposite of the truth. The note below quotes the first
  # symbol either way, but the log has to hold all of them for anyone to work from.
  capped ld.lld "${objs[@]}" "$WEAK_STUBS" "${KEEP_SYMS[@]}" --error-limit=0 -o "$WORK/$core.elf" \
        -m elf_x86_64 -pie --script "${ORBIS_LINK_SCRIPT:-$HERE/orbis-module.ld}" --eh-frame-hdr --no-rosegment \
        -L"$TOOLCHAIN/lib" -L"$ORBIS_COMPAT_DIR/build" "$COMMON_LIB" "$CORE_SUPPORT_LIB" \
        -lorbis-compat -lc -lkernel -lc++ -lSceNet -lSceUserService \
        "$TOOLCHAIN/lib/crtlib.o" >"$WORK/$core.link" 2>&1
  rc=$?
  if timed_out "$rc"; then
    report "$core" TIMEOUT "${#objs[@]}o" "$commit" "the link ran past ${CORE_TIMEOUT}s"
    continue
  elif [[ "$rc" -ne 0 ]]; then
    err="$(grep -m1 -oP 'undefined symbol: \K.*' "$WORK/$core.link" | cut -c1-46)"
    report "$core" LINK "${#objs[@]}o" "$commit" "${err:-link failed}"
    continue
  fi

  # ⚠ -k KEEPS GOING PAST A FAILURE, so a partial build still leaves objects and would link into
  # a module missing its own entry points. retro_run is the test: without it this is not a core,
  # whatever it built.
  # ⚠ NO `grep -q` IN A PIPELINE UNDER pipefail. grep -q exits the moment it matches, which closes
  # the pipe, which sends llvm-nm SIGPIPE, which makes the PIPELINE fail even though the symbol was
  # found. Whether it happens depends on whether the symbol dump fits the pipe buffer, so it looked
  # like a real verdict: gearboy and nestopia were reported as "linked without retro_run" while the
  # ELF beside them contained it, and cores with smaller symbol tables passed. A false failure that
  # scales with the size of the core is the worst kind - it condemns the big ones.
  syms="$(llvm-nm "$WORK/$core.elf" 2>/dev/null)"
  if [[ "$syms" != *" T retro_run"* ]]; then
    report "$core" NO-ABI "${#objs[@]}o" "$commit" "linked without retro_run"
    continue
  fi

  # ⚠ NEVER OVERWRITE A HAND-PORTED CORE. The harness clones UPSTREAM, so its
  # mednafen_psx_hw_libretro.prx is plain upstream Beetle - no orbis platform arm, no
  # ps4/orbis_lightrec_mem.c, no ORBIS dynarec default - and it lands on the same filename as the
  # fork in ~/src-ps4/beetle-psx-libretro. It did: Spyro went from the recompiler and the Vulkan
  # renderer back to whatever platform=unix leaves you with, and it read as a mysterious
  # slowdown rather than as a file being replaced.
  #
  # PS4_CORE_FORKS names the cores that have a fork of their own. The harness builds them and
  # says so, but will not write over the result.
  name="${core}_libretro"
  case " ${PS4_CORE_FORKS:-mednafen_psx_hw} " in
    *" $core "*)
      report "$core" FORK "${#objs[@]}o" "$commit" "built, NOT written - $core has a port of its own"
      continue ;;
  esac
  ( cd "$WORK" && export OO_PS4_TOOLCHAIN="$TOOLCHAIN" && capped "$TOOLCHAIN/bin/linux/create-fself" \
      -in="$core.elf" -out="$core.oelf" --lib="$name.prx" --paid 0x3800000000000011 ) >"$WORK/$core.fself" 2>&1
  rc=$?
  if timed_out "$rc"; then
    report "$core" TIMEOUT "${#objs[@]}o" "$commit" "create-fself ran past ${CORE_TIMEOUT}s"
    continue
  fi
  # ⚠ create-fself EXITS 0 WHEN IT REFUSES, so the file is the test, not the status. And it
  # refuses over a single unresolvable symbol, which its own message names - repeating that name
  # here is the difference between "it refused" and a verdict somebody can act on.
  if [[ ! -f "$WORK/$name.prx" ]]; then
    why="$(grep -m1 -oP 'missing library for symbol \(\K[^)]+' "$WORK/$core.fself" 2>/dev/null)"
    if [[ -z "$why" ]]; then
      why="$(llvm-nm -u "$WORK/$core.elf" 2>/dev/null | awk '$1=="w"{print $2; exit}')"
      [[ -n "$why" ]] && why="undefined weak: $why"
    fi
    report "$core" FSELF "${#objs[@]}o" "$commit" "${why:-create-fself refused it}"
    continue
  fi
  mv "$WORK/$name.prx" "$OUT/$name.prx"
  report "$core" OK "$(du -h "$OUT/$name.prx" | cut -f1)" "$commit" ""
done

# ⚠ THE EXIT STATUS ANSWERS "IS THE HARNESS BROKEN", NOT "DID EVERY CORE BUILD", AND THE DECISION
# LIVES HERE RATHER THAN IN THE WORKFLOW.
#
# The recipe carries 163 cores and only 101 have ever built anywhere. A core that will not compile
# is the STEADY STATE of this project, not an exception, so a shard that reddens over one of them
# takes the other fourteen - and the publish job behind them - down with it. Run 32944175297 lost
# a whole release that way.
#
# Every exit above this line is systemic: orbis-compat missing, no libretro-common objects to
# build the frontend archive from, the weak stubs or the support archive refusing to compile, a
# malformed argument. Those mean the next core would fail for the same reason as this one. A
# per-core COMPILE / LINK / CLONE / PATCH / FSELF / NO-ABI / TIMEOUT verdict does not: it is DATA,
# it is already in the manifest and in the table above, and the shard should upload what it built.
#
# The one per-core outcome that IS systemic is all of them at once. If nothing in the list
# produced a module, the cause is far more likely to be this harness or its toolchain than 15
# unrelated cores breaking on the same morning, and a green job with an empty out/ is the exact
# silent success this project has already paid for.
#
# ⚠ AND THE WORKFLOW MUST NOT SECOND-GUESS THIS BY PARSING THE MANIFEST FOR A PASS/FAIL. Two
# places deciding what counts as a failure is two places that will disagree, and the manifest
# cannot speak for the runs that died before writing one. The workflow READS the manifest - for
# the step summary, and to say which cores are missing from the index - and keys pass/fail on
# this status alone.
echo "== $N_OK built, $N_BAD failed, $N_SKIP skipped, $N_FORK left to a port of their own"
[[ "$N_BAD" -eq 0 ]] || echo "== did not build: ${FAILED[*]}"

if [[ $((N_OK + N_FORK)) -eq 0 ]]; then
  echo "build-cores: none of the ${#CORES[@]} core(s) asked for produced a module." >&2
  echo "   That is a broken harness far more often than it is $((N_BAD + N_SKIP)) core(s) all" >&2
  echo "   breaking on the same morning." >&2
  exit 1
fi
exit 0
