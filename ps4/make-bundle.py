#!/usr/bin/env python3
"""Build the all-cores archive the website offers for a console with no network.

    ps4/make-bundle.py --dist dist --out orbis-cores-<index date>.zip

⚠ The .prx files go in directly rather than the per-core zips. Someone installing without console
networking wants to unzip once and copy; a zip of zips makes them unpack a hundred times.

This was build_bundle() in ps4/make-site.py. The page itself moved to orbis-ports/website; the
bundle stayed here because it is built from dist/, which only the cores workflow has.
"""
import argparse
import glob
import os
import sys
import zipfile


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dist", required=True, help="directory of *.prx.zip")
    ap.add_argument("--out", required=True, help="the archive to write")
    args = ap.parse_args()

    members = sorted(glob.glob(os.path.join(args.dist, "*.prx.zip")))
    if not members:
        sys.stderr.write("!! no *.prx.zip in %s\n" % args.dist)
        return 1
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED) as bundle:
        for path in members:
            with zipfile.ZipFile(path) as inner:
                for entry in inner.infolist():
                    bundle.writestr(entry.filename, inner.read(entry.filename))
    print("== %s: %d cores, %d bytes" % (args.out, len(members), os.path.getsize(args.out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
