# Backline mixer: the larger window, opened from the Omarchy panels.
# A standalone Qt Quick app in the shape of Omarchy's own apps (one binary,
# themed live from Omarchy), built apart from the engine:
#   cmake -S ui -B build-ui && cmake --build build-ui
cmake_minimum_required(VERSION 3.21)
project(backline-mixer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 DBus Test QuickTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# Everything but main.cpp, shared by the app and the tests.
file(GLOB mixer_sources CONFIGURE_DEPENDS src/*.cpp src/*.h)
list(REMOVE_ITEM mixer_sources ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
add_library(mixer_core STATIC ${mixer_sources})
target_include_directories(mixer_core PUBLIC src)
target_link_libraries(mixer_core PUBLIC
  Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick Qt6::QuickControls2 Qt6::DBus PkgConfig::PIPEWIRE)

add_executable(backline-mixer src/main.cpp src/resources.qrc)
target_link_libraries(backline-mixer PRIVATE mixer_core)

enable_testing()
add_executable(mixer_tests tests/mixer_tests.cpp)
target_link_libraries(mixer_tests PRIVATE mixer_core Qt6::Test)
add_test(NAME mixer_tests COMMAND mixer_tests)

# QML component tests (Qt Quick Test), with the C++ types registered.
add_executable(qml_tests tests/qml_tests.cpp)
target_link_libraries(qml_tests PRIVATE mixer_core Qt6::QuickTest)
target_compile_definitions(qml_tests PRIVATE QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/qml")
add_test(NAME qml_tests COMMAND qml_tests)

install(TARGETS backline-mixer RUNTIME DESTINATION bin)
install(FILES pkgbuild/backline-mixer.desktop DESTINATION share/applications)
