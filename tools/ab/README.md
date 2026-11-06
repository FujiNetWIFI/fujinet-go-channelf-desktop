# tools/ab -- the console held to MAME

MAME (`~/Workspace/mame`, which has the FujiNet cartridge graft) is the reference.
Everything here runs it headless (`-video none -sound none -nothrottle`, hard-killed
by a timeout) and runs `chf_headless` (core/tests) on the same thing; the two are
compared and any difference is a bug in one of them. CI has neither MAME nor a ROM:
these are run by hand, and the committed goldens (core/tests/config_golden.h) carry
the result into CI. Tools that need the real BIOS exit 77 without `CHF_BIOS_DIR`
(a directory with `sl31253.rom`, `sl90025.rom` and `sl31254.rom`).

## What is compared

Per frame, from frame 0: the 204 x 232 picture (the PAL machines draw each VRAM row 5
lines tall in MAME and 4 in ours; one line per row is compared), the 8192 VRAM bytes,
the four port latches, and every I/O write with the CPU clock it happened at (a read is
logged as a write to the unmapped port $F0, so the clock of every read and every value
read is covered too). The F8 core's own timing therefore is held to MAME's: INS/OUTS 0/1
are direct accesses after the ROMC 1C, everything else sees the clock at the start of its
bus cycle.

MAME's frame notifier runs after the screen update but `pixels()` still holds the
previous frame: our frame i is MAME's pixel dump i + 1, and our VRAM record i is MAME's
record i. `chfab.py` has the pairing; `chf_frames.lua` the dumps (`pixels()` returns a
tuple: write only its first value; a soft reset re-runs the autoboot script, which the
script guards against; Lua integers overflow at 9.2 s of attoseconds, which it avoids).

## The tools

| tool | what |
|---|---|
| `chfab.py` | shared: running MAME and chf_headless, the probe-machine script builder, `ab()` (one run, one `Result`) |
| `chf_frames.lua` | MAME side: per-frame bitmap, VRAM and latches, an I/O write tap, soft resets, the cart's tick |
| `probes/vm.asm` | the probe machine: a Videocart that plays a byte script of port accesses and memory accesses (assemble with `~/asl/asl`) |
| `cart_ab.py` | frame for frame on any image (plain, or `--fujinet` for an app/CONFIG), `--play` for a canned input session, `--iolog`, `--machine` |
| `video_probes.py` | the 16 palette-column combinations, the strobe's semantics (bit 5 alone, held, repeated, absent), the hidden rows and columns, every complement and unused bit, read-back with both controllers and the console buttons pressed while it polls, port 0's bit 6 |
| `sound_ab.py` | the tone against `-samplerate 48000 -wavwrite`: every mode, changes inside the 2 ms forced-on window, every sub-sample phase, the same mode again, a long decay, on all four machines (the Videoplay 2 group's 0.5 gain included): the whole 16-bit stream, sample for sample |
| `softlist_ab.py` | every Videocart type through a private software list (synthetic entries, our own images): standard, Maze and Hangman's 2102, Schach's RAM, the two multicarts' banks, and RESET |
| `config_golden.py` | regenerates core/tests/config_golden.h: the hashes of CONFIG's frames (link down), NTSC and PAL; `--stub-rompath` for the stand-in BIOS's set |
| `app_ab.py` | the FujiNet apps: each scenario is recorded once from a private fujinet-pc (in its own network namespace) and replayed by `boipreplay.py` to MAME and to us; frames, VRAM, every I/O write and the tick are compared, and the replay checks both sides ask the same questions in the same order. fujitest, fujiboot, fujibank, CONFIG booting a plain image, and CONFIG booting M.U.L.E. (64K APPBANK), 5 Card Stud, Fujitzee and Battleship |
| `boipreplay.py` | the BoIP recorder (a proxy) and replay server |
| `all_carts.sh` | `cart_ab.py` over a directory of images |

## Running them

    cmake -S . -B build-wp2 -G Ninja -DFRONTEND=none -DWITH_FUJINET=OFF -DFUJINET_SRC=$PWD/third_party/fujinet-firmware
    cmake --build build-wp2 --target chf_headless
    export CHF_BIOS_DIR=/path/to/bios CHFAB_WORK=/tmp/chfab
    tools/ab/video_probes.py --report
    tools/ab/sound_ab.py --report
    tools/ab/softlist_ab.py --report
    tools/ab/cart_ab.py --report --frames 900 --iolog --fujinet config.bin
    tools/ab/app_ab.py --report

Never run them against the user's FujiNet (9995) or the production servers: MAME gets
`FUJINET_TCP=127.0.0.1:9` unless a scenario says otherwise, and `app_ab.py` records
inside `unshare -rn`, where nothing but its own fujinet-pc exists.

## Results (2026-11-05, real BIOS sl31253 / sl90025, MAME with the FujiNet graft)

All four machines (NTSC `channelf`, `channlf2`; PAL `sabavdpl`, `sabavpl2`), every probe:
frames, VRAM, latches and I/O clocks identical; the sound streams sample-exact; every
Videocart type identical; CONFIG with the link down identical for 900 frames; soft resets
at any frame identical; the apps identical through the replay; the tick identical every frame.
