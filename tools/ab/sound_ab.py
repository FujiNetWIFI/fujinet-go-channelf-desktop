#!/usr/bin/env python3
"""sound_ab.py -- the console's tone against MAME's recording, sample for sample.

    sound_ab.py [--machine M] [--only NAME] [--report]

Probe programs (tools/ab/probes/vm.asm) switch the sound bits of port 5 in
patterns that stress the model -- every mode, changes closer together than the
2 ms forced-on time, the same mode written again, a change straight from one
tone to another, a change landing on every sub-sample phase, a long decay, port
5 shared with the row -- and the result is compared with MAME's
`-samplerate 48000 -wavwrite` of the same cartridge on the same machine: the
whole 16-bit sample stream, from sample 0, must be identical (the PAL machines
included: the Videoplay 2 group's speaker gain is 0.5).

Exit status 1 on any difference; 77 without the real BIOS, MAME or the assembler.
"""
import argparse
import array
import concurrent.futures
import os
import subprocess
import sys
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chfab  # noqa: E402
from chfab import Script  # noqa: E402


def modes():
    s = Script().wait(255)
    for mode in (1, 0, 2, 0, 3, 0):
        s.w5(mode << 6 | 0x3F)
        for _ in range(30):
            s.wait(255)
    return s.w5(0x3F)


def rapid():
    # toggles with gaps from 1 to 60 units (a sample is ~3 units): every sub-sample phase,
    # and several changes inside the 2 ms forced-on time
    s = Script().wait(100)
    for gap in list(range(1, 61)) + list(range(60, 0, -3)):
        s.w5(0x40 | 0x3F).wait(gap).w5(0x3F).wait(gap + 1)
    for gap in range(1, 40):
        s.w5(0x80 | 0x3F).wait(gap).w5(0xC0 | 0x3F).wait(gap).w5(0x40 | 0x3F).wait(gap)
    for _ in range(40):
        s.wait(255)
    return s.w5(0x3F)


def retrigger():
    s = Script().wait(200)
    s.w5(0x7F)
    for _ in range(20):
        s.wait(255)
    for _ in range(10):                 # the same mode again: no retrigger
        s.w5(0x7F).wait(200)
    s.w5(0xBF)                          # 1 -> 2 straight: retrigger
    for _ in range(20):
        s.wait(255)
    s.w5(0xFF)                          # 2 -> 3
    for _ in range(20):
        s.wait(255)
    s.w5(0x7F)                          # 3 -> 1
    for _ in range(20):
        s.wait(255)
    s.w5(0x3F)                          # off: the phase and envelope freeze
    for _ in range(10):
        s.wait(255)
    s.w5(0x7F)                          # on again, the phase where it was
    for _ in range(30):
        s.wait(255)
    return s.w5(0x3F)


def long_decay():
    s = Script().wait(100).w5(0x7F)
    for _ in range(600):
        s.wait(255)
    s.w5(0xBF)
    for _ in range(600):
        s.wait(255)
    return s.w5(0xFF).wait(255).wait(255).w5(0x3F)


def rowbits():
    s = Script().wait(100)
    for i in range(48):
        mode = (i // 4) & 3
        s.pix(5 + i % 50, 10 + i, i & 3, sound=mode).wait(20 + i)
    return s.w5(0x3F)


# emulated seconds (whole: MAME's -seconds_to_run is an integer)
PROBES = {"modes": (modes, 1), "rapid": (rapid, 2), "retrigger": (retrigger, 2),
          "long": (long_decay, 4), "rowbits": (rowbits, 1)}


def read_wav(path):
    if not os.path.exists(path):
        return array.array("h")
    w = wave.open(path)
    a = array.array("h")
    a.frombytes(w.readframes(w.getnframes()))
    return a


def run_one(machine, name, build, secs, work, runner):
    """-> (ok, text)"""
    mname, pal, hz, first = chfab.MACHINES[machine]
    d = os.path.join(work, "%s-%s" % (machine, name))
    os.makedirs(d, exist_ok=True)
    cart = chfab.write_probe(build(), work, "snd-%s-%s.bin" % (machine, name))
    mwav, owav = os.path.join(d, "mame.wav"), os.path.join(d, "ours.wav")
    for p in (mwav, owav):
        if os.path.exists(p):
            os.unlink(p)
    chfab.run_mame(cart, None, {}, secs, machine=machine,
                   extra=["-samplerate", "48000", "-wavwrite", mwav], timeout=180)
    frames = int(secs * (50.0801 if pal else 60.5303)) + 2
    subprocess.run([runner, "-machine", machine, "-bios-dir", chfab.bios_dir(), "-bios", first,
                    "-frames", str(frames), "-game", cart, "-wav", owav], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)
    a, b = read_wav(mwav), read_wav(owav)
    n = min(len(a), len(b))
    diff = [i for i in range(n) if a[i] != b[i]]
    nz = sum(1 for x in a[:n] if x)
    ok = n > 0 and not diff and nz > 0
    return ok, "%s %-10s %-8s %d samples compared (%d sounding), %d differ%s" % (
        "OK  " if ok else "DIFF", name, machine, n, nz, len(diff),
        "" if not diff else ", first at %d: mame %d ours %d" % (diff[0], a[diff[0]], b[diff[0]]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--machine", action="append", default=[], choices=sorted(chfab.MACHINES))
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    if not chfab.bios_dir() or not os.path.exists(chfab.MAME) or not chfab.have_asl():
        print("SKIP: needs the real BIOS (CHF_BIOS_DIR), MAME and the assembler")
        return 77
    work = chfab.work_dir()
    runner = chfab.find_runner()
    machines = args.machine or sorted(chfab.MACHINES)
    chfab.vm_image(work)        # assemble once, before the threads
    jobs = [(m, n, b, secs) for m in machines for n, (b, secs) in PROBES.items() if not args.only or n in args.only]
    bad = 0
    # side by side, each job in its own scratch directory
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as ex:
        futs = [ex.submit(run_one, m, n, b, secs, work, runner) for (m, n, b, secs) in jobs]
        for f in futs:
            ok, text = f.result()
            bad += not ok
            if args.report or not ok:
                print(text)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
