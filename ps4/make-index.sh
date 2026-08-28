#!/usr/bin/env bash
# Zip every built core and write the listing RetroArch's Core Downloader reads.
#
#   ps4/make-index.sh [--in <dir>] [--out <dir>] [core]...
#
#   --in <dir>       where the .prx files are        (default ~/.cache/ps4-cores/out)
#   --out <dir>      zips and the index go here      (default <in>/index)
#   --index <name>   listing filename                (default .index-extended)
#   --date <D>       the date field, YYYY-MM-DD      (default today, UTC)
#   --info <dir>     copy the matching .info files into --out as well
#   --check          re-verify --out against the .prx files in --in; write nothing
#   --verify         check --out's index against its own archives alone; needs no .prx
#   [core]...        only these (bare name or filename); default is everything in --in
#
# One line per core, three space-separated fields, and the parser is in this repository:
# core_updater_list.c:780-870 splits on runs of spaces into [date] [crc] [filename] and drops any
# line that does not yield all three.
#
#     2026-08-25 a1b2c3d4 2048_libretro.prx.zip
#
# ⚠ THE CRC IS OF THE .prx, NOT OF THE .zip, AND THE PLAN SAYS OTHERWISE. This is the one place
# the prose and the code disagree, and the code is what runs on the console.
#
# The CRC in this file is only ever used for one thing: deciding whether an already-installed
# core is current. task_core_updater.c:832-853 hashes `download_handle->local_core_path` and
# compares it to the entry's crc. local_core_path is built by core_updater_list.c:466-471, which
# joins the cores directory to the listing's filename and then strips the archive extension - so
# it names the EXTRACTED module, `2048_libretro.prx`, and never the archive it arrived in.
#
# Publish the archive's CRC instead and the comparison can never match. Nothing errors: every
# core simply re-downloads on every visit to the Core Downloader, forever, and the only symptom
# is a console that seems to have no idea what it already has.
#
# ⚠ AND IT IS COMPUTED FROM THE FILE THAT SHIPS, NEVER FROM A REBUILD. create-fself is not
# byte-reproducible - two links from identical objects produced .prx files differing at byte 833.
# So an index generated in a later job, from a fresh build of the same commit, describes files
# that are not the ones in the release. Same failure as above and harder to see. The archive is
# written here, from the bytes hashed here, and the two leave together.
#
# ⚠ THE ZIP IS NOT DECORATION EITHER. RetroArch decides a download is an archive by its EXTENSION
# (path_is_compressed_file on the listing's filename), and that same decision is what makes it
# strip `.zip` to get the installed name. A bare `.prx` in the listing would be installed as
# `foo_libretro.prx` too - but these link at 700K to 50M and compress to about half that, over a
# console's wifi.
#
# Case and padding: string_hex_to_unsigned (libretro-common/string/stdstring.c:624) takes an
# optional 0x, accepts either case, and returns 0 on any non-hex character. Zero is also its
# failure value, which core_updater_list.c:375 treats as a bad line - so eight lowercase hex
# digits, zero-padded, and a core whose .prx genuinely hashes to 0 would be unrepresentable in
# this format at all. That is a one-in-four-billion problem this script reports rather than hides.
set -euo pipefail
export LC_ALL=C

IN="${HOME}/.cache/ps4-cores/out"; OUT=""; INDEX=".index-extended"
DATE=""; INFO=""; CHECK=0; VERIFY=0; NAMES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --in)    IN="$2";    shift 2 ;;
    --out)   OUT="$2";   shift 2 ;;
    --index) INDEX="$2"; shift 2 ;;
    --date)  DATE="$2";  shift 2 ;;
    --info)  INFO="$2";  shift 2 ;;
    --check) CHECK=1;    shift ;;
    --verify) VERIFY=1;  shift ;;
    -h|--help) sed -n '2,/^set -euo/p' "${BASH_SOURCE[0]}" | sed 's/^# \?//;$d'; exit 0 ;;
    -*) echo "make-index: unknown argument: $1" >&2; exit 2 ;;
    *)  NAMES+=("$1"); shift ;;
  esac
done
[[ $VERIFY -eq 1 || -d "$IN" ]] || { echo "make-index: no such directory: $IN" >&2; exit 2; }
OUT="${OUT:-$IN/index}"
DATE="${DATE:-$(date -u +%Y-%m-%d)}"

[[ "$DATE" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] \
  || { echo "make-index: --date wants YYYY-MM-DD (core_updater_list.c:331)" >&2; exit 2; }

command -v python3 >/dev/null \
  || { echo "make-index: python3 is required (zipfile + zlib do the work)" >&2; exit 1; }

# ⚠ --verify IS THE ONLY CHECK AVAILABLE ONCE THE .prx FILES ARE GONE, AND IT IS THE ONE THE
# PUBLISH JOB NEEDS. It re-reads each archive the index names, unpacks the module inside it and
# hashes that - so it confirms the published CRC against the bytes the console will actually
# receive, with no rebuild anywhere. A rebuild would prove nothing: create-fself is not
# byte-reproducible, so the second .prx is a different file with a different CRC and the
# comparison would condemn cores that are perfectly correct.
if [[ $VERIFY -eq 1 ]]; then
  [[ -d "$OUT" ]] || { echo "make-index: no such directory: $OUT" >&2; exit 2; }
  python3 - "$OUT" "$INDEX" <<'VERIFY_PY'
import os, sys, zlib, zipfile

out, index_name = sys.argv[1:3]
index_path = os.path.join(out, index_name)
if not os.path.exists(index_path):
    sys.exit("make-index: %s has no %s" % (out, index_name))

lines = open(index_path).read().splitlines()
zips = sorted(f for f in os.listdir(out) if f.endswith(".prx.zip"))
problems, named = [], []

for n, line in enumerate(lines, 1):
    fields = line.split(" ")
    if len(fields) != 3 or not all(fields):
        problems.append("%s:%d: %d fields, core_updater_list.c:854 wants 3"
                        % (index_name, n, len(fields)))
        continue
    date, crc_str, archive = fields
    named.append(archive)
    zpath = os.path.join(out, archive)
    if not os.path.exists(zpath):
        problems.append("%s:%d: names %s, which is not here" % (index_name, n, archive))
        continue
    try:
        crc = int(crc_str, 16)
    except ValueError:
        problems.append("%s:%d: %r is not hex" % (index_name, n, crc_str))
        continue
    member = archive[: -len(".zip")]
    try:
        with zipfile.ZipFile(zpath) as zf:
            if zf.namelist() != [member]:
                problems.append("%s: holds %r, not [%r]" % (archive, zf.namelist(), member))
                continue
            data = zf.read(member)
    except Exception as exc:
        problems.append("%s: will not unpack: %s" % (archive, exc))
        continue
    actual = zlib.crc32(data) & 0xFFFFFFFF
    if actual != crc:
        problems.append("%s: the index says %08x, the module inside hashes to %08x"
                        % (archive, crc, actual))

for orphan in sorted(set(zips) - set(named)):
    problems.append("%s is here but no line names it" % orphan)

if problems:
    for p in problems:
        print("make-index: " + p, file=sys.stderr)
    sys.exit("make-index: %d problem(s) between %s and %s" % (len(problems), index_name, out))

print("make-index: verified %d lines against %d archives in %s" % (len(lines), len(zips), out))
VERIFY_PY
  exit 0
fi

mkdir -p "$OUT"

python3 - "$IN" "$OUT" "$INDEX" "$DATE" "$INFO" "$CHECK" "${NAMES[@]+"${NAMES[@]}"}" <<'PY'
import os, sys, zlib, zipfile, time

src, out, index_name, date, info_dir, check = sys.argv[1:7]
wanted = sys.argv[7:]
check = check == "1"

def normalise(n):
    for suffix in (".prx.zip", ".zip", ".prx"):
        if n.endswith(suffix):
            n = n[: -len(suffix)]
            break
    return n if n.endswith("_libretro") else n + "_libretro"

names = sorted(f[:-4] for f in os.listdir(src) if f.endswith(".prx"))
if wanted:
    want = {normalise(w) for w in wanted}
    missing = sorted(want - set(names))
    if missing:
        sys.exit("make-index: no .prx in %s for: %s" % (src, " ".join(missing)))
    names = [n for n in names if n in want]

if not names:
    sys.exit("make-index: no .prx files in %s" % src)

lines, zips, failures = [], 0, []

for name in names:
    prx = os.path.join(src, name + ".prx")
    member = name + ".prx"
    archive = member + ".zip"
    zpath = os.path.join(out, archive)

    with open(prx, "rb") as fh:
        payload = fh.read()

    # The number that goes in the index: CRC32 of the module as it will exist on the console.
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    if crc == 0:
        failures.append("%s: CRC32 is 0, which core_updater_list.c:375 rejects as a bad line" % name)
        continue

    if not check:
        # Deterministic given the same .prx: the member's timestamp is the .prx's own, so
        # re-running this over an unchanged build directory rewrites identical archives.
        mtime = time.gmtime(os.path.getmtime(prx))
        zi = zipfile.ZipInfo(member, date_time=mtime[:6])
        zi.compress_type = zipfile.ZIP_DEFLATED
        zi.external_attr = 0o644 << 16
        tmp = zpath + ".part"
        with zipfile.ZipFile(tmp, "w") as zf:
            zf.writestr(zi, payload, compresslevel=9)
        os.replace(tmp, zpath)

    if not os.path.exists(zpath):
        failures.append("%s: %s was never written" % (name, archive))
        continue

    # ⚠ UNPACK IT AGAIN AND HASH WHAT COMES OUT, rather than trusting the CRC the archive claims.
    # A zip carries its member's CRC32 in two headers, so comparing the index to THOSE compares
    # a number to a copy of itself: an archive whose deflate stream was truncated or scribbled on
    # after the fact still reports the right CRC in its central directory and fails only on the
    # console. Decompressing the member is what actually crosses the compressor, and at ~1s per
    # 100 MB it is free next to the build that produced the input.
    with zipfile.ZipFile(zpath) as zf:
        entries = zf.namelist()
        if entries != [member]:
            failures.append("%s: archive holds %r, not [%r]" % (name, entries, member))
            continue
        if zf.getinfo(member).CRC != crc:
            failures.append("%s: archive header CRC %08x != module CRC %08x"
                            % (name, zf.getinfo(member).CRC, crc))
            continue
        try:
            unpacked = zf.read(member)
        except Exception as exc:
            failures.append("%s: %s will not unpack: %s" % (name, archive, exc))
            continue
    if len(unpacked) != len(payload) or (zlib.crc32(unpacked) & 0xFFFFFFFF) != crc:
        failures.append("%s: %s unpacks to %d bytes / %08x, module is %d / %08x"
                        % (name, archive, len(unpacked),
                           zlib.crc32(unpacked) & 0xFFFFFFFF, len(payload), crc))
        continue

    zips += 1
    lines.append("%s %08x %s" % (date, crc, archive))

if failures:
    for f in failures:
        print("make-index: " + f, file=sys.stderr)
    sys.exit("make-index: %d core(s) failed verification" % len(failures))

index_path = os.path.join(out, index_name)
if not check:
    with open(index_path, "w", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")

# ⚠ THE ASSERTION THE PLAN ASKS FOR, MADE HERE RATHER THAN ONLY IN THE WORKFLOW. A green run that
# produced an index describing fewer cores than it uploaded is the exact "silent success" this is
# supposed to catch, and the cheapest place to catch it is where both numbers already exist.
on_disk = sorted(f for f in os.listdir(out) if f.endswith(".prx.zip"))
written = len(open(index_path).read().splitlines()) if os.path.exists(index_path) else 0
if written != len(on_disk):
    sys.exit("make-index: %s has %d lines but %s holds %d archives"
             % (index_name, written, out, len(on_disk)))
if zips != len(lines):
    sys.exit("make-index: %d archives against %d index lines" % (zips, len(lines)))

# The metadata half: RetroArch's "Update Core Info Files" fetches a second listing from the
# assets URL, and the frontend package ships these too (plan phase 02). Same three fields.
if info_dir:
    if not os.path.isdir(info_dir):
        sys.exit("make-index: --info %s is not a directory" % info_dir)
    staged = 0
    for name in names:
        i = os.path.join(info_dir, name + ".info")
        if os.path.exists(i):
            with open(i, "rb") as fh:
                data = fh.read()
            with open(os.path.join(out, name + ".info"), "wb") as fh:
                fh.write(data)
            staged += 1
    print("make-index: staged %d/%d .info files" % (staged, len(names)))

total = sum(os.path.getsize(os.path.join(out, f)) for f in on_disk)
print("make-index: %d cores, %d archives, %.1f MiB, %s -> %s"
      % (len(names), len(on_disk), total / 1048576.0, index_name, out))
PY
