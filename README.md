# FujiNet Go Channel F Desktop

A native desktop emulator of the **Fairchild Channel F** with FujiNet built
in: boot the FujiNet CONFIG, browse a network host with the hand controller,
load games and FujiNet apps over the network, play them. Part of the FujiNet
Go desktop family (Linux GNOME and KDE, Windows, macOS).

| | |
|---|---|
| **Emulator** | MAME's Channel F driver (`channelf.cpp`, its video and sound) and its F8 CPU, transposed to C (the Arcadia, O2 and Studio II siblings' way). The F8 is ROMC-faithful: one function per bus state, the firmware's own ROMC decoder shadows it cycle for cycle in the tests. |
| **Machines** | Channel F (1976, NTSC), Channel F System II (NTSC), Saba Videoplay and Saba Videoplay 2 / Luxor (PAL, 50 Hz), all checked against MAME. Two hand controllers, each a knob with four axes (tilt eight ways, twist, pull, push), and the console's RESET, TIME, MODE, HOLD and START. |
| **BIOS** | None ships. FujiNet's CONFIG and every FujiNet app run on the app's own stand-in BIOS, written from a behavioural specification (`tools/stub/SPEC.md`). Videocarts need the console's BIOS: **Import BIOS…** takes your own `sl31253.rom` / `sl90025.rom` / `sl31254.rom`, a zip such as MAME's `channelf.zip`, or a folder. |
| **Cartridge** | The FujiNet Channel F cartridge (`fujinet-firmware` `pico/channelf`): a peer on the F8 bus with the 16K window at `$0800`, up to six 8K APPBANK pages at `$4800`, 30K of RAM at `$8000`, a 60 Hz tick and the mailbox at `$F800-$FFFF`, with CONFIG baked in. Its bus decode (`channelf_cart.h`), image planner (`chfmap`), mailbox service (`fujimail`) and wire codec (`fujibus`) are the cartridge firmware's own sources, staged verbatim. A Videocart opened from disk gets its own cartridge instead (std, maze, hangman, chess, multi), as a real console would have it. |
| **Frontends** | GNOME (GTK4/libadwaita), KDE (Qt6 Widgets), macOS (AppKit), Windows (Win32/D3D11), each with the display, a Controllers window, Preferences, BIOS import, a live FujiNet console log, built-in Help and a debugger. |
| **Packages** | Linux: Flatpak (GNOME and KDE runtimes), DEB, RPM, tar.gz. Windows: ZIP and an NSIS installer. macOS: zips for Apple Silicon and Intel. |

The icon is the family mark in the console's colours: a Videocart-yellow tile
(`#F2C300`), the case's charcoal (`#302F2F`) for the mark and the walnut trim
(`#6E4A30`) for the centre disc. The UI accent is the Videocart yellow with
dark text on it (dark variant `#8A6D00` on light themes).

## Your games

Open a cartridge with **Machine ▸ Open Cartridge…** (or drop it on the
window): a `.bin` or `.chf` image, or a `.zip` holding one. A FujiNet app (a
16K image with the `FUJI` claim at `$47FC`) runs on the FujiNet cartridge; a
Videocart gets the cartridge MAME's software list gives it, and needs the
imported BIOS. **Import Cartridge to SD…** copies an image into FujiNet's SD
folder for CONFIG to boot; Videocarts with their own RAM or banking (Maze,
Hangman, Chess, the multicarts) cannot run through the FujiNet cartridge and
are refused with the reason. Nothing copyrighted is embedded in any build:
see `COMPLIANCE.md`.

## Controllers (F9)

The Controllers window floats at a fixed size and draws both hand controllers
from above (the knob in a ring of eight direction wedges, the twist arrows
either side) and from the side (PULL and PUSH), and the console's buttons.
Drag from the knob to tilt it, press it to push, right-press to pull, use the
wheel to twist. Held controls light up whoever holds them.

| | Player 1 (right controller) | Player 2 (left controller) |
|---|---|---|
| Stick | arrow keys | keypad 8 2 4 6 |
| Twist counter-clockwise / clockwise | Z / X | keypad 7 / 9 |
| Pull / push | C / Space | keypad . / keypad 0 |
| Gamepad | D-pad or left stick = stick, shoulders = twist, East = pull, South = push; the right stick twists (x) and pulls/pushes (y) | the second pad, the same |

The console's 1 TIME, 2 MODE, 3 HOLD and 4 START are the keys 1-4 (gamepad
West, Back, North, Start); RESET is **F1** and Reset to CONFIG **Escape**.
RESET resets the F8 only, as the console's button does, so a running FujiNet
app restarts from the cartridge's current image with its RAM; **Reset to
CONFIG** is the power cycle. Every control can be given another key or button.
A gamepad's player (Automatic, Player 1 or Player 2) is remembered between runs.

## Debugger (F12)

Prompt · CPU & RAM · Disassembly · **Video** · Sound & I/O · Breakpoints ·
Cart · FujiNet. The CPU tab shows A, W flag by flag, ISAR in octal, PC0, PC1,
DC0, DC1, J/H/K/Q, the 64-byte scratchpad as an octal grid and the BIOS's K
stack. The Video tab shows the whole 128 × 64 video memory with its hidden
columns, each row's palette from columns 125/126, the palette table, the four
ports raw and decoded, every VRAM write (cell, value, PC, frame, line), a
cell's history under the pointer, and a beam view that shows what a CRT draws
mid-frame. Breakpoints are on execution, memory read and write, port in and
out, VRAM cells and scratchpad registers, with conditions.

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

fujinet-firmware is fetched at its pinned commit by the configure step, so a
plain `git clone` is enough. To develop against a working checkout of the
cartridge firmware (absolute path):

```sh
cmake -B build -DFUJINET_SRC=$HOME/Workspace/fujinet-firmware
```

`-DFRONTEND=none` builds just the core, the session and the tests;
`-DCHF_CORE_ONLY=ON` builds only the core and its tests (no SDL3);
`-DWITH_FUJINET=OFF` skips the firmware build (the machine boots CONFIG
reporting the link down). `-DWITH_CHF_ROMS=ON` embeds a BIOS from
`tools/roms/local/` for a private build; never for distribution.

The core is C11. Qt >= 6.7 and GTK >= 4.16 are needed for the Linux
frontends. The macOS bundle needs macOS 13.3 or later. Building needs no
assembler: the stand-in BIOS is committed as `core/chf/stub.c`
(`tools/stub/build-stub.sh` rebuilds it with the Macroassembler AS).

### Against MAME

`tools/ab/` runs the core against MAME's own Channel F (a MAME tree with the
firmware's FujiNet graft, `pico/channelf/emu/apply.sh`); see
`tools/ab/README.md`. `f8_ab.py` and `romc_ab.py` hold the CPU to MAME
instruction by instruction and ROMC record by record; `cart_ab.py`,
`video_probes.py`, `sound_ab.py`, `softlist_ab.py` and `app_ab.py` the whole
machine frame by frame; `config_golden.py` regenerates
`core/tests/config_golden.h`. `tools/stub/observe.py` checks a stand-in BIOS
against the real one's observed behaviour.

### Cross-building Windows on Linux

```sh
curl -LO https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-devel-3.4.12-mingw.tar.gz
tar xzf SDL3-devel-3.4.12-mingw.tar.gz
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake \
      -DFRONTEND=windows -DWITH_FUJINET=OFF \
      -DCMAKE_PREFIX_PATH="$PWD/SDL3-3.4.12/x86_64-w64-mingw32"
cmake --build build-win
cp SDL3-3.4.12/x86_64-w64-mingw32/bin/SDL3.dll build-win/frontends/windows/
```

`fujinet.dll` does not cross-build, so the release build is native
MSYS2/UCRT64.

## Ports

FujiNet's BoIP listener is on **11526** and its web admin UI on **11527**,
continuing the family's table so a standalone `fujinet-pc` or a sibling app
never collides:

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
| Arcadia 2001 | 11522 | 11523 |
| Atari 5200 | 11524 | 11525 |
| **Channel F** | **11526** | **11527** |

## Cutting a release

Pushing a `v*` tag builds every platform and, only if all of it passes,
publishes a **draft** release with `fujinet-go-channelf-{gnome,kde}-<version>-Linux.{deb,rpm,tar.gz}`,
`fujinet-go-channelf-<version>-windows.zip` and `-windows-setup.exe`,
`fujinet-go-channelf-<version>-macos-{arm64,x86_64}.zip` and
`online.fujinet.go.channelf.{gnome,kde}.flatpak`. The version is declared in
the tree (`project(… VERSION …)` and both metainfo files, which template it);
`check-version` stops the release if they disagree. Running the workflow by
hand is a dry run.

## Licence

GPL-3.0-or-later. See `COMPLIANCE.md` for per-component provenance.
"Fairchild", "Channel F" and "Videocart" are trademarks of their owners, used
to name the machine emulated; this project is not affiliated with them.
