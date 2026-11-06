#!/usr/bin/env python3
"""video_probes.py -- the console's video and ports against MAME, by probe programs.

    video_probes.py [--machine M] [--frames N] [--only NAME] [--report]

Each probe is a script for the probe machine (tools/ab/probes/vm.asm: a
Videocart that plays a table of port accesses, assembled with ~/asl/asl) and is
run in MAME and in chf_headless on the real BIOS ($CHF_BIOS_DIR), then compared
frame for frame: the 204 x 232 picture, the VRAM and the four port latches at
every frame, and every I/O write with the CPU clock it happened at (which also
carries every port READ, logged to the unmapped port $F0).

  palettes    the 16 combinations of the palette columns 125 and 126 (every
              value pair), each on its own row with all four pixel values
  strobe      port 0's semantics: $60 then $50, bit 5 alone, bit 5 held while
              the colour and address change, writes WITHOUT bit 5, repeats
  hidden      cells outside the TV's window (columns 0-3, 106-127, rows 0-3, 62-63)
  complement  every column, the rows, the unused bits of ports 1, 4 and 5
  readback    ports 0, 1, 4 and 5 read after writes, with the controllers and the
              console buttons pressed and released while it polls, and port 0's
              bit 6 (the controller gate) both ways
  sound-bits  port 5's sound bits are not part of the row

Exit status 1 on any difference; 77 when the real BIOS, MAME or the assembler
is missing.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import chfab  # noqa: E402
from chfab import Script  # noqa: E402


def palettes():
    s = Script()
    row = 4
    for a in range(4):
        for b in range(4):
            s.pix(row, 125, a).pix(row, 126, b).pix(row, 127, (a + b + 1) & 3)
            for k in range(4):
                s.pix(row, 4 + 3 * k, k).pix(row, 5 + 3 * k, k).pix(row, 6 + 3 * k, k)
            row += 1
    # the same four colours, one pixel wide, on the rest of the picture's rows: each of the 16 palettes
    # again but with the palette columns written BEFORE the pixels, and an overwrite of 125 afterwards
    for k in range(4):
        s.pix(24, 20 + k, k)
    s.pix(24, 125, 2).pix(24, 126, 0)           # palette 1 (a = 2 -> 1, b = 0)
    s.pix(25, 126, 3).pix(25, 20, 1)            # palette 2
    s.pix(25, 125, 1)                           # palette 3 (a = 1 -> 0, b = 3 -> 2): 2
    return s


def strobe():
    s = Script()
    # 1. the BIOS's own: $60 then $50
    s.pix(10, 10, 1)
    # 2. level-triggered: bit 5 alone, twice; the second write lands again
    s.w1(~(2 << 6)).w4(~12).w5(~10 & 0x3F).w0(0x20).w0(0x20)
    # 3. the strobe high, the colour and the address change, no port 0 write: nothing is stored;
    #    then a port 0 write stores the NEW colour at the NEW address
    s.w1(~(3 << 6)).w4(~14).w5(~10 & 0x3F).w0(0x60)
    s.w1(~(1 << 6)).w4(~16)
    s.w4(~18).w5(~11 & 0x3F)
    s.w0(0x50)                                  # bit 5 clear: no store (the colour 1 would land at (11, 18))
    s.w0(0x60)                                  # now it lands
    # 4. writes without bit 5 store nothing, whatever else they carry
    for i, v in enumerate((0x00, 0x10, 0x40, 0xC0, 0x80, 0x0F, 0x5F, 0xDF)):
        s.w1(~(2 << 6)).w4(~(20 + i)).w5(~12 & 0x3F).w0(v)
    # 5. every value with bit 5 stores
    for i, v in enumerate((0x20, 0x30, 0xA0, 0xE0, 0xFF, 0x3F, 0x21, 0x7F)):
        s.w1(~((i & 3) << 6)).w4(~(20 + i)).w5(~13 & 0x3F).w0(v)
    # 6. a strobe leaves the registers alone: a second strobe after changing only the colour
    #    re-stores at the same cell
    s.w1(~(1 << 6)).w4(~30).w5(~14 & 0x3F).w0(0x60).w0(0x50)
    s.w1(~(3 << 6)).w0(0x60).w0(0x50)
    # 7. unmapped ports do not store
    s.out(0x02, 0x20).out(0xF1, 0x60).out(0x03, 0x60)
    return s


def hidden():
    s = Script()
    v = 0
    for col in (0, 1, 2, 3, 106, 107, 110, 120, 124, 127):
        s.pix(20, col, (v := v + 1) & 3)
    for row in (0, 1, 2, 3, 62, 63):
        s.pix(row, 50, (v := v + 1) & 3).pix(row, 4, 3).pix(row, 105, 2)
    s.pix(4, 4, 1).pix(4, 105, 2).pix(61, 4, 3).pix(61, 105, 1)    # the corners of the window
    s.pix(3, 4, 3).pix(62, 4, 3).pix(4, 3, 3).pix(4, 106, 3)       # just outside it
    return s


def complement():
    s = Script()
    # a diagonal through every column and enough rows, every value
    for col in range(128):
        s.pix((col * 7) & 63, col, (col >> 1) & 3)
    return s


def complement2():
    s = Script()
    # the unused bits: port 1 below bit 6, port 4 bit 7, port 5 bits 7-6 (sound) must not matter
    for i in range(64):
        row, col, val = 8 + (i % 20), 8 + i, (i * 5) & 3
        s.w1((~(val << 6) & 0xC0) | (i & 0x3F))                     # colour with junk in the low bits
        s.w4(((~col) & 0x7F) | (0x80 if i & 1 else 0))              # bit 7 either way
        s.w5((~row) & 0x3F)                                         # sound bits left at mode 0
        s.w0(0x60).w0(0x50)
    return s


def readback():
    s = Script()
    # latches first: the read value is ~external | latch
    s.w0(0x00).w1(0xA5).w4(0x5A).w5(0x3C)
    for p in (0, 1, 4, 5):
        s.r(p)
    # the controller gate: port 0 bit 6 set / clear, reading both controllers
    for g in (0x00, 0x40, 0x20, 0x60, 0xC0, 0x1F):
        s.w0(g)
        for p in (0, 1, 4):
            s.r(p)
    # poll for a while with the controllers and buttons being pressed and released from outside
    for i in range(160):
        s.w0(0x00 if i % 2 == 0 else 0x40).r(0).r(1).r(4).wait(255).wait(255).wait(255)
    s.w0(0x00).w1(0xFF).w4(0xFF).w5(0xFF)
    for p in (0, 1, 4, 5):
        s.r(p)
    return s


def sound_bits():
    s = Script()
    # the row's own bits with the sound bits at every mode: the pixel lands on the same row
    for mode in range(4):
        s.pix(30 + mode, 40 + mode, mode, sound=mode)
        s.pix(34, 40 + mode, (mode + 1) & 3, sound=(mode + 1) & 3)
    s.w5(0x3F)
    return s


PROBES = {
    "palettes": (palettes, 30, "", ""),
    "strobe": (strobe, 20, "", ""),
    "hidden": (hidden, 20, "", ""),
    "complement": (complement, 30, "", ""),
    "complement2": (complement2, 30, "", ""),
    "readback": (readback, 90,
                 # console buttons and both controllers, pressed and released: frame:bit:down, frame:player:bit:down
                 "10:0:1,14:0:0,18:2:1,22:3:1,26:2:0,30:3:0,34:1:1,38:1:0",
                 "12:0:0:1,16:0:0:0,20:0:7:1,24:0:6:1,28:1:1:1,32:1:0:1,36:1:4:1,36:1:5:1,40:0:4:1,44:1:7:1,48:0:7:0,"
                 "52:0:6:0,56:1:1:0,58:1:0:0,60:1:4:0,60:1:5:0,62:0:4:0,64:1:7:0,66:0:2:1,66:0:3:1,70:0:2:0,70:0:3:0"),
    "sound-bits": (sound_bits, 30, "", ""),
}


def events(spec, n):
    return [tuple(int(x) for x in i.split(":")) for i in spec.split(",") if i]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--machine", default="channelf", choices=sorted(chfab.MACHINES))
    ap.add_argument("--frames", type=int, default=0, help="override each probe's frame count")
    ap.add_argument("--only", action="append", default=[])
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    if not chfab.bios_dir() or not os.path.exists(chfab.MAME) or not chfab.have_asl():
        print("SKIP: needs the real BIOS (CHF_BIOS_DIR), MAME and the assembler")
        return 77
    work = chfab.work_dir()
    bad = 0
    for name, (build, frames, keys, ctls) in PROBES.items():
        if args.only and name not in args.only:
            continue
        path = chfab.write_probe(build(), work, "probe-%s.bin" % name)
        r = chfab.ab(args.machine, path, args.frames or frames, work, keys=events(keys, 3), ctls=events(ctls, 4),
                     iolog=True)
        if not r.ok:
            bad += 1
        if args.report or not r.ok:
            print(chfab.report(name, args.machine, r))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
