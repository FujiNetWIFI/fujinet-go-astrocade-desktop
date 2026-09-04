# Windows frontend: native Win32 (GDI) over the shared session core. Built
# only on Windows hosts (or cross-compiled with cmake/toolchains/
# mingw-w64.cmake for a Wine smoke test). WIN32_EXECUTABLE => a WinMain entry
# point, no console window.

if(NOT WIN32)
  return()
endif()

enable_language(RC)

# The manifest carries the app version in its assembly identity, generated
# from PROJECT_VERSION. resource.rc picks it up through the RC include path.
configure_file(app.manifest.in "${CMAKE_CURRENT_BINARY_DIR}/app.manifest" @ONLY)

add_executable(fujinet-go-astrocade-windows WIN32
  main.c
  debugger/dbg_window.c
  resource.rc
)
# The core is all C (unlike the sibling ports whose emulator had a C++ TU), so
# no libstdc++ is needed on the link line -- astro_session/astro_core link
# cleanly with the C driver.
target_link_libraries(fujinet-go-astrocade-windows PRIVATE
  astro_session astro_core
  gdi32 user32 comdlg32 shell32 comctl32 ws2_32
)
target_include_directories(fujinet-go-astrocade-windows PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_BINARY_DIR}"
  "${CMAKE_SOURCE_DIR}/core/src")
target_compile_options(fujinet-go-astrocade-windows PRIVATE -Wall -Wextra)
target_compile_definitions(fujinet-go-astrocade-windows PRIVATE
  ASTRO_APP_ID="online.fujinet.go.astrocade")

# Bundle the MinGW runtime statically so the only loose DLL is SDL3.dll
# itself (astro_session links SDL3 for audio, via the SDL3::SDL3 import lib
# which stays dynamic).
target_link_options(fujinet-go-astrocade-windows PRIVATE
  -static-libgcc)

install(TARGETS fujinet-go-astrocade-windows RUNTIME DESTINATION bin)
