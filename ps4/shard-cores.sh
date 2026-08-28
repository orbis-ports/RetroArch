#!/usr/bin/env bash
# Split the recipe's buildable cores into N shards, deterministically.
#
#   ps4/shard-cores.sh --recipe <file> <index> <total>     names for one shard, one per line
#   ps4/shard-cores.sh --recipe <file> --plan 8            every shard, with its load
#   ps4/shard-cores.sh --recipe <file> --all               the whole set, in order
#
#   --recipe <file>    a libretro-super recipe (recipes/linux/cores-linux-x64-generic)
#   --weights <file>   size table used as a build-cost proxy   (default ps4/CORE-STATUS.md)
#   --round-robin      ignore the weights; deal cards in name order
#   --include-forks    do not subtract PS4_CORE_FORKS (see below)
#   --plan N           print the composition of all N shards and exit
#   --all              print the whole core set and exit
#
# ⚠ THE SET IS NOT SIMPLY "GENERIC", BECAUSE build-cores.sh's IS NOT EITHER. `--all` there takes
# every GENERIC core, but core_make_flags() overrules the recipe's build type for a named few:
# mupen64plus_next is GENERIC_GL upstream because GLideN64 is its default renderer, and it ships
# paraLLEl-RDP over Vulkan, which is the renderer this console can actually run. A shard list
# built from `$6=="GENERIC"` alone would silently drop the port's only N64 cores.
#
# So the override names are READ OUT OF build-cores.sh rather than copied here. Two lists that
# have to agree are two lists that will not, and the failure is silent - a core that simply never
# appears in any shard, in a matrix nobody reads line by line.
#
# ⚠ AND THE FORKS COME OUT. build-cores.sh clones UPSTREAM, builds mednafen_psx_hw, and then
# refuses to write it over the hand-ported one (search PS4_CORE_FORKS there for the incident).
# Ten minutes of a shard's wall clock for a file that is deleted. The fork has its own job.
#
# ⚠ DETERMINISTIC MEANS BYTE-IDENTICAL OUTPUT FOR THE SAME RECIPE, ON ANY MACHINE. No `sort -R`,
# no `$RANDOM`, no hash whose iteration order is an implementation detail, and LC_ALL=C so that
# a collating sequence with a different idea about `_` cannot re-deal the shards. A rerun of one
# failed shard has to build the same cores the first attempt did, or the publish job assembles
# an index over a set nobody chose.
set -euo pipefail
export LC_ALL=C

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

RECIPE=""; WEIGHTS="$HERE/CORE-STATUS.md"; ROUND_ROBIN=0; INCLUDE_FORKS=0
PLAN=0; SHOW_ALL=0; ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --recipe)        RECIPE="$2";  shift 2 ;;
    --weights)       WEIGHTS="$2"; shift 2 ;;
    --round-robin)   ROUND_ROBIN=1; shift ;;
    --include-forks) INCLUDE_FORKS=1; shift ;;
    --plan)          PLAN="$2"; shift 2 ;;
    --all)           SHOW_ALL=1; shift ;;
    -h|--help)       sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \?//;$d'; exit 0 ;;
    -*) echo "shard-cores: unknown argument: $1" >&2; exit 2 ;;
    *)  ARGS+=("$1"); shift ;;
  esac
done
[[ -n "$RECIPE" && -f "$RECIPE" ]] || { echo "shard-cores: --recipe <file> is required" >&2; exit 2; }

# --- the core set ------------------------------------------------------------------------------

# ⚠ GENERIC *AND* CMAKE, AND FROM ps4/core-recipe-extra AS WELL AS THE RECIPE. Getting either
# half wrong publishes a release that silently omits cores that built fine:
#
#   * CMAKE was excluded while this port had no toolchain file for it. It has one now, and
#     swanstation, arduous and thepowdertoy build - but a sweep that still filters on
#     $6=="GENERIC" leaves all three out of every shard, so they reach no index and no user.
#   * core-recipe-extra carries cores libretro-super's recipe does not have. dosbox_pure is the
#     port's only working DOS core and lives only there; a sharder reading $RECIPE alone has
#     never heard of it.
#
# Both were true of run 33211929629, which went green and published 101 cores - the same 101 as
# before, with none of the four built that day. A sweep must draw from exactly what
# build-cores.sh would.
RECIPE_EXTRA="$HERE/core-recipe-extra"
mapfile -t SET < <(awk '!/^[[:space:]]*(#|$)/ && ($6=="GENERIC" || $6=="CMAKE") {print $1}' \
    "$RECIPE" ${RECIPE_EXTRA:+$([[ -f "$RECIPE_EXTRA" ]] && echo "$RECIPE_EXTRA")} | sort -u)

# Plus whatever core_make_flags() in build-cores.sh overrules the build type for. Parsed, not
# copied: see the note at the top.
BUILDER="$HERE/build-cores.sh"
if [[ -f "$BUILDER" ]]; then
  mapfile -t OVERRIDES < <(
    awk '/^core_make_flags\(\)/ {inf=1; next}
         inf && /^}/          {exit}
         inf && /^[[:space:]]*[A-Za-z0-9_|]+\)/ {
             line=$0
             sub(/^[[:space:]]*/, "", line)
             sub(/\).*$/, "", line)
             n = split(line, alt, "|")
             for (i = 1; i <= n; i++) if (alt[i] != "*") print alt[i]
         }' "$BUILDER"
  )
  if [[ ${#OVERRIDES[@]} -eq 0 ]]; then
    echo "shard-cores: could not read core_make_flags() out of $BUILDER" >&2
    echo "shard-cores: refusing to shard a set that may be missing cores" >&2
    exit 1
  fi
  # A core the recipe does not carry cannot be built whatever the override says.
  for _o in "${OVERRIDES[@]}"; do
    awk -v c="$_o" '!/^[[:space:]]*(#|$)/ && $1==c {found=1} END{exit !found}' "$RECIPE" \
      && SET+=("$_o")
  done
fi

# Minus the forks, which have jobs of their own.
if [[ $INCLUDE_FORKS -eq 0 ]]; then
  _forks=" ${PS4_CORE_FORKS:-mednafen_psx_hw} "
  _kept=()
  for _c in "${SET[@]}"; do
    [[ "$_forks" == *" $_c "* ]] || _kept+=("$_c")
  done
  SET=("${_kept[@]}")
fi

mapfile -t SET < <(printf '%s\n' "${SET[@]}" | sort -u)
[[ ${#SET[@]} -gt 0 ]] || { echo "shard-cores: no cores in $RECIPE" >&2; exit 1; }

if [[ $SHOW_ALL -eq 1 ]]; then printf '%s\n' "${SET[@]}"; exit 0; fi

# --- weights -----------------------------------------------------------------------------------

# ⚠ THIS IS A PROXY AND IT IS NAMED ONE. CORE-STATUS.md records the size of the .prx, not the
# minutes the build took, and nothing here measures the latter. Output size is the only per-core
# number this repository actually has, and it tracks build cost well enough at the extremes that
# matter: fbalpha2012 at 27M and mame2003_plus at 50M are also the two that hold a shard open
# longest, and putting them in one shard by an accident of alphabet is exactly the imbalance
# worth avoiding. Cores with no row - the 62 that did not build, and anything new upstream - get
# DEFAULT_KIB, because a core that fails to link still compiles first and is not free.
#
# ⚠ AND IT READS ONLY THE FIRST TABLE. CORE-STATUS.md has two, and in the second one - "Did not
# build" - the column in this position holds a failure reason, not a size. Parsing both gives
# `link failed` a numeric weight of zero and hands every broken core to shard 0.
DEFAULT_KIB=1536

# shellcheck disable=SC2016  # this is an awk program; $2/$4 are awk's fields, not the shell's.
weights_awk='
  /^## Built and on the console/ { intab=1; next }
  /^## /                         { intab=0 }
  intab && /^\|[[:space:]]*`/ {
      core = $2; gsub(/[` \t]/, "", core)
      size = $4; gsub(/[ \t]/, "", size)
      # 732K, 1000K, 1,8M, 3.2M, 27M - decimal comma or point, K/M/G.
      unit = substr(size, length(size), 1)
      num  = substr(size, 1, length(size) - 1)
      gsub(/,/, ".", num)
      if (num !~ /^[0-9]+(\.[0-9]+)?$/) next
      if      (unit == "K") kib = num
      else if (unit == "M") kib = num * 1024
      else if (unit == "G") kib = num * 1024 * 1024
      else next
      printf "%s %d\n", core, kib + 0.5
  }'

declare -A W=()
if [[ $ROUND_ROBIN -eq 0 && -f "$WEIGHTS" ]]; then
  while read -r _c _k; do W["$_c"]="$_k"; done < <(awk -F'|' "$weights_awk" "$WEIGHTS")
fi

# --- assignment --------------------------------------------------------------------------------

# ⚠ LONGEST-PROCESSING-TIME-FIRST, AND THE TIE BREAKS BY NAME, NOT BY WHATEVER sort FELT LIKE.
# Heaviest core first into the lightest shard so far; equal weights order by name; equally loaded
# shards take the lowest index. Every step of that is total and reproducible, which is the whole
# requirement - a rerun of shard 5 must rebuild shard 5's cores and not a fresh deal.
assign() { # total -> "shard<TAB>core" per line
  local total="$1"
  local c
  for c in "${SET[@]}"; do printf '%s\t%s\n' "${W[$c]:-$DEFAULT_KIB}" "$c"; done \
    | if [[ $ROUND_ROBIN -eq 1 ]]; then sort -k2,2; else sort -k1,1nr -k2,2; fi \
    | awk -v total="$total" -v rr="$ROUND_ROBIN" '
        BEGIN { for (i = 0; i < total; i++) load[i] = 0 }
        {
          if (rr) { s = NR % total }
          else {
            s = 0
            for (i = 1; i < total; i++) if (load[i] < load[s]) s = i
          }
          load[s] += $1
          printf "%d\t%s\n", s, $2
        }'
}

if [[ "$PLAN" != 0 ]]; then
  [[ "$PLAN" =~ ^[0-9]+$ && "$PLAN" -gt 0 ]] || { echo "shard-cores: --plan wants a count" >&2; exit 2; }
  _kib_table="$(for _c in "${SET[@]}"; do printf '%s\t%s\n' "$_c" "${W[$_c]:-$DEFAULT_KIB}"; done)"
  assign "$PLAN" | awk -F'\t' -v total="$PLAN" -v tbl="$_kib_table" '
      BEGIN {
        n = split(tbl, rows, "\n")
        for (i = 1; i <= n; i++) { split(rows[i], f, "\t"); kib[f[1]] = f[2] }
      }
      { cnt[$1]++; load[$1] += kib[$2]; names[$1] = names[$1] (names[$1] ? " " : "") $2 }
      END {
        for (i = 0; i < total; i++)
          printf "shard %d: %3d cores %7.1f MiB  %s\n",
                 i, cnt[i] + 0, load[i] / 1024, names[i]
      }'
  exit 0
fi

[[ ${#ARGS[@]} -eq 2 ]] || { echo "shard-cores: need <index> <total>" >&2; exit 2; }
IDX="${ARGS[0]}"; TOTAL="${ARGS[1]}"
[[ "$IDX" =~ ^[0-9]+$ && "$TOTAL" =~ ^[0-9]+$ && "$TOTAL" -gt 0 ]] \
  || { echo "shard-cores: <index> and <total> must be numbers, <total> > 0" >&2; exit 2; }
[[ "$IDX" -lt "$TOTAL" ]] \
  || { echo "shard-cores: shard $IDX does not exist in a split of $TOTAL" >&2; exit 2; }

assign "$TOTAL" | awk -F'\t' -v want="$IDX" '$1 == want {print $2}' | sort
