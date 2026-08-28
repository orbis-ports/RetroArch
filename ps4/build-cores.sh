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
#   --jobs N          parallelism for each core's make
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
CORES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --recipe)  RECIPE="$2";  shift 2 ;;
    --work)    WORK="$2";    shift 2 ;;
    --out)     OUT="$2";     shift 2 ;;
    --patches) PATCHES="$2"; shift 2 ;;
    --jobs|-j) JOBS="$2";    shift 2 ;;
    --all)     ALL=1;        shift ;;
    --list)    LIST=1;       shift ;;
    --update)  UPDATE=1;     shift ;;
    --keep)    KEEP=1;       shift ;;
    -*) echo "build-cores: unknown argument: $1" >&2; exit 2 ;;
    *) CORES+=("$1"); shift ;;
  esac
done
[[ -n "$RECIPE" && -f "$RECIPE" ]] || { echo "build-cores: --recipe <file> is required" >&2; exit 2; }
OUT="${OUT:-$WORK/out}"

recipe_line() { awk -v c="$1" '!/^[[:space:]]*(#|$)/ && $1==c {print; exit}' "$RECIPE"; }

if [[ $LIST -eq 1 ]]; then
  awk '!/^[[:space:]]*(#|$)/ {printf "%-26s %s\n", $1, $6}' "$RECIPE" | sort
  exit 0
fi
if [[ $ALL -eq 1 ]]; then
  mapfile -t CORES < <(awk '!/^[[:space:]]*(#|$)/ && $6=="GENERIC" {print $1}' "$RECIPE" | sort)
fi
[[ ${#CORES[@]} -gt 0 ]] || { echo "build-cores: name a core, or pass --all" >&2; exit 2; }

mkdir -p "$WORK" "$OUT"

ORBIS_ARCH=(--target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables
            -isysroot "$TOOLCHAIN"
            -DORBIS -D__ORBIS__ -D__PS4__ -DPS4 -D_BSD_SOURCE=1)

# ⚠ THE INCLUDE ORDER IS NOT A PREFERENCE, AND C++ NEEDS A DIFFERENT ONE FROM C.
#
# C: the overlay ahead of the SDK, so its corrected constants win - MAP_ANON is 0x1002 on this
# kernel and the SDK's musl header says 0x0020.
#
# C++: libc++ FIRST, then the overlay, then the SDK. Getting this wrong is not a link error, it
# is `cmath:341: no member named 'abs' in the global namespace` - libc++'s <cmath> hoists the C
# library's abs into std:: and needs the C headers underneath it. This file had c++/v1 appended
# LAST on its first draft, which is the trap Makefile.orbis and ps4/HANDOFF.md both already
# describe. Writing a new tool next to a documented trap is not protection from it.
C_INCLUDES=(-isystem "$ORBIS_COMPAT_DIR/include" -isystem "$TOOLCHAIN/include")
CXX_INCLUDES=(-isystem "$TOOLCHAIN/include/c++/v1"
              -isystem "$ORBIS_COMPAT_DIR/include" -isystem "$TOOLCHAIN/include"
              -include orbis_prefix.h)
CC_ORBIS="clang ${ORBIS_ARCH[*]} ${C_INCLUDES[*]}"
CXX_ORBIS="clang++ ${ORBIS_ARCH[*]} ${CXX_INCLUDES[*]}"

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
WEAK_STUBS="$WORK/orbis_weak_stubs.o"
if [[ ! -f "$WEAK_STUBS" ]]; then
  $CC_ORBIS -O2 -c -o "$WEAK_STUBS" "$HERE/orbis_weak_stubs.c" \
    || { echo "build-cores: could not build the weak stubs" >&2; exit 1; }
fi

MANIFEST="$OUT/cores.manifest"
: > "$MANIFEST.new"

# ⚠ MERGE ON THE WAY OUT, NOT AT THE END OF THE LOOP. A sweep of 162 cores takes hours and the
# first one was stopped partway through: 64 cores built, every one of them missing from the
# manifest, because the merge was the last statement in the file. Work that is done but unrecorded
# gets done again. A trap records it however the run ends.
#
# MERGE and not replace, too: building one core must not delete the record of the other hundred.
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
trap merge_manifest EXIT INT TERM

printf '%-24s %-9s %-9s %-8s %s\n' CORE RESULT SIZE COMMIT NOTE
printf '%-24s %-9s %-9s %-8s %s\n' ------------------------ --------- --------- -------- ----

report() { # core result size commit note
  printf '%-24s %-9s %-9s %-8s %s\n' "$1" "$2" "$3" "$4" "$5"
  printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" "$5" >> "$MANIFEST.new"
}

for core in "${CORES[@]}"; do
  line="$(recipe_line "$core")"
  [[ -n "$line" ]] || { report "$core" SKIP - - "not in the recipe"; continue; }
  read -r _name dir url branch _fetch buildtype makefile subdir _rest <<<"$line"

  # GENERIC_GL wants an OpenGL context this port does not have; CMAKE wants a toolchain file.
  [[ "$buildtype" == GENERIC ]] || { report "$core" SKIP - - "build type $buildtype"; continue; }

  src="$WORK/$dir"
  if [[ ! -d "$src/.git" ]]; then
    rm -rf "$src"
    if ! git clone -q --depth 1 --recursive -b "$branch" "$url" "$src" 2>"$WORK/$core.clone"; then
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
  ( cd "$bdir" && make -f "${makefile:-Makefile}" platform=unix HAVE_CDROM=0 \
        CC="$CC_ORBIS" CXX="$CXX_ORBIS" AR=llvm-ar -k -j"$JOBS" ) >"$WORK/$core.log" 2>&1

  mapfile -t objs < <(find "$src" -name '*.o' -type f | sort)
  if [[ ${#objs[@]} -eq 0 ]]; then
    err="$(grep -m1 -E 'error:|No such file|No rule' "$WORK/$core.log" | cut -c1-46)"
    report "$core" COMPILE - "$commit" "${err:-no objects; single-shot link?}"
    continue
  fi

  if ! ld.lld "${objs[@]}" "$WEAK_STUBS" "${KEEP_SYMS[@]}" -o "$WORK/$core.elf" \
        -m elf_x86_64 -pie --script "$TOOLCHAIN/link.x" --eh-frame-hdr --no-rosegment \
        -L"$TOOLCHAIN/lib" -L"$ORBIS_COMPAT_DIR/build" "$COMMON_LIB" \
        -lorbis-compat -lc -lkernel -lc++ -lSceNet -lSceUserService \
        "$TOOLCHAIN/lib/crtlib.o" >"$WORK/$core.link" 2>&1; then
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

  name="${core}_libretro"
  ( cd "$WORK" && OO_PS4_TOOLCHAIN="$TOOLCHAIN" "$TOOLCHAIN/bin/linux/create-fself" \
      -in="$core.elf" -out="$core.oelf" --lib="$name.prx" --paid 0x3800000000000011 ) >"$WORK/$core.fself" 2>&1
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

