# The 2650 core's tests (included by core/tests/CMakeLists.txt, which defines
# chf_test()). They link chf_core, which must compile core/chf/s2650.c and
# dasm2650.c.
#
#   dasm2650      the disassembler against MAME's own (dasm2650_golden.h, from
#                 2650dasm.cpp compiled standalone: tools/ab/
#                 gen_dasm2650_golden.sh): all 256 opcodes' text, length and
#                 step flags at sixteen pc / operand triples, branch targets,
#                 and the disassembler against the core (the same ten illegal
#                 opcodes, every straight-line instruction advancing the PC by
#                 the printed length).
#   cpu_ops       the core against MAME's CPU, instruction for instruction:
#                 twelve probe cartridges (tools/ab/s2650_probes.py: every
#                 legal opcode but HALT, BCD, flags, every addressing mode and
#                 page-edge wrap, the return stack, the register banks, the
#                 SENSE pin, ~360k instructions) whose traces from MAME's
#                 channelf driver are committed as hashes in s2650_golden.h
#                 (tools/ab/s2650_ab.py --gen); needs neither MAME nor Python.
#   cpu_irq       what MAME's Channel F cannot exercise: interrupts, HALT, the
#                 FLAG and SENSE pins, the stack wrap, the core's own API.
#   cpu_ops_mame  the same probes run live in MAME, the whole traces diffed
#                 against the core's. SKIPs (77) without MAME ($MAME,
#                 ~/Workspace/mame/mame).

chf_test(dasm2650_test dasm2650_test.c)
add_test(NAME dasm2650 COMMAND dasm2650_test)

chf_test(s2650_ops_test s2650_ops_test.c)
target_compile_definitions(s2650_ops_test PRIVATE S2650_GOLDEN)
add_test(NAME cpu_ops COMMAND s2650_ops_test)

chf_test(s2650_irq_test s2650_irq_test.c)
add_test(NAME cpu_irq COMMAND s2650_irq_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME cpu_ops_mame
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/ab/s2650_ab.py"
            --check --exe $<TARGET_FILE:s2650_ops_test>)
  set_tests_properties(cpu_ops_mame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
