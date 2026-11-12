# Surface service (docs/api/Surface1.md). The protocol, profile and engine
# code is pure and unit-tested without PipeWire or a bus (tests/unit).
add_library(bl_surface_core STATIC mcu.c profile.c engine.c rtpmidi.c)
target_include_directories(bl_surface_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bl_surface_core PUBLIC bl_params PkgConfig::JSONC m)

add_library(bl_midi STATIC ${PROJECT_SOURCE_DIR}/common/bl_midibuf.c)
target_include_directories(bl_midi PUBLIC ${PROJECT_SOURCE_DIR}/common)

# Network surfaces (RTP-MIDI) are found by mDNS through avahi-daemon when
# avahi-client is there; without it, only by invitation or address.
pkg_check_modules(AVAHI IMPORTED_TARGET avahi-client)
add_executable(backline-surfaced surfaced.c rtpnet.c)
target_link_libraries(backline-surfaced PRIVATE bl_surface_core bl_midi bl_graph PkgConfig::PIPEWIRE PkgConfig::SYSTEMD PkgConfig::JSONC)
if(AVAHI_FOUND)
  target_link_libraries(backline-surfaced PRIVATE PkgConfig::AVAHI)
  target_compile_definitions(backline-surfaced PRIVATE BL_HAVE_AVAHI=1)
else()
  message(STATUS "avahi-client not found: backline-surfaced finds network surfaces by invitation or address only")
endif()
if(BL_HAVE_SPA_PARSER_BODY)
  target_compile_definitions(backline-surfaced PRIVATE BL_HAVE_SPA_PARSER_BODY=1)
endif()
target_compile_definitions(backline-surfaced PRIVATE BL_SURFACE_DATADIR="${CMAKE_INSTALL_FULL_DATADIR}/backline/surfaces"
  BL_SURFACE_SRCDIR="${PROJECT_SOURCE_DIR}/data/surfaces")
install(TARGETS backline-surfaced RUNTIME DESTINATION bin)
install(DIRECTORY ${PROJECT_SOURCE_DIR}/data/surfaces/ DESTINATION ${CMAKE_INSTALL_DATADIR}/backline/surfaces FILES_MATCHING PATTERN "*.surface.json")

# Test tools: a software MCU / X-Touch (a PipeWire MIDI device that speaks the
# protocol and records what it is sent), and a stub mixer program that
# publishes a Params1 tree and pushes views.
add_library(bl_mcusim STATIC ${PROJECT_SOURCE_DIR}/tools/mcusim.c)
target_include_directories(bl_mcusim PUBLIC ${PROJECT_SOURCE_DIR}/tools)
target_link_libraries(bl_mcusim PUBLIC bl_surface_core)
add_executable(backline-test-mcu-sim ${PROJECT_SOURCE_DIR}/tools/mcu-sim.c)
target_link_libraries(backline-test-mcu-sim PRIVATE bl_mcusim bl_midi PkgConfig::PIPEWIRE PkgConfig::JSONC)
if(BL_HAVE_SPA_PARSER_BODY)
  target_compile_definitions(backline-test-mcu-sim PRIVATE BL_HAVE_SPA_PARSER_BODY=1)
endif()
add_executable(backline-test-mixer ${PROJECT_SOURCE_DIR}/tools/test-mixer.c)
target_link_libraries(backline-test-mixer PRIVATE PkgConfig::SYSTEMD PkgConfig::JSONC m)
target_include_directories(backline-test-mixer PRIVATE ${PROJECT_SOURCE_DIR}/common)

if(BUILD_TESTING)
  add_executable(test_surface ${PROJECT_SOURCE_DIR}/tests/unit/test_surface.c)
  target_link_libraries(test_surface PRIVATE bl_surface_core bl_mcusim bl_midi)
  target_compile_definitions(test_surface PRIVATE BL_SURFACE_SRCDIR="${PROJECT_SOURCE_DIR}/data/surfaces")
  add_test(NAME surface_mcu_engine_sim COMMAND test_surface)
  add_executable(test_rtpmidi ${PROJECT_SOURCE_DIR}/tests/unit/test_rtpmidi.c)
  target_link_libraries(test_rtpmidi PRIVATE bl_surface_core)
  add_test(NAME surface_rtpmidi COMMAND test_rtpmidi)
  # Real hardware: pending (reported as skipped) until a bench pass.
  add_test(NAME surface_real_hardware COMMAND ${PROJECT_SOURCE_DIR}/tests/surface-hardware-pending)
  set_tests_properties(surface_real_hardware PROPERTIES SKIP_RETURN_CODE 77)
endif()
