#!/usr/bin/env bash
# all_carts.sh -- cart_ab.py over every cartridge in a directory, a few MAME runs
# at a time, with the canned input session (--play).
#
#   all_carts.sh DIR [FRAMES] [JOBS]      default 900 frames, 3 jobs
#
# Prints one line per cartridge and a count; exit status 1 if any differs.
# CHF_HEADLESS names the runner (copy it first if you rebuild meanwhile).
set -u
dir=${1:?usage: all_carts.sh DIR [FRAMES] [JOBS]}
frames=${2:-900}
jobs=${3:-3}
here=$(cd "$(dirname "$0")" && pwd)
out=$(mktemp)
find "$dir" -type f \( -name '*.bin' -o -name '*.rom' \) -print0 | sort -z |
  xargs -0 -P "$jobs" -I{} sh -c '
    w=$(mktemp -d); ARCAB_WORK=$w python3 "$0/cart_ab.py" --frames '"$frames"' --skip 0 --play --report "{}"; rm -rf "$w"' "$here" |
  tee "$out" | sort
total=$(grep -c . "$out")
ok=$(grep -c '^OK' "$out")
echo "$ok/$total cartridges identical to MAME"
rm -f "$out"
[ "$ok" = "$total" ]
