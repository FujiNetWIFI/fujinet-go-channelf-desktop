# chf_core: the toolkit-agnostic, SDL-free Fairchild Channel F core.
#
#   chf/                the emulator, MAME's Channel F driver transposed to C:
#                      f8 and dasmf8 (the Fairchild F8 / F3850 CPU, ROMC state
#                      by ROMC state, and its disassembler), video (the 128 x 64
#                      VRAM, the four port latches, the per-row palettes and the
#                      picture), sound (the DDS tone), carts (the Videocart
#                      mappers: standard, Maze and Hangman with their 2102 RAM,
#                      Schach and the multicarts; the type of a plain image from
#                      the hashes in the generated carts_table.h), machine (the
#                      memory map, the ports, the panel and controllers, the
#                      vertical blank, the slot), stub (the app's own stand-in
#                      for the console's BIOS) and the paced host thread. There
#                      is no BIOS or ROM to ship: CONFIG and the FujiNet apps run
#                      on the stand-in, and the user's own BIOS, imported, runs
#                      the Videocarts.
#
#   chf/fuji-generated/ the cartridge firmware's OWN sources -- fujimail.c
#                      (the mailbox), chfmap.c (the image planner), fujibus.c
#                      (the SLIP + FujiBus wire codec), channelf_cart.h (the bus
#                      decode, chf_read / chf_write), fuji_mailbox.h and the
#                      baked CONFIG client -- staged by
#                      cmake/StageFujiProto.cmake. Compiling these rather than
#                      reimplementing them is what makes the emulated cartridge
#                      and the real RP2040 one identical by construction.
#
#   debugger/          the debugger engine (core/include/chfdebug.h).
#
# The one port-specific cartridge file is chf/fujinet_cart.c, transposed from
# the firmware's MAME device (pico/channelf/emu/fujinet.cpp). The transport is
# chf/fujitcp_host.c, this app's own portable twin of the staged fujitcp.c.
#
# Nothing here links SDL. The session layer (SDL3 audio + gamepad) is a
# separate target so that stays true; no_sdl_link guards it.

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/chf/fuji-generated")

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

if(NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h")
  message(FATAL_ERROR
    "Staged FujiNet sources are missing. cmake/StageFujiProto.cmake should "
    "have produced them; re-run with -DFUJI_RESTAGE=ON.")
endif()

# The staged cartridge sources (the staged fujitcp.c is not compiled; see
# chf/fujitcp_host.c).
set(CHF_FUJI_SOURCES
  ${FUJI_GEN}/fujimail.c
  ${FUJI_GEN}/fujibus.c
  ${FUJI_GEN}/chfmap.c)

# The emulator's own modules. A module that has not landed yet is skipped, so
# the library builds at every step of the bring-up. Until stub.c (the stand-in
# BIOS) exists, stub_placeholder.c supplies an image that only parks the machine.
set(CHF_EMU_SOURCES)
foreach(_f f8 dasmf8 video sound carts machine stub fujinet_cart fujitcp_host host)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chf/${_f}.c")
    list(APPEND CHF_EMU_SOURCES chf/${_f}.c)
  endif()
endforeach()
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chf/stub.c")
  list(APPEND CHF_EMU_SOURCES chf/stub_placeholder.c)
endif()

# The debugger drives the machine, so it joins once the machine and the
# debugger's own Channel F port (vidview.c is its marker) exist.
option(CHF_CORE_ONLY "Build only the emulator and its machine-level tests (no debugger, no session)" OFF)
set(CHF_DEBUGGER_SOURCES)
if(NOT CHF_CORE_ONLY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chf/machine.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/vidview.c")
  foreach(_f debugger.c expr.c fujitrace.c prompt.c symbols.c symbols_builtin.c vidview.c)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/debugger/${_f}")
      list(APPEND CHF_DEBUGGER_SOURCES debugger/${_f})
    endif()
  endforeach()
endif()

add_library(chf_core STATIC
  ${CHF_EMU_SOURCES} ${CHF_FUJI_SOURCES} ${CHF_DEBUGGER_SOURCES})
# Development only: a stand-in for the debugger engine while it is written,
# so the session and the frontends link. Never set when debugger/ builds.
set(CHF_DEBUG_SHIM "" CACHE FILEPATH "A debugger stand-in source (development only)")
if(CHF_DEBUG_SHIM AND NOT CHF_DEBUGGER_SOURCES)
  target_sources(chf_core PRIVATE "${CHF_DEBUG_SHIM}")
  target_include_directories(chf_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/debugger")
endif()
set_target_properties(chf_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)

target_include_directories(chf_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/chf"
  # public because the debugger and the tests read the cart's state through
  # the staged fuji_mailbox.h and chfmap.h
  "${FUJI_GEN}")
target_include_directories(chf_core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/debugger")

# getaddrinfo/clock_gettime/nanosleep under a strict -std=c11.
target_compile_definitions(chf_core PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)

target_link_libraries(chf_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(chf_core PUBLIC ws2_32)
else()
  target_link_libraries(chf_core PUBLIC m)
endif()

# Our own modules are warning-clean; the staged tree is built with whatever
# warnings upstream is comfortable with.
set(CHF_WARN_SOURCES ${CHF_EMU_SOURCES} ${CHF_DEBUGGER_SOURCES})
list(REMOVE_ITEM CHF_WARN_SOURCES debugger/symbols_builtin.c)
set_property(SOURCE ${CHF_WARN_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>")

# ---- chf_session: the frontend contract ----------------------------------
#
# A separate target from chf_core so the core stays SDL-free: audio_sdl.c and
# gamepad_sdl.c live here. Found-or-skipped, exactly as the frontends are:
# `-DFRONTEND=none` on a machine (or a cross-toolchain) with no SDL3 must
# still configure and build the emulator and its tests. Not built until the
# machine and the host it drives exist.
if(NOT CHF_CORE_ONLY AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/session.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chf/host.c"
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chf/machine.c")
  find_package(SDL3 CONFIG QUIET)
  if(NOT SDL3_FOUND)
    message(STATUS "chf_session: SDL3 not found -- building the core and its "
                   "tests only (no audio, no gamepads, no frontends)")
  else()
    include(session.cmake)
  endif()
endif()

add_subdirectory(tests)
