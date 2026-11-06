# The debugger engine's tests. They drive the host (core/chf/host.h) directly,
# on the tests' own BIOS and the app's stand-in, so they need neither a session
# nor SDL nor a ROM; they link chf_core only.

# The FujiNet trace on byte streams built in the test: both directions, split
# frames, a send dropping a cut-short reply, repeats and repeated exchanges
# counted as one, the ring's limit and Clear.
chf_test(fujitrace_test fujitrace_test.c)
add_test(NAME fujitrace COMMAND fujitrace_test)

# The debugger contract (core/include/chfdebug.h) on the host's machine with the
# tests' own BIOS and a FujiNet app written out in machine code: attach stops the
# machine (before it exists, too), registers and their edits (a PC change
# refetches), every step kind (over a PI or PK, out by POP), disassembly with
# labels and the APPBANK tag, breakpoints of every class (exec, read, write, IN,
# OUT, VRAM cells, scratchpad) with conditions, the five spaces, the video views,
# overlays, hit test, cell readout, write log and beam view against a known VRAM
# program, the port and sound logs, the cartridge struct with the FujiNet status
# page and tick, a Videocart of each mapper, symbols in every format, the trace,
# Save, the prompt and its completion, the FujiNet trace over a loopback
# stand-in FujiNet, detach, and a power cycle with the debugger attached. The
# K-stack services run on the stand-in BIOS, and on the console's own when
# CHF_BIOS_DIR holds it (sl31253.rom, sl31254.rom). Run it 70+ times, 6-12 in
# parallel, after a change: it races the machine thread by design.
chf_test(debug_test debug_test.c)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)

# The FujiNet trace the way the Cart tab reads it, through a whole session with an
# EXTERNAL FujiNet (the cartridge dials a stand-in on loopback, no built-in
# runtime): it fills from session start with no window open, survives an attach
# and detach, goes on filling after Reset to CONFIG (with the debugger attached,
# too) and a restart, and with the window open before the session starts it
# fills once the machine runs. CHF_TEST_EXTERNAL=host:port points the test at a
# real, private fujinet-pc instead of the stand-in. Needs the session layer (SDL3) and the stand-in BIOS to run
# CONFIG; SKIPs (77) otherwise.
if(TARGET chf_session)
  add_executable(debug_fujinet_test debug_fujinet_test.c)
  target_link_libraries(debug_fujinet_test PRIVATE chf_session)
  target_include_directories(debug_fujinet_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(debug_fujinet_test PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
  set_target_properties(debug_fujinet_test PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(debug_fujinet_test PRIVATE -static)
  endif()
  add_test(NAME debug_fujinet COMMAND debug_fujinet_test)
  set_tests_properties(debug_fujinet PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
