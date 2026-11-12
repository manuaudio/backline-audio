cmake_minimum_required(VERSION 3.20)
project(backline VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3>=1.4)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile>=1.1)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)

# Shared graph helper (C, main-thread only).
add_library(bl_graph STATIC common/bl_graph.c)
target_include_directories(bl_graph PUBLIC common)
target_link_libraries(bl_graph PUBLIC PkgConfig::PIPEWIRE)

# Plugin host: one CLAP instance per process, as one PipeWire node.
add_executable(backline-plugin-worker host/worker.cpp)
target_include_directories(backline-plugin-worker PRIVATE third_party/clap/include)
target_link_libraries(backline-plugin-worker PRIVATE bl_graph PkgConfig::PIPEWIRE PkgConfig::SYSTEMD ${CMAKE_DL_LIBS})

# Recorder: N nodes into sample-aligned, timestamped files.
add_executable(backline-recorder recorder/recorder.c)
target_link_libraries(backline-recorder PRIVATE bl_graph PkgConfig::PIPEWIRE PkgConfig::SNDFILE m)

# Test tool: a virtual MIDI keyboard on the ALSA sequencer (what a USB
# keyboard looks like to PipeWire's MIDI bridge).
add_executable(backline-test-keyboard tools/test-keyboard.c)
target_link_libraries(backline-test-keyboard PRIVATE PkgConfig::ALSA)

install(TARGETS backline-plugin-worker backline-recorder RUNTIME DESTINATION bin)
