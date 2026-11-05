# Provide the fujinet-firmware checkout (see cmake/Dependencies.cmake) and
# stage the FujiNet Channel F cartridge's own sources into
# core/chf/fuji-generated, which is what core/CMakeLists.txt compiles into
# the emulator core.
#
# These are the cartridge firmware's OWN sources, from two roots:
#
#   pico/channelf/firmware   chfmap.c / chfmap.h (the image planner: the 16K
#                      window and up to six 8K APPBANK pages, the FUJI claim),
#                      channelf_cart.h (the bus decode: chf_read / chf_write,
#                      the RAM, the hotspots, the page window, and the ROMC
#                      state machine the hardware runs), fuji_mailbox.h (the
#                      register and status-page layout) and
#                      baked/fujiconfigrom.h (the baked CONFIG)
#   pico/common        fujimail.c / fujimail.h (the mailbox service: the
#                      REGSEL/REGDATA interlock, ACKSEQ published last, the
#                      DBC push receiver), fujibus.c / fujibus.h (SLIP +
#                      FujiBus wire codec), fuji_mailbox_proto.h /
#                      fuji_mailbox_check.h (the layout every cart shares) and
#                      the emulator-side SLIP-over-TCP transport (fujitcp.h)
#
# The firmware's pico/channelf/emu/apply.sh grafts the identical set into a
# MAME tree; this file is that graft for this repository. Emulator and
# cartridge stay identical by construction, not by discipline: nothing here
# is patched, only copied. The one port-specific file, the cart device
# itself, is this repository's own core/chf/fujinet_cart.c (transposed from
# the firmware's MAME device, pico/channelf/emu/fujinet.cpp).
#
# apply.sh additionally generates a fujins.h that prefixes every shared
# symbol, because several FujiNet carts share one MAME binary; it also renames
# every .c to .cpp for MAME's precompiled header. Only one FujiNet cart is
# ever linked here and the sources are C, so neither is reproduced. Do not
# add them "for symmetry".
#
# The staged fujitcp.c is POSIX sockets only and is not compiled: every build
# uses core/chf/fujitcp_host.c, this app's portable, abortable twin, against
# the SAME staged fujitcp.h. The staged .c is still copied so the staging
# result does not depend on the host.
#
# Staging is automatic: it runs when the staged tree is missing, when the pin
# has moved, when this file changes, or on demand with -DFUJI_RESTAGE=ON
# (which is also how to pick up uncommitted edits in a working checkout
# pointed at by FUJINET_SRC).

set(FUJI_GEN "${CMAKE_SOURCE_DIR}/core/chf/fuji-generated")

option(FUJI_RESTAGE "Re-stage the FujiNet protocol sources from the firmware checkout" OFF)

find_package(Python3 COMPONENTS Interpreter REQUIRED)

chf_provide_dependency(
  NAME fujinet-firmware
  PATH third_party/fujinet-firmware
  URL "${FUJINET_URL}"
  COMMIT "${FUJINET_COMMIT}"
  SENTINEL build.sh
  OVERRIDE FUJINET_SRC
  RESULT FUJINET_DIR)

set(FUJI_PROTO_DIR "${FUJINET_DIR}/pico")
if(NOT EXISTS "${FUJI_PROTO_DIR}/channelf/firmware/include/fuji_mailbox.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/channelf/firmware/include/channelf_cart.h"
   OR NOT EXISTS "${FUJI_PROTO_DIR}/channelf/firmware/baked/fujiconfigrom.h")
  message(FATAL_ERROR
    "${FUJINET_DIR} has no pico/channelf/firmware/{include/fuji_mailbox.h, "
    "include/channelf_cart.h, baked/fujiconfigrom.h} -- see cmake/Dependencies.cmake.")
endif()
# Protocol 2 (APPBANK pages and the tick, #1988) is what this app's cart serves.
file(READ "${FUJI_PROTO_DIR}/channelf/firmware/include/fuji_mailbox.h" _fuji_mbox)
file(READ "${FUJI_PROTO_DIR}/channelf/firmware/include/channelf_cart.h" _fuji_cart)
if(NOT _fuji_mbox MATCHES "FN_PROTO_VER[ \t]+2" OR NOT _fuji_cart MATCHES "CHF_EV_BANK")
  message(FATAL_ERROR
    "${FUJINET_DIR}'s Channel F cart is not protocol 2 (FN_PROTO_VER 2, CHF_EV_BANK); "
    "see cmake/Dependencies.cmake.")
endif()

# The exact file list apply.sh grafts into MAME, relative to pico/, plus the
# baked CONFIG (the cartridge firmware bakes the same bytes in).
set(FUJI_STAGE_FILES
  common/src/fujimail.c
  channelf/firmware/src/chfmap.c
  common/include/fujimail.h
  channelf/firmware/include/chfmap.h
  channelf/firmware/include/channelf_cart.h
  channelf/firmware/include/fuji_mailbox.h
  common/include/fuji_mailbox_proto.h
  common/include/fuji_mailbox_check.h
  channelf/firmware/baked/fujiconfigrom.h
  common/src/fujibus.c
  common/include/fujibus.h
  common/emu/fujitcp.c
  common/emu/fujitcp.h)

# What the staged tree was made from: the source identity plus a hash of this
# file (the only transform), so that editing the stage re-stages rather than
# silently leaving the old result in place.
set(_fuji_head "")
if(GIT_EXECUTABLE)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} -C "${FUJINET_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE _fuji_head OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
endif()
if(NOT _fuji_head)
  set(_fuji_head "${FUJINET_COMMIT} ${FUJINET_DIR}")
endif()
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" _fuji_stage_hash)
set(_fuji_want "${_fuji_head} ${_fuji_stage_hash}")

set(_fuji_staged "")
if(EXISTS "${FUJI_GEN}/.source-info")
  file(READ "${FUJI_GEN}/.source-info" _fuji_staged)
  string(STRIP "${_fuji_staged}" _fuji_staged)
endif()

if(FUJI_RESTAGE OR NOT EXISTS "${FUJI_GEN}/fuji_mailbox.h"
   OR NOT _fuji_staged STREQUAL _fuji_want)
  message(STATUS "Staging FujiNet protocol sources from ${FUJI_PROTO_DIR}")
  file(REMOVE_RECURSE "${FUJI_GEN}")
  file(MAKE_DIRECTORY "${FUJI_GEN}")

  foreach(_f IN LISTS FUJI_STAGE_FILES)
    if(NOT EXISTS "${FUJI_PROTO_DIR}/${_f}")
      file(REMOVE_RECURSE "${FUJI_GEN}")
      message(FATAL_ERROR
        "FujiNet protocol staging failed: ${FUJI_PROTO_DIR}/${_f} is missing "
        "(pin drift? see cmake/Dependencies.cmake).")
    endif()
    get_filename_component(_base "${_f}" NAME)
    file(COPY_FILE "${FUJI_PROTO_DIR}/${_f}" "${FUJI_GEN}/${_base}")
  endforeach()

  file(WRITE "${FUJI_GEN}/.source-info" "${_fuji_want}\n")
endif()

message(STATUS "FujiNet protocol sources staged from ${FUJI_PROTO_DIR}")
