; ---------------------------------------------------------------------------
; vm.asm -- the A/B tools' probe machine: a Videocart whose program is a byte
; script at $0B00 (tools/ab/chfab.py appends it), so a probe is a table of
; port accesses written from Python, not a program. Assemble: ~/asl/asl.
;
;   00        end (loops forever)
;   01 b      OUTS 0 <- b          the strobe port
;   02 b      OUTS 1 <- b          colour
;   03 b      OUTS 4 <- b          column
;   04 b      OUTS 5 <- b          row and sound
;   05        INS 0                the value read is written to port $F0
;   06        INS 1                (nothing is mapped there: it only shows up in
;   07        INS 4                 the I/O log, with the clock it happened at)
;   08        INS 5
;   09 n      wait n * ~12 CPU clocks
;   0A p      OUT p <- the next byte  (any port: 2-byte form, ROMC 1A)   [0A p b]
;   0B p      IN p, logged         (any port: ROMC 1B)                   [0B p]
;   0C h l b  store b at address $hhll (ROMC 05)
;   0D h l    load the byte at $hhll (ROMC 02) and log it to port $F0
; ---------------------------------------------------------------------------
	CPU F3850

; a branch to anywhere: BNZ over a JMP
JZ	MACRO target
	BNZ $+5
	JMP target
	ENDM

SCRIPT	EQU 0B00H
LOGPORT	EQU 0F0H

	ORG 0800H
	DB 55H
	DB 00H

	DI
	DCI SCRIPT

LOOP:	LM			; the op
	CI 0
	JZ DONE
	CI 1
	JZ W0
	CI 2
	JZ W1
	CI 3
	JZ W4
	CI 4
	JZ W5
	CI 5
	JZ R0
	CI 6
	JZ R1
	CI 7
	JZ R4
	CI 8
	JZ R5
	CI 9
	JZ WAIT
	CI 10
	JZ OUTP
	CI 11
	JZ INP
	CI 12
	JZ MW
	CI 13
	JZ MR
	JMP LOOP			; unknown: skipped

DONE:	JMP DONE

W0:	LM
	OUTS 0
	JMP LOOP
W1:	LM
	OUTS 1
	JMP LOOP
W4:	LM
	OUTS 4
	JMP LOOP
W5:	LM
	OUTS 5
	JMP LOOP

R0:	INS 0
	OUT LOGPORT
	JMP LOOP
R1:	INS 1
	OUT LOGPORT
	JMP LOOP
R4:	INS 4
	OUT LOGPORT
	JMP LOOP
R5:	INS 5
	OUT LOGPORT
	JMP LOOP

WAIT:	LM
	LR 2,A
WAIT1:	DS 2
	BNZ WAIT1
	JMP LOOP

; OUT p, b and IN p: the port is an immediate of the instruction, so the
; instruction is built in RAM... there is none. A fixed set instead: the ports
; the 2102 carts use and a few unmapped ones.
OUTP:	LM			; the port number selects a stub
	LR 3,A
	LM			; the data
	LR 4,A
	LR A,3
	CI 20H
	JZ O20
	CI 21H
	JZ O21
	CI 24H
	JZ O24
	CI 25H
	JZ O25
	CI 02H
	JZ O02
	LR A,4
	OUT 0F1H
	JMP LOOP
O20:	LR A,4
	OUT 20H
	JMP LOOP
O21:	LR A,4
	OUT 21H
	JMP LOOP
O24:	LR A,4
	OUT 24H
	JMP LOOP
O25:	LR A,4
	OUT 25H
	JMP LOOP
O02:	LR A,4
	OUT 02H
	JMP LOOP

INP:	LM
	CI 20H
	JZ I20
	CI 21H
	JZ I21
	CI 24H
	JZ I24
	CI 25H
	JZ I25
	CI 02H
	JZ I02
	IN 0F1H
	OUT LOGPORT
	JMP LOOP
I20:	IN 20H
	OUT LOGPORT
	JMP LOOP
I21:	IN 21H
	OUT LOGPORT
	JMP LOOP
I24:	IN 24H
	OUT LOGPORT
	JMP LOOP
I25:	IN 25H
	OUT LOGPORT
	JMP LOOP
I02:	IN 02H
	OUT LOGPORT
	JMP LOOP

; 0C h l b: a memory store
MW:	LM
	LR 10,A
	LM
	LR 11,A
	LM
	LR 3,A
	LR Q,DC			; the script pointer, after the operands
	LR DC,H
	LR A,3
	ST
	LR DC,Q
	JMP LOOP

; 0D h l: a memory load, logged
MR:	LM
	LR 10,A
	LM
	LR 11,A
	LR Q,DC
	LR DC,H
	LM
	OUT LOGPORT
	LR DC,Q
	JMP LOOP

	ORG 0AFFH
	DB 0FFH
	END
