# Compliance

Per-component provenance for `fujinet-go-channelf-desktop`, written before the
first public build, in the family tradition (see
`fujinet-go-o2-desktop/COMPLIANCE.md`, `fujinet-go-arcadia-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **The console core** (`core/chf/machine.c`, `video.c`, `sound.c`) | transposed to C from MAME: `src/mame/fairchild/channelf.cpp`, `channelf_v.cpp`, `channelf_a.cpp` (the Channel F driver, its video and its sound) | GPL-2.0-or-later; Juergen Buchmueller, Frank Palazzolo, Sean Riddle; kept in each file's header | compiled into `chf_core` |
| **The Videocarts** (`core/chf/carts.c`) | transposed from MAME's cartridge bus `src/devices/bus/chanf/{slot,rom}.cpp`; `carts_table.h` holds only the CRCs, sizes and names from MAME's `hash/channelf.xml` | BSD-3-Clause; Fabio Priuli | compiled into `chf_core` |
| **The F8 CPU** (`core/chf/f8.c`, `dasmf8.c`) | transposed from MAME `src/devices/cpu/f8/f8.cpp` and `f8dasm.cpp` | BSD-3-Clause; Juergen Buchmueller | compiled into `chf_core` |
| **The stand-in BIOS** (`core/chf/stub.asm`, assembled into `stub.c`) | written for this project from a behavioural specification (`tools/stub/SPEC.md`) made by observing the real BIOS as a black box; its author never saw the real BIOS's code | GPL-3.0-or-later | compiled into `chf_core`; runs when no BIOS is imported |
| **The FujiNet Channel F cartridge device** (`core/chf/fujinet_cart.c`) | transposed from the firmware's MAME device, `fujinet-firmware` `pico/channelf/emu/fujinet.cpp` | BSD-3-Clause, © Thomas Cherryhomes (the transposition GPL-3.0-or-later) | compiled into `chf_core` |
| **The cartridge firmware's own sources** (`fujimail.c`, `chfmap.c`, `channelf_cart.h`, `fuji_mailbox.h`, `fujiconfigrom.h` from `pico/channelf/firmware`; `fujibus.c` and `fujitcp.h` from `pico/common`) | `fujinet-firmware`, staged verbatim into `core/chf/fuji-generated/` by `cmake/StageFujiProto.cmake` | as marked in each file (the FujiNet project's, © Thomas Cherryhomes) | compiled into `chf_core`, never patched |
| **The CONFIG client** (`fujiconfigrom.h`, 16,384 bytes) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) `channelf/`, assembled with the Macroassembler AS and baked into the firmware | GPL-3.0 (the FujiNet project's) | the image the cartridge serves at power-on, as on the hardware |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| The session, debugger, Help and frontends (`core/src`, `core/debugger`, `frontends/{gnome,kde,windows,macos}`) | ported from the Arcadia, O2 and MS-DOS siblings' (this project's family, © Thomas Cherryhomes) | GPL-3.0-or-later | one executable per platform |
| GTK4, libadwaita (GNOME) / Qt6 (KDE) | the GNOME and Qt projects | LGPL-2.1-or-later / LGPL-3.0 | system libraries, dynamically linked |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it (GPL-2.0-or-later
code is combined under GPL-3.0-or-later; BSD-3-Clause and the permissive
licences are compatible): the combined work is distributed under GPL-3.0.

## What is not shipped, and why

**The console's BIOS** (`sl31253.rom`, `sl90025.rom`, `sl31254.rom`) and **the
Videocarts** are copyrighted and not freely licensed. This repository does not
redistribute them and every artifact it publishes embeds none. FujiNet's
programs run on the app's own stand-in BIOS; a Videocart needs the real one,
which the user imports from their own dumps (a file, a zip such as MAME's
`channelf.zip`, or a folder). `WITH_CHF_ROMS` (default OFF, and passed OFF
explicitly by every CI, release and Flatpak build) exists only for a
developer's private builds. `core/tests/no_embedded_roms.py` checks each
shipped binary for every 1K BIOS half and every cartridge image in a
developer's own collection; it SKIPs on a checkout that has none, which is
every CI and release build.

Games are opened by the user from disk (`.bin`, `.chf`, or a zip of one) or
served from their own FujiNet host.

## What is changed from MAME

The core is a transposition, not a copy: MAME's C++ device classes become C
structs and functions under MAME's own names (minus `m_`), so the two can be
read side by side. What is deliberately different:

- **The clockwork.** MAME runs the F8 and the screen in its scheduler; here
  the machine runs the CPU one instruction at a time and ends each frame at
  the vertical blank with the same integer-attosecond arithmetic.
- **The FujiNet cartridge** is the firmware's own mailbox service run on a
  thread, with an abortable portable transport (`core/chf/fujitcp_host.c`) in
  place of the staged `fujitcp.c`, which is not compiled. It reconnects when
  its link drops.
- **The stand-in BIOS** runs when no BIOS is imported; MAME needs the real one.

## Trademarks

"Fairchild", "Channel F", "Videocart", "Saba", "Videoplay" and "Luxor" are
trademarks of their owners, used only to name the machine this program
emulates. This project is not affiliated with them.
