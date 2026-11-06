# The F8 core's tests (included by core/tests/CMakeLists.txt). Each compiles
# core/chf/f8.c and dasmf8.c itself rather than linking chf_core, so they build
# and run whatever state the rest of core/chf is in.
#
#   dasmf8        the disassembler against MAME's own (dasmf8_golden.h, from
#                 f8dasm.cpp compiled standalone: tools/ab/gen_dasmf8_golden.sh):
#                 all 256 opcodes' text, length and step flags at sixteen pc /
#                 operand triples, branch targets; and against the core (the
#                 same fourteen illegal opcodes, every fall-through advancing the
#                 PC by the printed length, jumps landing on their target).
#   cpu_ops       the core against MAME's CPU, instruction for instruction: the
#                 probe cartridges of tools/ab/f8_probes.py (all 256 opcodes,
#                 every ISAR mode, every branch condition, full AS / ASD / AMD
#                 sweeps, the data counters, the page-edge cases, the ports,
#                 fuzz; ~880k instructions) whose traces from MAME's channelf
#                 driver are committed as hashes in f8_golden.h (tools/ab/
#                 f8_ab.py --gen); needs neither MAME nor Python.
#   cpu_irq       what MAME's Channel F cannot exercise: the interrupt entry
#                 and the privileged instructions, reset and f8_set_pc, when
#                 each callback sees `cycles`, clocks per instruction class,
#                 illegal opcodes, unwired callbacks.
#   romc_trace    the core's romc_hook stream against the ROMC trace MAME's F8
#                 writes (F8_ROMC_TRACE), as hashes in romc_golden.h (tools/ab/
#                 romc_ab.py --gen).
#   romc_shadow   the firmware's cartridge decoder (chf_bus_cycle) driven by
#                 that stream: its shadow PC0/PC1/DC0/DC1 equal the CPU's after
#                 every state of every probe, and it drives the bus where the F8
#                 guide says it must.
#   cpu_probes_fresh
#                 the committed goldens (f8_golden.h, romc_golden.h) are what
#                 tools/ab/f8_probes.py generates now: a probe changed without
#                 regenerating them fails here. Needs Python only.
#   cpu_ops_mame, romc_mame, cpu_timing_mame
#                 the same probes run live in MAME and diffed whole against the
#                 core's traces; and the clock count at every I/O port access
#                 and cart RAM access (what the callbacks see as `cycles`)
#                 against MAME's. SKIP (77) without MAME ($MAME,
#                 ~/Workspace/mame/mame).

function(f8_test name)
  add_executable(${name} ${ARGN}
    "${CMAKE_SOURCE_DIR}/core/chf/f8.c" "${CMAKE_SOURCE_DIR}/core/chf/dasmf8.c")
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/chf"
    "${CMAKE_SOURCE_DIR}/core/chf/fuji-generated" "${CMAKE_CURRENT_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  target_compile_options(${name} PRIVATE "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>")
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

f8_test(dasmf8_test dasmf8_test.c)
add_test(NAME dasmf8 COMMAND dasmf8_test)

f8_test(f8_ops_test f8_ops_test.c)
target_compile_definitions(f8_ops_test PRIVATE F8_GOLDEN)
add_test(NAME cpu_ops COMMAND f8_ops_test)

f8_test(f8_irq_test f8_irq_test.c)
add_test(NAME cpu_irq COMMAND f8_irq_test)

f8_test(romc_trace_test romc_trace_test.c)
target_compile_definitions(romc_trace_test PRIVATE ROMC_GOLDEN)
add_test(NAME romc_trace COMMAND romc_trace_test)

f8_test(romc_shadow_test romc_shadow_test.c)
add_test(NAME romc_shadow COMMAND romc_shadow_test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  add_test(NAME cpu_probes_fresh
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/ab/f8_ab.py" --fresh)
  add_test(NAME cpu_ops_mame
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/ab/f8_ab.py"
            --check --exe $<TARGET_FILE:f8_ops_test>)
  add_test(NAME romc_mame
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/ab/romc_ab.py"
            --check --exe $<TARGET_FILE:romc_trace_test>)
  add_test(NAME cpu_timing_mame
    COMMAND ${Python3_EXECUTABLE} -I "${CMAKE_SOURCE_DIR}/tools/ab/f8_ab.py"
            --timing --exe $<TARGET_FILE:f8_ops_test>)
  set_tests_properties(cpu_ops_mame romc_mame cpu_timing_mame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900)
endif()
