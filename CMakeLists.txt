cmake_minimum_required(VERSION 3.20)
project(backline VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-format-truncation)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.4)

# PipeWire 1.6 has a body-level pod parser and a stateful UMP converter; the
# worker uses them when present and the older iteration API otherwise.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES ${PIPEWIRE_INCLUDE_DIRS})
check_cxx_source_compiles("
#include <spa/pod/parser.h>
#include <spa/control/ump-utils.h>
int main() { spa_pod_parser p; spa_pod_parser_init_from_data(&p, nullptr, 0, 0, 0);
  const uint32_t *u = nullptr; size_t n = 0; uint64_t st = 0; uint8_t m[4];
  return spa_ump_to_midi(&u, &n, m, 4, &st); }" BL_HAVE_SPA_PARSER_BODY)
unset(CMAKE_REQUIRED_INCLUDES)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile>=1.1)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
pkg_check_modules(JSONC REQUIRED IMPORTED_TARGET json-c)

include(CTest)

# Shared graph helper (C, main-thread only).
add_library(bl_graph STATIC common/bl_graph.c)
target_include_directories(bl_graph PUBLIC common)
target_link_libraries(bl_graph PUBLIC PkgConfig::PIPEWIRE)

# Plugin host: one CLAP instance per process, as one PipeWire node.
add_executable(backline-plugin-worker host/worker.cpp)
target_include_directories(backline-plugin-worker PRIVATE third_party/clap/include)
target_link_libraries(backline-plugin-worker PRIVATE bl_graph PkgConfig::PIPEWIRE PkgConfig::SYSTEMD ${CMAKE_DL_LIBS})
if(BL_HAVE_SPA_PARSER_BODY)
  target_compile_definitions(backline-plugin-worker PRIVATE BL_HAVE_SPA_PARSER_BODY=1)
endif()

# Recorder: N nodes into sample-aligned, timestamped files.
add_library(bl_rec_align STATIC recorder/rec_align.c)
target_include_directories(bl_rec_align PUBLIC recorder)
add_library(bl_rec_engine STATIC recorder/rec_engine.c)
target_include_directories(bl_rec_engine PUBLIC recorder)
find_package(Threads REQUIRED)
target_link_libraries(bl_rec_engine PUBLIC bl_graph bl_rec_align PkgConfig::PIPEWIRE PkgConfig::SNDFILE Threads::Threads m)
add_executable(backline-recorder recorder/recorder.c)
target_link_libraries(backline-recorder PRIVATE bl_rec_engine)
# The recorder as a D-Bus service (Recorder1), with virtual soundcheck.
add_executable(backline-recorderd recorder/recorderd.c recorder/soundcheck.c)
target_link_libraries(backline-recorderd PRIVATE bl_rec_engine PkgConfig::SYSTEMD PkgConfig::JSONC)

# Test tool: a virtual MIDI keyboard on the ALSA sequencer (what a USB
# keyboard looks like to PipeWire's MIDI bridge).
add_executable(backline-test-keyboard tools/test-keyboard.c)
target_link_libraries(backline-test-keyboard PRIVATE PkgConfig::ALSA)
# Test tool: fake capture devices with a known input latency.
add_executable(backline-test-source tools/test-source.c)
target_link_libraries(backline-test-source PRIVATE PkgConfig::PIPEWIRE PkgConfig::SNDFILE m)


# Plugin-host supervisor: one systemd user scope per worker, restart with state.
add_library(bl_hostd_policy STATIC hostd/policy.c)
target_include_directories(bl_hostd_policy PUBLIC hostd)
target_link_libraries(bl_hostd_policy PUBLIC PkgConfig::JSONC)
add_executable(backline-hostd hostd/hostd.c)
target_link_libraries(backline-hostd PRIVATE bl_hostd_policy PkgConfig::SYSTEMD PkgConfig::JSONC)

# libbackline: the MIT C client library (D-Bus only), and the backline CLI.
add_library(backline SHARED lib/backline.c lib/services.c)
set_target_properties(backline PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0 C_VISIBILITY_PRESET default)
target_include_directories(backline PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(backline PRIVATE PkgConfig::SYSTEMD)
add_executable(backline-cli cli/backline.c cli/services.c)
set_target_properties(backline-cli PROPERTIES OUTPUT_NAME backline)
target_link_libraries(backline-cli PRIVATE backline PkgConfig::JSONC)

# Unit tests (no PipeWire daemon, no session bus needed).
if(BUILD_TESTING)
  add_executable(test_hostd_policy tests/unit/test_hostd_policy.c)
  target_link_libraries(test_hostd_policy PRIVATE bl_hostd_policy)
  add_test(NAME hostd_policy COMMAND test_hostd_policy)
  add_executable(test_rt tests/unit/test_rt.cpp)
  target_include_directories(test_rt PRIVATE host)
  find_package(Threads REQUIRED)
  target_link_libraries(test_rt PRIVATE Threads::Threads)
  add_test(NAME rt_spsc_osc COMMAND test_rt)
  add_executable(test_libbackline tests/unit/test_libbackline.c)
  target_link_libraries(test_libbackline PRIVATE backline bl_hostd_policy)
  add_test(NAME libbackline COMMAND test_libbackline)
  add_executable(test_rec_align tests/unit/test_rec_align.c)
  target_link_libraries(test_rec_align PRIVATE bl_rec_align)
  add_test(NAME rec_align COMMAND test_rec_align)
  add_executable(test_rec_cues tests/unit/test_rec_cues.c)
  target_link_libraries(test_rec_cues PRIVATE bl_rec_engine)
  add_test(NAME rec_cues COMMAND test_rec_cues)
  # LD_PRELOAD shim for the slow-disk proof (tests/recorderd-proof).
  add_library(slowdisk MODULE tests/tools/slowdisk.c)
  set_target_properties(slowdisk PROPERTIES PREFIX "")
  add_executable(rf64check tests/tools/rf64check.c)
  target_link_libraries(rf64check PRIVATE PkgConfig::SNDFILE m)
  target_link_libraries(slowdisk PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
endif()

# Clock service: transport, tempo, LTC/MTC, Ableton Link (clock/).
option(BACKLINE_CLOCK "Build the clock service (needs jack from pipewire-jack, and libltc)" ON)
if(BACKLINE_CLOCK)
  add_subdirectory(clock)
endif()

# Plugin registry: sandboxed probes, grades, cache, D-Bus query service.
option(BACKLINE_REGISTRY "Build the plugin registry" ON)
if(BACKLINE_REGISTRY)
  add_subdirectory(registry)
endif()

install(TARGETS backline-plugin-worker backline-recorder backline-recorderd backline-hostd backline-cli RUNTIME DESTINATION bin)
install(TARGETS backline LIBRARY DESTINATION lib)
install(DIRECTORY lib/include/backline DESTINATION include)

# D-Bus activation files (session bus).
include(GNUInstallDirs)
set(bindir ${CMAKE_INSTALL_FULL_BINDIR})
set(_svcs Recorder)
if(TARGET backline-registry)
  list(APPEND _svcs Registry)
endif()
if(TARGET backline-clockd)
  list(APPEND _svcs Clock)
endif()
foreach(svc ${_svcs})
  configure_file(data/dbus/io.github.manuaudio.Backline.${svc}.service.in
                 ${CMAKE_BINARY_DIR}/dbus/io.github.manuaudio.Backline.${svc}.service @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/dbus/io.github.manuaudio.Backline.${svc}.service
          DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
endforeach()

# Registry auto-rescan: a user .path unit, and a pacman hook for packagers.
foreach(u backline-registry-rescan.path backline-registry-rescan.service)
  configure_file(data/systemd/${u}.in ${CMAKE_BINARY_DIR}/systemd/${u} @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/systemd/${u} DESTINATION lib/systemd/user)
endforeach()
install(FILES data/pacman/backline-plugins.hook DESTINATION ${CMAKE_INSTALL_DATADIR}/libalpm/hooks)
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/backline)
