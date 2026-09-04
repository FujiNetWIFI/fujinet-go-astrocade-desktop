# macOS frontend: native AppKit (Objective-C) over the shared session core.
# Built only on Apple hosts. NOT built or run-verified in the environment this
# was written in (Linux) -- transposed from the verified GNOME/KDE/Win32
# frontends, validated on the macOS CI runner. See main.m's header.

if(NOT APPLE)
  return()
endif()

enable_language(OBJC)

set(MACOS_ICON "${CMAKE_SOURCE_DIR}/data/icons/fujinet-go-astrocade.icns")
set_source_files_properties(${MACOS_ICON} PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")

add_executable(fujinet-go-astrocade-macos MACOSX_BUNDLE
  main.m
  AppDelegate.m
  DisplayView.m
  AstroKeyForward.m
  ${MACOS_ICON}
)
set_target_properties(fujinet-go-astrocade-macos PROPERTIES
  OUTPUT_NAME "FujiNet Go Astrocade"
  MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
  MACOSX_BUNDLE_ICON_FILE "fujinet-go-astrocade.icns"
)
# The core is all C, so no libc++ is needed on the link line (unlike the
# sibling ports, whose emulator had a C++ TU). AppKit + the frameworks the
# display/browser use.
target_link_libraries(fujinet-go-astrocade-macos PRIVATE
  astro_session
  astro_core
  "-framework Cocoa"
  "-framework CoreVideo"
  "-framework QuartzCore"
)
target_include_directories(fujinet-go-astrocade-macos PRIVATE
                           "${CMAKE_CURRENT_SOURCE_DIR}"
                           "${CMAKE_SOURCE_DIR}/core/src")
target_compile_options(fujinet-go-astrocade-macos PRIVATE -Wall -Wextra -fobjc-arc)
target_compile_definitions(fujinet-go-astrocade-macos PRIVATE
  ASTRO_APP_ID="online.fujinet.go.astrocade.macos")

# Pack the FujiNet runtime into the bundle beside the executable: paths.c's
# exe_dir() probe finds libfujinet.dylib and exe_dir()/fujinet/fnconfig.ini
# there, so no session API change is needed. The runtime is built by this
# same build (cmake/FujiNetRuntime.cmake).
if(TARGET fujinet-runtime)
  add_dependencies(fujinet-go-astrocade-macos fujinet-runtime)
endif()
if(TARGET fujinet-runtime OR EXISTS "${FUJINET_OUT}/libfujinet.dylib")
  add_custom_command(TARGET fujinet-go-astrocade-macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/libfujinet.dylib"
            "$<TARGET_FILE_DIR:fujinet-go-astrocade-macos>/"
    COMMAND ${CMAKE_COMMAND} -E copy "${FUJINET_OUT}/fnconfig.ini"
            "$<TARGET_FILE_DIR:fujinet-go-astrocade-macos>/fujinet/fnconfig.ini"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/data"
            "$<TARGET_FILE_DIR:fujinet-go-astrocade-macos>/fujinet/data"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${FUJINET_OUT}/SD"
            "$<TARGET_FILE_DIR:fujinet-go-astrocade-macos>/fujinet/SD"
    COMMENT "Bundling the FujiNet runtime into the app")
else()
  message(STATUS "macOS frontend: FujiNet runtime disabled; app runs without FujiNet")
endif()

install(TARGETS fujinet-go-astrocade-macos BUNDLE DESTINATION .)
