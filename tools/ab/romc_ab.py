#!/usr/bin/env python3
"""romc_ab.py -- the core's ROMC stream against MAME's, on the F8 probe cartridges.

MAME's F8 core, grafted (pico/channelf/emu/f8trace.py in the firmware tree, and
this MAME build) to write one 10-byte record per ROMC state when F8_ROMC_TRACE
names a file: romc, dbus, PC0, PC1, DC0, DC1 (16-bit little-endian), the registers
as the state left them. core/tests/romc_trace_test --run prints the same stream
from core/chf/f8.c's romc_hook. Each probe's window is the records from the ROMC 00
that fetches its START opcode (PC0 = START + 1) to the one that fetches its DONE
opcode, both ends included.

    romc_ab.py --gen [PROBE ...]     write core/tests/romc_golden.h: each probe's
                                     record count and FNV-1a hash of the window
                                     (committed, so the romc_trace test needs
                                     neither MAME nor this script)
    romc_ab.py --check [PROBE ...]   run MAME and the core live and diff the
                                     windows record by record; --exe gives
                                     romc_trace_test
    romc_ab.py --states [PROBE ...]  which of the 32 states MAME's windows reach

Exit status: 0 equal, 1 a difference or a failure, 77 when MAME is missing.

SPDX-License-Identifier: GPL-3.0-or-later
"""

import argparse
import concurrent.futures
import glob
import os
import shutil
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import f8_ab as A                  # noqa: E402
import f8_probes as P              # noqa: E402

ROOT = A.ROOT
GOLDEN = os.path.join(ROOT, "core", "tests", "romc_golden.h")
REC = 10


def window(records, start, done):
    """the records from the fetch of START's opcode to the fetch of DONE's, or None"""
    n = len(records) // REC
    s_lo, s_hi = (start + 1) & 0xFF, ((start + 1) >> 8) & 0xFF
    d_lo, d_hi = (done + 1) & 0xFF, ((done + 1) >> 8) & 0xFF
    i = None
    for k in range(n):
        o = k * REC
        if records[o] == 0 and records[o + 2] == s_lo and records[o + 3] == s_hi:
            i = k
            break
    if i is None:
        return None
    for k in range(i + 1, n):
        o = k * REC
        if records[o] == 0 and records[o + 2] == d_lo and records[o + 3] == d_hi:
            return records[i * REC:(k + 1) * REC]
    return None


def core_window(exe, name, img, start, done, work):
    path = os.path.join(work, name + ".bin")
    with open(path, "wb") as f:
        f.write(img)
    r = subprocess.run([exe, "--run", path, "%X" % start, "%X" % done], capture_output=True)
    if r.returncode != 0 or not r.stdout:
        sys.exit("romc_ab.py: core run of %s failed: rc %d: %s" % (name, r.returncode, r.stderr.decode().strip()))
    return r.stdout


def compare(name, mame, core):
    nm, nc = len(mame) // REC, len(core) // REC
    for k in range(min(nm, nc)):
        a, b = mame[k * REC:(k + 1) * REC], core[k * REC:(k + 1) * REC]
        if a != b:
            print("%s: first difference at record %d" % (name, k))
            print("  romc dbus pc0  pc1  dc0  dc1")
            for j in range(max(0, k - 3), min(nm, nc, k + 2)):
                for who, buf in (("MAME", mame), ("core", core)):
                    r = buf[j * REC:(j + 1) * REC]
                    print("%s %s  %02X   %02X  %04X %04X %04X %04X" % (
                        "->" if j == k else "  ", who, r[0], r[1], r[2] | r[3] << 8, r[4] | r[5] << 8,
                        r[6] | r[7] << 8, r[8] | r[9] << 8))
            return False
    if nm != nc:
        print("%s: MAME has %d records, core %d" % (name, nm, nc))
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probes", nargs="*")
    ap.add_argument("--gen", action="store_true")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--states", action="store_true")
    ap.add_argument("--exe", default=(sorted(glob.glob(os.path.join(ROOT, "build*", "core", "tests", "romc_trace_test")))
                                      or [os.path.join(ROOT, "build", "core", "tests", "romc_trace_test")])[0])
    ap.add_argument("--keep", metavar="DIR", help="keep the work files here")
    ap.add_argument("--jobs", type=int, default=4, help="MAME runs at once")
    a = ap.parse_args()

    allp = P.probes()
    work = a.keep or tempfile.mkdtemp(prefix="romcab-")
    os.makedirs(work, exist_ok=True)
    try:
        A.find_mame()
        names = A.probe_names(a.probes)
        with concurrent.futures.ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = {n: ex.submit(A.mame_run, n, *allp[n], work) for n in names}
            bad = 0
            total = 0
            entries = []
            hist = [0] * 32
            for n in names:
                img, st, dn = allp[n]
                _, records, _ = futs[n].result()
                win = window(records, st, dn)
                if win is None:
                    sys.exit("romc_ab.py: no ROMC window for %s in MAME's trace" % n)
                for k in range(len(win) // REC):
                    hist[win[k * REC] & 31] += 1
                total += len(win) // REC
                if a.states:
                    print("%-8s %9d records" % (n, len(win) // REC))
                    continue
                core = core_window(a.exe, n, img, st, dn, work)
                ok = compare(n, win, core)
                print("%-8s %9d records  %s" % (n, len(win) // REC, "equal" if ok else "DIFFERENT"))
                sys.stdout.flush()
                bad += 0 if ok else 1
                entries.append((n, len(win) // REC, A.fnv(win)))
        print("%d records in all, %d probe(s) differ" % (total, bad))
        print("states reached (MAME):" + "".join(" %02X" % s for s in range(32) if hist[s]))
        print("not reached:          " + "".join(" %02X" % s for s in range(32) if not hist[s]))
        if a.gen and not bad and not a.probes and not a.states:
            out = ["/* romc_golden.h -- GENERATED by tools/ab/romc_ab.py --gen: for each probe of",
                   " * f8_golden.h, the count and FNV-1a hash of the ROMC records (10 bytes each: romc,",
                   " * dbus, PC0, PC1, DC0, DC1, little-endian) MAME's F8 core wrote from the fetch of",
                   " * the probe's START opcode to the fetch of its DONE opcode. romc_trace_test holds",
                   " * core/chf/f8.c's romc_hook stream to them. */",
                   "static const struct { const char *name; unsigned records; uint64_t hash; } romc_probes[] = {"]
            for n, cnt, h in entries:
                out.append("    { \"%s\", %d, 0x%016XULL }," % (n, cnt, h))
            out.append("};")
            with open(GOLDEN, "w") as f:
                f.write("\n".join(out) + "\n")
            print("wrote", GOLDEN)
        return 1 if bad else 0
    except A.Missing as e:
        print(e)
        return 77
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
