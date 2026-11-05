# FujiNet Go Channel F Desktop

A native desktop emulator of the **Fairchild Channel F** with FujiNet built
in: boot the FujiNet CONFIG, browse a network host with the disc and keypad,
load games and FujiNet apps over the network, play them. Part of the FujiNet
Go desktop family (Linux GNOME and KDE, Windows, macOS).

| | |
|---|---|
| **Emulator** | MAME's Channel F driver, its Signetics 2650 and its 2637 UVI video and sound chip, transposed to C (the Studio II and O2 siblings' way). The 2650 runs one instruction at a time and the UVI draws lazily, so the core uses a small fraction of one CPU core. |
| **Machines** | Channel F (NTSC, 60 Hz, 262 lines) and a PAL clone row (50 Hz, 312 lines, 4.433619/5 MHz; **not verified against MAME**, which runs every clone at NTSC). Two hand controllers: 12-key keypad, disc and side fire each, plus the console's START, OPTION, SELECT and RESET. |
| **No BIOS, no ROMs to import** | The Channel F has no BIOS and the character set is part of the UVI source, so nothing but your games is needed. CONFIG and every FujiNet app run as they are. |
| **Cartridge** | The FujiNet Channel F cartridge (`fujinet-firmware` `pico/channelf`): the 8K program window, up to 70K of paged app image (APPBANK), 1K of cart RAM, a 60 Hz tick, the mailbox at `$2B00-$2FFF`, and CONFIG baked in. Its mailbox service (`fujimail`), connector decode (`chfmap`), APPBANK serving (`arcapp`) and wire codec (`fujibus`) are the cartridge firmware's own sources, staged verbatim. A game opened from disk gets its own mapper cartridge instead (standard 4K/8K, Golf), as a real console would have it. |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/D3D11) — each with the display, a Controllers window, Preferences, a live FujiNet console log, and a debugger. |
| **Packages** | Linux: Flatpak (GNOME and KDE runtimes), DEB, RPM, tar.gz. Windows: ZIP and an NSIS installer. macOS: signed/notarised zips (unsigned without secrets). |

The icon is the family mark in the console's own colours, sampled from the
photograph: charcoal case (`#2D2928`), keypad and disc gold (`#F2C300`) and
the wood-grain strip's brown (`#5B3B26`) for the centre disc. The UI accent is
the gold (dark variant `#8A6D00` on light themes), marking held keys, the Map
target, the current line in the disassembly and the status dot.

## Your games

Open a cartridge with **File ▸ Open cartridge…** (or drop it on the window):
a `.bin`, `.rom` or `.chf` image of up to 8 KB, or a `.zip` holding one.
A FujiNet app (an image that claims the mailbox) runs on the FujiNet
cartridge; any other image gets the mapper a real cartridge would have, and
FujiNet is not reachable until the next power cycle. Nothing copyrighted is
embedded in any build: see `COMPLIANCE.md`. The **Palladium Video-Computer-Game**
(16 keys) is not emulated, as MAME marks it not working.

## Controllers window (F9)

A fixed-size floating window drawn like the console: two hand controllers with
the **12-key keypad** (1-9, CLEAR, 0, ENTER), the **disc** and the side fire
button, and the panel's START, OPTION, SELECT and the red RESET. Click a key to
press it, drag the disc to steer it (the pots follow the pointer, analog); held keys light up in the accent colour
whoever holds them. **Map** rebinds any key, disc direction or fire to a host
key or a gamepad button (a key moves to its new target and leaves its old
one), **Defaults** puts them back. Fire is the keypad's `2` wire, as on the
hardware.

| | Default |
|---|---|
| Player 1 keypad | `1`-`9`, **Backspace** = CLEAR, `0`, **Enter** = ENTER |
| Player 1 disc | arrow keys, **Right Ctrl** = fire |
| Player 2 keypad | `Q W E` / `A S D` / `Z X C` / `V B N` |
| Player 2 disc | keypad **8 4 6 2**, keypad **0** = fire |
| START / OPTION / SELECT | F2 / F3 / F4 |
| Gamepads | the left stick drives the disc **proportionally** (Preferences ▸ analog stick; off = eight-way switch), D-pad = directions, South = fire, Start = START, X = OPTION, Back = SELECT |
| RESET / Reset to CONFIG | **F1** / **Escape** |
| Controllers / Fullscreen / Debugger | F9 / F11 / F12 (debugger: F5 run/stop, F7 step, F8 step over, Shift+F8 step out) |

RESET holds the 2650's reset line only, as the console's button does, so a
running FujiNet app restarts from the cartridge's current image; **Reset to
CONFIG** is the power cycle.

## Debugger (F12)

Prompt · CPU & RAM · Disassembly · **Video (the 2637 UVI)** · Sound & I/O ·
Breakpoints · Cart · FujiNet. The CPU tab shows R0, R1-R3 in both register
banks, PSU and PSL decoded flag by flag, the 15-bit IAR and the 8-deep return
stack; breakpoints are on execution, read and write, with step, step over and
step out. The Video tab shows the rendered display (live frame, re-rendered
from the registers, or the frame in progress up to the beam) with the
background and the four objects switchable and boxed; every object with its
position, size, glyph and colour and a rendered bitmap; the 16×13 / 16×26
character screen with its colour bits; the user-defined characters; every
register decoded and editable; collisions; and the font and palette sheets.
The FujiNet tab decodes the cartridge's FujiBus traffic from the moment the
app starts.

## External FujiNet

A switch in Preferences makes the app connect to another FujiNet instance at a
host:port you enter, e.g. `127.0.0.1:11526`, instead of starting the built-in
one.

## Building

```sh
cmake -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

fujinet-firmware is fetched at its pinned commit by the configure step — a
plain `git clone` with no `--recurse-submodules` is enough. To develop against
a working checkout of the cartridge firmware (use an absolute path):

```sh
cmake -B build -DFUJINET_SRC=$HOME/Workspace/fujinet-firmware
```

`-DFRONTEND=none` builds just the core, the session and the tests;
`-DARC_CORE_ONLY=ON` builds only the core and its tests (no SDL3);
`-DWITH_FUJINET=OFF` skips the firmware build (the machine boots CONFIG
reporting the link down).

The core is C11. Qt >= 6.7 and GTK >= 4.16 are needed for the Linux
frontends (an older toolkit makes CMake skip that frontend). The macOS bundle
needs macOS 13.3 or later. Nothing needs an assembler.

### Against MAME

`tools/ab/` runs the core against MAME's own Channel F driver (the MAME build with
the firmware's FujiNet graft, `pico/channelf/emu/apply.sh`, for CONFIG):

- `cart_ab.py` / `all_carts.sh` — whole cartridges, frame for frame, with a
  scripted input session (panel, both keypads, both discs, fire), using your
  own dumps.
- `uvi_probes.py` — generated stress carts for the UVI (objects at every edge,
  zoom, collisions, both screen modes).
- `config_golden.py` — regenerates `core/tests/config_golden.h`, MAME's frame
  hashes of CONFIG (no ROM needed to run the test that holds the core to them:
  `config_frames`).
- `s2650_ab.py` — the 2650 against MAME's, instruction by instruction on 12
  generated probe carts; `gen_dasm2650_golden.sh` — the disassembler's text for
  all 256 opcodes.

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
wine build-win/frontends/windows/fujinet-go-channelf-windows.exe
```

This is the desk-side check. `fujinet.dll` does not cross-build — the
firmware's PC target needs mingw builds of its dependencies — so the release
build is native MSYS2/UCRT64, which builds it.

## Ports

FujiNet's BoIP listener is on **11526** and its web admin UI on **11527** —
high ports of this app's own, continuing the family's table, so a standalone
`fujinet-pc` or a sibling app never collides:

| App | BoIP | Web UI |
|---|---|---|
| ADAM | 65216 | 65214 |
| Apple II | 1985 | 64001 |
| CoCo | 65504 | 64002 |
| Intellivision | 65503 | 64003 |
| MSX | 65505 | 64003 |
| Astrocade | 11500 | 11501 |
| ColecoVision | 11502 | 11503 |
| Atari 2600 | 11504 | 11505 |
| NES | 11506 | 11507 |
| Master System | 11508 | 11509 |
| Atari 7800 | 11510 | 11511 |
| H89 | 11512 | 11513 |
| Atari 800 | 11514 | 11515 |
| MS-DOS | 11516 | 11517 |
| Studio II | 11518 | 11519 |
| Odyssey 2 | 11520 | 11521 |
| **Channel F** | **11526** | **11527** |

FujiNet listens and the cartridge dials in, so the session starts FujiNet
first and waits for its listener before powering on. The cartridge's mailbox
service runs on its own thread, as it runs on the cartridge's second core, so a
slow network transaction never stalls the picture.

## How this was verified

- **The core against MAME** (all run on this machine, with MAME's own driver and
  the author's own cartridge dumps; none of them are in the repo):
  - the 2650: 12 generated probe programs, 358,415 instructions, traces
    (PC, PSU/PSL, R0-R3, exact cycles) identical; 245 of 256 opcodes executed
    (the rest are HALT and the illegal ones); the disassembler's text, length
    and flags for all 256 opcodes. Interrupts and HALT could not be compared
    (MAME's Channel F never raises an IRQ) and rest on hand-written tests;
  - the whole machine: all 48 cartridges identical to MAME for 900 of 900
    frames under a scripted input session, and 13 generated UVI stress
    carts, frame for frame from frame 0; CONFIG's frames match from frame 0
    (`config_frames`, no ROM needed). A MAME quirk — a doubled object near the
    bottom reads past its background buffer — is reproduced on purpose;
  - **Not verified:** the PAL row against MAME (MAME runs every clone at NTSC)
    and the beeper against a recording.
- **Live FujiNet:** CONFIG reaches the hosts page, browses and boots a game
  from a real in-process FujiNet; a FujiNet app that claims the mailbox keeps
  it and runs.
- **`ctest`**: the core alone with no SDL, the 2650 and UVI, the machine and
  cartridges, the cartridge against a loopback FujiNet (a pushed image staged,
  swapped in at the next `$2DFE` read, the trace tap byte for byte with the
  wire), the paced host (60.00 Hz measured), the session, bindings, the
  keypad layout, media routing, gamepads, and the debugger contract.
- **GNOME, KDE, Windows, macOS** — see `TODO` for each frontend's state.

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes what it produced as a **draft** release:

| Asset | Contents |
|---|---|
| `fujinet-go-channelf-gnome-<version>-Linux.{deb,rpm,tar.gz}` | the GNOME frontend, packaged with CPack |
| `fujinet-go-channelf-kde-<version>-Linux.{deb,rpm,tar.gz}` | the KDE frontend, packaged with CPack |
| `fujinet-go-channelf-<version>-windows.zip` | the exe, `fujinet.dll`, and the `fujinet/` runtime tree |
| `fujinet-go-channelf-<version>-windows-setup.exe` | NSIS installer, per-user, no admin rights |
| `fujinet-go-channelf-<version>-macos-{arm64,x86_64}.zip` | the `.app` bundle for Apple Silicon and Intel, FujiNet inside |
| `online.fujinet.go.channelf.{gnome,kde}.flatpak` | single-file bundles: `flatpak install ./…flatpak` |

The version is declared in the tree (`project(… VERSION …)` and both metainfo
files, which template it), not derived from the tag; `check-version` stops the
release if they disagree. To release 0.1.1: set the version in
`CMakeLists.txt`, add a `<release>` entry with its date to both
`frontends/*/data/*.metainfo.xml.in`, commit, then
`git tag -a v0.1.1 && git push origin v0.1.1`. Running the workflow by hand
(Actions → Release → Run workflow) is a dry run: everything is built and
tested and the artifacts are attached to the run, but no release is made.

The Windows release build is native MSYS2/UCRT64, and the `release.yml` job
checks the exe's and `fujinet.dll`'s import tables against a system-DLL
whitelist. The macOS jobs sign and notarise when the `MACOS_*` secrets are
set, and check the bundle for leaked Homebrew dylibs.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance. "Emerson"
and "Channel F" are trademarks of their owners, used to name the machine
emulated; this project is not affiliated with them.
