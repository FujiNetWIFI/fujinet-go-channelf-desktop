// f8_dasm_golden.cpp -- MAME's own F8 disassembler, driven over every opcode,
// to produce core/tests/dasmf8_golden.h (see gen_dasmf8_golden.sh).
//
// Compiles MAME's src/devices/cpu/f8/f8dasm.cpp standalone: it needs only
// util::disasm_interface and util::stream_format, so a short `emu.h` shim
// stands in for the whole emulator.
//
// SPDX-License-Identifier: BSD-3-Clause
// (drives MAME's f8dasm.cpp; copyright-holders Juergen Buchmueller -- see COMPLIANCE.md)

#include "emu.h"
#include "f8dasm.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace {

struct variant { unsigned pc, b1, b2; };

// pc, first and second operand byte. MAME prints a branch target as base_pc +
// (int8_t)offset + 1 in 32 bits, so every triple keeps it inside $0000-$FFFF
// (outside, MAME's text has five or eight digits and dasmf8 wraps; the test
// pins that separately).
const variant variants[] = {
	{ 0x0123, 0x5a, 0xc3 }, { 0x0802, 0x00, 0x00 }, { 0x1ffe, 0x7f, 0xff }, { 0x1fff, 0x80, 0x12 },
	{ 0x2010, 0xfe, 0xa5 }, { 0x3ffd, 0x01, 0x00 }, { 0x4000, 0xa1, 0x7f }, { 0x5fff, 0xc2, 0x80 },
	{ 0x6ffd, 0xe3, 0x34 }, { 0x7ffe, 0x7f, 0xff }, { 0x8000, 0xff, 0x01 }, { 0x0100, 0x40, 0x9c },
	{ 0x0abc, 0x15, 0x2a }, { 0xff00, 0x0f, 0x55 }, { 0x0800, 0xf0, 0x33 }, { 0x0777, 0x3f, 0xc0 },
};
const int NV = sizeof variants / sizeof variants[0];

struct bytes : util::disasm_interface::data_buffer
{
	util::disasm_interface::u8 b[0x10000] = {};
	util::disasm_interface::u8 r8(util::disasm_interface::offs_t pc) const override { return b[pc & 0xffff]; }
	util::disasm_interface::u16 r16(util::disasm_interface::offs_t pc) const override { return (b[pc & 0xffff] << 8) | b[(pc + 1) & 0xffff]; }
	util::disasm_interface::u32 r32(util::disasm_interface::offs_t) const override { return 0; }
	util::disasm_interface::u64 r64(util::disasm_interface::offs_t) const override { return 0; }
};

} // namespace

int main()
{
	f8_disassembler dasm;
	static bytes buf;

	std::printf("/* dasmf8_golden.h -- MAME's own disassembly of every F8 opcode, from\n"
	            " * src/devices/cpu/f8/f8dasm.cpp compiled standalone (tools/ab/\n"
	            " * f8_dasm_golden.cpp, run by tools/ab/gen_dasmf8_golden.sh). GENERATED;\n"
	            " * dasmf8_test.c holds dasmf8.c to it.\n"
	            " *\n"
	            " * Each opcode is disassembled at the NV pc/operand triples of\n"
	            " * dasmf8_variants; a one-byte opcode has one text, the others NV.\n"
	            " * flags is MAME's result >> 28: 8 = SUPPORTED, 4 = STEP_OUT, 2 = STEP_OVER,\n"
	            " * 1 = STEP_COND. */\n");
	std::printf("#define DASMF8_NV %d\n", NV);
	std::printf("static const struct { uint16_t pc; uint8_t b1, b2; } dasmf8_variants[DASMF8_NV] = {\n");
	for (const variant &v : variants)
		std::printf("    { 0x%04X, 0x%02X, 0x%02X },\n", v.pc, v.b1, v.b2);
	std::printf("};\n");
	std::printf("static const struct { uint8_t len; uint8_t flags; const char *text[DASMF8_NV]; } dasmf8_golden[256] = {\n");

	for (unsigned op = 0; op < 256; op++)
	{
		std::string text[NV];
		unsigned len = 0, flags = 0;
		for (int vi = 0; vi < NV; vi++)
		{
			const variant &v = variants[vi];
			for (unsigned i = 0; i < 0x10000; i++) buf.b[i] = 0;
			buf.b[v.pc] = op;
			buf.b[(v.pc + 1) & 0xffff] = v.b1;
			buf.b[(v.pc + 2) & 0xffff] = v.b2;
			std::ostringstream os;
			util::disasm_interface::offs_t r = dasm.disassemble(os, v.pc, buf, buf);
			len = r & util::disasm_interface::LENGTHMASK;
			flags = (r & ~util::disasm_interface::LENGTHMASK) >> 28;
			text[vi] = os.str();
		}
		std::printf("    { %u, %u, { \"%s\"", len, flags, text[0].c_str());
		if (len > 1)
			for (int vi = 1; vi < NV; vi++)
				std::printf(", \"%s\"", text[vi].c_str());
		std::printf(" } },   /* %02X */\n", op);
	}
	std::printf("};\n");
	return 0;
}
