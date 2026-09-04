# KDE frontend: plain Qt6 Widgets (deliberately NOT KDE Frameworks, so it
# builds on any Qt6). Found-or-skipped so a lean machine can still build the
# core / other frontends. FujiNet Configuration opens in the system browser.

# The top-level project() is C-only (the core is all C); Qt6's own find_package
# runs CXX compile checks, so enable C++ here, scoped to this frontend.
enable_language(CXX)

find_package(Qt6 6.4 COMPONENTS Widgets QUIET)

if(NOT Qt6_FOUND)
  message(STATUS "KDE frontend: Qt6 Widgets not found -- skipping")
  return()
endif()

set(CMAKE_AUTOMOC ON)

set(KDE_SOURCES
  main.cpp
  MainWindow.cpp
  DisplayWidget.cpp
  KeypadWindow.cpp
  SettingsDialog.cpp
  DebuggerWindow.cpp
)

set(KDE_APP_ID online.fujinet.go.astrocade.kde)

add_executable(fujinet-go-astrocade-kde ${KDE_SOURCES})
set_target_properties(fujinet-go-astrocade-kde PROPERTIES CXX_STANDARD 17)
target_link_libraries(fujinet-go-astrocade-kde PRIVATE
                      astro_session astro_core Qt6::Widgets m)
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
