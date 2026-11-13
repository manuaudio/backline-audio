# Control service: OSC and MIDI to any parameter (docs/api/Control1.md). No
# new dependencies: its OSC codec and network policy are its own (osc.c,
# policy.c), unit-tested below.
add_library(bl_control_core STATIC osc.c policy.c mapping.c)
target_include_directories(bl_control_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bl_control_core PUBLIC m)
# The Params1 parameter tree: shared by controld and surfaced.
add_library(bl_params STATIC params.c)
target_include_directories(bl_params PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bl_params PUBLIC PkgConfig::JSONC m)

add_executable(backline-controld controld.c paramsd.c ${PROJECT_SOURCE_DIR}/common/bl_midibuf.c)
target_link_libraries(backline-controld PRIVATE bl_control_core bl_params bl_graph PkgConfig::PIPEWIRE PkgConfig::SYSTEMD PkgConfig::JSONC)
if(BL_HAVE_SPA_PARSER_BODY)
  target_compile_definitions(backline-controld PRIVATE BL_HAVE_SPA_PARSER_BODY=1)
endif()
install(TARGETS backline-controld RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_executable(test_control ${PROJECT_SOURCE_DIR}/tests/unit/test_control.c)
  target_link_libraries(test_control PRIVATE bl_control_core)
  add_test(NAME control_osc_policy_mapping COMMAND test_control)
  add_executable(test_params ${PROJECT_SOURCE_DIR}/tests/unit/test_params.c)
  target_link_libraries(test_params PRIVATE bl_params)
  add_test(NAME params_tree COMMAND test_params)
  # Params1 under load, offline: a private dbus-daemon (no activation),
  # controld --no-graph (never PipeWire), 5000 parameters, 8 OSC clients x 2000.
  add_executable(backline-test-params-load ${PROJECT_SOURCE_DIR}/tests/tools/params-load.c)
  target_link_libraries(backline-test-params-load PRIVATE bl_control_core PkgConfig::SYSTEMD Threads::Threads m)
  add_test(NAME params_load COMMAND backline-test-params-load $<TARGET_FILE:backline-controld>
           --json ${CMAKE_BINARY_DIR}/params-load.json)
  set_tests_properties(params_load PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
