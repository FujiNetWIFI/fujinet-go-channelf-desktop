# tools/roms

Developer-only. Drop your own Channel F BIOS ROMs into `tools/roms/local/`
(git-ignored), loose or in MAME's `channelf.zip`:

| file (MAME's name) | what                                                    |
|--------------------|---------------------------------------------------------|
| `sl31253.rom`      | Channel F / Videoplay BIOS, first half (1 KB)           |
| `sl90025.rom`      | Channel F System II / Videoplay 2 BIOS, first half (1 KB) |
| `sl31254.rom`      | BIOS second half, every console (1 KB)                  |

They are recognised by size and CRC-32, whatever they are called. A 2 KB file
that is a first half followed by `sl31254` is split. Then:

- build with `-DWITH_CHF_ROMS=ON`, which embeds what is there so a development
  build has its BIOS from the first start (NOT for redistribution);
- put the Videocarts you test with in `tools/roms/local/` too: the
  `no_embedded_roms_*` tests then have something to probe for, so they check
  rather than skip.

Shipped builds are built with `-DWITH_CHF_ROMS=OFF` (CONFIG and the FujiNet
apps boot on the app's own stand-in BIOS), and nothing in `local/` is ever
committed. See COMPLIANCE.md.
