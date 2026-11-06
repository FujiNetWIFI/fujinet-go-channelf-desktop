# core tests (see each file's header). Network and ROM-dependent tests exit
# 77, which ctest reports as skipped.

function(chf_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE chf_core)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    CHF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

# The core links and runs alone: no SDL, no session (a session symbol would
# fail the link).
chf_test(no_sdl_link_test no_sdl_link_test.c)
add_test(NAME no_sdl_link COMMAND no_sdl_link_test)

# Module tests live in <name>.test.cmake fragments beside their sources, each
# registering its own tests with chf_test() / add_test(); they are picked up
# here so a module can land with its tests without touching this file.
file(GLOB _chf_test_fragments "${CMAKE_CURRENT_SOURCE_DIR}/*.test.cmake")
list(SORT _chf_test_fragments)
if(CHF_CORE_ONLY)
  # the debugger's tests need the debugger, which this build leaves out
  list(FILTER _chf_test_fragments EXCLUDE REGEX "debugger\\.test\\.cmake$")
endif()
foreach(_frag IN LISTS _chf_test_fragments)
  include("${_frag}")
endforeach()

# These need the session layer, which needs SDL3. On a build without it
# (a cross-toolchain, or a lean machine) only the core libraries build.
if(TARGET chf_session)

function(chf_session_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE chf_session)
  target_include_directories(${name} PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
    "${CMAKE_SOURCE_DIR}/core/debugger")
  target_compile_definitions(${name} PRIVATE _GNU_SOURCE _DEFAULT_SOURCE
    CHF_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  set_target_properties(${name} PROPERTIES C_STANDARD 11)
  if(WIN32)
    target_link_options(${name} PRIVATE -static)
  endif()
endfunction()

# The whole app headless, like a user: a session with the in-process
# FujiNet, a script of presses and waits, screenshots. Built, not registered.
chf_session_test(chf_drive chf_drive.c)

# The frontend contract, against a real temporary XDG tree: settings persist,
# paths resolve, CONFIG boots on the stand-in, the hand controllers, the
# pointer, a wheel pulse and the console's buttons reach the machine, a
# FujiNet app opens and runs, RESET restarts it, Reset to CONFIG ejects it, a
# game is refused without the console's BIOS (and runs with MAME's, when it
# is on this machine), an image the console cannot take is refused with a
# reason, and the PAL clone runs at 50 Hz.
chf_session_test(session_test session_test.c)
add_test(NAME session COMMAND session_test)
set_tests_properties(session PROPERTIES TIMEOUT 180)

# The remappable binding table and the keyboard translator, pure: both hand
# controllers' eight contacts, the four console buttons, F1 for RESET,
# steal-on-rebind, persistence, names.
chf_session_test(bindings_test bindings_test.c)
add_test(NAME bindings COMMAND bindings_test)

# The on-screen controllers' layout for the Channel F, its hit test and the
# stick's gesture maths: no overlapping controls, every contact on exactly one
# control, wedges that agree with chfsession_stick_from_vector. No machine is
# started.
chf_session_test(controls_test controls_test.c)
add_test(NAME controls COMMAND controls_test)

# Where a dropped file goes (a BIOS ROM to the ROM directory, a cartridge to
# the cartridge directory, the rest to the SD folder), what makes a cartridge
# image acceptable, and Import Cartridge to SD with its refusals.
chf_session_test(media_test media_test.c)
add_test(NAME media COMMAND media_test)

# Import BIOS: the three ROMs by size and CRC-32 from a file, a zip or a
# folder, a 2K file split in two, an unknown dump never taken, which BIOS
# each machine runs. MAME's channelf.zip is imported too when it is on this
# machine.
chf_session_test(roms_import_test roms_import_test.c)
add_test(NAME roms_import COMMAND roms_import_test)

# The built-in Help covers what the app has: settings, machines, ROMs, keys,
# ports, the debugger's tabs and prompt commands, and every context a Help
# button opens.
chf_session_test(help_test help_test.c)
add_test(NAME help COMMAND help_test)

# The external-FujiNet address field's parser.
chf_session_test(hostport_test hostport_test.c)
add_test(NAME hostport COMMAND hostport_test)

# External FujiNet: the cartridge dials a stand-in listener and no built-in
# runtime is started. SKIPs (77) on Windows (POSIX sockets).
chf_session_test(external_test external_test.c)
add_test(NAME external COMMAND external_test)
set_tests_properties(external PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The gamepad layer's pure helpers, and that the SDL subsystem starts and
# stops with no hardware attached.
chf_session_test(gamepad_test gamepad_test.c)
add_test(NAME gamepad COMMAND gamepad_test)

# The gamepad layer end to end with an SDL3 virtual gamepad: hot-plug, the
# default mapping onto the hand controller (D-pad and left stick, shoulders,
# East, South, the right stick), buttons rebound to a console button and to
# RESET, the sources kept apart, unplug releasing what was held. SKIPs (77)
# where SDL has no virtual joysticks.
chf_session_test(gamepad_map_test gamepad_map_test.c)
add_test(NAME gamepad_map COMMAND gamepad_map_test)
set_tests_properties(gamepad_map PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

# The real in-process FujiNet: brings libfujinet up on its port, boots the
# CONFIG client and confirms the cartridge's link came up (and the debugger's
# FujiNet trace saw both ways), and comes up again after a power cycle. SKIPs
# (77) when the runtime was not built (-DWITH_FUJINET=OFF). Serial: it owns
# the port.
chf_session_test(fujibus_smoke fujibus_smoke.c)
add_test(NAME fujibus_smoke COMMAND fujibus_smoke)
set_tests_properties(fujibus_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RUN_SERIAL ON)

# These two need the FujiNet runtime and the firmware's own test clients.
if(TRUE)
# CONFIG, driven like a user, boots the suite's own app from the SD host
# through FujiNet (screens recognised by their glyph codes on the UVI's
# character screen). SKIPs (77) without the FujiNet runtime. Serial: it owns
# the port.
chf_session_test(config_boot_test config_boot_test.c)
add_test(NAME config_boot COMMAND config_boot_test)
set_tests_properties(config_boot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)

# A network boot end to end: the cartridge bring-up's fujitest client makes a
# real transaction and fujiboot mounts S2COUNTER.BIN (put there by Import
# Cartridge to SD) from the SD host, FujiNet pushes it, the cart stages and
# swaps it in, and the stand-in runs it; twice, with the mailbox on its worker
# thread and inline. Needs CHF_TESTROM_DIR (fujinet-firmware
# pico/channelf/build, built with BOOT_HOST=0 BOOT_PATH=/S2COUNTER.BIN); SKIPs
# without.
chf_session_test(netboot_test netboot_test.c)
add_test(NAME netboot COMMAND netboot_test)
set_tests_properties(netboot PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON)
add_test(NAME netboot_sync COMMAND netboot_test)
set_tests_properties(netboot_sync PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180 RUN_SERIAL ON
  ENVIRONMENT "CHF_CART_SYNC=1")

endif()
endif()
