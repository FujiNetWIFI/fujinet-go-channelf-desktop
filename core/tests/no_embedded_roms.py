#!/usr/bin/env python3
"""Assert that a binary carries none of the commercial Channel F cartridges.

There is no console BIOS to ship (the Channel F has none) and the games are the
user's own files, so nothing copyrighted may be in any artifact. This makes
the COMPLIANCE.md claim checkable: it takes a distinctive slice out of every
cartridge image in <rom-dir> (a developer's own dumps -- loose .bin files or
zips, each at most 8 KB) and greps the binary for it. Anything found got
compiled in, which would make the artifact non-redistributable.

The cartridge firmware's own test programs and the FujiNet CONFIG are ours and
are not in <rom-dir>. With no images there, the check SKIPs (ctest 77): true
of every CI and release checkout.

usage: no_embedded_roms.py <binary> <rom-dir>
"""

import sys
import zipfile
from pathlib import Path

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
    """(name, bytes) of every cartridge image in the directory or in a zip in it."""
    for f in sorted(romdir.iterdir()):
        if not f.is_file():
            continue
        if zipfile.is_zipfile(f):
            with zipfile.ZipFile(f) as z:
                blobs = [(i.filename, z.read(i)) for i in z.infolist()
                         if not i.is_dir() and i.file_size <= 0x4000]
        elif f.stat().st_size <= 0x4000:
            blobs = [(f.name, f.read_bytes())]
        else:
            continue
        yield from blobs


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
        # No cartridges to check against -- true of every CI and release build
        # (see COMPLIANCE.md). SKIP (ctest exit code 77), not fail: there is nothing wrong with the
        # binary, just nothing here to probe it with.
        print("no_embedded_roms: SKIP: no ROM images found in "
              f"{romdir} -- nothing to check against")
        return 77

    for name in leaked:
        print(f"FAIL: {name} is embedded in this build")

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
