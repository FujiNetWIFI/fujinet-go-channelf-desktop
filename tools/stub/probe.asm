; probe.asm -- the probe cartridge for observe.py (assemble: asl, CPU F3850).
;
; $0800 must hold $55 for the console to enter the cartridge at $0802.  The
; spin loops let the Lua driver (obs.lua) inspect the machine between runs:
; it sets the machine state while the CPU spins, points the CPU at the NOP of
; a launch pad, and waits for the CPU to come back to the pad's return spin.
; No pad reads or depends on any BIOS byte; the pads only CALL entry points.
;
; pad       launch instruction   what it leaves free in the pre-state
; --------  -------------------  -----------------------------------------
; clr/push/pop   PI $xxxx        A is overwritten (high byte of target)
; pk             PK              K is the target; A and Q are free
; lr             LR P0,Q         Q is the target; A and K are free; PC1 is
;                                set by the driver (= ret_lr)
; jmp            JMP $0000       A is overwritten (= 0); cartridge re-entry

        CPU     F3850

        ORG     0800h
        DB      55h                     ; cartridge signature
        DB      0                       ; $0801 (byte after the signature)
entry:  BR      entry                   ; $0802: arrival spin

        ORG     0820h
pad_clr:  NOP
        PI      00D0h
ret_clr:  BR    ret_clr

        ORG     0830h
pad_push: NOP
        PI      0107h
ret_push: BR    ret_push

        ORG     0840h
pad_pop:  NOP
        PI      011Eh
ret_pop:  BR    ret_pop

        ORG     0850h
pad_pk:   NOP
        PK
ret_pk:   BR    ret_pk

        ORG     0860h
pad_lr:   NOP
        LR      P0,Q
ret_lr:   BR    ret_lr

        ORG     0870h
pad_jmp:  NOP
        JMP     0000h
