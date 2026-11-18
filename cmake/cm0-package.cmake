#
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
#
# SPDX-License-Identifier: MIT
# Packaging and install layout for CardputerZero Debian builds.
# Adapted for SondeChase from the CardputerZero Template (cmake/cm0-package.cmake):
# same install layout and CPack settings; no systemd service (the app is started from
# APPLaunch); LVGL, the PNG decoder and the TTF renderer are linked statically, so the
# only shared-library dependency is libc.

include(GNUInstallDirs)

set(APP_DISPLAY_NAME "SondeChase" CACHE STRING "Human-readable application name used by launchers and package filename")
set(APP_DEBIAN_REVISION "rev1" CACHE STRING "Debian package revision/vendor suffix")
set(APP_DEBIAN_ARCHITECTURE "arm64" CACHE STRING "Debian package architecture")
set(APP_MAINTAINER "hieuha <hieuha@users.noreply.github.com>" CACHE STRING "Debian package maintainer")
set(APP_PACKAGE_DESCRIPTION "Radiosonde receiver and chase display (RTL-SDR, GPS)" CACHE STRING "Debian package summary")

set(APP_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/package")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/app.desktop.in"
    "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
    @ONLY
)

install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/conf/sondechase.conf"
    DESTINATION "/etc"
)

install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Rajdhani-SemiBold.ttf"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/Orbitron-Bold.ttf"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/fonts"
)
# Preloaded map packs go here (optional; downloaded tiles live in the user's cache directory).
install(DIRECTORY DESTINATION "${CMAKE_INSTALL_DATADIR}/${PROJECT_NAME}/tiles")

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/assets/images/sondechase.png"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/APPLaunch/share/images"
)
install(FILES "${APP_GENERATED_DIR}/${PROJECT_NAME}.desktop"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/APPLaunch/applications"
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-Rajdhani.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/OFL-Orbitron.txt"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_OUTPUT_FILE_PREFIX "${CMAKE_CURRENT_SOURCE_DIR}/dist")
set(CPACK_PACKAGE_NAME "${APP_DISPLAY_NAME}")
set(CPACK_PACKAGE_VENDOR "${APP_MAINTAINER}")
set(CPACK_PACKAGE_CONTACT "${APP_MAINTAINER}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "${APP_PACKAGE_DESCRIPTION}")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME "${APP_DISPLAY_NAME}_${PROJECT_VERSION}_${APP_DEBIAN_REVISION}_${APP_DEBIAN_ARCHITECTURE}")

string(TOLOWER "${APP_DISPLAY_NAME}" APP_DEBIAN_PACKAGE_NAME)
string(REGEX REPLACE "[^a-z0-9+.-]" "-" APP_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_NAME "${APP_DEBIAN_PACKAGE_NAME}")
set(CPACK_DEBIAN_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_DEBIAN_PACKAGE_RELEASE "${APP_DEBIAN_REVISION}")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${APP_DEBIAN_ARCHITECTURE}")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "${APP_MAINTAINER}")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
# curl: map tile download. Child processes started at run time and not shipped in this package:
# rtl_fm / rtl_power (rtl-sdr), fsk_demod + rs41mod (scripts/install_decoders.sh), gpsd, pacat.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, curl")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gpsd, pulseaudio-utils, coreutils")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/templates/conffiles.in"
    "${APP_GENERATED_DIR}/conffiles"
    @ONLY
)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA "${APP_GENERATED_DIR}/conffiles")

include(CPack)
