# The Windows frontend: Win32 with a Direct3D 11 picture (GDI fallback), no
# toolkit.
#
# WIN32_EXECUTABLE gives WinMain and no console window. The manifest is
# generated from PROJECT_VERSION rather than repeating it, so the version
# lives in exactly one place (the top-level project() call).

if(NOT WIN32)
  message(STATUS "windows frontend: not a Windows build -- skipping")
  return()
endif()

configure_file(app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/app.manifest @ONLY)

# windres needs to find app.ico and app.manifest; the .rc references both by
# bare name, so give it both the source dir (icon) and the binary dir
# (generated manifest).
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I${CMAKE_CURRENT_SOURCE_DIR} -I${CMAKE_CURRENT_BINARY_DIR}")
enable_language(RC)

add_executable(fujinet-go-channelf-windows WIN32
  main.c
  display.c
  import.c
  fujilog.c
  prefs.c
  help/help_window.c
  controllers/controllers_window.c
  debugger/dbg_window.c
  debugger/dbg_views.c
  resource.rc
)
target_include_directories(fujinet-go-channelf-windows PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(fujinet-go-channelf-windows PRIVATE
  chf_session
  # d3d11: the picture (display.c; d3dcompiler_47 and avrt are loaded, not
  # linked). dwmapi: DwmFlush, the GDI fallback's frame clock. comdlg32: the
  # file dialogs. shell32 and ole32: DragQueryFile, ShellExecute for the web
  # UI and the folder browser. comctl32: the tab controls, trackbar and
  # status bar (and the v6 manifest above is what makes them look like this
  # decade).
  d3d11 dwmapi gdi32 user32 comdlg32 shell32 ole32 comctl32)
target_compile_options(fujinet-go-channelf-windows PRIVATE -Wall -Wextra)

# Bundle the MinGW runtime statically so the .exe has no loose DLL deps.
# This has to live on the TARGET, not in the toolchain file: that only
# covers the cross-compile, leaving the native MSYS2/UCRT64 build that
# actually ships the release importing libwinpthread-1.dll.
target_link_options(fujinet-go-channelf-windows PRIVATE
  -static -static-libgcc -static-libstdc++)

install(TARGETS fujinet-go-channelf-windows RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
