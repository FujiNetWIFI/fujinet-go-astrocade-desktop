# The FujiNet runtime: fujinet-pc built from the pinned firmware
# (astrocade-bringup) as a shared library the app dlopen's at run time, the
# same design as every sibling desktop port (tools/fujinet/
# build-fujinet-desktop.sh, PC_TARGET=RS232, FUJINET_EMBEDDED patches, mbedTLS
# system-or-pinned). The emulator's cart device (core/astro/fujinet_cart.c)
# dials this runtime's BoIP listener on 127.0.0.1:11500; its web admin UI
# binds 127.0.0.1:11501.
#
# NOTE: the runtime build is landed in the FujiNet milestone (see the plan's
# M4). Until then WITH_FUJINET defaults OFF and the app runs the emulator with
# the cart's TCP link down -- the mailbox tolerates that (the CONFIG client
# reports "no link"), exactly as it does when a standalone fujinet-pc simply
# is not running. Turning this ON without the build script in place is a
# configure error rather than a silent no-op.

option(WITH_FUJINET "Build and bundle the FujiNet runtime (see the plan M4)" OFF)

if(WITH_FUJINET)
  message(FATAL_ERROR
    "WITH_FUJINET=ON: the runtime build (tools/fujinet/build-fujinet-desktop.sh) "
    "is not wired up yet -- it lands in the FujiNet milestone. Leave WITH_FUJINET "
    "OFF for now; the emulator and its FujiNet cart still build and run, with the "
    "BoIP link down until a fujinet-pc (or this runtime) is listening on port "
    "${ASTRO_BOIP_PORT_DEFAULT}.")
endif()
