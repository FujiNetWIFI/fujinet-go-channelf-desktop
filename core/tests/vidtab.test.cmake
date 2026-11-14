# The Video tab's controller (core/debugger/vidtab.c, chfvidtab.h): on a stopped
# machine with a VRAM written through the debugger's own spaces, and on the
# CONFIG cartridge through a whole session. They need the debugger, which a
# CHF_CORE_ONLY build leaves out.
if(NOT CHF_CORE_ONLY)
  chf_test(vidtab_test vidtab_test.c)
  add_test(NAME vidtab COMMAND vidtab_test)
  set_tests_properties(vidtab PROPERTIES TIMEOUT 120)

  if(TARGET chf_session)
    add_executable(vidtab_live_test vidtab_live_test.c)
    target_link_libraries(vidtab_live_test PRIVATE chf_session)
    target_include_directories(vidtab_live_test PRIVATE "${CMAKE_SOURCE_DIR}/core/src"
      "${CMAKE_SOURCE_DIR}/core/debugger")
    target_compile_definitions(vidtab_live_test PRIVATE _GNU_SOURCE _DEFAULT_SOURCE)
    set_target_properties(vidtab_live_test PROPERTIES C_STANDARD 11)
    if(WIN32)
      target_link_options(vidtab_live_test PRIVATE -static)
    endif()
    add_test(NAME vidtab_live COMMAND vidtab_live_test)
    set_tests_properties(vidtab_live PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
  endif()
endif()
