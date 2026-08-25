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
# ⚠ AND THE CLONES ARE A DISK PROBLEM BEFORE THEY ARE A TIME ONE. This work directory reaches
# 15 GB across the recipe and a GitHub-hosted runner has about 14 GB free, so a shard that keeps
# every clone runs out of disk partway through and reports whatever a full disk looks like -
# usually a compile error in an unrelated core. --drop-clones removes each clone once the harness
# is finished with it, successful or not, so at most one is on disk at a time. Nothing worth
# keeping lives in there: the logs, the .elf, the .prx and the manifest are all in $WORK itself.
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
DROP_CLONES=-1        # -1: decide from $CI below.  0: keep.  1: drop.
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
    --drop-clones) DROP_CLONES=1; shift ;;
    --keep-clones) DROP_CLONES=0; shift ;;
    -*) echo "build-cores: unknown argument: $1" >&2; exit 2 ;;
    *) CORES+=("$1"); shift ;;
  esac
done
[[ -n "$RECIPE" && -f "$RECIPE" ]] || { echo "build-cores: --recipe <file> is required" >&2; exit 2; }
OUT="${OUT:-$WORK/out}"

if [[ $DROP_CLONES -eq -1 ]]; then
  case "${CI:-}" in true|TRUE|1) DROP_CLONES=1 ;; *) DROP_CLONES=0 ;; esac
fi
# --keep is the incremental edit-compile loop; deleting the tree it is incremental against is
# not a combination that means anything. It wins.
[[ $KEEP -eq 1 ]] && DROP_CLONES=0

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
# mupen64plus_next is GENERIC_GL there because GLideN64 is its default renderer - but it also ships
# paraLLEl-RDP over Vulkan, which is the renderer this console can actually run, and the core's
# patch tree makes that the only one it offers. Skipping it on the recipe's say-so would have
# skipped a Vulkan core for being an OpenGL one.
#
# Anything listed here is built whatever its build type says. The flags are the recipe's own for
# that core, minus the ones platform=unix already sets.
core_make_flags() {
  case "$1" in
    mupen64plus_next) echo "HAVE_PARALLEL_RDP=1 HAVE_PARALLEL_RSP=1 HAVE_THR_AL=1 LLE=1 WITH_DYNAREC=x86_64 FORCE_GLES3=1" ;;
    parallel_n64)     echo "HAVE_PARALLEL=1 HAVE_PARALLEL_RSP=1 HAVE_THR_AL=1 WITH_DYNAREC=x86_64" ;;
    *)                echo "" ;;
  esac
}

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

on_exit() { drop_clone; merge_manifest; }
trap on_exit EXIT INT TERM

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
  # Unless core_make_flags names the core - see the comment there for why the recipe's build type
  # is not the last word on whether a core can render here.
  makeflags="$(core_make_flags "$core")"
  [[ "$buildtype" == GENERIC || -n "$makeflags" ]] \
    || { report "$core" SKIP - - "build type $buildtype"; continue; }

  # Whatever the previous core left behind goes now, before this one's clone lands beside it.
  drop_clone

  src="$WORK/$dir"
  [[ $DROP_CLONES -eq 1 ]] && CLONE_TO_DROP="$src"
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
  # $makeflags is a list of make variables and must word-split - hence the disable below.
  # shellcheck disable=SC2086
  ( cd "$bdir" && make -f "${makefile:-Makefile}" platform=unix HAVE_CDROM=0 $makeflags \
        CC="$CC_ORBIS" CXX="$CXX_ORBIS" AR=llvm-ar -k -j"$JOBS" ) >"$WORK/$core.log" 2>&1

  mapfile -t objs < <(find "$src" -name '*.o' -type f | sort)
  if [[ ${#objs[@]} -eq 0 ]]; then
    err="$(grep -m1 -E 'error:|No such file|No rule' "$WORK/$core.log" | cut -c1-46)"
    report "$core" COMPILE - "$commit" "${err:-no objects; single-shot link?}"
    continue
  fi

  # ⚠ --error-limit=0, AND IT CHANGES WHAT THE VERDICT MEANS. ld.lld stops REPORTING after twenty
  # errors and says so in a line easy to miss. mupen64plus_next's first link here printed twenty
  # undefined glXxx symbols and not one of the twenty-two its recompiler was missing, which reads
  # as "a GL core, otherwise complete" - the opposite of the truth. The note below quotes the first
  # symbol either way, but the log has to hold all of them for anyone to work from.
  if ! ld.lld "${objs[@]}" "$WEAK_STUBS" "${KEEP_SYMS[@]}" --error-limit=0 -o "$WORK/$core.elf" \
        -m elf_x86_64 -pie --script "${ORBIS_LINK_SCRIPT:-$HERE/orbis-module.ld}" --eh-frame-hdr --no-rosegment \
        -L"$TOOLCHAIN/lib" -L"$ORBIS_COMPAT_DIR/build" "$COMMON_LIB" "$CORE_SUPPORT_LIB" \
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

