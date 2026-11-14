/* memtools: search parsing, the space-aware range parser, the watch list, trace
 * filters, breakpoint text and region names (no ROMs needed). */
#include <stdio.h>
#include <string.h>

#include "chfmemtools.h"
#include "debugger_internal.h"

static int fails;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void)
{
    uint8_t pat[CHFSEARCH_MAX], mask;
    int count, flags;
    /* JMP $0A40 (29 0A 40), BR7 forward, LR P0,Q, a plain LR A,KU */
    chfdebug_trace jmp = { .pc = 0x0802, .len = 3, .bytes = { 0x29, 0x0A, 0x40 }, .page = -1 };
    chfdebug_trace pi = { .pc = 0x083D, .len = 3, .bytes = { 0x28, 0x09, 0x00 }, .page = -1 };
    chfdebug_trace pop = { .pc = 0x0910, .len = 1, .bytes = { 0x1C }, .page = -1 };
    chfdebug_trace lrpq = { .pc = 0x0A10, .len = 1, .bytes = { 0x0D }, .page = -1 };
    chfdebug_trace lr = { .pc = 0x0500, .len = 1, .bytes = { 0x00 }, .page = -1 };
    char buf[160];
    chfdebug *d = chfdebug_create();
    uint32_t low, high;

    CHECK(d != NULL, "an engine without a machine");
    CHECK(chfsearch_parse(CHFSEARCH_HEX, "5A 3c,$FF 0x10", pat, &count, &mask, &flags), "hex parses");
    CHECK(count == 4 && pat[0] == 0x5A && pat[1] == 0x3C && pat[2] == 0xFF && pat[3] == 0x10, "hex bytes");
    CHECK(mask == 0xFF && flags == 0, "hex mask/flags");
    CHECK(chfsearch_parse(CHFSEARCH_HEX, "5", pat, &count, &mask, &flags) && count == 1 && pat[0] == 5,
          "lone digit");
    CHECK(!chfsearch_parse(CHFSEARCH_HEX, "zz", pat, &count, &mask, &flags), "bad hex");
    CHECK(!chfsearch_parse(CHFSEARCH_HEX, "  ", pat, &count, &mask, &flags), "empty hex");
    CHECK(chfsearch_parse(CHFSEARCH_TEXT, "Ab", pat, &count, &mask, &flags) && count == 2 &&
          pat[0] == 'A' && (flags & CHFDEBUG_SEARCH_NOCASE), "text");

    /* ranges, by space */
    CHECK(chfsym_parse_range(d, CHF_SPACE_PROG, "0810-0812", &low, &high) && low == 0x810 && high == 0x812, "prog range");
    CHECK(chfsym_parse_range(d, CHF_SPACE_PROG, "$812..$810", &low, &high) && low == 0x810 && high == 0x812, "swapped");
    CHECK(chfsym_parse_range(d, CHF_SPACE_PROG, "CART_ENTRY", &low, &high) && low == 0x802 && high == 0x802, "a label");
    CHECK(chfsym_parse_range(d, CHF_SPACE_PROG, "CART_ENTRY+3", &low, &high) && low == 0x805, "label + n");
    CHECK(!chfsym_parse_range(d, CHF_SPACE_PROG, "nonesuch", &low, &high), "an unknown name");
    CHECK(chfsym_parse_range(d, CHF_SPACE_SCRATCH, "r12-r15", &low, &high) && low == 12 && high == 15, "scratch r12-r15");
    CHECK(chfsym_parse_range(d, CHF_SPACE_SCRATCH, "59", &low, &high) && low == 59 && high == 59, "a bare scratch number is decimal");
    CHECK(chfsym_parse_range(d, CHF_SPACE_SCRATCH, "k", &low, &high) && low == 12, "scratch k = r12");
    CHECK(chfsym_parse_range(d, CHF_SPACE_SCRATCH, "$3B", &low, &high) && low == 59, "scratch $3B");
    CHECK(!chfsym_parse_range(d, CHF_SPACE_SCRATCH, "r64", &low, &high) && !chfsym_parse_range(d, CHF_SPACE_SCRATCH, "64", &low, &high),
          "scratch stops at r63");
    CHECK(chfsym_parse_range(d, CHF_SPACE_VRAM, "10,20", &low, &high) && low == 10 * 128 + 20, "vram row,col");
    CHECK(chfsym_parse_range(d, CHF_SPACE_VRAM, "10,20-12,30", &low, &high) && low == 10 * 128 + 20 && high == 12 * 128 + 30,
          "vram row,col range");
    CHECK(chfsym_parse_range(d, CHF_SPACE_VRAM, "1FFF", &low, &high) && low == 0x1FFF, "vram last cell");
    CHECK(!chfsym_parse_range(d, CHF_SPACE_VRAM, "2000", &low, &high) && !chfsym_parse_range(d, CHF_SPACE_VRAM, "64,0", &low, &high),
          "vram stops at row 63");
    CHECK(chfsym_parse_range(d, CHF_SPACE_IO, "ROW_SOUND", &low, &high) && low == 5, "a port name");
    CHECK(chfsym_parse_range(d, CHF_SPACE_IO, "20-21", &low, &high) && low == 0x20 && high == 0x21, "io range, hex");
    CHECK(!chfsym_parse_range(d, CHF_SPACE_IO, "100", &low, &high), "io stops at 255");
    CHECK(chfsym_parse_range(d, CHF_SPACE_CART, "10000-107FF", &low, &high) && low == 0x10000 && high == 0x107FF, "cart arena");

    chfwatch_clear();
    CHECK(chfwatch_add(CHF_SPACE_PROG, "  $0810 ") == 0 && chfwatch_add(CHF_SPACE_SCRATCH, "r12") == 1, "add");
    CHECK(chfwatch_add(CHF_SPACE_PROG, "   ") == -1 && chfwatch_add(9, "x") == -1, "empty or bad-space add refused");
    CHECK(chfwatch_count() == 2 && !strcmp(chfwatch_text(0), "$0810"), "text trimmed");
    CHECK(chfwatch_space(0) == CHF_SPACE_PROG && chfwatch_space(1) == CHF_SPACE_SCRATCH, "spaces kept");
    CHECK(chfwatch_range(d, 1, &low, &high) && low == 12 && high == 12, "watch range in its space");
    chfwatch_format(d, 1, buf, sizeof buf);
    CHECK(!strncmp(buf, "r12", 3) && strstr(buf, "SCRATCH r12"), "scratch watch text");
    {
        unsigned gen = chfwatch_generation();
        chfwatch_remove(0);
        CHECK(chfwatch_count() == 1 && !strcmp(chfwatch_text(0), "r12") && chfwatch_space(0) == CHF_SPACE_SCRATCH, "remove");
        CHECK(chfwatch_generation() != gen, "generation bumps");
    }
    chfwatch_clear();
    CHECK(chfwatch_count() == 0, "clear");

    CHECK(chfdebug_trace_match(&jmp, CHFDEBUG_TRACE_ALL, 0, 0), "all");
    CHECK(chfdebug_trace_match(&jmp, CHFDEBUG_TRACE_BRANCHES, 0, 0), "JMP is a branch");
    CHECK(chfdebug_trace_match(&pi, CHFDEBUG_TRACE_BRANCHES, 0, 0), "PI is a call");
    CHECK(chfdebug_trace_match(&pop, CHFDEBUG_TRACE_BRANCHES, 0, 0), "POP is a return");
    CHECK(chfdebug_trace_match(&lrpq, CHFDEBUG_TRACE_BRANCHES, 0, 0), "LR P0,Q jumps");
    CHECK(!chfdebug_trace_match(&lr, CHFDEBUG_TRACE_BRANCHES, 0, 0), "LR A,KU is not");
    CHECK(chfdebug_trace_match(&jmp, CHFDEBUG_TRACE_RANGE, 0x0800, 0x08FF), "in range");
    CHECK(!chfdebug_trace_match(&lr, CHFDEBUG_TRACE_RANGE, 0x0800, 0x08FF), "out of range");
    CHECK(chfdebug_trace_match(&jmp, CHFDEBUG_TRACE_RANGE, 0x08FF, 0x0800), "range swaps");
    chfdebug_trace_format(&jmp, buf, sizeof buf);
    CHECK(!strncmp(buf, "0802  JMP", 9) && strstr(buf, "0A40"), "trace format");

    {
        chfdebug_breakpoint bp;
        memset(&bp, 0, sizeof bp);
        bp.type = CHFDEBUG_BP_WRITE; bp.start = 0x8000; bp.end = 0x80FF;
        snprintf(bp.condition, sizeof bp.condition, "value != old");
        chfbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Write  $8000-$80FF  if value != old"), "write bp description");
        bp.type = CHFDEBUG_BP_ACCESS; bp.start = bp.end = 0x8010; bp.condition[0] = 0;
        chfbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Access  $8010"), "access bp description");
        bp.type = CHFDEBUG_BP_OUT; bp.start = bp.end = 5;
        chfbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Out  5"), "port bp description");
        bp.type = CHFDEBUG_BP_EXEC | CHFDEBUG_BP_WRITE; bp.start = bp.end = 0x0810;
        chfbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Exec/Write  $0810"), "memory bp description");
        bp.type = CHFDEBUG_BP_VRAM; bp.start = 10 * 128 + 20; bp.end = 12 * 128 + 40;
        chfbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "VRAM  row 10 col 20 to row 12 col 40"), "vram bp description");
        bp.type = CHFDEBUG_BP_SCRATCH; bp.start = 12; bp.end = 15;
        chfbp_describe(NULL, &bp, buf, sizeof buf);
        CHECK(!strcmp(buf, "Scratch  r12-r15"), "scratch bp description");
    }
    {
        static const chfdebug_region map[] = {
            { 0x0000, 0x07FF, 1, 0, "BIOS stand-in" }, { 0x0800, 0x0FFF, 1, 0, "Cartridge ROM" },
        };
        CHECK(chfmem_region_name(map, 2, 0x0820, buf, sizeof buf) > 0 && !strcmp(buf, "Cartridge ROM"),
              "region name");
        CHECK(chfmem_region_name(map, 2, 0x0900 + 0x1000, buf, sizeof buf) == 0 && !buf[0], "no region");
    }
    CHECK(!strcmp(chfmem_space_name(CHF_SPACE_VRAM), "VRAM") && !strcmp(chfmem_space_name(9), "?"), "space names");

    chfdebug_free(d);
    printf(fails ? "memtools_test: %d failed\n" : "memtools_test: ok\n", fails);
    return fails != 0;
}
