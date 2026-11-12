# Player service: files into the graph on the shared transport timeline.
add_library(bl_player_clip STATIC clip.c)
target_include_directories(bl_player_clip PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bl_player_clip PUBLIC PkgConfig::PIPEWIRE PkgConfig::SNDFILE Threads::Threads m)
add_executable(backline-playerd playerd.c)
target_link_libraries(backline-playerd PRIVATE bl_player_clip bl_graph PkgConfig::SYSTEMD PkgConfig::JSONC)
install(TARGETS backline-playerd RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_executable(test_player_span ${PROJECT_SOURCE_DIR}/tests/unit/test_player_span.c)
  target_link_libraries(test_player_span PRIVATE bl_player_clip)
  add_test(NAME player_span COMMAND test_player_span)
  # test tool: a native node that records what it receives, with the
  # transport frame of every cycle (tests/player-proof)
  add_executable(backline-test-tap ${PROJECT_SOURCE_DIR}/tests/tools/test-tap.c)
  target_link_libraries(backline-test-tap PRIVATE PkgConfig::PIPEWIRE m)
endif()
