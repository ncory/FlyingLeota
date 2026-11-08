cmake_minimum_required(VERSION 3.16)
project(leota_core LANGUAGES CXX)

# The motion core: plain C++17 with no hardware or OS calls, so the same code runs in
# the ESP32-P4 firmware, the PC simulator and the tests.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(leota_core
  src/config.cpp
  src/geometry.cpp
  src/profile.cpp
  src/wind.cpp
  src/rig.cpp
  src/scmp.cpp
  src/settings.cpp
  src/calibration.cpp)
target_include_directories(leota_core
  PUBLIC include
  PRIVATE third_party/ArduinoJson)

# The simulated rig, for tests and the PC simulator. The firmware doesn't link it.
add_library(leota_plant src/plant.cpp)
target_link_libraries(leota_plant PUBLIC leota_core)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(leota_core PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
  target_compile_options(leota_plant PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
endif()

option(LEOTA_TESTS "Build the tests" ON)
if(LEOTA_TESTS)
  enable_testing()
  add_executable(leota_core_tests
    tests/main.cpp
    tests/test_geometry.cpp
    tests/test_profile.cpp
    tests/test_wind.cpp
    tests/test_rig.cpp
    tests/test_scmp.cpp
    tests/test_settings.cpp
    tests/test_calibration.cpp)
  target_include_directories(leota_core_tests PRIVATE third_party/doctest third_party/ArduinoJson tests)
  target_link_libraries(leota_core_tests PRIVATE leota_core leota_plant)
  add_test(NAME leota_core_tests COMMAND leota_core_tests)
endif()
