#!/usr/bin/env python3
"""Turn a `fatal:` crash line from the console back into a function name.

⚠ WHY THIS IS NOT `addr2line`, AND WHY THE OBVIOUS SHORTCUT IS WRONG. The eboot is a PIE: every
address in its symbol table is an offset from zero, and every address in a crash log is that
offset plus a load base the kernel chose. Subtracting a base "everyone knows" - 0x400000 - is a
guess, and a guess that fails SILENTLY: on 2026-08-31 it put rip 0x1a01572 inside musl's qsort
helper `trinkle`, at a byte that is not an instruction boundary, on a `shrd` that cannot touch
memory at all. The name was wrong and looked plausible. Searching for a base that lands on a
memory-touching instruction boundary does not rescue it either - there are 386 of them in this
image, so the constraint identifies nothing.

So this tool never guesses. It takes the base from the log, and it can check itself:

  1. --map-start: orbis-compat's handler prints, right under the register line,
         fatal: rip 0x... is in 0xSTART-0xEND prot 0x5 (r-x) ...
     0xSTART is the load base of the mapping rip is in. That is the number to pass.

  2. --code: the same handler dumps 32 bytes around rip,
         fatal: code 0x... (rip in this row): 48 8b 05 e6 13 81 01 ...
     Paste those bytes and this locates them in .text by CONTENT. No base is needed, and if the
     bytes are not found the answer is "this ELF is not the binary that crashed" - which is worth
     far more than a name that fits nothing.

Give both when you have both: they are computed independently and must agree.

    ps4/symbolise.py --rip 0x1a01572 --map-start 0x400000
    ps4/symbolise.py --rip 0x1a01572 --code "48 8b 05 e6 13 81 01 49 39 c4 74 d3" --code-addr 0x1a01562

SPDX-License-Identifier: MIT
"""
import argparse, re, subprocess, sys

def sh(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout

def text_section(elf):
    """(vaddr, file_offset, size) of .text, read from the section headers."""
    for line in sh("llvm-readelf", "-S", elf).splitlines():
        m = re.match(r'\s*\[\s*\d+\]\s+\.text\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
        if m:
            return int(m.group(1), 16), int(m.group(2), 16), int(m.group(3), 16)
    sys.exit("symbolise: no .text section in %s" % elf)

def functions(elf):
    """Every FUNC symbol with a size, sorted. Local statics included - they are most of the
    image, and leaving them out is how a lookup lands on the wrong name rather than on none."""
    out = []
    for line in sh("llvm-readelf", "--symbols", elf).splitlines():
        p = line.split()
        if len(p) < 8 or not p[0].endswith(':'):
            continue
        try:
            val, size = int(p[1], 16), int(p[2], 0)
        except ValueError:
            continue
        if p[3] == 'FUNC' and size:
            out.append((val, size, p[-1]))
    out.sort()
    return out

def containing(funcs, addr):
    return [f for f in funcs if f[0] <= addr < f[0] + f[1]]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default="retroarch_orbis.elf")
    ap.add_argument("--rip", required=True, help="the rip from the `fatal: rip ...` line")
    ap.add_argument("--map-start", help="0xSTART from the `fatal: rip ... is in 0xSTART-0xEND` line")
    ap.add_argument("--code", help="hex bytes from a `fatal: code ...` row")
    ap.add_argument("--code-addr", help="the address that `fatal: code` row was printed for")
    a = ap.parse_args()

    rip = int(a.rip, 16)
    tv, toff, tsize = text_section(a.elf)
    bases = {}

    if a.map_start:
        bases["--map-start"] = int(a.map_start, 16)

    if a.code:
        blob = bytes(int(x, 16) for x in a.code.replace(",", " ").split())
        if len(blob) < 8:
            sys.exit("symbolise: give at least 8 bytes of --code; fewer will match by accident")
        text = open(a.elf, "rb").read()[toff:toff + tsize]
        hits = []
        i = text.find(blob)
        while i != -1:
            hits.append(tv + i)
            i = text.find(blob, i + 1)
        if not hits:
            print("--code: NOT FOUND in .text.")
            print("  Those bytes are not in this binary, so this ELF is not the one that crashed.")
            print("  Find the eboot that was installed, or rebuild that exact commit.")
        elif len(hits) > 1:
            print("--code: %d matches - ambiguous, paste more bytes." % len(hits))
        else:
            code_vaddr = hits[0]
            if a.code_addr:
                bases["--code"] = int(a.code_addr, 16) - code_vaddr
            else:
                print("--code: found at elf vaddr %#x (pass --code-addr to turn it into a base)"
                      % code_vaddr)

    if not bases:
        sys.exit("symbolise: no base could be established - pass --map-start and/or "
                 "--code with --code-addr.")

    if len(set(bases.values())) > 1:
        print("⚠ THE TWO SOURCES DISAGREE, so at least one of them is not describing this ELF:")
        for k, v in bases.items():
            print("    %-12s -> base %#x" % (k, v))
        print("  Trust neither. Get the binary that actually ran.")
        return 1

    base = next(iter(bases.values()))
    addr = rip - base
    print("base        %#x   (from %s)" % (base, ", ".join(bases)))
    print("rip %#x -> elf vaddr %#x" % (rip, addr))
    if not (tv <= addr < tv + tsize):
        print("⚠ that is OUTSIDE .text (%#x..%#x) - wrong base or wrong binary." % (tv, tv + tsize))
        return 1

    hit = containing(functions(a.elf), addr)
    if hit:
        v, s, n = hit[0]
        print("FUNCTION    %s  +%#x   [%#x .. %#x)" % (n, addr - v, v, v + s))
        print()
        lo = max(v, addr - 0x30)
        for line in sh("llvm-objdump", "-d",
                       "--start-address=%#x" % v, "--stop-address=%#x" % (v + s),
                       a.elf).splitlines():
            m = re.match(r'\s*([0-9a-f]+):', line)
            if m and lo <= int(m.group(1), 16) <= addr + 0x20:
                mark = "  <-- rip" if int(m.group(1), 16) == addr else ""
                print(line.rstrip() + mark)
        # ⚠ A rip that is not an instruction boundary is the tell that the base is wrong, and it
        # is the check that would have caught the `trinkle` answer before it was written down.
        starts = {int(m.group(1), 16)
                  for m in (re.match(r'\s*([0-9a-f]+):', l)
                            for l in sh("llvm-objdump", "-d",
                                        "--start-address=%#x" % v,
                                        "--stop-address=%#x" % (v + s), a.elf).splitlines())
                  if m}
        if addr not in starts:
            print()
            print("⚠ rip is MID-INSTRUCTION in this function. A CPU cannot fault there, so the")
            print("  base or the binary is wrong however plausible the name above looks.")
    else:
        print("no FUNC symbol covers that address (it may be in a PLT or a data-in-text island)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
