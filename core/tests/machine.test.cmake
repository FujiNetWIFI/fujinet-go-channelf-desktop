# The machine's tests and tools. chf_headless is the side of the MAME A/B runs
# that is ours (tools/ab/): built, not registered.
chf_test(chf_headless chf_headless.c)

# The VRAM, its four ports and the picture: the register complements, the
# level-triggered strobe, the read-back and the controller gate, all sixteen
# palette-column combinations, the frame's geometry. No machine.
chf_test(video_test video_test.c)
add_test(NAME video COMMAND video_test)

# The tone against MAME's own recording of the 1 kHz mode (the first samples and
# a hash of 3000), the mode-0 freeze, retriggering.
chf_test(sound_test sound_test.c)
add_test(NAME sound COMMAND sound_test)

# The whole console, no host thread and no ROM, on the tests' own BIOS: the
# memory map, ports, pads, strobe, the vertical blank's exact timing (NTSC and
# PAL), the tone, RESET, the stand-in's rule, the FujiNet cartridge's tick and
# the debugger's spaces and bookkeeping.
chf_test(machine_test machine_test.c)
add_test(NAME machine COMMAND machine_test)

# The mappers (standard, Maze and Hangman's 2102, Schach, the multicarts, the
# software list's table, RESET), and the FujiNet cartridge and its transport
# against a stand-in FujiNet on loopback: a MOUNT_IMAGE with a pushed image, the
# staging, the swap at $FDFE, APPBANK pages, the connector decode and the trace
# tap held to the bytes on the wire. The network half is POSIX (77).
chf_test(cart_test cart_test.c)
add_test(NAME cart COMMAND cart_test)
set_tests_properties(cart PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# CONFIG on the FujiNet cartridge against frame hashes made by MAME
# (tools/ab/config_golden.py), frame for frame from frame 0, NTSC and PAL: with
# the app's stand-in BIOS (no ROM: the CI's half) and, when CHF_BIOS_DIR has the
# user's BIOS, with the real one. SKIPs (77) with neither.
chf_test(config_frames_test config_frames_test.c)
add_test(NAME config_frames COMMAND config_frames_test)
set_tests_properties(config_frames PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

# The paced host: a Videocart paints, the machine keeps its own frame rate (NTSC
# and PAL), and a stand-in-only console parks on a Videocart.
chf_test(boot_smoke boot_smoke.c)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES TIMEOUT 60)
