# astro_core: the toolkit-agnostic, SDL-free Bally Astrocade core.
#
# Unlike the sibling ports, whose emulator is a staged upstream (jzIntv /
# XRoar / openMSX) selected by which backend files it compiles, this core is
# transposed in-tree from MAME's astrocde home-console driver (BSD-3-Clause;
# see COMPLIANCE.md) around floooh's cycle-stepped chips z80.h (zlib). It has
# no windowing, audio or input toolkit of its own: the machine publishes
# frames and audio into the host layer (core/astro/host.c), which the session
# and frontends pull from. So there is nothing to keep SDL-free by discipline
# here -- the core simply has no SDL, and no_sdl_link_test guards that as the
# session layer grows.
#
# The FujiNet cartridge is the one cart device (core/astro/fujinet_cart.c); it
# compiles the cartridge firmware's OWN protocol sources verbatim from
# core/astro/fuji-generated (staged by cmake/StageFujiProto.cmake), so the
# emulator and the real RP2040 cartridge cannot drift.

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(Python3 COMPONENTS Interpreter REQUIRED)

set(GEN "${CMAKE_CURRENT_SOURCE_DIR}/astro/fuji-generated")
if(NOT EXISTS "${GEN}/fuji_mailbox.h")
  message(FATAL_ERROR
    "Staged FujiNet protocol sources are missing. cmake/StageFujiProto.cmake "
    "should have produced them; re-run with -DFUJI_RESTAGE=ON.")
endif()

# ---- the emulator core + the staged protocol sources ----------------------
set(ASTRO_CORE_SOURCES
  astro/machine.c
  astro/video.c
  astro/sound.c
  astro/host.c
  astro/fujinet_cart.c
  debugger/debugger.c
  debugger/vidview.c
  "${GEN}/fujimail.c"
  "${GEN}/fujibus.c"
  "${GEN}/astromap.c"
)
# fujitcp is the emulator-side transport. The staged .c is POSIX sockets; a
# Windows build compiles a committed Winsock twin against the same staged
# fujitcp.h instead (that file lands with the Windows frontend milestone).
if(WIN32)
  list(APPEND ASTRO_CORE_SOURCES astro/fujitcp_win32.c)
else()
  list(APPEND ASTRO_CORE_SOURCES "${GEN}/fujitcp.c")
endif()
list(TRANSFORM ASTRO_CORE_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/"
     REGEX "^astro/")

add_library(astro_core STATIC ${ASTRO_CORE_SOURCES})
set_target_properties(astro_core PROPERTIES
  C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(astro_core PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/astro"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  # public because fujinet_cart.h -- part of the core's own API surface
  # (astro_internal.h -> host.h include it) -- includes the staged astromap.h.
  "${GEN}")
# fujitcp.c uses getaddrinfo/gettimeofday -- POSIX, behind _DEFAULT_SOURCE
# under a strict -std=c11.
if(NOT WIN32)
  target_compile_definitions(astro_core PRIVATE _DEFAULT_SOURCE)
endif()
target_link_libraries(astro_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(astro_core PUBLIC ws2_32)
else()
  target_link_libraries(astro_core PUBLIC m)
endif()
# our own modules are warning-clean; the staged firmware sources are vendored.
set_source_files_properties(
  astro/machine.c astro/video.c astro/sound.c astro/host.c astro/fujinet_cart.c
  debugger/debugger.c debugger/vidview.c
  PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra")

# ---- the Bally system ROMs -------------------------------------------------
# The core takes the BIOS from the caller (astro_host_opts.bios); this table
# only decides whether the app can find one embedded at startup vs. having the
# user import it. What changes with WITH_ASTROCADE_ROMS is only whether the
# table has anything in it. See COMPLIANCE.md.
set(ASTRO_ROM_DIR_SRC "${CMAKE_SOURCE_DIR}/tools/roms")
set(ASTRO_SYSTEM_ROMS
  "${ASTRO_ROM_DIR_SRC}/astro.bin"
  "${ASTRO_ROM_DIR_SRC}/ballyhlc.bin"
  "${ASTRO_ROM_DIR_SRC}/bioswhit.bin")

if(WITH_ASTROCADE_ROMS)
  set(_embed_roms ${ASTRO_SYSTEM_ROMS})
else()
  set(_embed_roms "")
  message(STATUS
    "WITH_ASTROCADE_ROMS=OFF: the Bally system ROMs are NOT embedded; the app "
    "reads them from the user's ROM directory at run time")
endif()

set(ASTRO_ROMS_C "${CMAKE_CURRENT_BINARY_DIR}/roms_embedded.c")
add_custom_command(
  OUTPUT "${ASTRO_ROMS_C}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
          "${ASTRO_ROMS_C}"
          --roms ${_embed_roms}
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/roms/embed-roms.py"
          ${_embed_roms}
  COMMENT "Embedding the Bally ROMs (WITH_ASTROCADE_ROMS=${WITH_ASTROCADE_ROMS})"
  VERBATIM)

add_library(astro_roms STATIC "${ASTRO_ROMS_C}")
set_target_properties(astro_roms PROPERTIES C_STANDARD 11
  POSITION_INDEPENDENT_CODE ON)
target_include_directories(astro_roms PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/astro")
target_link_libraries(astro_core PUBLIC astro_roms)

# ---- astro_session: the toolkit-agnostic session API (astrosession.h) ------
# A SEPARATE target from astro_core, not more files bolted onto it: audio_sdl.c
# needs SDL3, and astro_core's whole point is staying SDL-free (verified by
# no_sdl_link_test, which links astro_core alone). Were audio_sdl.c added
# directly to astro_core, every consumer of that archive would pick up an SDL3
# runtime dependency whether or not it plays audio.
find_package(SDL3 CONFIG REQUIRED)

add_library(astro_session STATIC
  src/settings.c
  src/paths.c
  src/roms.c
  src/session.c
  src/bindings.c
  src/astro_keymap.c
  src/audio_sdl.c
  src/fujinet_runtime.c
)
set_target_properties(astro_session PROPERTIES C_STANDARD 11)
target_include_directories(astro_session PRIVATE src)
target_link_libraries(astro_session PUBLIC astro_core PRIVATE SDL3::SDL3)
# fujinet_runtime.c dlopen's libfujinet at run time (no link-time dependency,
# so a -DWITH_FUJINET=OFF build still links). These tell paths_provision_
# fujinet where to find the runtime and its asset tree.
target_link_libraries(astro_session PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(astro_session PRIVATE
  ASTRO_DEV_FUJINET_OUT="${CMAKE_SOURCE_DIR}/tools/fujinet/work/out"
  ASTRO_INSTALL_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/fujinet-go-astrocade"
  ASTRO_INSTALL_LIBDIR="${CMAKE_INSTALL_FULL_LIBDIR}/fujinet-go-astrocade"
)
# -Wno-format-truncation: the path buffers are ASTRO_PATH_MAX and the compiler
# can't prove the "%s/settings.ini" suffix fits -- it does (the dirs are
# bounded well under that), and a truncated path just fails to open, harmless.
target_compile_options(astro_session PRIVATE -Wall -Wextra -Wno-format-truncation)

# ---- tests -----------------------------------------------------------------
add_executable(boot_smoke tests/boot_smoke.c)
target_link_libraries(boot_smoke astro_core)

add_executable(funcgen_test tests/funcgen_test.c)
target_link_libraries(funcgen_test astro_core)

add_executable(keypad_matrix_test tests/keypad_matrix_test.c)
target_link_libraries(keypad_matrix_test astro_core)

add_executable(mapper_test tests/mapper_test.c)
target_link_libraries(mapper_test astro_core)

# boot_smoke needs the real BIOS; it exits 77 (SKIP) without one, so point it
# at the dev ROM dir when the BIOS is present there and let ctest skip
# otherwise (a WITH_ASTROCADE_ROMS=OFF checkout has none).
add_test(NAME boot_smoke
         COMMAND boot_smoke "${ASTRO_ROM_DIR_SRC}/astro.bin")
set_tests_properties(boot_smoke PROPERTIES SKIP_RETURN_CODE 77)

add_test(NAME funcgen_test COMMAND funcgen_test)
add_test(NAME keypad_matrix_test COMMAND keypad_matrix_test)
add_test(NAME mapper_test COMMAND mapper_test)

add_executable(debug_test tests/debug_test.c)
target_link_libraries(debug_test astro_core)
add_test(NAME debug_test COMMAND debug_test "${ASTRO_ROM_DIR_SRC}/astro.bin")
set_tests_properties(debug_test PROPERTIES SKIP_RETURN_CODE 77)

# astro_core must carry no SDL: this links it ALONE.
add_executable(no_sdl_link_test tests/no_sdl_link_test.c)
target_link_libraries(no_sdl_link_test astro_core)
add_test(NAME no_sdl_link_test COMMAND no_sdl_link_test)

add_executable(bindings_test tests/bindings_test.c)
target_link_libraries(bindings_test astro_session)
target_include_directories(bindings_test PRIVATE src)
add_test(NAME bindings_test COMMAND bindings_test)

add_executable(session_test tests/session_test.c)
target_link_libraries(session_test astro_session)
# session_test exercises a real start/stop when handed a BIOS; point it at the
# dev ROM when present (SKIP-friendly: it just skips the start/stop otherwise).
add_test(NAME session_test
         COMMAND session_test "${ASTRO_ROM_DIR_SRC}/astro.bin")
