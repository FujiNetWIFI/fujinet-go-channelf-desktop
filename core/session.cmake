# chf_session: the frontend contract (core/include/chfsession.h), included
# from core/CMakeLists.txt once SDL3 is found.

set(CHF_SESSION_SOURCES
  src/session.c
  src/settings.c
  src/paths.c
  src/media.c
  src/roms.c
  src/zipread.c
  src/controls.c
  src/input.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c
  src/gamepad_sdl.c
  src/debug.c
  src/help.c)

# The BIOS is copyrighted firmware and is never shipped. Declared here so a
# build that includes only core/ sees it too (the top level may declare it
# first: option() keeps the first value).
#   OFF (default) -- the shipping configuration: nothing embedded, the machine
#                    boots CONFIG and the FujiNet apps on its own stand-in, and
#                    core/tests/no_embedded_roms.py checks that claim against
#                    the shipped binaries.
#   ON            -- embed whatever recognised BIOS ROMs a developer put in
#                    tools/roms/local/ so a development build has them from
#                    the first start. NOT for redistribution.
# See COMPLIANCE.md.
option(WITH_CHF_ROMS "Embed the Channel F BIOS found in tools/roms/local (see COMPLIANCE.md)" OFF)

# The embedded ROM table: empty unless WITH_CHF_ROMS.
set(CHF_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
find_package(Python3 COMPONENTS Interpreter REQUIRED)
if(WITH_CHF_ROMS)
  file(GLOB CHF_ROM_INPUTS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/local/*")
  add_custom_command(
    OUTPUT "${CHF_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            "${CHF_ROMS_C}" "${CMAKE_SOURCE_DIR}/tools/roms/local"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py" ${CHF_ROM_INPUTS}
    COMMENT "Embedding the BIOS ROMs in tools/roms/local (WITH_CHF_ROMS=ON: NOT redistributable)"
    VERBATIM)
else()
  add_custom_command(
    OUTPUT "${CHF_ROMS_C}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
            --none "${CHF_ROMS_C}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
    VERBATIM)
endif()

# The Help, embedded: data/help/help.md as topics of styled spans
# (core/include/chfhelp.h), which every frontend lays out with its own
# toolkit. help_data.h declares the table; the build generates it.
set(CHF_HELP_C "${CMAKE_BINARY_DIR}/generated/help_content.c")
add_custom_command(
  OUTPUT "${CHF_HELP_C}"
  COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          --src "${CMAKE_SOURCE_DIR}/data/help/help.md"
          --out-c "${CHF_HELP_C}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/help/make-help.py"
          "${CMAKE_SOURCE_DIR}/data/help/help.md"
  COMMENT "Help"
  VERBATIM)

add_library(chf_session STATIC ${CHF_SESSION_SOURCES} "${CHF_ROMS_C}" "${CHF_HELP_C}")
set_target_properties(chf_session PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(chf_session PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(chf_session PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/debugger")
target_link_libraries(chf_session PUBLIC chf_core SDL3::SDL3)
# setenv()/getaddrinfo() under a strict -std=c11
target_compile_definitions(chf_session PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
set_property(SOURCE ${CHF_SESSION_SOURCES} APPEND PROPERTY COMPILE_OPTIONS
  "$<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra>"
  # paths are bounded by CHF_PATH_MAX and joined with snprintf on purpose;
  # GCC's truncation analysis flags every such join
  "$<$<C_COMPILER_ID:GNU>:-Wno-format-truncation>")

# Where an uninstalled build and an installed copy find the FujiNet runtime
# (core/src/paths.c probes these after the executable's own directory).
target_compile_definitions(chf_session PRIVATE
  CHF_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  CHF_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-channelf"
  CHF_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-channelf")
if(WIN32)
  target_link_libraries(chf_session PUBLIC ws2_32)
else()
  target_link_libraries(chf_session PUBLIC ${CMAKE_DL_LIBS})
endif()
