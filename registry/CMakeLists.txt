# backline-registry: the system plugin index. SPDX-License-Identifier: MIT
pkg_check_modules(LILV IMPORTED_TARGET lilv-0)
pkg_check_modules(CRYPTO REQUIRED IMPORTED_TARGET libcrypto)

# One plugin per process: the probe the registry runs inside a sandbox scope.
add_executable(backline-probe probe.c)
target_include_directories(backline-probe PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)
target_link_libraries(backline-probe PRIVATE ${CMAKE_DL_LIBS} m)
if(LILV_FOUND)
  target_compile_definitions(backline-probe PRIVATE BL_HAVE_LILV=1)
  target_link_libraries(backline-probe PRIVATE PkgConfig::LILV)
else()
  message(STATUS "lilv not found: backline-probe is built without LV2 support")
endif()

# Pure logic (grades, cache keys, filters, products, hashing): unit-tested.
add_library(bl_registry STATIC reglib.c)
target_include_directories(bl_registry PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bl_registry PUBLIC PkgConfig::JSONC PkgConfig::CRYPTO)

add_executable(backline-registry registry.c runner.c)
target_link_libraries(backline-registry PRIVATE bl_registry PkgConfig::SYSTEMD PkgConfig::JSONC)
target_compile_definitions(backline-registry PRIVATE
  BL_PROBE_BUILD_PATH="$<TARGET_FILE:backline-probe>")
add_dependencies(backline-registry backline-probe)

install(TARGETS backline-probe backline-registry RUNTIME DESTINATION bin)

# Test plugins with known behaviour (ok, crash, hang), as .clap files.
foreach(kind OK CRASH HANG)
  string(TOLOWER ${kind} lower)
  add_library(backline-test-${lower} MODULE ${PROJECT_SOURCE_DIR}/tests/plugins/test-plugins.c)
  target_compile_definitions(backline-test-${lower} PRIVATE BL_TEST_${kind}=1)
  target_include_directories(backline-test-${lower} PRIVATE ${PROJECT_SOURCE_DIR}/third_party/clap/include)
  target_link_libraries(backline-test-${lower} PRIVATE m)
  set_target_properties(backline-test-${lower} PROPERTIES PREFIX "" SUFFIX ".clap"
    C_VISIBILITY_PRESET hidden
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test-plugins)
endforeach()

if(BUILD_TESTING)
  add_executable(test_registry ${PROJECT_SOURCE_DIR}/tests/unit/test_registry.c)
  target_link_libraries(test_registry PRIVATE bl_registry)
  add_test(NAME registry-unit COMMAND test_registry)
  # The probe against the test plugins: no PipeWire, no D-Bus, no sandbox.
  add_test(NAME registry-probe-ok COMMAND backline-probe clap-check
    ${CMAKE_BINARY_DIR}/test-plugins/backline-test-ok.clap backline.test.sine)
  # A whole scan of the test plugins in the fallback sandbox (no systemd
  # needed): the crash and the hang are contained and graded, and the
  # helper processes they start are killed (the registry is a subreaper).
  add_test(NAME registry-scan-contained COMMAND ${CMAKE_COMMAND} -E env
    BACKLINE_CACHE_DIR=${CMAKE_BINARY_DIR}/test-registry-cache
    $<TARGET_FILE:backline-registry> scan --force --no-sandbox --only-paths --timeout 2
    --path clap:${CMAKE_BINARY_DIR}/test-plugins)
  set_tests_properties(registry-scan-contained PROPERTIES TIMEOUT 30
    PASS_REGULAR_EXPRESSION "1 crashed, 1 timed out, 0 errors.*clap +4 +2 +0 +2 +0")
endif()
