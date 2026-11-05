# Compliance

Per-component provenance for `fujinet-go-channelf-desktop`, written before the
first public build, in the family tradition (see
`fujinet-go-o2-desktop/COMPLIANCE.md`, `fujinet-go-studio2-desktop/COMPLIANCE.md`).

## What ships

| Component | Origin | Licence | How it enters the build |
|---|---|---|---|
| This application | this repository | GPL-3.0-or-later | — |
| **The console core** (`core/chf/machine.c`, `carts.c`) | transposed to C from MAME: `src/mame/channelf/channelf.cpp` (the Channel F driver and its clones) and the cartridge bus `src/devices/bus/channelf/{slot,rom}.cpp` | driver GPL-2.0-or-later (copyright-holder Peter Trauner); cart bus BSD-3-Clause (Fabio Priuli); kept in each file's header | compiled into `chf_core` |
| **The 2650** (`core/chf/s2650.c`, `dasm2650.c`) | transposed from MAME `src/devices/cpu/s2650/s2650.cpp/.h` and `2650dasm.cpp/.h` (the Signetics-mnemonic disassembler only) | BSD-3-Clause; Juergen Buchmueller | compiled into `chf_core` |
| **The 2637 UVI video** (`core/chf/uvi.c`), including the glyph table | transposed from MAME `src/mame/channelf/arcadia_v.cpp` and the palette in `channelf.cpp` | GPL-2.0-or-later; Peter Trauner | compiled into `chf_core` |
| **The beeper** (`core/chf/sound.c`) | transposed from MAME `src/mame/channelf/arcadia_a.cpp` | GPL-2.0-or-later; David Viens, Peter Trauner | compiled into `chf_core` |
| **The FujiNet Channel F cartridge device** (`core/chf/fujinet_cart.c`) | transposed from the firmware's MAME device, `fujinet-firmware` `pico/channelf/emu/fujinet.cpp` | BSD-3-Clause, © Thomas Cherryhomes (the transposition GPL-3.0-or-later) | compiled into `chf_core` |
| **The cartridge firmware's own sources** (`fujimail.c`, `chfmap.c`, `arcapp.h`, `fuji_mailbox.h`, `fujiconfigrom.h` from `pico/channelf/firmware`; `fujibus.c` and `fujitcp.h` from `pico/common`) | `fujinet-firmware`, staged verbatim into `core/chf/fuji-generated/` by `cmake/StageFujiProto.cmake` | as marked in each file (the FujiNet project's, © Thomas Cherryhomes) | compiled into `chf_core`, never patched |
| **The CONFIG client** (`fujiconfigrom.h`, 8,192 bytes) | [`FujiNetWIFI/fujinet-config`](https://github.com/FujiNetWIFI/fujinet-config) `channelf/`, assembled with the Macroassembler AS and baked into the firmware (PR #2259) | GPL-3.0 (the FujiNet project's) | the image the cartridge serves at power-on, as on the hardware |
| **FujiNet firmware** (`libfujinet`) | [`FujiNetWIFI/fujinet-firmware`](https://github.com/FujiNetWIFI/fujinet-firmware), PC target `RS232` | GPL-3.0-or-later | built as a shared library by `tools/fujinet/build-fujinet-desktop.sh`, `dlopen`'d at run time |
| The session, debugger and frontends (`core/src`, `core/debugger`, `frontends/{gnome,kde,windows,macos}`) | ported from the O2 and Studio II siblings' (this project's family, © Thomas Cherryhomes) | GPL-3.0-or-later | one executable per platform |
| GTK4, libadwaita (GNOME) / Qt6 (KDE) | the GNOME and Qt projects | LGPL-2.1-or-later / LGPL-3.0 | system libraries, dynamically linked |
| SDL3 | libsdl-org | Zlib | system package on Linux; linked statically on macOS and Windows |
| mbedTLS 3.6.x | Mbed-TLS | Apache-2.0 | for `libfujinet`: system package where it is a usable 3.x, otherwise the pinned source |

Everything above is GPL-3.0-or-later or compatible with it (GPL-2.0-or-later
code is combined under GPL-3.0-or-later; BSD-3-Clause and the permissive
licences are compatible): the combined work is distributed under GPL-3.0.

## What is not shipped, and why

**The Channel F cartridges** (the commercial games) are copyrighted and not
freely licensed. This repository does not redistribute them and every artifact
it publishes embeds none: the machine needs no BIOS (the console has none) and
no character-set ROM (the glyph table is part of the UVI source above). The
only cartridge data in the program is the FujiNet CONFIG, which is ours.
`core/tests/no_embedded_roms.py` checks each shipped binary for a slice of
every cartridge image in a developer's own collection; it SKIPs on a checkout
that has none, which is every CI and release build.

Games are opened by the user from disk (`.bin`, `.rom`, `.chf`, or a zip of
one) or served from their own FujiNet host.

## What is changed from MAME

The core is a transposition, not a copy: MAME's C++ device classes become C
structs and functions under MAME's own names (minus `m_`), so the two can be
read side by side. What is deliberately different:

- **The clockwork.** MAME runs the 2650 and the line interrupt in its
  scheduler; here the machine runs the CPU one instruction at a time and fires
  the line timer with the same integer-attosecond arithmetic after the
  instruction that crosses it, which MAME's timeslicing amounts to. The
  interrupt line is sampled before every instruction (MAME samples at the
  start of a timeslice and after CPSU/RETE); the Channel F never raises it.
- **Lazy video.** The UVI draws lines on demand rather than from a per-line
  callback, with the same result frame for frame (all 48 cartridges identical
  for 900 frames, MAME quirks included).
- **The FujiNet cartridge** is the firmware's own mailbox service run on a
  thread, with an abortable portable transport (`core/chf/fujitcp_host.c`) in
  place of the staged `fujitcp.c`, which is not compiled.
- **A PAL row** (312 lines, 50 Hz, 4.433619/5 MHz) exists for the PAL clones;
  MAME runs every clone at NTSC, so it is not verified against MAME.
- The **Palladium** (16 keys) is not emulated, as MAME marks it not working.

## Trademarks

"Emerson", "Channel F" and the clones' names are trademarks of their owners,
used only to name the machine this program emulates. This project is not
affiliated with them.
