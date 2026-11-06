#!/usr/bin/env python3
"""cart_ab.py -- the whole machine against MAME, frame for frame, on cartridges.

    cart_ab.py [--machine M] [--frames N] [--fujinet] [--play] [--keys SPEC]
               [--ctl SPEC] [--report] CART...

For each cartridge image it runs MAME (`channelf`, `channlf2`, `sabavdpl` or
`sabavpl2`, the real BIOS from MAME's own ROM path) with tools/ab/chf_frames.lua
dumping every frame's bitmap, the VRAM and the port latches, and runs
chf_headless (core/tests) on the same image with the BIOS in $CHF_BIOS_DIR,
dumping the same, then compares them frame by frame: the 204 x 232 pictures
(the PAL machines' rows are 5 lines tall in MAME, 4 in ours: one line per row
is compared) AND the VRAM and the four port latches at every frame. It prints
how many frames are identical and where the first difference is.

A plain image runs as MAME's `chess` Videocart (and a 256K one as `multi`);
--fujinet runs it on the FujiNet cartridge in both (an app or CONFIG, link
down).

Input goes to both sides: --keys "frame:bit:down" (console buttons, bit 0 TIME ..
3 START), --ctl "frame:player:bit:down" (player 0 = the right controller /
port 1; bit = the controller byte's: 0 right 1 left 2 back 3 forward 4 twist
CCW 5 twist CW 6 pull 7 push), comma separated. --play is a canned session
that presses everything.

Environment: MAME_DIR (default ~/Workspace/mame), CHF_HEADLESS (the runner),
CHF_BIOS_DIR (the real BIOS: skipped, exit 77, without it), CHFAB_WORK. Exit
status 1 if any cartridge differs.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chfab  # noqa: E402


def canned_session():
    keys, ctls = [], []
    t = 40
    for b in range(4):
        keys += [(t, b, 1), (t + 4, b, 0)]
        t += 10
    for p in (0, 1):
        for b in range(8):
            ctls += [(t, p, b, 1), (t + 5, p, b, 0)]
            t += 9
        for combo in ((0, 3), (1, 3), (0, 2), (1, 2), (4, 6), (5, 7)):
            ctls += [(t, p, combo[0], 1), (t, p, combo[1], 1), (t + 6, p, combo[0], 0), (t + 6, p, combo[1], 0)]
            t += 10
    return keys, ctls


def parse_events(spec, n):
    out = []
    for item in [k for k in spec.split(",") if k]:
        out.append(tuple(int(x) for x in item.split(":")))
        assert len(out[-1]) == n, item
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("carts", nargs="+")
    ap.add_argument("--machine", default="channelf", choices=sorted(chfab.MACHINES))
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--keys", default="")
    ap.add_argument("--ctl", default="")
    ap.add_argument("--play", action="store_true", help="a canned input session")
    ap.add_argument("--fujinet", action="store_true", help="run the image on the FujiNet cart in both")
    ap.add_argument("--skip", type=int, default=0, help="lead-in frames not counted against the verdict")
    ap.add_argument("--iolog", action="store_true", help="also compare every I/O write and its CPU clock")
    ap.add_argument("--report", action="store_true", help="print every cartridge, not only the differing ones")
    args = ap.parse_args()

    bios = chfab.bios_dir()
    if not bios:
        print("SKIP: no real BIOS (set CHF_BIOS_DIR)")
        return 77
    work = chfab.work_dir()
    keys = parse_events(args.keys, 3)
    ctls = parse_events(args.ctl, 4)
    if args.play:
        k, c = canned_session()
        keys, ctls = keys + k, ctls + c

    bad = 0
    for cart in args.carts:
        r = chfab.ab(args.machine, cart, args.frames, work, keys=keys, ctls=ctls, fujinet=args.fujinet,
                     skip=args.skip, iolog=args.iolog)
        if not r.ok:
            bad += 1
        if args.report or not r.ok:
            print(chfab.report(os.path.basename(cart), args.machine, r))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
