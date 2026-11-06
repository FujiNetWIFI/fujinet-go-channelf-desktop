#!/usr/bin/env python3
"""Assert that a WITH_CHF_ROMS=OFF binary carries none of the Channel F's
BIOS and none of the commercial Videocarts.

This makes the COMPLIANCE.md claim checkable: it takes a distinctive slice
out of each image in <rom-dir> and greps the built binary for it. Anything
found got compiled in, which would make the artifact non-redistributable.

The BIOS halves are found the way the importer finds them, by size and CRC-32
whatever the file is called, also inside zips (MAME's channelf.zip). Any other
image in the directory (loose or zipped, up to 256K) is taken as a Videocart
and probed too. The app's own stand-in BIOS (core/chf/stub.asm) and the
FujiNet CONFIG are ours and are not looked for.

With no images there the check SKIPs (ctest 77): true of every CI and
release checkout.

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import sys
import zipfile
import zlib
from pathlib import Path

# (size, CRC-32) of the BIOS halves, from MAME's channelf.cpp
FINGERPRINTS = {
    (0x400, 0x04694ED9): "sl31253.rom",
    (0x400, 0x015C1E38): "sl90025.rom",
    (0x400, 0x9C047BA3): "sl31254.rom",
}
MAX_IMAGE = 0x40000

PROBE = 64


def probe_bytes(data):
    """A slice distinctive enough that finding it means something.

    Reject any chunk dominated by a single byte value and require real
    variety, so a run of 0x00 or 0xFF padding -- which occurs in almost any
    binary of a reasonable size -- can never be mistaken for a match. Scan the
    whole file rather than a few fixed offsets.
    """
    max_run = PROBE // 2      # no single byte value may cover half the slice
    min_distinct = 12

    for start in range(0, max(1, len(data) - PROBE), PROBE):
        chunk = data[start:start + PROBE]
        if len(chunk) != PROBE:
            continue
        if max(chunk.count(b) for b in set(chunk)) > max_run:
            continue
        if len(set(chunk)) < min_distinct:
            continue
        return chunk
    return None


def images(romdir):
    """(name, bytes) of every image in the directory or in a zip in it: the
    BIOS halves under their MAME names, anything else as a Videocart."""
    for f in sorted(romdir.iterdir()):
        if not f.is_file():
            continue
        if zipfile.is_zipfile(f):
            with zipfile.ZipFile(f) as z:
                blobs = [(f"{f.name}:{info.filename}", z.read(info)) for info in z.infolist()
                         if not info.is_dir() and info.file_size <= MAX_IMAGE]
        elif f.stat().st_size <= MAX_IMAGE:
            blobs = [(f.name, f.read_bytes())]
        else:
            continue
        for label, data in blobs:
            name = FINGERPRINTS.get((len(data), zlib.crc32(data) & 0xFFFFFFFF))
            yield (name or label), data


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    binary = Path(sys.argv[1]).read_bytes()
    romdir = Path(sys.argv[2])
    if not romdir.is_dir():
        print(f"no_embedded_roms: SKIP: {romdir} does not exist -- nothing to check against")
        return 77

    checked = 0
    leaked = []
    unprobeable = []

    for name, data in images(romdir):
        if len(data) < 128:
            continue
        chunk = probe_bytes(data)
        if chunk is None:
            # Nothing distinctive enough to search for -- report it rather
            # than quietly treating it as checked.
            unprobeable.append(name)
            continue
        checked += 1
        if chunk in binary:
            leaked.append(name)

    if checked == 0:
        # No real ROMs to check against -- true of every CI and release build,
        # since the copyrighted ROMs (see COMPLIANCE.md) are gitignored
        # and never present on a checkout that isn't a developer's own machine.
        # SKIP (ctest exit code 77), not fail: there is nothing wrong with the
        # binary, just nothing here to probe it with.
        print("no_embedded_roms: SKIP: no ROM images found in "
              f"{romdir} -- nothing to check against")
        return 77

    for name in leaked:
        print(f"FAIL: {name} is embedded in a WITH_CHF_ROMS=OFF build")

    if leaked:
        return 1
    if unprobeable:
        print("no_embedded_roms: NOT checked (no distinctive slice): "
              + ", ".join(sorted(unprobeable)))
    print(f"no_embedded_roms: {checked} ROM image(s) checked; none embedded, "
          "as intended")
    return 0


if __name__ == "__main__":
    sys.exit(main())
