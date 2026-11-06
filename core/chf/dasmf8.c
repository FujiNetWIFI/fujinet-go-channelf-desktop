/* dasmf8.c -- the F8 disassembler (see f8.h).
 *
 * Transposed from MAME's src/devices/cpu/f8/f8dasm.cpp (BSD-3-Clause,
 * copyright-holders Juergen Buchmueller -- see COMPLIANCE.md). The text,
 * spacing and operand format are MAME's, so a listing here and MAME's debugger
 * read alike; core/tests/dasmf8_test.c holds the two to each other for all 256
 * opcodes. MAME's switch is folded by opcode range; its STEP_OVER / STEP_OUT /
 * STEP_COND flags become F8_DASM_* (PK, which MAME marks as a step out, is a
 * call here: it is PI through K).
 *
 * Copyright (C) 2026 Thomas Cherryhomes
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "f8.h"

static const char *const rname[16] = {
    "R0", "R1", "R2", "R3", "R4", "R5", "R6", "R7",
    "R8", "J",  "HU", "HL", "KU", "KL", "QU", "QL"
};

int dasmf8_length(uint8_t op)
{
    if (op == 0x28 || op == 0x29 || op == 0x2a)
        return 3;
    if ((op >= 0x20 && op <= 0x27) || (op >= 0x80 && op <= 0x87) || op == 0x8f ||
        (op >= 0x90 && op <= 0x9f))
        return 2;
    return 1;
}

unsigned dasmf8_op_flags(uint8_t op)
{
    if (op == 0x28 || op == 0x0c)
        return F8_DASM_CALL;
    if (op == 0x1c)
        return F8_DASM_RETURN;
    if (op == 0x29 || op == 0x90)
        return F8_DASM_BRANCH;
    if ((op >= 0x80 && op <= 0x87) || op == 0x8f || (op >= 0x91 && op <= 0x9f))
        return F8_DASM_BRANCH | F8_DASM_COND;
    return 0;
}

int dasmf8(char *buf, size_t bufsz, uint16_t pc, const uint8_t op[3], unsigned *flags,
           int *target)
{
    char t[48];
    const uint8_t o = op[0];
    const int len = dasmf8_length(o);
    const unsigned ab = ((unsigned)op[1] << 8) | op[2];                     /* r16(pc + 1) */
    const unsigned rel = (uint16_t)(pc + (int8_t)op[1] + 1);                /* base_pc + offset + 1 */
    int tgt = -1;

    if (o <= 0x03)
        snprintf(t, sizeof t, "LR   A,%s", rname[o + 12]);
    else if (o <= 0x07)
        snprintf(t, sizeof t, "LR   %s,A", rname[(o - 0x04) + 12]);
    else if ((o & 0x0f) <= 0x0e && (((o >> 4) >= 3 && (o >> 4) <= 5) || (o >> 4) >= 0x0c)) {
        /* DS, LR A,x, LR x,A, AS, ASD, XS, NS: a scratchpad register (0-B), or ISAR (S) and
         * ISAR++ (I), ISAR-- (D) */
        static const char *const sid[3] = { "S", "I", "D" };
        const char *const rn = (o & 0x0f) <= 0x0b ? rname[o & 15] : sid[(o & 15) - 0x0c];
        switch (o >> 4) {
        case 0x3: snprintf(t, sizeof t, "DS   %s", rn); break;
        case 0x4: snprintf(t, sizeof t, "LR   A,%s", rn); break;
        case 0x5: snprintf(t, sizeof t, "LR   %s,A", rn); break;
        case 0xc: snprintf(t, sizeof t, "AS   %s", rn); break;
        case 0xd: snprintf(t, sizeof t, "ASD  %s", rn); break;
        case 0xe: snprintf(t, sizeof t, "XS   %s", rn); break;
        default:  snprintf(t, sizeof t, "NS   %s", rn); break;
        }
    }
    else if (o >= 0x60 && o <= 0x67)
        snprintf(t, sizeof t, "LISU %d", o & 0x07);
    else if (o >= 0x68 && o <= 0x6f)
        snprintf(t, sizeof t, "LISL %d", o & 0x07);
    else if (o >= 0x71 && o <= 0x7f)
        snprintf(t, sizeof t, "LIS  H'%02X'", o & 0x0f);
    else if (o >= 0xa4 && o <= 0xaf)
        snprintf(t, sizeof t, "INS  H'%02X'", o & 0x0f);
    else if (o >= 0xb4 && o <= 0xbf)
        snprintf(t, sizeof t, "OUTS H'%02X'", o & 0x0f);
    else {
        switch (o) {
        case 0x08: snprintf(t, sizeof t, "LR   K,P"); break;
        case 0x09: snprintf(t, sizeof t, "LR   P,K"); break;
        case 0x0a: snprintf(t, sizeof t, "LR   A,IS"); break;
        case 0x0b: snprintf(t, sizeof t, "LR   IS,A"); break;
        case 0x0c: snprintf(t, sizeof t, "PK"); break;
        case 0x0d: snprintf(t, sizeof t, "LR   P0,Q"); break;
        case 0x0e: snprintf(t, sizeof t, "LR   Q,DC"); break;
        case 0x0f: snprintf(t, sizeof t, "LR   DC,Q"); break;
        case 0x10: snprintf(t, sizeof t, "LR   DC,H"); break;
        case 0x11: snprintf(t, sizeof t, "LR   H,DC"); break;
        case 0x12: snprintf(t, sizeof t, "SR   1"); break;
        case 0x13: snprintf(t, sizeof t, "SL   1"); break;
        case 0x14: snprintf(t, sizeof t, "SR   4"); break;
        case 0x15: snprintf(t, sizeof t, "SL   4"); break;
        case 0x16: snprintf(t, sizeof t, "LM"); break;
        case 0x17: snprintf(t, sizeof t, "ST"); break;
        case 0x18: snprintf(t, sizeof t, "COM"); break;
        case 0x19: snprintf(t, sizeof t, "LNK"); break;
        case 0x1a: snprintf(t, sizeof t, "DI"); break;
        case 0x1b: snprintf(t, sizeof t, "EI"); break;
        case 0x1c: snprintf(t, sizeof t, "POP"); break;
        case 0x1d: snprintf(t, sizeof t, "LR   W,J"); break;
        case 0x1e: snprintf(t, sizeof t, "LR   J,W"); break;
        case 0x1f: snprintf(t, sizeof t, "INC"); break;
        case 0x20: snprintf(t, sizeof t, "LI   H'%02X'", op[1]); break;
        case 0x21: snprintf(t, sizeof t, "NI   H'%02X'", op[1]); break;
        case 0x22: snprintf(t, sizeof t, "OI   H'%02X'", op[1]); break;
        case 0x23: snprintf(t, sizeof t, "XI   H'%02X'", op[1]); break;
        case 0x24: snprintf(t, sizeof t, "AI   H'%02X'", op[1]); break;
        case 0x25: snprintf(t, sizeof t, "CI   H'%02X'", op[1]); break;
        case 0x26: snprintf(t, sizeof t, "IN   H'%02X'", op[1]); break;
        case 0x27: snprintf(t, sizeof t, "OUT  H'%02X'", op[1]); break;
        case 0x28: snprintf(t, sizeof t, "PI   H'%04X'", ab); tgt = (int)ab; break;
        case 0x29: snprintf(t, sizeof t, "JMP  H'%04X'", ab); tgt = (int)ab; break;
        case 0x2a: snprintf(t, sizeof t, "DCI  H'%04X'", ab); break;
        case 0x2b: snprintf(t, sizeof t, "NOP"); break;
        case 0x2c: snprintf(t, sizeof t, "XDC"); break;
        case 0x70: snprintf(t, sizeof t, "CLR"); break;
        case 0x81: case 0x85: snprintf(t, sizeof t, "BP   H'%04X'", rel); tgt = (int)rel; break;
        case 0x82: snprintf(t, sizeof t, "BC   H'%04X'", rel); tgt = (int)rel; break;
        case 0x84: snprintf(t, sizeof t, "BZ   H'%04X'", rel); tgt = (int)rel; break;
        case 0x80: case 0x83: case 0x86: case 0x87:
            snprintf(t, sizeof t, "BT   H'%02X',H'%04X'", o & 0x07, rel);
            tgt = (int)rel;
            break;
        case 0x88: snprintf(t, sizeof t, "AM"); break;
        case 0x89: snprintf(t, sizeof t, "AMD"); break;
        case 0x8a: snprintf(t, sizeof t, "NM"); break;
        case 0x8b: snprintf(t, sizeof t, "OM"); break;
        case 0x8c: snprintf(t, sizeof t, "XM"); break;
        case 0x8d: snprintf(t, sizeof t, "CM"); break;
        case 0x8e: snprintf(t, sizeof t, "ADC"); break;
        case 0x8f: snprintf(t, sizeof t, "BR7  H'%04X'", rel); tgt = (int)rel; break;
        case 0x90: snprintf(t, sizeof t, "BR   H'%04X'", rel); tgt = (int)rel; break;
        case 0x91: case 0x95: snprintf(t, sizeof t, "BM   H'%04X'", rel); tgt = (int)rel; break;
        case 0x92: snprintf(t, sizeof t, "BNC  H'%04X'", rel); tgt = (int)rel; break;
        case 0x94: snprintf(t, sizeof t, "BNZ  H'%04X'", rel); tgt = (int)rel; break;
        case 0x98: snprintf(t, sizeof t, "BNO  H'%04X'", rel); tgt = (int)rel; break;
        case 0x93: case 0x96: case 0x97: case 0x99: case 0x9a: case 0x9b:
        case 0x9c: case 0x9d: case 0x9e: case 0x9f:
            snprintf(t, sizeof t, "BF   H'%02X',H'%04X'", o & 0x0f, rel);
            tgt = (int)rel;
            break;
        case 0xa0: case 0xa1: snprintf(t, sizeof t, "INS  %d", o & 0x0f); break;
        case 0xb0: case 0xb1: snprintf(t, sizeof t, "OUTS %d", o & 0x0f); break;
        default: snprintf(t, sizeof t, "DC   H'%02X' (?)", o); break;
        }
    }

    if (buf && bufsz) {
        strncpy(buf, t, bufsz - 1);
        buf[bufsz - 1] = '\0';
    }
    if (flags)
        *flags = dasmf8_op_flags(o);
    if (target)
        *target = tgt;
    return len;
}
