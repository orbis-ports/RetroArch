#!/usr/bin/env bash
# Build a libretro core for the PlayStation 4, either way round.
#
#   ps4/build-core.sh --core <dir> --out <path> [--prx] [--source <file>]...
#
#   --core     the core's source directory
#   --out      where to write: an .a for the static build, a .prx for the module
#   --prx      build a loadable module instead of an archive
#   --source   a source file, relative to --core, or absolute
#   --common   a libretro-common directory to use INSTEAD of the core's own
#   --name     display name; also writes a minimal <out>.info beside the output
#   --no-content  this core runs with no content loaded (a game rather than an emulator)
#
# ⚠ --no-content IS WHAT PUTS A CORE IN THE MAIN MENU'S "Standalone Cores" ROW, and its absence
# is why that row said "No Cores Available" with two perfectly good contentless cores installed.
# The row is filtered by content_show_contentless_cores, which defaults to 2 =
# MENU_CONTENTLESS_CORES_DISPLAY_SINGLE_PURPOSE (config.def.h:919), and that filter demands BOTH
# supports_no_game AND single_purpose (menu/menu_contentless_cores.c:433). This template wrote
# neither, so every core built by it was excluded from a list it belonged in.
#
# ⚠ THE .info IS WHAT MAKES THE MENU READABLE. RetroArch's core list shows the FILENAME
# until it finds <core>.info in the core-info directory (/data/retroarch/info here); the
# name the core reports through retro_get_system_info only appears once it is loaded. So a
# console with three cores shows three filenames, which is exactly as useful as it sounds.
# This writes the minimum RetroArch reads; a core with real metadata should ship the real
# file from libretro-super instead.
#
# ⚠ --common EXISTS BECAUSE A CORE'S VENDORED libretro-common PREDATES THIS PLATFORM.
# libretro-2048's copy still has the orbisdev-era ORBIS branch in vfs_implementation.c -
# `#include <sys/dirent.h>` (a header this SDK does not ship) and `#include <orbisFile.h>`
# (psxdev's SDK). Every core that vendored libretro-common before the port existed carries
# the same thing. The frontend's copy is the one that has been fixed for this console, and
# libretro-common is a utility library rather than part of the core/frontend ABI, so using
# the frontend's is not a compatibility risk in the way sharing an allocator would be.
#
# ⚠ THE TWO BUILDS DIFFER IN MORE THAN THE LINK LINE, and that is the point of this script.
#
# A STATIC core is compiled with -DSTATIC_LINKING and omits its own copies of
# libretro-common: the frontend it is linked into already has them, and two copies in one
# link is a duplicate-symbol error.
#
# A PRX core must carry them. It is a separate image with its own symbol table, and the
# frontend's copies are not visible to it - the first attempt at this linked without them
# and got `undefined symbol: fill_pathname_join`. libretro's ABI needs no imports from the
# eboot, which is what makes a core a good fit for a PRX; the core's *implementation* is a
# different question, and this is its answer.
#
# Toolchain flags are the same ones Makefile.orbis uses, for the same reasons - see the
# comments there and in orbis-compat/cmake/ps4-openorbis.cmake.
set -euo pipefail

# ⚠ The six lines that cannot be shared - see orbis-compat/scripts/ps4/orbis-env.sh. Sibling
# directory first, because that is what a fresh clone of the orbis-ports organisation looks like.
for _c in "${ORBIS_COMPAT_DIR:-}" "$(dirname "${BASH_SOURCE[0]}")/../../orbis-compat" "$HOME/src-ps4/orbis-compat"; do
  [[ -n "$_c" && -f "$_c/scripts/ps4/orbis-env.sh" ]] && { ORBIS_COMPAT_DIR="$_c"; break; }
done
[[ -n "${ORBIS_COMPAT_DIR:-}" ]] || {
  echo "build-core: orbis-compat not found - clone https://github.com/orbis-ports/orbis-compat" >&2
  echo "            next to this repository, or set ORBIS_COMPAT_DIR" >&2
  exit 1
}
. "${ORBIS_COMPAT_DIR}/scripts/ps4/orbis-env.sh"
TOOLCHAIN="$OO_PS4_TOOLCHAIN"
ORBIS_COMPAT="$ORBIS_COMPAT_DIR"
CORE_DIR=""
OUT=""
COMMON_DIR=""
DISPLAY_NAME=""
NO_CONTENT=0
PRX=0
EXTRA_SOURCES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core)   CORE_DIR="$2"; shift 2 ;;
    --out)    OUT="$2";      shift 2 ;;
    --prx)    PRX=1;         shift ;;
    --source) EXTRA_SOURCES+=("$2"); shift 2 ;;
    --common) COMMON_DIR="$2"; shift 2 ;;
    --name)   DISPLAY_NAME="$2"; shift 2 ;;
    --no-content) NO_CONTENT=1;   shift ;;
    *) echo "build-core: unknown argument: $1" >&2; exit 2 ;;
  esac
done

[[ -n "$CORE_DIR" ]] || { echo "build-core: --core is required" >&2; exit 2; }
[[ -n "$OUT"      ]] || { echo "build-core: --out is required"  >&2; exit 2; }
[[ -f "$TOOLCHAIN/link.x" ]] || { echo "build-core: no toolchain at $TOOLCHAIN" >&2; exit 1; }

CORE_DIR="$(cd "$CORE_DIR" && pwd)"
INT="$(mktemp -d)"
trap 'rm -rf "$INT"' EXIT

CFLAGS=(
  --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2
  -isysroot "$TOOLCHAIN"
  -isystem "$ORBIS_COMPAT/include"
  -isystem "$TOOLCHAIN/include"
  -DORBIS -D__ORBIS__ -D__PS4__ -DPS4 -D_BSD_SOURCE=1 -D__LIBRETRO__
)

# The replacement's headers go FIRST, so the core's own code and the common code it links
# against agree on one set of declarations rather than each seeing its own.
if [[ -n "$COMMON_DIR" ]]; then
  CFLAGS+=(-I"$COMMON_DIR/include")
fi
CFLAGS+=(-I"$CORE_DIR" -I"$CORE_DIR/libretro-common/include")

# The core's own sources. Anything beyond these comes in with --source.
SOURCES=("${EXTRA_SOURCES[@]}")

if [[ $PRX -eq 0 ]]; then
  CFLAGS+=(-DSTATIC_LINKING)
fi

echo "== compiling $(basename "$CORE_DIR") ($([[ $PRX -eq 1 ]] && echo prx || echo static))"
OBJS=()
for src in "${SOURCES[@]}"; do
  obj="$INT/$(echo "$src" | tr '/' '_')"
  obj="${obj%.c}.o"
  if [[ "$src" == /* ]]; then
    path="$src"
    obj="$INT/$(basename "$src" .c).o"
  else
    path="$CORE_DIR/$src"
  fi
  clang "${CFLAGS[@]}" -c -o "$obj" "$path"
  OBJS+=("$obj")
done

mkdir -p "$(dirname "$OUT")"

if [[ $PRX -eq 0 ]]; then
  rm -f "$OUT"
  llvm-ar rcs "$OUT" "${OBJS[@]}"
  echo "static: $OUT ($(du -h "$OUT" | cut -f1))"
else
  # crtlib.o rather than crt1.o, and --lib rather than --eboot: the two differences that
  # make a module instead of an executable. Everything else matches the eboot link.
  # "cannot find entry symbol _start" is expected and not an error: a module has no entry
  # point, which is the whole difference from the eboot.
  ld.lld "${OBJS[@]}" -o "$INT/core.elf" \
    -m elf_x86_64 -pie --script "$TOOLCHAIN/link.x" --eh-frame-hdr --no-rosegment \
    -L"$TOOLCHAIN/lib" -lc -lkernel -lc++ "$TOOLCHAIN/lib/crtlib.o" 2>&1 \
    | grep -v "cannot find entry symbol _start" || true

  [[ -f "$INT/core.elf" ]] || { echo "build-core: the module did not link" >&2; exit 1; }

  # create-fself writes --lib relative to the working directory, so it runs in one we own.
  ( cd "$INT" && "$TOOLCHAIN/bin/linux/create-fself" \
      -in=core.elf -out=core.oelf \
      --lib="$(basename "$OUT")" --paid 0x3800000000000011 )

  cp "$INT/$(basename "$OUT")" "$OUT"
  echo "prx: $OUT ($(du -h "$OUT" | cut -f1))"
fi

if [[ -n "$DISPLAY_NAME" ]]; then
  info="${OUT%.*}.info"
  cat > "$info" <<INFO
display_name = "$DISPLAY_NAME"
corename = "$DISPLAY_NAME"
systemname = "$DISPLAY_NAME"
manufacturer = ""
categories = "Game"
supports_no_game = "$([[ $NO_CONTENT -eq 1 ]] && echo true || echo false)"
single_purpose = "$([[ $NO_CONTENT -eq 1 ]] && echo true || echo false)"
authors = ""
supported_extensions = ""
license = ""
permissions = ""
display_version = ""
supports_no_game = "true"
INFO
  echo "info: $info"
fi
