#!/usr/bin/env bash
# all_carts.sh -- cart_ab.py over every cartridge in a directory, a few MAME runs
# at a time, with the canned input session (--play).
#
#   all_carts.sh DIR [FRAMES] [JOBS] [MACHINE]    default 900 frames, 3 jobs, channelf
#
# Prints one line per cartridge and a count; exit status 1 if any differs. Needs the
# real BIOS (CHF_BIOS_DIR) and MAME; CHF_HEADLESS names the runner (copy it first if you
# rebuild meanwhile). A plain image runs as MAME's `chess` Videocart, as it does here;
# the commercial Videocarts are the user's own, none ship with this repository.
set -u
dir=${1:?usage: all_carts.sh DIR [FRAMES] [JOBS] [MACHINE]}
frames=${2:-900}
jobs=${3:-3}
machine=${4:-channelf}
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp)
find "$dir" -type f \( -name '*.bin' -o -name '*.chf' \) -print0 | sort -z |
  xargs -0 -P "$jobs" -I{} sh -c '
    w=$(mktemp -d); CHFAB_WORK=$w python3 "$0/cart_ab.py" --machine '"$machine"' --frames '"$frames"' --play --report "{}"; rm -rf "$w"' "$here" |
  tee "$out" | sort
total=$(grep -c . "$out")
ok=$(grep -c '^OK' "$out")
echo "$ok/$total cartridges identical to MAME"
rm -f "$out"
[ "$ok" = "$total" ]
