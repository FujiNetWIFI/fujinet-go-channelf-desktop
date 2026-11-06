; stub.asm -- the app's own stand-in for the Channel F console's BIOS.
;
; Written from tools/stub/SPEC.md alone. Four entry points, nothing else:
;
;   $0000  RESET        scratchpad := 0 (r59 := $28), enter the cartridge at
;                       $0802 if and only if [$0800] = $55
;   $00D0  clear screen with r3 = 0: VRAM columns 1-110 and 122-126 of every
;                       row := 0 (or the colour in r3 bits 7-6), the tone bits kept
;   $0107  push K       the K stack: 2-byte entries in the scratchpad, r59 the
;   $011E  pop K        pointer, growing upward
;
; Every byte not assigned below stays $FF, an illegal opcode: executing one
; parks the app's machine (the session says "Import BIOS"). Assemble with
; tools/stub/build-stub.sh, which writes core/chf/stub.c.
;
; Copyright (C) 2026 Thomas Cherryhomes
; SPDX-License-Identifier: GPL-3.0-or-later

        CPU     F3850

; ---- RESET ---------------------------------------------------------------
;
; The cartridge handshake reads $0800 and nothing else. Arithmetic never
; changes W's I bit, so it survives without being saved; the flags are then
; set to Z C S by 255 + 1. Left behind: A = $08 (JMP's high byte), ISAR = $3B,
; DC0 = $0801. No port is touched, and neither is the VRAM.

        ORG     0000h
reset:  DCI     0800h
        LM                              ; A := [$0800], DC0 := $0801
        CI      55h
        BZ      enter
park:   DB      0FFh                    ; not a cartridge: park the machine here,
        BR      park                    ; and keep a CPU that ignores $FF off $0802

enter:  LIS     0
        LISU    7
        LISL    7                       ; ISAR := 63
wipe:   LR      D,A                     ; r[ISAR] := 0, ISAR low digit counts down
        BR7     wipe                    ; eight to a block
        LR      A,IS                    ; ISAR = 8*block + 7
        AI      0F8h                    ; carry unless this was block 0
        BNC     wiped
        LR      IS,A                    ; next block down
        LIS     0
        BR      wipe
wiped:  LISU    7
        LISL    3                       ; ISAR := 59
        LI      28h
        LR      S,A                     ; r59 := $28, the K stack pointer
        LI      0FFh
        INC                             ; A = 0, flags Z C S
        JMP     0802h                   ; A := $08

; ---- clear screen (r3 = 0) -----------------------------------------------
;
; Walks rows 63..0. Per row: port 5 selects the row (its tone bits 7-6, read
; back from the latch, stay as they were); the five hidden columns 122-126 take
; the colour in r3 bits 7-6 when r3 bits 5-0 are zero, else 0; columns 1-110
; take 0. Columns 0, 111-121 and 127 are never written. Port 0 ends at $00.
;
; Scratchpad used: r0 row, r1 port-4 byte (~column), r2 cells left, r5 tone
; bits, r6 port-1 byte of the hidden columns, r7 $60, r8 $50 (strobe on, off).
; All of them are the routine's to clobber; r3 is only read. No stack, no
; PI/PK, so PC1 still holds the caller's return address for POP.

clear:  IN      5                       ; port-5 latch
        NI      0C0h
        LR      5,A
        LI      0FFh                    ; colour 0: bits 7-6 = ~0
        LR      6,A
        LR      A,3
        NI      3Fh
        BNZ     plain                   ; r3 is not a bare colour
        LR      A,3
        NI      0C0h
        XI      0C0h                    ; port 1 carries ~colour in bits 7-6
        OI      3Fh
        LR      6,A
plain:  LI      60h
        LR      7,A
        LI      50h
        LR      8,A
        LI      40h
        OUTS    0                       ; controller gate off the VRAM registers
        LI      3Fh
        LR      0,A

row:    LR      A,0                     ; port 5 := ~row | tone bits
        COM
        NI      3Fh
        AS      5
        OUTS    5

        LR      A,6                     ; hidden columns 122..126: ~col = $85..$81
        OUTS    1
        LI      85h
        LR      1,A
        LIS     5
        LR      2,A
hide:   LR      A,1
        OUTS    4
        LR      A,7
        OUTS    0                       ; strobe: the cell takes the value in port 1
        LR      A,8
        OUTS    0
        DS      1
        DS      2
        BNZ     hide

        LI      0FFh                    ; columns 1..110: ~col = $FE..$91, value 0
        OUTS    1
        LI      0FEh
        LR      1,A
        LI      110
        LR      2,A
fill:   LR      A,1
        OUTS    4
        LR      A,7
        OUTS    0
        LR      A,8
        OUTS    0
        DS      1
        DS      2
        BNZ     fill

        DS      0                       ; carry unless that was row 0
        BC      row
        LIS     0
        OUTS    0
        POP

; ---- the K stack ----------------------------------------------------------
;
; p = r59 & 63. Both routines park ISAR in r7 and restore it from there, and
; every other scratchpad byte they touch is a slot or r59. LR IS,A keeps six
; bits, which is the "& 63" of the slot arithmetic. They sit after the entry
; points, which are only 23 bytes apart.

        ORG     0121h

; push K: r[p] := KU; r[p+1] := KL (read after the first store); r59 := p+2;
; W := S only.
pushk:  LR      A,IS
        LR      7,A
        LISU    7
        LISL    3
        LR      A,S                     ; r59
        LR      IS,A                    ; ISAR := p
        LR      A,KU
        LR      S,A
        LR      A,IS
        INC
        LR      IS,A                    ; ISAR := p+1
        LR      A,KL
        LR      S,A
        LR      A,IS
        INC
        NI      3Fh                     ; p+2
        LISU    7
        LISL    3
        LR      S,A                     ; r59 := p+2
        LIS     1
        NI      1                       ; W low bits := 0001
        LR      A,7
        LR      IS,A
        POP

; pop K: q = p-2; KL := r[q+1], then KU := r[q]; r59 := q. The last flag
; operation is (q+1) + $FF, which gives W = C S (Z C S for q = 0, none for q = 63).
popk:   LR      A,IS
        LR      7,A
        LISU    7
        LISL    3
        LR      A,S                     ; r59
        AI      0FEh                    ; minus 2
        LR      IS,A                    ; ISAR := q
        LR      A,IS
        INC
        LR      IS,A                    ; ISAR := q+1
        LR      A,S
        LR      KL,A
        LR      A,IS
        AI      0FFh                    ; q ($FF for q = 63), and the flags
        LR      IS,A                    ; ISAR := q
        LR      A,S
        LR      KU,A
        LR      A,IS
        LISU    7
        LISL    3
        LR      S,A                     ; r59 := q
        LR      A,7
        LR      IS,A
        POP

; ---- the entry points (the routines above are placed where they fit) -------

        ORG     00D0h
        JMP     clear

        ORG     0107h
        JMP     pushk

        ORG     011Eh
        JMP     popk
