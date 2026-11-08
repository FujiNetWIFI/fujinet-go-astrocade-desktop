# KDE frontend: plain Qt6 Widgets (deliberately NOT KDE Frameworks, so it
# builds on any Qt6). Found-or-skipped so a lean machine can still build the
# core / other frontends. FujiNet Configuration opens in the system browser.

# The top-level project() is C-only (the core is all C); Qt6's own find_package
# runs CXX compile checks, so enable C++ here, scoped to this frontend.
enable_language(CXX)

# Qt 6.7 for QRhiWidget (the GPU display); ShaderTools bakes its shaders.
find_package(Qt6 6.7 COMPONENTS Widgets GuiPrivate ShaderTools QUIET)

if(NOT Qt6_FOUND)
  message(STATUS "KDE frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

set(CMAKE_AUTOMOC ON)

set(KDE_SOURCES
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  SoftDisplayWidget.cpp
  FrameFeed.cpp
  KeyForward.cpp
  keypad/KeypadWindow.cpp
  SettingsDialog.cpp
  help/HelpWindow.cpp
  debugger/DebuggerWindow.cpp
)

set(KDE_APP_ID online.fujinet.go.astrocade.kde)

add_executable(fujinet-go-astrocade-kde ${KDE_SOURCES})
set_target_properties(fujinet-go-astrocade-kde PROPERTIES CXX_STANDARD 17)
target_link_libraries(fujinet-go-astrocade-kde PRIVATE
                      astro_session astro_core Qt6::Widgets Qt6::GuiPrivate m)
# The display's shaders, baked for every backend and embedded as resources.
# PREFIX "/" puts them at :/shaders/display.*.qsb ("/shaders" would nest it).
qt_add_shaders(fujinet-go-astrocade-kde "display_shaders"
  PREFIX "/"
  FILES shaders/display.vert shaders/display.frag)
target_include_directories(fujinet-go-astrocade-kde PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_SOURCE_DIR}/core/src")
target_compile_options(fujinet-go-astrocade-kde PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-astrocade-kde PRIVATE
  ASTRO_APP_ID="${KDE_APP_ID}")

install(TARGETS fujinet-go-astrocade-kde RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${KDE_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${KDE_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# The key translation and the Controllers window, on the offscreen platform.
find_package(Qt6 COMPONENTS Test QUIET)
if(Qt6Test_FOUND)
  add_executable(keypad_test keypad_test.cpp KeyForward.cpp
                 keypad/KeypadWindow.cpp help/HelpWindow.cpp)
  set_target_properties(keypad_test PROPERTIES CXX_STANDARD 17)
  target_link_libraries(keypad_test PRIVATE astro_session astro_core
                        Qt6::Widgets Qt6::Test m)
  target_include_directories(keypad_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                             "${CMAKE_SOURCE_DIR}/core/src")
  target_compile_options(keypad_test PRIVATE -Wall -Wextra)
  add_test(NAME keypad_kde COMMAND keypad_test)
  set_tests_properties(keypad_kde PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# The Debugger window (and F12 from the main window), on the offscreen
# platform, against a running session whose RAM holds a hand-assembled loop.
if(Qt6Test_FOUND)
  add_executable(debugger_test debugger_test.cpp debugger/DebuggerWindow.cpp
                 MainWindow.cpp DisplayWidget.cpp SoftDisplayWidget.cpp
                 FrameFeed.cpp SettingsDialog.cpp KeyForward.cpp
                 keypad/KeypadWindow.cpp help/HelpWindow.cpp)
  set_target_properties(debugger_test PROPERTIES CXX_STANDARD 17)
  target_link_libraries(debugger_test PRIVATE astro_session astro_core
                        Qt6::Widgets Qt6::GuiPrivate Qt6::Test m)
  qt_add_shaders(debugger_test "display_shaders_test"
    PREFIX "/"
    FILES shaders/display.vert shaders/display.frag)
  target_include_directories(debugger_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                             "${CMAKE_SOURCE_DIR}/core/src")
  target_compile_options(debugger_test PRIVATE -Wall -Wextra)
  target_compile_definitions(debugger_test PRIVATE
    ASTRO_APP_ID="${KDE_APP_ID}")
  add_test(NAME debugger_kde COMMAND debugger_test)
  set_tests_properties(debugger_kde PROPERTIES
                       ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_kde
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_kde
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${KDE_APP_ID}.metainfo.xml")
endif()
