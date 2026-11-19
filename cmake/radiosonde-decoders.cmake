# The two programs the app runs as its decoder pipeline, fsk_demod (codec2, LGPL-2.1) and rs41mod
# (rs1729/RS, GPL-3.0), built from the radiosonde_auto_rx release tarball with the same sources and flags
# as its Makefiles, and installed in /usr/lib/sondechase so the package decodes with nothing else installed.
include(FetchContent)
include(GNUInstallDirs)

set(SC_AUTO_RX_VERSION "1.9.0")
FetchContent_Declare(auto_rx
    URL "https://github.com/projecthorus/radiosonde_auto_rx/archive/refs/tags/v${SC_AUTO_RX_VERSION}.tar.gz"
    URL_HASH SHA256=5d5e7ccc7a3a243dfa721b4e04b7d34855d36e82f5b19ca9524021d0fd1e1443
)
FetchContent_GetProperties(auto_rx)
if(NOT auto_rx_POPULATED)
  if(POLICY CMP0169)
    cmake_policy(PUSH)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(auto_rx) # sources only: auto_rx has no CMake build
  if(POLICY CMP0169)
    cmake_policy(POP)
  endif()
endif()

set(SC_MOD "${auto_rx_SOURCE_DIR}/demod/mod")
set(SC_UTILS "${auto_rx_SOURCE_DIR}/utils")

add_executable(rs41mod "${SC_MOD}/rs41mod.c" "${SC_MOD}/demod_mod.c" "${SC_MOD}/bch_ecc_mod.c")
target_compile_definitions(rs41mod PRIVATE VER_JSN_STR="${SC_AUTO_RX_VERSION}")
target_compile_options(rs41mod PRIVATE -O3 -w)
set_source_files_properties("${SC_MOD}/demod_mod.c" PROPERTIES COMPILE_OPTIONS -Ofast)

add_executable(fsk_demod "${SC_UTILS}/fsk_demod.c" "${SC_UTILS}/fsk.c" "${SC_UTILS}/modem_stats.c"
    "${SC_UTILS}/kiss_fftr.c" "${SC_UTILS}/kiss_fft.c")
target_compile_options(fsk_demod PRIVATE -O3 -w)

foreach(t rs41mod fsk_demod)
  target_link_libraries(${t} PRIVATE m)
  set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/decoders")
endforeach()

# Not CMAKE_INSTALL_LIBDIR: on Debian that is the multiarch lib/aarch64-linux-gnu, and the store only
# accepts a directory named after the package directly under /usr/lib. Must match SC_DECODER_DIR.
install(TARGETS rs41mod fsk_demod RUNTIME DESTINATION "lib/${PROJECT_NAME}")
install(FILES "${auto_rx_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}" RENAME "LICENSE-radiosonde_auto_rx")
