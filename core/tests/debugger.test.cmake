# The debugger engine's tests. They drive the host (core/chf/host.h) directly,
# on the no-BIOS stand-in running the baked CONFIG, so they need neither a
# session nor SDL nor a ROM; they link chf_core only.

# The FujiNet trace on byte streams built in the test: both directions, split
# frames, a send dropping a cut-short reply, repeats and repeated exchanges
# counted as one, the ring's limit and Clear.
chf_test(fujitrace_test fujitrace_test.c)
add_test(NAME fujitrace COMMAND fujitrace_test)

# The debugger contract: attach stops the machine (before the session starts,
# too), registers and their edits, every step kind, disassembly, breakpoints
# of every class with conditions, memory in the four spaces, the VDC views,
# objects and hit test, I/O, the cartridge, symbols in every format, the
# trace, Save, the prompt and its completion, the FujiNet trace over a
# loopback stand-in FujiNet, detach, and a power cycle with the debugger
# attached.
chf_test(debug_test debug_test.c)
add_test(NAME debug COMMAND debug_test)
set_tests_properties(debug PROPERTIES TIMEOUT 120)
