# The machine's tests and tools. chf_headless is the side of the MAME A/B runs
# that is ours (tools/ab/): built, not registered.
chf_test(chf_headless chf_headless.c)

# The 2637 UVI on its own: characters, colour bits, multicolour, UDCs, 8x16
# rows, refresh mode, scroll, block graphics, objects, collisions, SENSE, the
# pots, and the debugger's views, pixel by pixel.
chf_test(uvi_test uvi_test.c)
add_test(NAME uvi COMMAND uvi_test)

# The whole machine, no host thread: the memory map, SENSE, the line timer, the
# keypads and the fire wire, the pots, the beeper, RESET.
chf_test(machine_test machine_test.c)
add_test(NAME machine COMMAND machine_test)

# CONFIG on the FujiNet cartridge against frame hashes made by MAME
# (tools/ab/config_golden.py): no ROM needed, frame for frame from frame 0.
chf_test(config_frames_test config_frames_test.c)
add_test(NAME config_frames COMMAND config_frames_test)

# The mappers, and the FujiNet cartridge and its transport against a stand-in
# FujiNet on loopback: a MOUNT_IMAGE with a pushed image, the staging, the swap
# at $2DFE, the connector decode and the trace tap held to the bytes on the
# wire. The network half is POSIX (77).
chf_test(cart_test cart_test.c)
add_test(NAME cart COMMAND cart_test)
set_tests_properties(cart PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The paced host: CONFIG paints, the machine keeps its own frame rate.
chf_test(boot_smoke boot_smoke.c)
add_test(NAME boot_smoke COMMAND boot_smoke)
set_tests_properties(boot_smoke PROPERTIES TIMEOUT 60)
