# Control service: OSC and MIDI to any parameter (docs/api/Control1.md). No
# new dependencies: its OSC codec and network policy are its own (osc.c,
# policy.c), unit-tested below.
add_library(bl_control_core STATIC osc.c policy.c mapping.c)
target_include_directories(bl_control_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bl_control_core PUBLIC m)

add_executable(backline-controld controld.c)
target_link_libraries(backline-controld PRIVATE bl_control_core bl_graph PkgConfig::PIPEWIRE PkgConfig::SYSTEMD PkgConfig::JSONC)
if(BL_HAVE_SPA_PARSER_BODY)
  target_compile_definitions(backline-controld PRIVATE BL_HAVE_SPA_PARSER_BODY=1)
endif()
install(TARGETS backline-controld RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_executable(test_control ${PROJECT_SOURCE_DIR}/tests/unit/test_control.c)
  target_link_libraries(test_control PRIVATE bl_control_core)
  add_test(NAME control_osc_policy_mapping COMMAND test_control)
endif()
