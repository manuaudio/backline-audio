# Clock service. clockd is a JACK client of pipewire-jack, which is how a
# client owns PipeWire's position segment (the JACK timebase master) and
# drives its transport; every PipeWire node sees the result in
# spa_io_position. Ableton Link is GPL-2.0-or-later: with BACKLINE_LINK the
# clockd *binary* is a combined work under the GPL (docs/DECISIONS.md D3);
# its sources here stay MIT and nothing else links Link.
pkg_check_modules(JACK REQUIRED IMPORTED_TARGET jack)
pkg_check_modules(LTC REQUIRED IMPORTED_TARGET ltc)

add_library(bl_timecode STATIC timecode.c)
target_include_directories(bl_timecode PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

set(BACKLINE_LINK "AUTO" CACHE STRING "Ableton Link in clockd: ON (fetch if needed), OFF, AUTO (if BACKLINE_LINK_DIR is set)")
set(BACKLINE_ASIO_INCLUDE "" CACHE PATH "Standalone asio include dir (default: search the system)")
set(BACKLINE_LINK_DIR "" CACHE PATH "An Ableton Link source tree (Link-4.1.1); empty: fetch it when BACKLINE_LINK=ON")
set(_link OFF)
if(BACKLINE_LINK_DIR)
  set(_link ON)
elseif(BACKLINE_LINK STREQUAL "ON")
  include(FetchContent)
  FetchContent_Declare(abletonlink
    URL https://github.com/Ableton/link/archive/refs/tags/Link-4.1.1.tar.gz
    URL_HASH SHA256=23983a87eecc550be849598acb5219fbf1a16a5b4266a50ba4471b4f6c34f4a4)
  FetchContent_GetProperties(abletonlink)
  if(NOT abletonlink_POPULATED)
    FetchContent_Populate(abletonlink)
  endif()
  set(BACKLINE_LINK_DIR ${abletonlink_SOURCE_DIR})
  set(_link ON)
endif()

add_executable(backline-clockd clockd.cpp)
target_link_libraries(backline-clockd PRIVATE bl_timecode PkgConfig::JACK PkgConfig::LTC PkgConfig::SYSTEMD Threads::Threads)
if(_link)
  # Link is header-only. Release tarballs don't carry its asio submodule, so
  # use the system's standalone asio (Arch: asio, Debian: libasio-dev) unless
  # the tree has the submodule.
  set(_asio "${BACKLINE_LINK_DIR}/modules/asio-standalone/asio/include")
  if(NOT EXISTS "${_asio}/asio/version.hpp")
    find_path(_asio_sys asio/version.hpp HINTS ${BACKLINE_ASIO_INCLUDE} ENV CPATH)
    if(NOT _asio_sys)
      message(FATAL_ERROR "Ableton Link needs standalone asio headers (package asio / libasio-dev, or BACKLINE_ASIO_INCLUDE)")
    endif()
    set(_asio "${_asio_sys}")
  endif()
  add_library(bl_link INTERFACE)
  target_include_directories(bl_link SYSTEM INTERFACE ${BACKLINE_LINK_DIR}/include ${_asio})
  target_compile_definitions(bl_link INTERFACE LINK_PLATFORM_UNIX=1 LINK_PLATFORM_LINUX=1)
  target_link_libraries(bl_link INTERFACE Threads::Threads atomic)
  add_library(Ableton::Link ALIAS bl_link)
  target_link_libraries(backline-clockd PRIVATE Ableton::Link)
  target_compile_definitions(backline-clockd PRIVATE BL_HAVE_LINK=1)
  message(STATUS "clockd: with Ableton Link from ${BACKLINE_LINK_DIR} (the clockd binary is GPL-2.0-or-later)")
  # test peer for the Link proof (GPL too, test only)
  if(BUILD_TESTING)
    add_executable(backline-test-linkpeer ${PROJECT_SOURCE_DIR}/tests/tools/linkpeer.cpp)
    target_link_libraries(backline-test-linkpeer PRIVATE Ableton::Link PkgConfig::JACK Threads::Threads)
  endif()
else()
  message(STATUS "clockd: without Ableton Link (set BACKLINE_LINK=ON or BACKLINE_LINK_DIR)")
endif()
install(TARGETS backline-clockd RUNTIME DESTINATION bin)

if(BUILD_TESTING)
  add_executable(test_timecode ${PROJECT_SOURCE_DIR}/tests/unit/test_timecode.c)
  target_link_libraries(test_timecode PRIVATE bl_timecode PkgConfig::LTC)
  add_test(NAME timecode COMMAND test_timecode)
  # test tool: native PipeWire node that generates/decodes LTC and MTC and
  # logs the transport it sees in spa_io_position
  add_executable(backline-test-timecode ${PROJECT_SOURCE_DIR}/tests/tools/test-timecode.c)
  target_link_libraries(backline-test-timecode PRIVATE bl_timecode PkgConfig::LTC PkgConfig::PIPEWIRE m)
endif()
