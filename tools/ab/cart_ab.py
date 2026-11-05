#!/usr/bin/env python3
"""cart_ab.py -- the whole machine against MAME, frame for frame, on cartridges.

    cart_ab.py [--frames N] [--keys SPEC] [--machine-cart fujinet] [--report] CART...

For each cartridge image (a plain .bin; the Golf mapper is picked by CRC like
MAME's softlist does) it runs MAME (`channelf`, no BIOS needed) with
tools/ab/chf_frames.lua dumping every frame as 192x262 32-bit ARGB, and runs
build*/core/tests/chf_headless on the same image, -raw dumping the same way,
then compares the two frame by frame and prints how many of the frames are
identical, how many once the first lead-in frames are skipped, and where the
first difference is.

Key presses go to both sides: SPEC is "frame:bit:down" (bit = the matrix bit of
host.h, e.g. 1 = player 1 keypad 2 = fire wire, 24 START), comma separated,
e.g. 120:24:1,126:24:0 presses START at frame 120. --joy takes
"frame:stick:bits" (host.h's stick bits: 1 UP 2 RIGHT 4 DOWN 8 LEFT 16 FIRE) and
--play is a canned session (START, keypad digits, both discs in every
direction, fire) that gets the games out of attract mode.

Environment: MAME_DIR (default ~/Workspace/mame), CHF_HEADLESS (the runner),
ARCAB_WORK (scratch directory). Exit status 1 if any cartridge differs after
the lead-in.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chfab  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("carts", nargs="+")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--keys", default="")
    ap.add_argument("--joy", default="", help="frame:stick:bits,... digital stick events")
    ap.add_argument("--play", action="store_true", help="a canned input session")
    ap.add_argument("--skip", type=int, default=2,
                    help="lead-in frames not counted against the verdict (default 2)")
    ap.add_argument("--fujinet", action="store_true",
                    help="run the image on the FujiNet cart in both (an app or CONFIG)")
    ap.add_argument("--report", action="store_true", help="print every cartridge, not only the differing ones")
    ap.add_argument("--keep", action="store_true", help="keep the raw dumps (mame.raw / ours.raw)")
    args = ap.parse_args()

    runner = chfab.find_runner()
    work = chfab.work_dir()
    keys = []
    for item in [k for k in args.keys.split(",") if k]:
        f, b, d = (int(x) for x in item.split(":"))
        keys.append((f, b, d))

    joys = []
    for item in [k for k in args.joy.split(",") if k]:
        f, st, b = (int(x, 0) for x in item.split(":"))
        joys.append((f, st, b))
    if args.play:
        # START, then each stick through its directions and fire, keypad digits
        keys += [(60, 24, 1), (64, 24, 0), (70, 25, 1), (74, 25, 0), (80, 26, 1), (84, 26, 0)]
        for k in range(12):
            keys += [(100 + 6 * k, k, 1), (103 + 6 * k, k, 0), (100 + 6 * k, 12 + k, 1), (103 + 6 * k, 12 + k, 0)]
        t = 190
        for st in (0, 1):
            for b in (1, 2, 4, 8, 3, 6, 12, 9, 16):
                joys += [(t, st, b), (t + 8, st, 0)]
                t += 14
        keys += [(t, 24, 1), (t + 4, 24, 0)]

    # MAME has no fire button: it is the keypad-2 wire, so a fire event is that
    # key, which is exactly what the machine ORs in
    # MAME applies a value set from its frame notifier one frame late (the
    # ioport layer polls at the next frame), so its events go one frame earlier
    # than ours, which latch at the start of the frame they name
    press = []
    for f, b, d in keys:
        tag, mask = chfab.key_port(b)
        press.append((max(f - 1, 0), "%s:%d:%d" % (tag, mask, d)))
    jstate = {}
    for f, st, b in sorted(joys):
        old = jstate.get(st, 0)
        for bit in range(5):
            if (old ^ b) >> bit & 1:
                if bit == 4:
                    tag, mask = chfab.key_port(1 + 12 * st)
                else:
                    tag, mask = chfab.stick_port(st, bit)
                press.append((max(f - 1, 0), "%s:%d:%d" % (tag, mask, (b >> bit) & 1)))
        jstate[st] = b
    press.sort(key=lambda x: x[0])
    ours_keys = list(keys)

    bad = 0
    for cart in args.carts:
        name = os.path.basename(cart)
        mraw = os.path.join(work, "mame.raw")
        oraw = os.path.join(work, "ours.raw")
        for p in (mraw, oraw):
            if os.path.exists(p):
                os.unlink(p)
        env = {"CHF_FRAMES": mraw, "CHF_FRAMES_N": args.frames + 1,
               "CHF_PRESS": ",".join("%d:%s" % (f, p) for f, p in press),
               "FUJINET_TCP": "127.0.0.1:9"}
        chfab.run_mame(os.path.abspath(cart), os.path.join(HERE, "chf_frames.lua"), env,
                       args.frames / 59 + 3, timeout=240,
                       cartslot="fujinet" if args.fujinet else None)
        cmd = [runner, "-frames", str(args.frames), "-raw", oraw,
               "-cart" if args.fujinet else "-game", cart]
        for f, b, d in ours_keys:
            cmd += ["-key", "%d:%d:%d" % (f, b, d)]
        for f, st, b in joys:
            cmd += ["-joy", "%d:%d:%d" % (f, st, b)]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=300)

        a = open(mraw, "rb").read() if os.path.exists(mraw) else b""
        # MAME's frame notifier runs BEFORE the screen's update, so what pixels()
        # holds at notifier n is frame n-1: MAME's first dump is the blank power-on
        # bitmap and ours[i] is MAME's dump i+1
        a = a[chfab.FRAME:]
        b = open(oraw, "rb").read()
        n = min(len(a), len(b)) // chfab.FRAME
        diff = []
        for i in range(n):
            fa = a[i * chfab.FRAME:(i + 1) * chfab.FRAME]
            fb = b[i * chfab.FRAME:(i + 1) * chfab.FRAME]
            if fa != fb:
                diff.append(i)
        same = n - len(diff)
        late = [i for i in diff if i >= args.skip]
        status = "OK  " if n and not late else "DIFF"
        if status == "DIFF":
            bad += 1
        if args.report or status == "DIFF":
            print("%s %-44s %d/%d frames identical%s" % (
                status, name[:44], same, n,
                "" if not diff else ", differing: %s" % (
                    ",".join(map(str, diff[:8])) + (",..." if len(diff) > 8 else ""))))
        if not args.keep:
            for p in (mraw, oraw):
                if os.path.exists(p):
                    os.unlink(p)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
