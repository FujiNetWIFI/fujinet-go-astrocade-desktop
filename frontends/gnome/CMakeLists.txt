# GNOME frontend: GTK4 + libadwaita. Found-or-skipped so a lean machine can
# still build the core / other frontends.
#
# FujiNet Configuration opens in the system browser (the config pages' OAuth/
# JS need a real browser); there is no embedded webview.

find_package(PkgConfig REQUIRED)
pkg_check_modules(ADW IMPORTED_TARGET libadwaita-1>=1.4 gtk4>=4.10)

if(NOT ADW_FOUND)
  message(STATUS "GNOME frontend: libadwaita/gtk4 not found -- skipping")
  return()
endif()

set(GNOME_SOURCES
  main.c
  window.c
  display.c
  prefs.c
  keypad/keypad_window.c
  debugger/dbg_window.c
)

set(GNOME_APP_ID online.fujinet.go.astrocade.gnome)

add_executable(fujinet-go-astrocade-gnome ${GNOME_SOURCES})
target_link_libraries(fujinet-go-astrocade-gnome PRIVATE
                      astro_session astro_core PkgConfig::ADW m)
target_include_directories(fujinet-go-astrocade-gnome PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_SOURCE_DIR}/core/src")
target_compile_options(fujinet-go-astrocade-gnome PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-astrocade-gnome PRIVATE
  ASTRO_APP_ID="${GNOME_APP_ID}"
  ASTRO_SOURCE_ICON_DIR="${CMAKE_SOURCE_DIR}/data/icons")

install(TARGETS fujinet-go-astrocade-gnome RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES data/${GNOME_APP_ID}.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES data/${GNOME_APP_ID}.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Catch a malformed launcher/metainfo at build time.
find_program(DESKTOP_FILE_VALIDATE desktop-file-validate)
if(DESKTOP_FILE_VALIDATE)
  add_test(NAME desktop_entry_gnome
           COMMAND ${DESKTOP_FILE_VALIDATE}
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.desktop")
endif()
find_program(APPSTREAMCLI appstreamcli)
if(APPSTREAMCLI)
  add_test(NAME metainfo_gnome
           COMMAND ${APPSTREAMCLI} validate --no-net --pedantic
                   "${CMAKE_CURRENT_SOURCE_DIR}/data/${GNOME_APP_ID}.metainfo.xml")
endif()
