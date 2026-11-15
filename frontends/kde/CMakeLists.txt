# The KDE frontend: plain Qt6 Widgets.
#
# Deliberately NOT KDE Frameworks. It picks up Breeze through the platform
# theme anyway, and staying framework-free keeps this frontend usable outside
# a KDE session -- and keeps the door open to reusing it elsewhere.
#
# Found-or-skipped, like the GNOME one: a machine without Qt6 development
# packages still builds the core, the tests and the other frontend.

set(CMAKE_AUTOMOC ON)

# Qt 6.7 for QRhiWidget (the GPU display); ShaderTools bakes its shaders. The
# display's QRhi types come from GuiPrivate, which ties the build to this Qt:
# that is known, so its configure-time notice is switched off.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.7 COMPONENTS Widgets Network GuiPrivate ShaderTools QUIET)
if(NOT Qt6_FOUND)
  message(STATUS "kde frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

# The display's shaders, as the app and its resource test bake them: the files
# land at :/shaders/display.{vert,frag}.qsb. PREFIX "/", not "/shaders" (the
# files are listed as shaders/display.*): a wrong prefix once put them at
# :/shaders/shaders/..., the loads came up empty and the software display
# silently took over everywhere.
set(CHF_DISPLAY_SHADERS shaders/display.vert shaders/display.frag)

add_executable(fujinet-go-channelf-kde
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SoftDisplayWidget.cpp
  FrameFeed.cpp
  KeyForward.cpp
  SettingsDialog.cpp
  FujiNetWindows.cpp
  controllers/ControllersWindow.cpp
  debugger/DebuggerWindow.cpp
  debugger/ScratchPanel.cpp
  debugger/VideoTab.cpp
  debugger/ChipTabs.cpp
  help/HelpWindow.cpp
)
target_link_libraries(fujinet-go-channelf-kde PRIVATE chf_session Qt6::Widgets Qt6::GuiPrivate Qt6::Network)
# The display's shaders, baked for every backend and embedded as resources.
qt_add_shaders(fujinet-go-channelf-kde "display_shaders"
  PREFIX "/"
  FILES ${CHF_DISPLAY_SHADERS})
target_compile_options(fujinet-go-channelf-kde PRIVATE -Wall -Wextra)
target_include_directories(fujinet-go-channelf-kde PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(fujinet-go-channelf-kde PRIVATE
  CHF_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

install(TARGETS fujinet-go-channelf-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

configure_file(data/online.fujinet.go.channelf.kde.desktop.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.channelf.kde.desktop
               @ONLY)
configure_file(data/online.fujinet.go.channelf.kde.metainfo.xml.in
               ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.channelf.kde.metainfo.xml
               @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.channelf.kde.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.channelf.kde.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_file_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.channelf.kde.desktop)
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   ${CMAKE_CURRENT_BINARY_DIR}/online.fujinet.go.channelf.kde.metainfo.xml)
endif()

# Guards the display's shader resources: the same qt_add_shaders call (same
# prefix, same files) in a tiny program that asks for the exact paths
# DisplayWidget.cpp loads, and fails when one is missing or not a shader.
add_executable(display_shaders_test display_shaders_test.cpp)
target_link_libraries(display_shaders_test PRIVATE Qt6::Gui Qt6::GuiPrivate)
qt_add_shaders(display_shaders_test "display_shaders_guard"
  PREFIX "/"
  FILES ${CHF_DISPLAY_SHADERS})
add_test(NAME display_shaders_kde COMMAND display_shaders_test)
set_tests_properties(display_shaders_kde PROPERTIES
                     ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# The Debugger window (and F12 from the main window), on the offscreen
# platform, against a running session on the stand-in BIOS.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
  add_executable(debugger_test debugger_test.cpp debugger/DebuggerWindow.cpp
                 debugger/ScratchPanel.cpp debugger/VideoTab.cpp debugger/ChipTabs.cpp
                 MainWindow.cpp DisplayWidget.cpp SoftDisplayWidget.cpp
                 FrameFeed.cpp SettingsDialog.cpp KeyForward.cpp
                 FujiNetWindows.cpp help/HelpWindow.cpp
                 controllers/ControllersWindow.cpp)
  set_target_properties(debugger_test PROPERTIES CXX_STANDARD 17)
  target_link_libraries(debugger_test PRIVATE chf_session Qt6::Widgets
                        Qt6::GuiPrivate Qt6::Network Qt6::Test)
  qt_add_shaders(debugger_test "display_shaders_test"
    PREFIX "/"
    FILES ${CHF_DISPLAY_SHADERS})
  target_include_directories(debugger_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_options(debugger_test PRIVATE -Wall -Wextra)
  target_compile_definitions(debugger_test PRIVATE
    CHF_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")
  add_test(NAME debugger_kde COMMAND debugger_test)
  set_tests_properties(debugger_kde PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
