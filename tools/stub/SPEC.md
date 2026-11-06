# Channel F BIOS stand-in: behavioural specification

Clean-room specification of the four BIOS entry points the FujiNet Channel F
programs use. It describes what the routines **do** (state before and after a
call, as seen from a cartridge), not how the real ones are coded. Everything
below was measured in MAME's `channelf` driver by `tools/stub/observe.py`; no
BIOS byte was read, dumped or disassembled: only machine state before and
after a call, and the OUT instructions executed in between, were recorded.

A stand-in is correct when `observe.py --check` passes against it (see
"How to test" at the end) and `core/tests/bios_contract.h` passes in the app's
own emulator.

## 0. What the stand-in is

Two 1 KiB ROMs, `sl31253.rom` (first half, $0000-$03FF) and `sl31254.rom`
(second half, $0400-$07FF), appear at $0000-$07FF. The programs use exactly
four addresses:

| address | name | entered by |
|---|---|---|
| $0000 | RESET | power-on, or `JMP $0000` from a cartridge's swap stub |
| $00D0 | clear screen | `PI $00D0` with r3 = 0 |
| $0107 | push K | `PI $0107` |
| $011E | pop K | `PI $011E` |

Nothing else in the real BIOS (built-in games, title screen, other
subroutines) is used. The routines are entered with `PI`, which loads A with
the high byte of the target ($00 / $01 / $01) and PC1 with the return address,
and they return with `POP` (PC0 := PC1). The three subroutines may use `JMP`
into code elsewhere in the ROM: only the addresses above are fixed.

Scratchpad names used below: r0..r63; K = r12:r13 (KU:KL), Q = r14:r15,
H = r10:r11, J = r9. ISAR is 6 bits. W bits: 0 = S, 1 = C, 2 = Z, 3 = O,
4 = I (interrupt enable; arithmetic never changes it).

### Tiers

* **Tier 1**: FujiNet code may depend on it. A stand-in must match; the C
  contract and `observe.py --check` fail on a mismatch.
* **Tier 2**: a value the real BIOS happens to leave behind (loop leftovers,
  internal return addresses, flags). A correct program cannot rely on it, since
  it is not documented and differs between the two real BIOS versions in one
  case. Matching is free or cheap: the tools warn only. Values are given so a
  stand-in can reproduce them if that is easy.
* **Tier 3**: corner cases where the real routine misbehaves or depends on
  state nobody supplies. No promise.

## 1. RESET and the cartridge handshake ($0000)

**Condition.** The BIOS reads the byte at $0800 (the only cartridge address it
reads; $0801 is never examined: all 256 values of $0801 behave alike). If it is
$55 the machine proceeds to the cartridge; for every one of the other 255 values
(tested exhaustively) the BIOS **does not enter the cartridge** (no instruction
is fetched from $0802). The check is repeated on every entry to $0000.

**State when control reaches the cartridge at $0802** (identical for power-on
and for `JMP $0000` with arbitrary prior state; 200 random pre-states checked):

| item | value | tier |
|---|---|---|
| scratchpad | **all 64 bytes $00 except r59 = $28** (the K-stack pointer; K, Q, H, J are zero) | 1 |
| VRAM | untouched (not cleared) | 1 |
| port latches (0, 1, 4, 5) | untouched; **no OUT and no IN is executed** | 1 |
| W | `(W_before & $10) | $07`: I unchanged, Z C S set, O clear (power-on: $07) | I bit 1, flags 2 |
| A | $08 | 2 |
| ISAR | $3B (59) | 2 |
| DC0 | $0801 | 2 |
| DC1, PC1 | unchanged | 2 |

(There is no screen clear, no delay, no sound change.) Time from `JMP $0000`
to the first fetch at $0802: about 1,470 ticks (1 tick = 1/895,000 s); from
power-on 1,457 ticks. The real hardware's power-on contents of scratchpad and
VRAM are undefined; MAME zero-fills both, so "untouched" means "whatever was
there".

Essential for FujiNet: the zeroed scratchpad with r59 = $28 (the default K
stack lives at r40 upward), the untouched VRAM and latches, and the I bit.

## 2. K stack: push K ($0107) and pop K ($011E)

A stack of 2-byte entries kept in the scratchpad, r59 as the pointer. It grows
**upward**. `p` = r59 & $3F (the top two bits of r59 are ignored on use; r59
itself is always written back in 0..63). Slot indexes wrap modulo 64. Initial
pointer after RESET: $28. One FujiNet app sets its own pointer to 16: nothing
special happens; the pointer is just a scratchpad index.

### push K

1. r7 := ISAR at entry (ISAR is parked in r7).
2. scratchpad[p] := KU (r12).
3. scratchpad[(p+1) & 63] := KL (r13, read **after** step 2: this only differs
   when p = 13, where step 2 overwrites r13).
4. r59 := (p + 2) & 63.
5. A := r7 (as it is after steps 2-3); ISAR := A & $3F. So A = old ISAR and
   ISAR is restored, **except** when p is 6 or 7 (a K byte lands on r7), where
   both are junk (tier 3).
6. W := (W & $10) | $01 (only S set).
7. Return (`POP`).

Preserved: PC1, DC0, DC1, ports, VRAM, all scratchpad bytes except r7, the two
slots and r59 (K, Q, H, J included; K itself changes only if a slot overlaps
r12/r13).

### pop K

1. r7 := ISAR at entry.
2. q := (p - 2) & 63.
3. KL (r13) := scratchpad[(q+1) & 63], **then** KU (r12) := scratchpad[q]
   (KL first; this only matters when q = 13).
4. r59 := q.
5. A := r7; ISAR := A & $3F (never junk: pop does not write r7).
6. W := (W & $10) | f, with f = $3 (C S) normally, $7 (Z C S) when q = 0, $0
   when q = 63 (tier 2).
7. Return (`POP`).

Preserved: as for push, except K := the popped pair, r7 and r59.

Tiers: slot/K/r59 effects and ISAR preserved (except the p = 6/7 case): 1.
r7 (junk after the call), A (= old ISAR), the W flags: 2. Cycle count: push 61
ticks, pop 63 (including the `PI`).

Exhaustively checked for every r59 value 0..255 with random everything else,
through three call forms (PI, PK, `LR P0,Q`: so A and K at entry do not matter
beyond K being the data pushed). Zero mismatches against the rules above.

## 3. Clear screen ($00D0)

**Entry.** `PI $00D0` with r3 = 0 (FujiNet's calls). r59 must be sane: the
routine uses the K stack (it pushes the return address and one internal
return address, i.e. four slots r[p]..r[p+3], and pops them again) and
returns for every pointer value p in 14..48 (tested with random state); the
FujiNet defaults $28 and 16 are inside. Outside that range the real routine
runs off the rails for most values (0-8, 11-13, 49-54, 56-59, 61, 63; in the
trials the machine ended up re-entering the cartridge at $0802 after a fresh
RESET); 9, 10, 55, 60 and 62 returned only by luck. A stand-in does not need
to survive those (tier 3); it only has to handle 14..48 and need not use the
stack at all.

### VRAM result (tier 1 for r3 = 0)

VRAM is 128 columns x 64 rows, one cell per (row, col), values 0-3, offset
row*128 + col.

For r3 = 0, whatever the rest of the state: in **every row 0..63**
(rows 58-63 are the hidden rows; they are cleared too)

* columns **1..110** are set to 0,
* columns **122..126** are set to 0 (122-124 are hidden; **125 and 126 are the
  per-row palette columns**, so every row ends in palette 0),
* columns 0, **111..121** and 127 are **not touched**: they keep whatever they
  held (a stand-in that clears the whole array would be wrong here; the
  contract starts from random VRAM to catch that).

The result depends on nothing but r3 and the previous content of the cells
that are not written (200 random states plus one-field-at-a-time perturbation
of every scratchpad byte, DC0, DC1, ISAR, W and all four latches: the only
other thing that changes is the sound bits in the port-5 writes, below).

### Other r3 values (colour in bits 7-6; informational, tier 3 except the first row)

r3 bits 7-6 = c is a colour 0..3; bit 4 is ignored. Columns 1..110 are always
cleared to 0. What the five hidden columns 122..126 receive:

| r3 | columns 122..126 |
|---|---|
| $00 | 0 |
| $40, $80, $C0 (c = 1, 2, 3) | **c in every row** when r31 & 3 = 0 (the state after RESET); when r31 & 3 != 0 the same, except rows 0 and 50..63 get 0 |
| anything else with bits 5-0 != 0 | 0 or c per cell in period-5 row patterns (rows 1..63 repeat every 5 rows; row 0 equals row 4; with r31 & 3 != 0 rows 0 and 50..63 are blank). Full tables for both versions and both r31 cases: `obs-data/clr_hidden.json` |

With the app's palette decode `(col126 & 2) | (col125 >> 1)` per row, the
$40/$80/$C0 forms give palette 0 (c = 1) or palette 3 (c = 2, 3).

For r3 with bit 5 set and bits 3-0 in 0..3, or bits 3-0 = 5, the real routine
reads further scratchpad bytes (r19 and r32 seen) and may not return with
random contents; those are tier 3 in the contract. r3 with bits 3-0 = 5 also
leaves some hidden cells unwritten. "r31" is a real input of the real routine
(the stack pushes can land on r31 when p is 28..31, and then the pushed bytes
count): this only matters for r3 != 0.

### Registers and other state

Tier 1 (must be preserved):

* every scratchpad byte **except** r0, r1, r2, r3, r5, r6, r7, r8, r12, r13,
  r52, r53, r54 and the four stack slots r[p]..r[p+3]. In particular r4,
  J (r9), H (r10, r11), Q (r14, r15), r16..r39 outside the slots, r44..r51,
  r55..r58, r60..r63 are preserved.
* r59 keeps the pointer: r59 & 63 (tier 1 when r59 < 64, which it always is in
  FujiNet; the real routine returns the masked value).
* DC1 and the I bit of W.

Tier 2 (real-BIOS leftovers; reproduce if cheap): r0 = $0D, r1 = $73 ($72 on
sl90025), r2 = $41, r5 = r6 = r8 = 0, r7 = $1F, r52 = $1F, r53 = r3, r54 = r4
(pre-call), r3 unchanged (cleared when r31 & 3 != 0 and r3 != 0), K := the
return address (PC1 at entry), stack slots [p],[p+1] = return address high and
low, [p+2] = $00, [p+3] = $E7; A = $1F; ISAR = $1F; W flags = C S ($3);
PC1 = $0107; DC0 = $07AD.

### Ports (what the OUT/IN traffic looks like)

The routine writes only ports 0, 1, 4, 5 and reads only port 5 (to pick up the sound bits).

* **Port 5 sound bits 7-6 are preserved**: every port-5 write carries the
  sound bits it found in the port-5 latch (read back with `IN 5`, which returns
  the output latch: the emulator's port-5 read must do that), so the tone state
  is unchanged. Tier 1.
* Final latches: port 0 = **$00** (tier 1: leaves the controller-select bit
  clear), port 1 = $FF, port 4 = $91 ($92 on sl90025), port 5 = $3F | (sound
  bits) (tier 2).
* Sequence used by the real routine (a stand-in may copy it; the core's
  emulator accepts it): once `OUT 0,$40`; per row `OUT 5` with `(~row & $3F) |
  sound`; per cell `OUT 4` = `~col`, `OUT 1` = value byte ($FF for 0, otherwise
  bits 7-6 = ~value), `OUT 0,$60`, `OUT 0,$50`; at the end `OUT 0,$00`. Real
  counts: 15,548 OUT 0, 7,475 each OUT 1 and OUT 4, 1,495 OUT 5 (65 row passes
  of 115 cells; the 65th pass is row 0 again).

Duration: about 1.14 million ticks (78 frames, 1.28 s); the figure moves by a
few hundred ticks with the data. Nothing FujiNet does depends on it.

## 4. The two real first-half BIOS versions

sl31253 ("Channel F") and sl90025 ("Luxor"). RESET, push K and pop K behave
identically on all 2,640 cases (and on all 263 cartridge-signature runs). Clear
screen differs in three places, everything else (registers, A, ISAR, W, PC1,
DC0, hidden columns, r3 patterns, stack use) is identical:

| | sl31253 | sl90025 |
|---|---|---|
| columns always cleared to 0 | 1..110 | **0..109** |
| r1 after the call | $73 | $72 |
| last port-4 write | $91 | $92 |

(Columns 111..121 and 127 are untouched on both; on sl90025 column 110 is
untouched too.) The contract and `--like` default to sl31253; the header's
`e_vram_fnv_alt` holds sl90025's VRAM digest.

## 5. What FujiNet's programs need, in short

* RESET: $55 at $0800 gate; zeroed scratchpad with r59 = $28; VRAM, ports, I
  untouched; control at $0802.
* push/pop K: exact slot layout, upward growth, 2-byte entries, KU first, r59
  arithmetic mod 64 on the low six bits, ISAR preserved, K registers restored by
  pop.
* clear screen with r3 = 0: columns 1..110 and 122..126 of all 64 rows := 0, the
  other columns untouched, sound bits and every register outside the clobber
  set preserved, port 0 latch left at $00, return by `POP`.

Incidental: every Tier-2 constant, the cycle counts, the OUT ordering, the
non-zero-r3 patterns beyond the colour-only forms, behaviour outside
r59 & 63 in 14..48 for clear screen, r7/A/W flags after push and pop.

Implementation hints (not requirements): `JMP $0802` at the end of RESET
loads A with $08 for free; keep the entry W in J (r9) until the last moment and
zero r9 last; the routines at $0107/$011E/$00D0 can be a `JMP` to code
elsewhere (the three entries are only 55 and 23 bytes apart). A throw-away
prototype written from this document alone (about 150 lines of F8 assembly, no
K-stack use in clear screen) passes `--check` with no tier-1 mismatch.

## 6. How to test

```
tools/stub/observe.py                       # observe both real BIOS versions, rewrite obs-data/
tools/stub/observe.py --header core/tests/bios_contract.h
# a stand-in: build 1 KiB halves, put them in <dir>/channelf/{sl31253,sl31254}.rom
tools/stub/observe.py --check --bios sl31253 --rompath <dir>   # exit 1 on a tier-1 mismatch
```

`--check` runs the whole suite (about 2,640 seeded cases: random states, r59
sweeps, r3 sweeps, one-field perturbations, the contract set) plus the
cartridge-signature sweep through MAME with your ROMs, compares every case with
the executable models in `biosmodel.py` (which reproduce the real BIOS on all
observed cases, tiers 1 and 2) and prints tier-1 mismatches per group. MAME
warns about the checksums and runs it. `--like sl90025` selects the other
clear-screen column range.

`core/tests/bios_contract.h` holds 220 pre/post cases (12 RESET, 34 clear
screen, 87 push, 87 pop, with masks for what is tier 1/tier 2) plus the 256-entry
`chf_bios_clr_r3_fnv` table; the protocol is in the file header. Each case is
the real BIOS's observed state, so it needs no tool to run.

Method: `obs.lua` waits for the console to hand control to the probe cartridge
($0802), writes a complete pre-state (A, W, ISAR, DC0, DC1, PC1, the scratchpad,
the four port latches, all of VRAM), points the CPU at a launch pad in the
cartridge (`PI`, `PK`, `LR P0,Q` or `JMP $0000`) and waits for the return spin;
then it records the post-state and a summary of the OUT instructions executed in
between (port, value). Sources: `probe.asm`, `obs.lua`, `obslib.py`,
`biosmodel.py`, `genheader.py`, `observe.py`; raw observations in `obs-data/`.

## 7. Not determined

* The non-$55 outcome beyond "does not enter the cartridge" (by instruction).
* Real-hardware power-on contents of scratchpad, VRAM and latches (MAME zero-fills).
* The behaviour of clear screen for r3 with bit 5 set and low nibble 0-3, or low
  nibble 5, with arbitrary scratchpad (extra inputs: r19, r32 seen; the real
  routine does not return for some values); and *why* r31 & 3 matters.
* Timing beyond tick counts; PAL machines (the same ROMs, different clock); the
  Channel F II ("channlf2") was not run separately.
* Interrupt behaviour (the driver has no interrupt source).
