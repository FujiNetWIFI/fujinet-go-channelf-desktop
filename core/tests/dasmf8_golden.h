/* dasmf8_golden.h -- MAME's own disassembly of every F8 opcode, from
 * src/devices/cpu/f8/f8dasm.cpp compiled standalone (tools/ab/
 * f8_dasm_golden.cpp, run by tools/ab/gen_dasmf8_golden.sh). GENERATED;
 * dasmf8_test.c holds dasmf8.c to it.
 *
 * Each opcode is disassembled at the NV pc/operand triples of
 * dasmf8_variants; a one-byte opcode has one text, the others NV.
 * flags is MAME's result >> 28: 8 = SUPPORTED, 4 = STEP_OUT, 2 = STEP_OVER,
 * 1 = STEP_COND. */
#define DASMF8_NV 16
static const struct { uint16_t pc; uint8_t b1, b2; } dasmf8_variants[DASMF8_NV] = {
    { 0x0123, 0x5A, 0xC3 },
    { 0x0802, 0x00, 0x00 },
    { 0x1FFE, 0x7F, 0xFF },
    { 0x1FFF, 0x80, 0x12 },
    { 0x2010, 0xFE, 0xA5 },
    { 0x3FFD, 0x01, 0x00 },
    { 0x4000, 0xA1, 0x7F },
    { 0x5FFF, 0xC2, 0x80 },
    { 0x6FFD, 0xE3, 0x34 },
    { 0x7FFE, 0x7F, 0xFF },
    { 0x8000, 0xFF, 0x01 },
    { 0x0100, 0x40, 0x9C },
    { 0x0ABC, 0x15, 0x2A },
    { 0xFF00, 0x0F, 0x55 },
    { 0x0800, 0xF0, 0x33 },
    { 0x0777, 0x3F, 0xC0 },
};
static const struct { uint8_t len; uint8_t flags; const char *text[DASMF8_NV]; } dasmf8_golden[256] = {
    { 1, 8, { "LR   A,KU" } },   /* 00 */
    { 1, 8, { "LR   A,KL" } },   /* 01 */
    { 1, 8, { "LR   A,QU" } },   /* 02 */
    { 1, 8, { "LR   A,QL" } },   /* 03 */
    { 1, 8, { "LR   KU,A" } },   /* 04 */
    { 1, 8, { "LR   KL,A" } },   /* 05 */
    { 1, 8, { "LR   QU,A" } },   /* 06 */
    { 1, 8, { "LR   QL,A" } },   /* 07 */
    { 1, 8, { "LR   K,P" } },   /* 08 */
    { 1, 8, { "LR   P,K" } },   /* 09 */
    { 1, 8, { "LR   A,IS" } },   /* 0A */
    { 1, 8, { "LR   IS,A" } },   /* 0B */
    { 1, 12, { "PK" } },   /* 0C */
    { 1, 8, { "LR   P0,Q" } },   /* 0D */
    { 1, 8, { "LR   Q,DC" } },   /* 0E */
    { 1, 8, { "LR   DC,Q" } },   /* 0F */
    { 1, 8, { "LR   DC,H" } },   /* 10 */
    { 1, 8, { "LR   H,DC" } },   /* 11 */
    { 1, 8, { "SR   1" } },   /* 12 */
    { 1, 8, { "SL   1" } },   /* 13 */
    { 1, 8, { "SR   4" } },   /* 14 */
    { 1, 8, { "SL   4" } },   /* 15 */
    { 1, 8, { "LM" } },   /* 16 */
    { 1, 8, { "ST" } },   /* 17 */
    { 1, 8, { "COM" } },   /* 18 */
    { 1, 8, { "LNK" } },   /* 19 */
    { 1, 8, { "DI" } },   /* 1A */
    { 1, 8, { "EI" } },   /* 1B */
    { 1, 12, { "POP" } },   /* 1C */
    { 1, 8, { "LR   W,J" } },   /* 1D */
    { 1, 8, { "LR   J,W" } },   /* 1E */
    { 1, 8, { "INC" } },   /* 1F */
    { 2, 8, { "LI   H'5A'", "LI   H'00'", "LI   H'7F'", "LI   H'80'", "LI   H'FE'", "LI   H'01'", "LI   H'A1'", "LI   H'C2'", "LI   H'E3'", "LI   H'7F'", "LI   H'FF'", "LI   H'40'", "LI   H'15'", "LI   H'0F'", "LI   H'F0'", "LI   H'3F'" } },   /* 20 */
    { 2, 8, { "NI   H'5A'", "NI   H'00'", "NI   H'7F'", "NI   H'80'", "NI   H'FE'", "NI   H'01'", "NI   H'A1'", "NI   H'C2'", "NI   H'E3'", "NI   H'7F'", "NI   H'FF'", "NI   H'40'", "NI   H'15'", "NI   H'0F'", "NI   H'F0'", "NI   H'3F'" } },   /* 21 */
    { 2, 8, { "OI   H'5A'", "OI   H'00'", "OI   H'7F'", "OI   H'80'", "OI   H'FE'", "OI   H'01'", "OI   H'A1'", "OI   H'C2'", "OI   H'E3'", "OI   H'7F'", "OI   H'FF'", "OI   H'40'", "OI   H'15'", "OI   H'0F'", "OI   H'F0'", "OI   H'3F'" } },   /* 22 */
    { 2, 8, { "XI   H'5A'", "XI   H'00'", "XI   H'7F'", "XI   H'80'", "XI   H'FE'", "XI   H'01'", "XI   H'A1'", "XI   H'C2'", "XI   H'E3'", "XI   H'7F'", "XI   H'FF'", "XI   H'40'", "XI   H'15'", "XI   H'0F'", "XI   H'F0'", "XI   H'3F'" } },   /* 23 */
    { 2, 8, { "AI   H'5A'", "AI   H'00'", "AI   H'7F'", "AI   H'80'", "AI   H'FE'", "AI   H'01'", "AI   H'A1'", "AI   H'C2'", "AI   H'E3'", "AI   H'7F'", "AI   H'FF'", "AI   H'40'", "AI   H'15'", "AI   H'0F'", "AI   H'F0'", "AI   H'3F'" } },   /* 24 */
    { 2, 8, { "CI   H'5A'", "CI   H'00'", "CI   H'7F'", "CI   H'80'", "CI   H'FE'", "CI   H'01'", "CI   H'A1'", "CI   H'C2'", "CI   H'E3'", "CI   H'7F'", "CI   H'FF'", "CI   H'40'", "CI   H'15'", "CI   H'0F'", "CI   H'F0'", "CI   H'3F'" } },   /* 25 */
    { 2, 8, { "IN   H'5A'", "IN   H'00'", "IN   H'7F'", "IN   H'80'", "IN   H'FE'", "IN   H'01'", "IN   H'A1'", "IN   H'C2'", "IN   H'E3'", "IN   H'7F'", "IN   H'FF'", "IN   H'40'", "IN   H'15'", "IN   H'0F'", "IN   H'F0'", "IN   H'3F'" } },   /* 26 */
    { 2, 8, { "OUT  H'5A'", "OUT  H'00'", "OUT  H'7F'", "OUT  H'80'", "OUT  H'FE'", "OUT  H'01'", "OUT  H'A1'", "OUT  H'C2'", "OUT  H'E3'", "OUT  H'7F'", "OUT  H'FF'", "OUT  H'40'", "OUT  H'15'", "OUT  H'0F'", "OUT  H'F0'", "OUT  H'3F'" } },   /* 27 */
    { 3, 10, { "PI   H'5AC3'", "PI   H'0000'", "PI   H'7FFF'", "PI   H'8012'", "PI   H'FEA5'", "PI   H'0100'", "PI   H'A17F'", "PI   H'C280'", "PI   H'E334'", "PI   H'7FFF'", "PI   H'FF01'", "PI   H'409C'", "PI   H'152A'", "PI   H'0F55'", "PI   H'F033'", "PI   H'3FC0'" } },   /* 28 */
    { 3, 8, { "JMP  H'5AC3'", "JMP  H'0000'", "JMP  H'7FFF'", "JMP  H'8012'", "JMP  H'FEA5'", "JMP  H'0100'", "JMP  H'A17F'", "JMP  H'C280'", "JMP  H'E334'", "JMP  H'7FFF'", "JMP  H'FF01'", "JMP  H'409C'", "JMP  H'152A'", "JMP  H'0F55'", "JMP  H'F033'", "JMP  H'3FC0'" } },   /* 29 */
    { 3, 8, { "DCI  H'5AC3'", "DCI  H'0000'", "DCI  H'7FFF'", "DCI  H'8012'", "DCI  H'FEA5'", "DCI  H'0100'", "DCI  H'A17F'", "DCI  H'C280'", "DCI  H'E334'", "DCI  H'7FFF'", "DCI  H'FF01'", "DCI  H'409C'", "DCI  H'152A'", "DCI  H'0F55'", "DCI  H'F033'", "DCI  H'3FC0'" } },   /* 2A */
    { 1, 8, { "NOP" } },   /* 2B */
    { 1, 8, { "XDC" } },   /* 2C */
    { 1, 8, { "DC   H'2D' (?)" } },   /* 2D */
    { 1, 8, { "DC   H'2E' (?)" } },   /* 2E */
    { 1, 8, { "DC   H'2F' (?)" } },   /* 2F */
    { 1, 8, { "DS   R0" } },   /* 30 */
    { 1, 8, { "DS   R1" } },   /* 31 */
    { 1, 8, { "DS   R2" } },   /* 32 */
    { 1, 8, { "DS   R3" } },   /* 33 */
    { 1, 8, { "DS   R4" } },   /* 34 */
    { 1, 8, { "DS   R5" } },   /* 35 */
    { 1, 8, { "DS   R6" } },   /* 36 */
    { 1, 8, { "DS   R7" } },   /* 37 */
    { 1, 8, { "DS   R8" } },   /* 38 */
    { 1, 8, { "DS   J" } },   /* 39 */
    { 1, 8, { "DS   HU" } },   /* 3A */
    { 1, 8, { "DS   HL" } },   /* 3B */
    { 1, 8, { "DS   S" } },   /* 3C */
    { 1, 8, { "DS   I" } },   /* 3D */
    { 1, 8, { "DS   D" } },   /* 3E */
    { 1, 8, { "DC   H'3F' (?)" } },   /* 3F */
    { 1, 8, { "LR   A,R0" } },   /* 40 */
    { 1, 8, { "LR   A,R1" } },   /* 41 */
    { 1, 8, { "LR   A,R2" } },   /* 42 */
    { 1, 8, { "LR   A,R3" } },   /* 43 */
    { 1, 8, { "LR   A,R4" } },   /* 44 */
    { 1, 8, { "LR   A,R5" } },   /* 45 */
    { 1, 8, { "LR   A,R6" } },   /* 46 */
    { 1, 8, { "LR   A,R7" } },   /* 47 */
    { 1, 8, { "LR   A,R8" } },   /* 48 */
    { 1, 8, { "LR   A,J" } },   /* 49 */
    { 1, 8, { "LR   A,HU" } },   /* 4A */
    { 1, 8, { "LR   A,HL" } },   /* 4B */
    { 1, 8, { "LR   A,S" } },   /* 4C */
    { 1, 8, { "LR   A,I" } },   /* 4D */
    { 1, 8, { "LR   A,D" } },   /* 4E */
    { 1, 8, { "DC   H'4F' (?)" } },   /* 4F */
    { 1, 8, { "LR   R0,A" } },   /* 50 */
    { 1, 8, { "LR   R1,A" } },   /* 51 */
    { 1, 8, { "LR   R2,A" } },   /* 52 */
    { 1, 8, { "LR   R3,A" } },   /* 53 */
    { 1, 8, { "LR   R4,A" } },   /* 54 */
    { 1, 8, { "LR   R5,A" } },   /* 55 */
    { 1, 8, { "LR   R6,A" } },   /* 56 */
    { 1, 8, { "LR   R7,A" } },   /* 57 */
    { 1, 8, { "LR   R8,A" } },   /* 58 */
    { 1, 8, { "LR   J,A" } },   /* 59 */
    { 1, 8, { "LR   HU,A" } },   /* 5A */
    { 1, 8, { "LR   HL,A" } },   /* 5B */
    { 1, 8, { "LR   S,A" } },   /* 5C */
    { 1, 8, { "LR   I,A" } },   /* 5D */
    { 1, 8, { "LR   D,A" } },   /* 5E */
    { 1, 8, { "DC   H'5F' (?)" } },   /* 5F */
    { 1, 8, { "LISU 0" } },   /* 60 */
    { 1, 8, { "LISU 1" } },   /* 61 */
    { 1, 8, { "LISU 2" } },   /* 62 */
    { 1, 8, { "LISU 3" } },   /* 63 */
    { 1, 8, { "LISU 4" } },   /* 64 */
    { 1, 8, { "LISU 5" } },   /* 65 */
    { 1, 8, { "LISU 6" } },   /* 66 */
    { 1, 8, { "LISU 7" } },   /* 67 */
    { 1, 8, { "LISL 0" } },   /* 68 */
    { 1, 8, { "LISL 1" } },   /* 69 */
    { 1, 8, { "LISL 2" } },   /* 6A */
    { 1, 8, { "LISL 3" } },   /* 6B */
    { 1, 8, { "LISL 4" } },   /* 6C */
    { 1, 8, { "LISL 5" } },   /* 6D */
    { 1, 8, { "LISL 6" } },   /* 6E */
    { 1, 8, { "LISL 7" } },   /* 6F */
    { 1, 8, { "CLR" } },   /* 70 */
    { 1, 8, { "LIS  H'01'" } },   /* 71 */
    { 1, 8, { "LIS  H'02'" } },   /* 72 */
    { 1, 8, { "LIS  H'03'" } },   /* 73 */
    { 1, 8, { "LIS  H'04'" } },   /* 74 */
    { 1, 8, { "LIS  H'05'" } },   /* 75 */
    { 1, 8, { "LIS  H'06'" } },   /* 76 */
    { 1, 8, { "LIS  H'07'" } },   /* 77 */
    { 1, 8, { "LIS  H'08'" } },   /* 78 */
    { 1, 8, { "LIS  H'09'" } },   /* 79 */
    { 1, 8, { "LIS  H'0A'" } },   /* 7A */
    { 1, 8, { "LIS  H'0B'" } },   /* 7B */
    { 1, 8, { "LIS  H'0C'" } },   /* 7C */
    { 1, 8, { "LIS  H'0D'" } },   /* 7D */
    { 1, 8, { "LIS  H'0E'" } },   /* 7E */
    { 1, 8, { "LIS  H'0F'" } },   /* 7F */
    { 2, 9, { "BT   H'00',H'017E'", "BT   H'00',H'0803'", "BT   H'00',H'207E'", "BT   H'00',H'1F80'", "BT   H'00',H'200F'", "BT   H'00',H'3FFF'", "BT   H'00',H'3FA2'", "BT   H'00',H'5FC2'", "BT   H'00',H'6FE1'", "BT   H'00',H'807E'", "BT   H'00',H'8000'", "BT   H'00',H'0141'", "BT   H'00',H'0AD2'", "BT   H'00',H'FF10'", "BT   H'00',H'07F1'", "BT   H'00',H'07B7'" } },   /* 80 */
    { 2, 9, { "BP   H'017E'", "BP   H'0803'", "BP   H'207E'", "BP   H'1F80'", "BP   H'200F'", "BP   H'3FFF'", "BP   H'3FA2'", "BP   H'5FC2'", "BP   H'6FE1'", "BP   H'807E'", "BP   H'8000'", "BP   H'0141'", "BP   H'0AD2'", "BP   H'FF10'", "BP   H'07F1'", "BP   H'07B7'" } },   /* 81 */
    { 2, 9, { "BC   H'017E'", "BC   H'0803'", "BC   H'207E'", "BC   H'1F80'", "BC   H'200F'", "BC   H'3FFF'", "BC   H'3FA2'", "BC   H'5FC2'", "BC   H'6FE1'", "BC   H'807E'", "BC   H'8000'", "BC   H'0141'", "BC   H'0AD2'", "BC   H'FF10'", "BC   H'07F1'", "BC   H'07B7'" } },   /* 82 */
    { 2, 9, { "BT   H'03',H'017E'", "BT   H'03',H'0803'", "BT   H'03',H'207E'", "BT   H'03',H'1F80'", "BT   H'03',H'200F'", "BT   H'03',H'3FFF'", "BT   H'03',H'3FA2'", "BT   H'03',H'5FC2'", "BT   H'03',H'6FE1'", "BT   H'03',H'807E'", "BT   H'03',H'8000'", "BT   H'03',H'0141'", "BT   H'03',H'0AD2'", "BT   H'03',H'FF10'", "BT   H'03',H'07F1'", "BT   H'03',H'07B7'" } },   /* 83 */
    { 2, 9, { "BZ   H'017E'", "BZ   H'0803'", "BZ   H'207E'", "BZ   H'1F80'", "BZ   H'200F'", "BZ   H'3FFF'", "BZ   H'3FA2'", "BZ   H'5FC2'", "BZ   H'6FE1'", "BZ   H'807E'", "BZ   H'8000'", "BZ   H'0141'", "BZ   H'0AD2'", "BZ   H'FF10'", "BZ   H'07F1'", "BZ   H'07B7'" } },   /* 84 */
    { 2, 9, { "BP   H'017E'", "BP   H'0803'", "BP   H'207E'", "BP   H'1F80'", "BP   H'200F'", "BP   H'3FFF'", "BP   H'3FA2'", "BP   H'5FC2'", "BP   H'6FE1'", "BP   H'807E'", "BP   H'8000'", "BP   H'0141'", "BP   H'0AD2'", "BP   H'FF10'", "BP   H'07F1'", "BP   H'07B7'" } },   /* 85 */
    { 2, 9, { "BT   H'06',H'017E'", "BT   H'06',H'0803'", "BT   H'06',H'207E'", "BT   H'06',H'1F80'", "BT   H'06',H'200F'", "BT   H'06',H'3FFF'", "BT   H'06',H'3FA2'", "BT   H'06',H'5FC2'", "BT   H'06',H'6FE1'", "BT   H'06',H'807E'", "BT   H'06',H'8000'", "BT   H'06',H'0141'", "BT   H'06',H'0AD2'", "BT   H'06',H'FF10'", "BT   H'06',H'07F1'", "BT   H'06',H'07B7'" } },   /* 86 */
    { 2, 9, { "BT   H'07',H'017E'", "BT   H'07',H'0803'", "BT   H'07',H'207E'", "BT   H'07',H'1F80'", "BT   H'07',H'200F'", "BT   H'07',H'3FFF'", "BT   H'07',H'3FA2'", "BT   H'07',H'5FC2'", "BT   H'07',H'6FE1'", "BT   H'07',H'807E'", "BT   H'07',H'8000'", "BT   H'07',H'0141'", "BT   H'07',H'0AD2'", "BT   H'07',H'FF10'", "BT   H'07',H'07F1'", "BT   H'07',H'07B7'" } },   /* 87 */
    { 1, 8, { "AM" } },   /* 88 */
    { 1, 8, { "AMD" } },   /* 89 */
    { 1, 8, { "NM" } },   /* 8A */
    { 1, 8, { "OM" } },   /* 8B */
    { 1, 8, { "XM" } },   /* 8C */
    { 1, 8, { "CM" } },   /* 8D */
    { 1, 8, { "ADC" } },   /* 8E */
    { 2, 9, { "BR7  H'017E'", "BR7  H'0803'", "BR7  H'207E'", "BR7  H'1F80'", "BR7  H'200F'", "BR7  H'3FFF'", "BR7  H'3FA2'", "BR7  H'5FC2'", "BR7  H'6FE1'", "BR7  H'807E'", "BR7  H'8000'", "BR7  H'0141'", "BR7  H'0AD2'", "BR7  H'FF10'", "BR7  H'07F1'", "BR7  H'07B7'" } },   /* 8F */
    { 2, 8, { "BR   H'017E'", "BR   H'0803'", "BR   H'207E'", "BR   H'1F80'", "BR   H'200F'", "BR   H'3FFF'", "BR   H'3FA2'", "BR   H'5FC2'", "BR   H'6FE1'", "BR   H'807E'", "BR   H'8000'", "BR   H'0141'", "BR   H'0AD2'", "BR   H'FF10'", "BR   H'07F1'", "BR   H'07B7'" } },   /* 90 */
    { 2, 9, { "BM   H'017E'", "BM   H'0803'", "BM   H'207E'", "BM   H'1F80'", "BM   H'200F'", "BM   H'3FFF'", "BM   H'3FA2'", "BM   H'5FC2'", "BM   H'6FE1'", "BM   H'807E'", "BM   H'8000'", "BM   H'0141'", "BM   H'0AD2'", "BM   H'FF10'", "BM   H'07F1'", "BM   H'07B7'" } },   /* 91 */
    { 2, 9, { "BNC  H'017E'", "BNC  H'0803'", "BNC  H'207E'", "BNC  H'1F80'", "BNC  H'200F'", "BNC  H'3FFF'", "BNC  H'3FA2'", "BNC  H'5FC2'", "BNC  H'6FE1'", "BNC  H'807E'", "BNC  H'8000'", "BNC  H'0141'", "BNC  H'0AD2'", "BNC  H'FF10'", "BNC  H'07F1'", "BNC  H'07B7'" } },   /* 92 */
    { 2, 9, { "BF   H'03',H'017E'", "BF   H'03',H'0803'", "BF   H'03',H'207E'", "BF   H'03',H'1F80'", "BF   H'03',H'200F'", "BF   H'03',H'3FFF'", "BF   H'03',H'3FA2'", "BF   H'03',H'5FC2'", "BF   H'03',H'6FE1'", "BF   H'03',H'807E'", "BF   H'03',H'8000'", "BF   H'03',H'0141'", "BF   H'03',H'0AD2'", "BF   H'03',H'FF10'", "BF   H'03',H'07F1'", "BF   H'03',H'07B7'" } },   /* 93 */
    { 2, 9, { "BNZ  H'017E'", "BNZ  H'0803'", "BNZ  H'207E'", "BNZ  H'1F80'", "BNZ  H'200F'", "BNZ  H'3FFF'", "BNZ  H'3FA2'", "BNZ  H'5FC2'", "BNZ  H'6FE1'", "BNZ  H'807E'", "BNZ  H'8000'", "BNZ  H'0141'", "BNZ  H'0AD2'", "BNZ  H'FF10'", "BNZ  H'07F1'", "BNZ  H'07B7'" } },   /* 94 */
    { 2, 9, { "BM   H'017E'", "BM   H'0803'", "BM   H'207E'", "BM   H'1F80'", "BM   H'200F'", "BM   H'3FFF'", "BM   H'3FA2'", "BM   H'5FC2'", "BM   H'6FE1'", "BM   H'807E'", "BM   H'8000'", "BM   H'0141'", "BM   H'0AD2'", "BM   H'FF10'", "BM   H'07F1'", "BM   H'07B7'" } },   /* 95 */
    { 2, 9, { "BF   H'06',H'017E'", "BF   H'06',H'0803'", "BF   H'06',H'207E'", "BF   H'06',H'1F80'", "BF   H'06',H'200F'", "BF   H'06',H'3FFF'", "BF   H'06',H'3FA2'", "BF   H'06',H'5FC2'", "BF   H'06',H'6FE1'", "BF   H'06',H'807E'", "BF   H'06',H'8000'", "BF   H'06',H'0141'", "BF   H'06',H'0AD2'", "BF   H'06',H'FF10'", "BF   H'06',H'07F1'", "BF   H'06',H'07B7'" } },   /* 96 */
    { 2, 9, { "BF   H'07',H'017E'", "BF   H'07',H'0803'", "BF   H'07',H'207E'", "BF   H'07',H'1F80'", "BF   H'07',H'200F'", "BF   H'07',H'3FFF'", "BF   H'07',H'3FA2'", "BF   H'07',H'5FC2'", "BF   H'07',H'6FE1'", "BF   H'07',H'807E'", "BF   H'07',H'8000'", "BF   H'07',H'0141'", "BF   H'07',H'0AD2'", "BF   H'07',H'FF10'", "BF   H'07',H'07F1'", "BF   H'07',H'07B7'" } },   /* 97 */
    { 2, 9, { "BNO  H'017E'", "BNO  H'0803'", "BNO  H'207E'", "BNO  H'1F80'", "BNO  H'200F'", "BNO  H'3FFF'", "BNO  H'3FA2'", "BNO  H'5FC2'", "BNO  H'6FE1'", "BNO  H'807E'", "BNO  H'8000'", "BNO  H'0141'", "BNO  H'0AD2'", "BNO  H'FF10'", "BNO  H'07F1'", "BNO  H'07B7'" } },   /* 98 */
    { 2, 9, { "BF   H'09',H'017E'", "BF   H'09',H'0803'", "BF   H'09',H'207E'", "BF   H'09',H'1F80'", "BF   H'09',H'200F'", "BF   H'09',H'3FFF'", "BF   H'09',H'3FA2'", "BF   H'09',H'5FC2'", "BF   H'09',H'6FE1'", "BF   H'09',H'807E'", "BF   H'09',H'8000'", "BF   H'09',H'0141'", "BF   H'09',H'0AD2'", "BF   H'09',H'FF10'", "BF   H'09',H'07F1'", "BF   H'09',H'07B7'" } },   /* 99 */
    { 2, 9, { "BF   H'0A',H'017E'", "BF   H'0A',H'0803'", "BF   H'0A',H'207E'", "BF   H'0A',H'1F80'", "BF   H'0A',H'200F'", "BF   H'0A',H'3FFF'", "BF   H'0A',H'3FA2'", "BF   H'0A',H'5FC2'", "BF   H'0A',H'6FE1'", "BF   H'0A',H'807E'", "BF   H'0A',H'8000'", "BF   H'0A',H'0141'", "BF   H'0A',H'0AD2'", "BF   H'0A',H'FF10'", "BF   H'0A',H'07F1'", "BF   H'0A',H'07B7'" } },   /* 9A */
    { 2, 9, { "BF   H'0B',H'017E'", "BF   H'0B',H'0803'", "BF   H'0B',H'207E'", "BF   H'0B',H'1F80'", "BF   H'0B',H'200F'", "BF   H'0B',H'3FFF'", "BF   H'0B',H'3FA2'", "BF   H'0B',H'5FC2'", "BF   H'0B',H'6FE1'", "BF   H'0B',H'807E'", "BF   H'0B',H'8000'", "BF   H'0B',H'0141'", "BF   H'0B',H'0AD2'", "BF   H'0B',H'FF10'", "BF   H'0B',H'07F1'", "BF   H'0B',H'07B7'" } },   /* 9B */
    { 2, 9, { "BF   H'0C',H'017E'", "BF   H'0C',H'0803'", "BF   H'0C',H'207E'", "BF   H'0C',H'1F80'", "BF   H'0C',H'200F'", "BF   H'0C',H'3FFF'", "BF   H'0C',H'3FA2'", "BF   H'0C',H'5FC2'", "BF   H'0C',H'6FE1'", "BF   H'0C',H'807E'", "BF   H'0C',H'8000'", "BF   H'0C',H'0141'", "BF   H'0C',H'0AD2'", "BF   H'0C',H'FF10'", "BF   H'0C',H'07F1'", "BF   H'0C',H'07B7'" } },   /* 9C */
    { 2, 9, { "BF   H'0D',H'017E'", "BF   H'0D',H'0803'", "BF   H'0D',H'207E'", "BF   H'0D',H'1F80'", "BF   H'0D',H'200F'", "BF   H'0D',H'3FFF'", "BF   H'0D',H'3FA2'", "BF   H'0D',H'5FC2'", "BF   H'0D',H'6FE1'", "BF   H'0D',H'807E'", "BF   H'0D',H'8000'", "BF   H'0D',H'0141'", "BF   H'0D',H'0AD2'", "BF   H'0D',H'FF10'", "BF   H'0D',H'07F1'", "BF   H'0D',H'07B7'" } },   /* 9D */
    { 2, 9, { "BF   H'0E',H'017E'", "BF   H'0E',H'0803'", "BF   H'0E',H'207E'", "BF   H'0E',H'1F80'", "BF   H'0E',H'200F'", "BF   H'0E',H'3FFF'", "BF   H'0E',H'3FA2'", "BF   H'0E',H'5FC2'", "BF   H'0E',H'6FE1'", "BF   H'0E',H'807E'", "BF   H'0E',H'8000'", "BF   H'0E',H'0141'", "BF   H'0E',H'0AD2'", "BF   H'0E',H'FF10'", "BF   H'0E',H'07F1'", "BF   H'0E',H'07B7'" } },   /* 9E */
    { 2, 9, { "BF   H'0F',H'017E'", "BF   H'0F',H'0803'", "BF   H'0F',H'207E'", "BF   H'0F',H'1F80'", "BF   H'0F',H'200F'", "BF   H'0F',H'3FFF'", "BF   H'0F',H'3FA2'", "BF   H'0F',H'5FC2'", "BF   H'0F',H'6FE1'", "BF   H'0F',H'807E'", "BF   H'0F',H'8000'", "BF   H'0F',H'0141'", "BF   H'0F',H'0AD2'", "BF   H'0F',H'FF10'", "BF   H'0F',H'07F1'", "BF   H'0F',H'07B7'" } },   /* 9F */
    { 1, 8, { "INS  0" } },   /* A0 */
    { 1, 8, { "INS  1" } },   /* A1 */
    { 1, 8, { "DC   H'A2' (?)" } },   /* A2 */
    { 1, 8, { "DC   H'A3' (?)" } },   /* A3 */
    { 1, 8, { "INS  H'04'" } },   /* A4 */
    { 1, 8, { "INS  H'05'" } },   /* A5 */
    { 1, 8, { "INS  H'06'" } },   /* A6 */
    { 1, 8, { "INS  H'07'" } },   /* A7 */
    { 1, 8, { "INS  H'08'" } },   /* A8 */
    { 1, 8, { "INS  H'09'" } },   /* A9 */
    { 1, 8, { "INS  H'0A'" } },   /* AA */
    { 1, 8, { "INS  H'0B'" } },   /* AB */
    { 1, 8, { "INS  H'0C'" } },   /* AC */
    { 1, 8, { "INS  H'0D'" } },   /* AD */
    { 1, 8, { "INS  H'0E'" } },   /* AE */
    { 1, 8, { "INS  H'0F'" } },   /* AF */
    { 1, 8, { "OUTS 0" } },   /* B0 */
    { 1, 8, { "OUTS 1" } },   /* B1 */
    { 1, 8, { "DC   H'B2' (?)" } },   /* B2 */
    { 1, 8, { "DC   H'B3' (?)" } },   /* B3 */
    { 1, 8, { "OUTS H'04'" } },   /* B4 */
    { 1, 8, { "OUTS H'05'" } },   /* B5 */
    { 1, 8, { "OUTS H'06'" } },   /* B6 */
    { 1, 8, { "OUTS H'07'" } },   /* B7 */
    { 1, 8, { "OUTS H'08'" } },   /* B8 */
    { 1, 8, { "OUTS H'09'" } },   /* B9 */
    { 1, 8, { "OUTS H'0A'" } },   /* BA */
    { 1, 8, { "OUTS H'0B'" } },   /* BB */
    { 1, 8, { "OUTS H'0C'" } },   /* BC */
    { 1, 8, { "OUTS H'0D'" } },   /* BD */
    { 1, 8, { "OUTS H'0E'" } },   /* BE */
    { 1, 8, { "OUTS H'0F'" } },   /* BF */
    { 1, 8, { "AS   R0" } },   /* C0 */
    { 1, 8, { "AS   R1" } },   /* C1 */
    { 1, 8, { "AS   R2" } },   /* C2 */
    { 1, 8, { "AS   R3" } },   /* C3 */
    { 1, 8, { "AS   R4" } },   /* C4 */
    { 1, 8, { "AS   R5" } },   /* C5 */
    { 1, 8, { "AS   R6" } },   /* C6 */
    { 1, 8, { "AS   R7" } },   /* C7 */
    { 1, 8, { "AS   R8" } },   /* C8 */
    { 1, 8, { "AS   J" } },   /* C9 */
    { 1, 8, { "AS   HU" } },   /* CA */
    { 1, 8, { "AS   HL" } },   /* CB */
    { 1, 8, { "AS   S" } },   /* CC */
    { 1, 8, { "AS   I" } },   /* CD */
    { 1, 8, { "AS   D" } },   /* CE */
    { 1, 8, { "DC   H'CF' (?)" } },   /* CF */
    { 1, 8, { "ASD  R0" } },   /* D0 */
    { 1, 8, { "ASD  R1" } },   /* D1 */
    { 1, 8, { "ASD  R2" } },   /* D2 */
    { 1, 8, { "ASD  R3" } },   /* D3 */
    { 1, 8, { "ASD  R4" } },   /* D4 */
    { 1, 8, { "ASD  R5" } },   /* D5 */
    { 1, 8, { "ASD  R6" } },   /* D6 */
    { 1, 8, { "ASD  R7" } },   /* D7 */
    { 1, 8, { "ASD  R8" } },   /* D8 */
    { 1, 8, { "ASD  J" } },   /* D9 */
    { 1, 8, { "ASD  HU" } },   /* DA */
    { 1, 8, { "ASD  HL" } },   /* DB */
    { 1, 8, { "ASD  S" } },   /* DC */
    { 1, 8, { "ASD  I" } },   /* DD */
    { 1, 8, { "ASD  D" } },   /* DE */
    { 1, 8, { "DC   H'DF' (?)" } },   /* DF */
    { 1, 8, { "XS   R0" } },   /* E0 */
    { 1, 8, { "XS   R1" } },   /* E1 */
    { 1, 8, { "XS   R2" } },   /* E2 */
    { 1, 8, { "XS   R3" } },   /* E3 */
    { 1, 8, { "XS   R4" } },   /* E4 */
    { 1, 8, { "XS   R5" } },   /* E5 */
    { 1, 8, { "XS   R6" } },   /* E6 */
    { 1, 8, { "XS   R7" } },   /* E7 */
    { 1, 8, { "XS   R8" } },   /* E8 */
    { 1, 8, { "XS   J" } },   /* E9 */
    { 1, 8, { "XS   HU" } },   /* EA */
    { 1, 8, { "XS   HL" } },   /* EB */
    { 1, 8, { "XS   S" } },   /* EC */
    { 1, 8, { "XS   I" } },   /* ED */
    { 1, 8, { "XS   D" } },   /* EE */
    { 1, 8, { "DC   H'EF' (?)" } },   /* EF */
    { 1, 8, { "NS   R0" } },   /* F0 */
    { 1, 8, { "NS   R1" } },   /* F1 */
    { 1, 8, { "NS   R2" } },   /* F2 */
    { 1, 8, { "NS   R3" } },   /* F3 */
    { 1, 8, { "NS   R4" } },   /* F4 */
    { 1, 8, { "NS   R5" } },   /* F5 */
    { 1, 8, { "NS   R6" } },   /* F6 */
    { 1, 8, { "NS   R7" } },   /* F7 */
    { 1, 8, { "NS   R8" } },   /* F8 */
    { 1, 8, { "NS   J" } },   /* F9 */
    { 1, 8, { "NS   HU" } },   /* FA */
    { 1, 8, { "NS   HL" } },   /* FB */
    { 1, 8, { "NS   S" } },   /* FC */
    { 1, 8, { "NS   I" } },   /* FD */
    { 1, 8, { "NS   D" } },   /* FE */
    { 1, 8, { "DC   H'FF' (?)" } },   /* FF */
};
