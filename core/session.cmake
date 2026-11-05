# chf_session: the frontend contract (core/include/chfsession.h), included
# from core/CMakeLists.txt once SDL3 is found.

set(CHF_SESSION_SOURCES
  src/session.c
  src/settings.c
  src/paths.c
  src/media.c
  src/zipread.c
  src/keyboard.c
  src/fujinet_runtime.c
  src/audio_sdl.c
  src/bindings.c
  src/hid_keys.c
  src/gamepad_sdl.c
  src/debug.c)

add_library(chf_session STATIC ${CHF_SESSION_SOURCES})
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
