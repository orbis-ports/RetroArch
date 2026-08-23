#!/usr/bin/env bash
# Try to build many libretro cores for the PlayStation 4, and say honestly which ones worked.
#
#   ps4/build-cores.sh --work <dir> --out <dir> [--jobs N] [--recipe <file>] <core>...
#
# ⚠ IT DOES NOT PATCH THE CORES' MAKEFILES, and that is the whole design. Beetle PSX HW needed a
# hand-written `orbis` platform arm, and 99 candidate cores is 99 of those. Instead the toolchain
# flags are pushed INSIDE $(CC) and $(CXX) - not into CFLAGS - because a libretro Makefile
# routinely does `CFLAGS := ...` and throws away anything the caller passed, while almost none of
# them rewrite CC. `platform=unix` then gives the core a sane arm to start from.
#
# ⚠ AND THE LINK IS EXPECTED TO FAIL. `platform=unix` links a shared object with the host's
# compiler driver; there is no such thing here. The objects are what we want, so make runs with
# -k and the .o files are collected afterwards and linked with ld.lld into a .prx - the same two
# steps as beetle-psx-libretro/ps4/build.sh, which is where this shape comes from.
#
# What this cannot do is make a core CORRECT. It reports what compiled. A core that builds and
# then draws nothing is a pass here and a failure on the console, and only the console can tell
# the difference.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

for _c in "${ORBIS_COMPAT_DIR:-}" "$ROOT/../orbis-compat" "$HOME/src-ps4/orbis-compat"; do
  [[ -n "$_c" && -f "$_c/scripts/ps4/orbis-env.sh" ]] && { ORBIS_COMPAT_DIR="$_c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || { echo "build-cores: orbis-compat not found" >&2; exit 1; }
. "$ORBIS_COMPAT_DIR/scripts/ps4/orbis-env.sh"
TOOLCHAIN="$OO_PS4_TOOLCHAIN"

WORK=""; OUT=""; JOBS="$(nproc)"; RECIPE=""
CORES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --work)   WORK="$2";   shift 2 ;;
    --out)    OUT="$2";    shift 2 ;;
    --jobs|-j) JOBS="$2";  shift 2 ;;
    --recipe) RECIPE="$2"; shift 2 ;;
    -*) echo "build-cores: unknown argument: $1" >&2; exit 2 ;;
    *) CORES+=("$1"); shift ;;
  esac
done
[[ -n "$WORK" && -n "$OUT" ]] || { echo "build-cores: --work and --out are required" >&2; exit 2; }
[[ -n "$RECIPE" ]] || { echo "build-cores: --recipe is required (libretro-super recipe file)" >&2; exit 2; }
mkdir -p "$WORK" "$OUT"

ORBIS_ARCH=(--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables
            -isysroot "$TOOLCHAIN"
            -DORBIS -D__ORBIS__ -D__PS4__ -DPS4 -D_BSD_SOURCE=1)

# ⚠ THE INCLUDE ORDER IS NOT A PREFERENCE, AND C++ NEEDS A DIFFERENT ONE FROM C.
#
# For C: the overlay ahead of the SDK, so its corrected constants win - MAP_ANON is 0x1002 here
# and the SDK's musl header says 0x0020.
#
# For C++: libc++ FIRST, then the overlay, then the SDK. Getting this wrong is not a link error,
# it is `cmath:341: no member named 'abs' in the global namespace` - libc++'s <cmath> hoists the
# C library's abs into std:: and needs the C headers to arrive underneath it. This harness had
# c++/v1 appended LAST and three C++ cores compiled almost nothing because of it; the same
# ordering, and the same symptom, is already recorded in Makefile.orbis and in ps4/HANDOFF.md.
#
# -include orbis_prefix.h for the same reason Makefile.orbis passes it: 26 of the SDK's 189
# orbis/ headers name size_t and friends without including <stddef.h> themselves.
C_INCLUDES=(-isystem "$ORBIS_COMPAT_DIR/include" -isystem "$TOOLCHAIN/include")
CXX_INCLUDES=(-isystem "$TOOLCHAIN/include/c++/v1"
              -isystem "$ORBIS_COMPAT_DIR/include" -isystem "$TOOLCHAIN/include"
              -include orbis_prefix.h)
CC_ORBIS="clang ${ORBIS_ARCH[*]} ${C_INCLUDES[*]}"
CXX_ORBIS="clang++ ${ORBIS_ARCH[*]} ${CXX_INCLUDES[*]}"

# ⚠ THE libretro ABI HAS TO BE NAMED OR LTO THROWS IT AWAY, AND THE BUILD STILL SUCCEEDS.
#
# Several cores compile with -flto under platform=unix, so their .o files are LLVM bitcode -
# llvm-nm prints dashes where the address would be. ld.lld links bitcode happily, runs LTO, and
# internalises everything unreachable from an entry point. A module has no entry point, so
# "everything" is everything: snes9x2010 linked to a 915 KiB ELF containing ZERO retro_* symbols
# and ld.lld reported success.
#
# Naming them with -u makes them GC roots. This is the whole libretro API surface as of
# libretro.h; a core missing one of these is not a core, so nothing here is optional.
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
KEEP=()
for _s in "${LIBRETRO_ABI[@]}"; do KEEP+=(-u "$_s"); done

# ⚠ AND THE FRONTEND'S libretro-common, AS AN ARCHIVE, AFTER the core's own objects.
#
# Three of the first five cores tried failed on retro_vfs_*_impl and cdrom_*: their Makefiles do
# not build the parts of libretro-common they call, because on a normal platform the shared
# object resolves them lazily against the frontend. A .prx cannot - it has its own symbol table.
#
# An ARCHIVE and not a list of objects, deliberately: archive members are pulled only for symbols
# still undefined, so a core that DID build its own copy keeps it and there is no duplicate. And
# the frontend's copy is the one fixed for this console - see ps4/build-core.sh on --common.
COMMON_LIB="$WORK/liborbis-retro-common.a"
if [[ ! -f "$COMMON_LIB" ]]; then
  mapfile -t _common < <(find "$ROOT/libretro-common" -name '*.o' -type f | sort)
  if [[ ${#_common[@]} -eq 0 ]]; then
    echo "build-cores: no libretro-common objects in $ROOT - build the frontend first" >&2
    exit 1
  fi
  # ⚠ AND THE PARTS THE FRONTEND ITSELF DOES NOT BUILD, because a core may still call them.
  # genesis_plus_gx wants cdrom_lba_to_msf; libretro-common/cdrom/cdrom.c is in the tree and the
  # frontend has no reason to compile it, so it is in no object anywhere. Best-effort: try every
  # .c that has no .o yet and archive the ones that build. The ones that do not are network and
  # platform code this console has no business running, and a core that needs one of those will
  # say so by name at link time - which is a better answer than a silently short archive.
  _extra_dir="$WORK/common-extra"
  mkdir -p "$_extra_dir"
  _built=0; _failed=0
  while IFS= read -r csrc; do
    [[ -f "${csrc%.c}.o" ]] && continue
    _obj="$_extra_dir/$(echo "${csrc#$ROOT/libretro-common/}" | tr '/' '_')"
    _obj="${_obj%.c}.o"
    if [[ ! -f "$_obj" ]]; then
      if $CC_ORBIS -O2 -I"$ROOT/libretro-common/include" -c -o "$_obj" "$csrc" >/dev/null 2>&1; then
        _built=$((_built + 1))
      else
        rm -f "$_obj"; _failed=$((_failed + 1)); continue
      fi
    fi
    _common+=("$_obj")
  done < <(find "$ROOT/libretro-common" -name '*.c' -type f | sort)

  llvm-ar rcs "$COMMON_LIB" "${_common[@]}"
  echo "== libretro-common fallback: ${#_common[@]} objects (${_built} compiled here, ${_failed} would not build)"
fi

printf '%-24s %-9s %-9s %s\n' CORE RESULT SIZE NOTE
printf '%-24s %-9s %-9s %s\n' ------------------------ --------- --------- ----

for core in "${CORES[@]}"; do
  line="$(awk -v c="$core" '!/^\s*(#|$)/ && $1==c {print; exit}' "$RECIPE")"
  if [[ -z "$line" ]]; then
    printf '%-24s %-9s %-9s %s\n' "$core" SKIP - "not in the recipe"
    continue
  fi
  read -r _name dir url branch _fetch buildtype makefile subdir _rest <<<"$line"

  if [[ "$buildtype" != GENERIC ]]; then
    # GENERIC_GL wants an OpenGL context this port does not have; CMAKE wants a toolchain file.
    printf '%-24s %-9s %-9s %s\n' "$core" SKIP - "build type $buildtype"
    continue
  fi

  src="$WORK/$dir"
  if [[ ! -d "$src" ]]; then
    if ! git clone -q --depth 1 --recursive -b "$branch" "$url" "$src" 2>"$WORK/$core.clone"; then
      printf '%-24s %-9s %-9s %s\n' "$core" CLONE-ERR - "$(tail -1 "$WORK/$core.clone" | cut -c1-60)"
      continue
    fi
  fi

  bdir="$src/${subdir:-.}"
  [[ -d "$bdir" ]] || bdir="$src"

  find "$bdir" -name '*.o' -type f -delete 2>/dev/null
  ( cd "$bdir" && make -f "${makefile:-Makefile}" platform=unix \
        CC="$CC_ORBIS" CXX="$CXX_ORBIS" AR=llvm-ar -k -j"$JOBS" ) \
      >"$WORK/$core.log" 2>&1

  mapfile -t objs < <(find "$bdir" -name '*.o' -type f | sort)
  if [[ ${#objs[@]} -eq 0 ]]; then
    err="$(grep -m1 -E 'error:|No such file' "$WORK/$core.log" | cut -c1-58)"
    printf '%-24s %-9s %-9s %s\n' "$core" COMPILE - "${err:-no objects produced}"
    continue
  fi

  # ⚠ SOME OBJECTS DO NOT BELONG TO THE CORE. -k keeps going past a failure, so a partial build
  # still leaves .o files behind and would link into a .prx missing its own entry points. The
  # retro_* symbols are the test: a module without them is not a libretro core whatever it built.
  if ! ld.lld "${objs[@]}" "${KEEP[@]}" -o "$WORK/$core.elf" \
        -m elf_x86_64 -pie --script "$TOOLCHAIN/link.x" --eh-frame-hdr --no-rosegment \
        -L"$TOOLCHAIN/lib" -L"$ORBIS_COMPAT_DIR/build" \
        "$COMMON_LIB" \
        -lorbis-compat -lc -lkernel -lc++ -lSceNet -lSceUserService "$TOOLCHAIN/lib/crtlib.o" \
        >"$WORK/$core.link" 2>&1; then
    err="$(grep -m1 -E 'undefined symbol|error' "$WORK/$core.link" | cut -c1-58)"
    printf '%-24s %-9s %-9s %s\n' "$core" LINK "${#objs[@]}o" "${err:-link failed}"
    continue
  fi

  if ! llvm-nm "$WORK/$core.elf" 2>/dev/null | grep -q ' T retro_run$'; then
    printf '%-24s %-9s %-9s %s\n' "$core" NO-ABI "${#objs[@]}o" "linked without retro_run"
    continue
  fi

  name="${core}_libretro"
  ( cd "$WORK" && OO_PS4_TOOLCHAIN="$TOOLCHAIN" "$TOOLCHAIN/bin/linux/create-fself" \
      -in="$core.elf" -out="$core.oelf" --lib="$name.prx" --paid 0x3800000000000011 ) >/dev/null 2>&1
  if [[ ! -f "$WORK/$name.prx" ]]; then
    printf '%-24s %-9s %-9s %s\n' "$core" FSELF "${#objs[@]}o" "create-fself refused the ELF"
    continue
  fi
  mv "$WORK/$name.prx" "$OUT/$name.prx"
  printf '%-24s %-9s %-9s %s\n' "$core" OK "$(du -h "$OUT/$name.prx" | cut -f1)" ""
done
