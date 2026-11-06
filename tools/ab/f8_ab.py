#!/usr/bin/env python3
"""f8_ab.py -- the F8 core against MAME, on generated probe cartridges.

The probes (f8_probes.py) are cartridge images for MAME's `channelf` driver.
MAME runs each from reset with the debugger trace of f8_trace.lua and logs the
CPU state before every instruction, from the probe's START to its DONE.
core/tests/f8_ops_test runs the same images on core/chf/f8.c in a model of the
console (core/tests/f8_probe_machine.h) and prints the same rows. MAME's ROMC
trace (the graft's F8_ROMC_TRACE) comes out of the same run for romc_ab.py.

    f8_ab.py --gen [PROBE ...]     write core/tests/f8_golden.h: the probe images
                                   plus, from MAME, each trace's length and FNV-1a
                                   hash (committed, so the f8_ops test needs
                                   neither MAME nor this script)
    f8_ab.py --check [PROBE ...]   run MAME and the core live and diff the whole
                                   traces; --exe gives f8_ops_test
    f8_ab.py --timing [PROBE ...]  the clock count at every I/O port access and cart RAM
                                   access (the callbacks' `cycles`) against MAME's
                                   (default probes: io, mem); --exe as above
    f8_ab.py --fresh               check the committed goldens (f8_golden.h, romc_golden.h)
                                   against what f8_probes.py generates now: the same probes,
                                   images, START and DONE (no MAME, no core; exit 1 if stale)
    f8_ab.py --mame PROBE          print MAME's trace for one probe
    f8_ab.py --coverage [PROBE ...] which opcodes the probes execute (--exe)
    f8_ab.py --list                the probes

Exit status: 0 equal, 1 a difference or a failure, 77 when MAME is missing
(ctest reads that as skipped).

Environment: MAME (the mame binary; default ~/Workspace/mame/mame, run from its
own directory so -autoboot_script is honoured). MAME is always run under
`timeout -s KILL`: it ignores SIGTERM with -video none.

SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import f8_probes as P          # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
GOLDEN = os.path.join(ROOT, "core", "tests", "f8_golden.h")
LUA = os.path.join(HERE, "f8_trace.lua")

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF


class Missing(Exception):
    pass


def find_mame():
    path = os.path.expanduser(os.environ.get("MAME", "~/Workspace/mame/mame"))
    if os.access(path, os.X_OK):
        return path
    found = shutil.which("mame")
    if found:
        return found
    raise Missing("mame not found (set $MAME)")


def find_exe():
    import glob
    cands = sorted(glob.glob(os.path.join(ROOT, "build*", "core", "tests", "f8_ops_test")))
    return cands[0] if cands else os.path.join(ROOT, "build", "core", "tests", "f8_ops_test")


def fnv(data, h=FNV_OFFSET):
    for b in data:
        h = ((h ^ b) * FNV_PRIME) & MASK64
    return h


def mame_run(name, img, start, done, work, taps=False):
    """Run one probe in MAME. Returns (rows, romc, taps): the normalised trace rows (strings,
    cycles counted from START, START .. DONE), the raw ROMC trace records, and with `taps`
    the port and cart RAM accesses from START on as (kind, addr, data, cycles) tuples."""
    mame = find_mame()
    run = os.path.join(work, name)
    shutil.rmtree(run, ignore_errors=True)
    os.makedirs(run)
    cart = os.path.join(run, "cart.bin")
    with open(cart, "wb") as f:
        f.write(img)
    trace = os.path.join(run, "trace.txt")
    romc = os.path.join(run, "romc.bin")
    env = dict(os.environ, F8_TRACE=trace, F8_DONE="0x%X" % done, F8_FRAMES="3000", F8_ROMC_TRACE=romc)
    tapf = os.path.join(run, "taps.txt")
    if taps:
        env["F8_TAP"] = tapf
    cmd = ["timeout", "-s", "KILL", "900", mame, "channelf", "-bios", "sl31253", "-cart", cart,
           "-video", "none", "-sound", "none", "-nothrottle", "-seconds_to_run", "600",
           "-debug", "-debugger", "none", "-autoboot_script", LUA, "-skip_gameinfo",
           "-cfg_directory", os.path.join(run, "cfg"),
           "-snapshot_directory", os.path.join(run, "snap"),
           "-nvram_directory", os.path.join(run, "nv")]
    # MAME must run from its own directory or it ignores -autoboot_script
    r = subprocess.run(cmd, env=env, cwd=os.path.dirname(os.path.realpath(mame)), stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if not os.path.exists(trace):
        sys.exit("f8_ab.py: MAME wrote no trace for %s:\n%s" % (name, r.stdout[-2000:]))
    rows = []
    on = False
    base = 0
    hit = False
    with open(trace) as f:
        for line in f:
            fl = line.split()
            if len(fl) != 11 or len(fl[0]) != 4 or fl[0][-1] == ":":
                continue
            pc = int(fl[0], 16)
            if not on:
                if pc != start:
                    continue
                on = True
                base = int(fl[10])
            fl[10] = str(int(fl[10]) - base)
            rows.append(" ".join(fl))
            if pc == done:
                hit = True
                break
    if not hit:
        sys.exit("f8_ab.py: %s never reached its end marker in MAME (%d rows)" % (name, len(rows)))
    with open(romc, "rb") as f:
        records = f.read()
    shutil.rmtree(os.path.join(run, "cfg"), ignore_errors=True)
    shutil.rmtree(os.path.join(run, "nv"), ignore_errors=True)
    shutil.rmtree(os.path.join(run, "snap"), ignore_errors=True)
    tl = None
    if taps:
        tl = []
        with open(tapf) as f:
            for line in f:
                kind, addr, data, cyc = line.split()
                c = float(cyc) - base
                if c > -0.5:                   # before START the prologue's own accesses
                    tl.append((kind, int(addr, 16), int(data, 16), int(round(c))))
    return rows, records, tl


def core_rows(exe, name, img, start, done, work):
    path = os.path.join(work, name + ".bin")
    with open(path, "wb") as f:
        f.write(img)
    r = subprocess.run([exe, "--run", path, "%X" % start, "%X" % done], capture_output=True, text=True)
    rows = r.stdout.splitlines()
    if r.returncode != 0 or not rows:
        sys.exit("f8_ab.py: core run of %s failed: rc %d, %d rows: %s" % (name, r.returncode, len(rows), r.stderr.strip()))
    return rows, r.stderr.strip()


def core_accesses(exe, name, img, start, done, work):
    path = os.path.join(work, name + ".bin")
    with open(path, "wb") as f:
        f.write(img)
    r = subprocess.run([exe, "--accesses", path, "%X" % start, "%X" % done], capture_output=True, text=True)
    if r.returncode != 0 or not r.stdout:
        sys.exit("f8_ab.py: core run of %s failed: rc %d: %s" % (name, r.returncode, r.stderr.strip()))
    out = []
    for line in r.stdout.splitlines():
        kind, addr, data, cyc = line.split()
        out.append((kind, int(addr, 16), int(data, 16), int(cyc)))
    return out


def row_hash(rows):
    h = FNV_OFFSET
    for row in rows:
        h = fnv((row + "\n").encode(), h)
    return h


def segments(img):
    """(offset, bytes) runs of the image that are not NOP fill (a gap of 32 or more fill
    bytes ends a run); the offset is from $0800"""
    segs = []
    i = 0
    n = len(img)
    while i < n:
        if img[i] == P.FILL:
            i += 1
            continue
        j = i
        gap = 0
        while j < n and gap < 32:
            gap = gap + 1 if img[j] == P.FILL else 0
            j += 1
        j -= gap
        segs.append((i, img[i:j]))
        i = j
    return segs


def compare(name, mame, core):
    n = min(len(mame), len(core))
    for i in range(n):
        if mame[i] != core[i]:
            lo = max(0, i - 3)
            print("%s: first difference at row %d" % (name, i))
            print("  pc   a  w  is pc1  dc0  dc1  r0..r15                          s1       s2       cycles")
            for k in range(lo, min(n, i + 2)):
                print("%s MAME %s" % ("->" if k == i else "  ", mame[k]))
                print("%s core %s" % ("->" if k == i else "  ", core[k]))
            return False
    if len(mame) != len(core):
        print("%s: MAME has %d rows, core %d" % (name, len(mame), len(core)))
        return False
    return True


def check_fresh():
    """True when the committed goldens describe the probes the generator makes now"""
    import base64
    import re
    ok = True
    allp = P.probes()
    with open(GOLDEN) as f:
        text = f.read()
    seen = []
    for m in re.finditer(r'\{ "(\w+)", (\d+), 0x([0-9A-F]+), 0x([0-9A-F]+), (\d+), 0x([0-9A-F]+)ULL, (\d+), \{\n(.*?)\n    \} \},',
                         text, re.S):
        name, size, start, done = m.group(1), int(m.group(2)), int(m.group(3), 16), int(m.group(4), 16)
        seen.append(name)
        if name not in allp:
            print("f8_golden.h has a probe the generator no longer makes: %s" % name)
            ok = False
            continue
        img = bytearray([P.FILL]) * size
        for sm in re.finditer(r'\{ 0x([0-9A-F]+), (\d+),\n((?:\s+"[^"]*"\n?)+) \},', m.group(8) + " },"):
            lits = re.findall(r'"([^"]*)"', sm.group(3))
            data = base64.b64decode("".join(lits))
            img[int(sm.group(1), 16):int(sm.group(1), 16) + len(data)] = data
        gimg, gst, gdn = allp[name]
        if bytes(img) != gimg or (start, done) != (gst, gdn):
            print("f8_golden.h: probe %s differs from the generator's" % name)
            ok = False
    if seen != list(allp):
        print("f8_golden.h has probes %s, the generator makes %s" % (seen, list(allp)))
        ok = False
    with open(os.path.join(ROOT, "core", "tests", "romc_golden.h")) as f:
        rnames = re.findall(r'\{ "(\w+)", \d+, 0x[0-9A-F]+ULL \}', f.read())
    if rnames != list(allp):
        print("romc_golden.h has probes %s, the generator makes %s" % (rnames, list(allp)))
        ok = False
    print("goldens %s the generator's probes (%d)" % ("match" if ok else "DO NOT match", len(allp)))
    return ok


def probe_names(wanted):
    allp = P.probes()
    if not wanted:
        return list(allp)
    out = []
    for w in wanted:
        hits = [n for n in allp if n == w or n.startswith(w + "_")]
        if not hits:
            sys.exit("f8_ab.py: no probe %s (have %s)" % (w, ", ".join(allp)))
        out.extend(hits)
    return out


def write_golden(entries):
    """entries: (name, size, start, done, rows, hash, segs)"""
    import base64
    nseg = max(len(e[6]) for e in entries)
    out = ["/* f8_golden.h -- GENERATED by tools/ab/f8_ab.py --gen: the probe cartridges",
           " * of tools/ab/f8_probes.py and, from MAME's F8, each trace's row count and",
           " * FNV-1a hash (of the rows, one per instruction, \"pc a w is pc1 dc0 dc1 r0..r15",
           " * s1 s2 cycles\\n\", from START to DONE; see core/tests/f8_probe_machine.h).",
           " * f8_ops_test holds core/chf/f8.c to them; romc_trace_test and romc_shadow_test",
           " * run the same images. An image is its non-NOP runs: seg[].addr is an offset",
           " * from $0800 and .b64 the bytes, base64 (pm_b64 in f8_probe_machine.h); the",
           " * rest of the 8K image is NOP ($2B). */",
           "#define F8_PROBE_NSEG %d" % nseg,
           "static const struct {",
           "    const char *name; unsigned size, start, done, rows; uint64_t hash;",
           "    unsigned nseg; struct { unsigned addr, len; const char *b64; } seg[F8_PROBE_NSEG];",
           "} f8_probes[] = {"]
    for name, size, start, done, rows, h, segs in entries:
        out.append("    { \"%s\", %d, 0x%04X, 0x%04X, %d, 0x%016XULL, %d, {" %
                   (name, size, start, done, rows, h, len(segs)))
        for addr, d in segs:
            b = base64.b64encode(d).decode()
            out.append("        { 0x%04X, %d," % (addr, len(d)))
            for k in range(0, len(b), 96):
                out.append("          \"%s\"" % b[k:k + 96])
            out[-1] += " },"
        out.append("    } },")
    out.append("};")
    with open(GOLDEN, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", GOLDEN)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probes", nargs="*")
    ap.add_argument("--gen", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--mame", metavar="PROBE")
    ap.add_argument("--coverage", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--timing", action="store_true")
    ap.add_argument("--fresh", action="store_true")
    ap.add_argument("--exe", default=find_exe())
    ap.add_argument("--keep", metavar="DIR", help="keep the work files here")
    ap.add_argument("--jobs", type=int, default=4, help="MAME runs at once")
    a = ap.parse_args()

    allp = P.probes()
    if a.fresh:
        return 0 if check_fresh() else 1
    if a.list:
        for n, (img, st, dn) in allp.items():
            print("%-8s start %04X done %04X" % (n, st, dn))
        return 0
    work = a.keep or tempfile.mkdtemp(prefix="f8ab-")
    os.makedirs(work, exist_ok=True)
    try:
        if a.mame:
            img, st, dn = allp[a.mame]
            print("\n".join(mame_run(a.mame, img, st, dn, work)[0]))
            return 0
        names = probe_names(a.probes)
        if a.timing:
            find_mame()
            bad = 0
            for n in (names if a.probes else ["io", "mem"]):
                img, st, dn = allp[n]
                _, _, mame = mame_run(n, img, st, dn, work, taps=True)
                core = core_accesses(a.exe, n, img, st, dn, work)
                same = mame == core
                if not same:
                    for i, (m, c) in enumerate(zip(mame, core)):
                        if m != c:
                            print("%s: access %d: MAME %s, core %s" % (n, i, m, c))
                            break
                    else:
                        print("%s: MAME made %d accesses, core %d" % (n, len(mame), len(core)))
                print("%-8s %6d accesses  %s" % (n, len(mame), "equal" if same else "DIFFERENT"))
                bad += 0 if same else 1
            return 1 if bad else 0
        if a.coverage:
            seen = set()
            for n in names:
                img, st, dn = allp[n]
                p = os.path.join(work, n + ".bin")
                with open(p, "wb") as f:
                    f.write(img)
                r = subprocess.run([a.exe, "--coverage", p, "%X" % st, "%X" % dn], capture_output=True, text=True)
                seen |= {int(x, 16) for x in r.stdout.splitlines()[0].split()}
            print("%d opcodes executed" % len(seen))
            print("not executed: " + " ".join("%02X" % i for i in range(256) if i not in seen))
            return 0
        if a.check or a.gen:
            find_mame()
            with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
                futs = {n: ex.submit(mame_run, n, *allp[n], work) for n in names}
                bad = 0
                total = 0
                entries = []
                for n in names:
                    img, st, dn = allp[n]
                    mame, _, _ = futs[n].result()
                    core, info = core_rows(a.exe, n, img, st, dn, work)
                    ok = compare(n, mame, core)
                    print("%-8s %8d rows  %s   (%s)" % (n, len(mame), "equal" if ok else "DIFFERENT", info))
                    sys.stdout.flush()
                    total += len(mame)
                    bad += 0 if ok else 1
                    entries.append((n, len(img), st, dn, len(mame), row_hash(mame), segments(img)))
            print("%d rows in all, %d probe(s) differ" % (total, bad))
            if a.gen and not bad and not a.probes:
                write_golden(entries)
            elif a.gen and a.probes:
                print("not writing the golden for a subset of the probes")
            return 1 if bad else 0
        ap.print_help()
        return 2
    except Missing as e:
        print(e)
        return 77
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
