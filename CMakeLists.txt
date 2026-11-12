cmake_minimum_required(VERSION 3.20)
project(backline VERSION 0.4.0 LANGUAGES C CXX)

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
find_package(Threads REQUIRED)

# Shared graph helper (C, main-thread only).
add_library(bl_graph STATIC common/bl_graph.c)
target_include_directories(bl_graph PUBLIC common)
target_link_libraries(bl_graph PUBLIC PkgConfig::PIPEWIRE)

# Plugin host: one plugin instance per process, as one PipeWire node. The
# format sits behind one interface (host/plugin.h): CLAP always; VST3 when the
# VST3 SDK is found; LV2 when lilv is.
add_executable(backline-plugin-worker host/worker.cpp host/plugin_clap.cpp)
target_include_directories(backline-plugin-worker PRIVATE third_party/clap/include)
target_link_libraries(backline-plugin-worker PRIVATE bl_graph PkgConfig::PIPEWIRE PkgConfig::SYSTEMD ${CMAKE_DL_LIBS})
if(BL_HAVE_SPA_PARSER_BODY)
  target_compile_definitions(backline-plugin-worker PRIVATE BL_HAVE_SPA_PARSER_BODY=1)
endif()

# LV2 through lilv (ISC).
pkg_check_modules(LILV IMPORTED_TARGET lilv-0)
option(BACKLINE_LV2 "Host LV2 plugins in the worker (needs lilv)" ${LILV_FOUND})
if(BACKLINE_LV2)
  if(NOT LILV_FOUND)
    message(FATAL_ERROR "BACKLINE_LV2=ON but lilv-0 was not found")
  endif()
  target_sources(backline-plugin-worker PRIVATE host/plugin_lv2.cpp)
  target_compile_definitions(backline-plugin-worker PRIVATE BL_HAVE_LV2=1)
  target_link_libraries(backline-plugin-worker PRIVATE PkgConfig::LILV)
endif()

# VST3 through the VST3 SDK's hosting classes (MIT since 3.8). Only the
# sources the worker needs are compiled, into a static library.
pkg_check_modules(VST3SDK QUIET vst3sdk)
set(_vst3_default "")
if(VST3SDK_FOUND)
  pkg_get_variable(_vst3_inc vst3sdk includedir)
  if(NOT _vst3_inc)
    string(REGEX REPLACE "^-I" "" _vst3_inc "${VST3SDK_CFLAGS}")
  endif()
  set(_vst3_default "${_vst3_inc}")
endif()
if(NOT _vst3_default AND EXISTS /usr/src/vst3sdk/pluginterfaces)
  set(_vst3_default /usr/src/vst3sdk)
endif()
set(BACKLINE_VST3SDK_DIR "${_vst3_default}" CACHE PATH "VST3 SDK root (pluginterfaces/, base/, public.sdk/)")
set(_vst3_found OFF)
if(BACKLINE_VST3SDK_DIR AND EXISTS "${BACKLINE_VST3SDK_DIR}/public.sdk/source/vst/hosting/plugprovider.cpp")
  set(_vst3_found ON)
endif()
option(BACKLINE_VST3 "Host VST3 plugins in the worker (needs the VST3 SDK)" ${_vst3_found})
if(BACKLINE_VST3)
  if(NOT _vst3_found)
    message(FATAL_ERROR "BACKLINE_VST3=ON but no VST3 SDK at BACKLINE_VST3SDK_DIR='${BACKLINE_VST3SDK_DIR}'")
  endif()
  set(_s ${BACKLINE_VST3SDK_DIR})
  add_library(bl_vst3sdk STATIC
    ${_s}/pluginterfaces/base/coreiids.cpp
    ${_s}/pluginterfaces/base/funknown.cpp
    ${_s}/pluginterfaces/base/ustring.cpp
    ${_s}/base/source/baseiids.cpp
    ${_s}/base/source/fbuffer.cpp
    ${_s}/base/source/fdebug.cpp
    ${_s}/base/source/fobject.cpp
    ${_s}/base/source/fstring.cpp
    ${_s}/base/source/updatehandler.cpp
    ${_s}/base/thread/source/flock.cpp
    ${_s}/public.sdk/source/common/commonstringconvert.cpp
    ${_s}/public.sdk/source/common/memorystream.cpp
    ${_s}/public.sdk/source/common/threadchecker_linux.cpp
    ${_s}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${_s}/public.sdk/source/vst/hosting/eventlist.cpp
    ${_s}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${_s}/public.sdk/source/vst/hosting/module.cpp
    ${_s}/public.sdk/source/vst/hosting/module_linux.cpp
    ${_s}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${_s}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${_s}/public.sdk/source/vst/hosting/plugprovider.cpp
    ${_s}/public.sdk/source/vst/hosting/processdata.cpp
    ${_s}/public.sdk/source/vst/utility/stringconvert.cpp
    ${_s}/public.sdk/source/vst/vstinitiids.cpp
    ${_s}/public.sdk/source/vst/vstpresetfile.cpp)
  target_include_directories(bl_vst3sdk SYSTEM PUBLIC ${_s})
  target_compile_definitions(bl_vst3sdk PUBLIC RELEASE=1)
  target_compile_options(bl_vst3sdk PRIVATE -w)
  set_target_properties(bl_vst3sdk PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(bl_vst3sdk PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
  target_sources(backline-plugin-worker PRIVATE host/plugin_vst3.cpp)
  target_compile_definitions(backline-plugin-worker PRIVATE BL_HAVE_VST3=1)
  target_link_libraries(backline-plugin-worker PRIVATE bl_vst3sdk)
endif()
message(STATUS "plugin worker formats: CLAP yes, VST3 ${BACKLINE_VST3}, LV2 ${BACKLINE_LV2}")

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
add_library(backline SHARED lib/backline.c lib/services.c lib/player.c lib/control.c)
set_target_properties(backline PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION 0 C_VISIBILITY_PRESET default)
target_include_directories(backline PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/lib/include> $<INSTALL_INTERFACE:include>)
target_link_libraries(backline PRIVATE PkgConfig::SYSTEMD)
add_executable(backline-cli cli/backline.c cli/services.c cli/player.c cli/control.c)
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
  add_executable(test_midi_follow tests/unit/test_midi_follow.c)
  add_test(NAME midi_follow COMMAND test_midi_follow)
  # LD_PRELOAD shim for the slow-disk proof (tests/recorderd-proof).
  add_library(slowdisk MODULE tests/tools/slowdisk.c)
  set_target_properties(slowdisk PROPERTIES PREFIX "")
  add_executable(rf64check tests/tools/rf64check.c)
  target_link_libraries(rf64check PRIVATE PkgConfig::SNDFILE m)
  target_link_libraries(slowdisk PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
  # The transport into plugins: position math and CLAP conversion.
  add_executable(test_transport tests/unit/test_transport.cpp)
  target_include_directories(test_transport PRIVATE host third_party/clap/include)
  target_link_libraries(test_transport PRIVATE PkgConfig::PIPEWIRE)
  add_test(NAME transport COMMAND test_transport)
  # Beat-click test plugins for tests/transport-proof: they click on every
  # beat of the host transport (tests/plugins/beatclick.h).
  add_library(backline-test-beatclick-clap MODULE tests/plugins/beatclick-clap.c)
  target_include_directories(backline-test-beatclick-clap PRIVATE third_party/clap/include)
  target_link_libraries(backline-test-beatclick-clap PRIVATE m)
  set_target_properties(backline-test-beatclick-clap PROPERTIES PREFIX "" OUTPUT_NAME backline-test-beatclick SUFFIX ".clap"
    C_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/transport-plugins)
  if(BACKLINE_LV2)
    set(_bc ${CMAKE_BINARY_DIR}/transport-plugins/lv2/backline-test-beatclick.lv2)
    add_library(backline-test-beatclick-lv2 MODULE tests/plugins/beatclick-lv2.c)
    target_include_directories(backline-test-beatclick-lv2 PRIVATE ${LILV_INCLUDE_DIRS})
    target_link_libraries(backline-test-beatclick-lv2 PRIVATE m)
    set_target_properties(backline-test-beatclick-lv2 PROPERTIES PREFIX "" OUTPUT_NAME backline-test-beatclick
      C_VISIBILITY_PRESET hidden LIBRARY_OUTPUT_DIRECTORY ${_bc})
    configure_file(tests/plugins/beatclick.lv2/manifest.ttl.in ${_bc}/manifest.ttl @ONLY)
    configure_file(tests/plugins/beatclick.lv2/beatclick.ttl ${_bc}/beatclick.ttl COPYONLY)
  endif()
  if(BACKLINE_VST3)
    set(_s ${BACKLINE_VST3SDK_DIR})
    add_library(backline-test-beatclick-vst3 MODULE tests/plugins/beatclick-vst3.cpp
      ${_s}/public.sdk/source/main/pluginfactory.cpp
      ${_s}/public.sdk/source/main/linuxmain.cpp
      ${_s}/public.sdk/source/vst/vstsinglecomponenteffect.cpp
      ${_s}/public.sdk/source/vst/vstcomponentbase.cpp
      ${_s}/public.sdk/source/vst/vstbus.cpp
      ${_s}/public.sdk/source/vst/vstparameters.cpp
      ${_s}/public.sdk/source/common/pluginview.cpp
      ${_s}/public.sdk/source/common/commoniids.cpp)
    target_include_directories(backline-test-beatclick-vst3 PRIVATE tests/plugins)
    target_compile_options(backline-test-beatclick-vst3 PRIVATE -w)
    target_link_libraries(backline-test-beatclick-vst3 PRIVATE bl_vst3sdk)
    set_target_properties(backline-test-beatclick-vst3 PROPERTIES PREFIX "" OUTPUT_NAME backline-test-beatclick
      CXX_VISIBILITY_PRESET hidden
      LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/transport-plugins/backline-test-beatclick.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux)
  endif()
endif()

# Clock service: transport, tempo, LTC/MTC, Ableton Link (clock/).
option(BACKLINE_CLOCK "Build the clock service (needs jack from pipewire-jack, and libltc)" ON)
if(BACKLINE_CLOCK)
  add_subdirectory(clock)
endif()

# Player service: files on the shared timeline (player/).
option(BACKLINE_PLAYER "Build the player service" ON)
if(BACKLINE_PLAYER)
  add_subdirectory(player)
endif()

# Plugin registry: sandboxed probes, grades, cache, D-Bus query service.
# Control service: OSC and MIDI to any parameter (control/).
option(BACKLINE_CONTROL "Build the control service (OSC and MIDI mapping)" ON)
if(BACKLINE_CONTROL)
  add_subdirectory(control)
endif()

option(BACKLINE_REGISTRY "Build the plugin registry" ON)
if(BACKLINE_REGISTRY)
  add_subdirectory(registry)
endif()

# The user units land where systemd reads user units, for this prefix.
if(BUILD_TESTING)
  add_test(NAME install-layout COMMAND ${PROJECT_SOURCE_DIR}/tests/install-layout-test ${CMAKE_BINARY_DIR})
  set_tests_properties(install-layout PROPERTIES TIMEOUT 60)
endif()

# The session manager's own policy, on a private PipeWire and WirePlumber
# (tests/autolink-proof; skipped when they aren't installed): an instrument
# with no target plays to the default output and follows it when it changes.
if(BUILD_TESTING AND BACKLINE_REGISTRY)
  add_test(NAME autolink COMMAND ${PROJECT_SOURCE_DIR}/tests/autolink-proof)
  set_tests_properties(autolink PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90
    ENVIRONMENT "BL_BIN=${CMAKE_BINARY_DIR};BL_OUT=${CMAKE_BINARY_DIR}/tests-out/autolink")
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
if(TARGET backline-playerd)
  list(APPEND _svcs Player)
endif()
if(TARGET backline-controld)
  list(APPEND _svcs Control)
endif()
foreach(svc ${_svcs})
  configure_file(data/dbus/io.github.manuaudio.Backline.${svc}.service.in
                 ${CMAKE_BINARY_DIR}/dbus/io.github.manuaudio.Backline.${svc}.service @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/dbus/io.github.manuaudio.Backline.${svc}.service
          DESTINATION ${CMAKE_INSTALL_DATADIR}/dbus-1/services)
endforeach()

# Registry auto-rescan: a user .path unit, and a pacman hook for packagers.
# User units go where systemd looks for them: lib/systemd/user under a system
# prefix (/usr, /usr/local), and $XDG_DATA_HOME/systemd/user for a prefix in
# the home directory (~/.local/share/systemd/user; systemd never reads
# ~/.local/lib/systemd/user). The pacman hook needs root and pacman's own
# directory, so only a /usr install has it.
set(_unitdir lib/systemd/user)
if(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "" AND CMAKE_INSTALL_PREFIX MATCHES "^$ENV{HOME}/")
  set(_xdg_data $ENV{XDG_DATA_HOME})
  if(NOT _xdg_data)
    set(_xdg_data $ENV{HOME}/.local/share)
  endif()
  set(_unitdir ${_xdg_data}/systemd/user)
endif()
set(BACKLINE_SYSTEMD_USER_UNIT_DIR ${_unitdir} CACHE STRING "Where the systemd user units go (relative to the prefix, or absolute)")
foreach(u backline-registry-rescan.path backline-registry-rescan.service)
  configure_file(data/systemd/${u}.in ${CMAKE_BINARY_DIR}/systemd/${u} @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/systemd/${u} DESTINATION ${BACKLINE_SYSTEMD_USER_UNIT_DIR})
endforeach()
if(CMAKE_INSTALL_PREFIX STREQUAL "/usr")
  install(FILES data/pacman/backline-plugins.hook DESTINATION ${CMAKE_INSTALL_DATADIR}/libalpm/hooks)
endif()
install(DIRECTORY DESTINATION ${CMAKE_INSTALL_FULL_LOCALSTATEDIR}/lib/backline)

# Omarchy commands (omarchy-backline-*): the scripts, their lib, catalogs, menu
# rows and hotkeys under share/backline-audio/omarchy, linked into bin. The
# bar plugin itself is installed by `omarchy plugin add` (manifest.json at the
# repository root); `omarchy-backline-setup` adds the menu rows and hotkeys.
option(BACKLINE_OMARCHY "Install the omarchy-backline-* commands" ON)
if(BACKLINE_OMARCHY)
  set(_om ${CMAKE_INSTALL_DATADIR}/backline-audio/omarchy)
  file(GLOB _om_bin ${PROJECT_SOURCE_DIR}/omarchy/bin/omarchy-backline-*)
  install(PROGRAMS ${_om_bin} DESTINATION ${_om}/bin)
  install(FILES omarchy/lib/omarchy-backline.sh omarchy/lib/omarchy-backline-setup.py DESTINATION ${_om}/lib)
  install(DIRECTORY omarchy/i18n omarchy/hypr omarchy/extensions DESTINATION ${_om})
  foreach(f ${_om_bin})
    get_filename_component(n ${f} NAME)
    file(RELATIVE_PATH _rel ${CMAKE_INSTALL_FULL_BINDIR} ${CMAKE_INSTALL_FULL_DATADIR}/backline-audio/omarchy/bin/${n})
    install(CODE "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}\")
file(CREATE_LINK \"${_rel}\" \"\$ENV{DESTDIR}${CMAKE_INSTALL_FULL_BINDIR}/${n}\" SYMBOLIC)")
  endforeach()
endif()
