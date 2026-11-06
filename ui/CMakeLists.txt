cmake_minimum_required(VERSION 3.16)
project(leota_ui LANGUAGES C CXX)

# The touchscreen UI, in LVGL 9.2. The same code runs on the ESP32-P4's 7 in panel and,
# for development, inside the PC simulator.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET leota_core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()

# LVGL: a local checkout if given, else fetched with git (tag v9.2.2).
set(LEOTA_LVGL_DIR "" CACHE PATH "A local LVGL 9.2.2 checkout (empty: fetch it with git)")
set(LV_CONF_PATH "${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_THORVG_INTERNAL ON CACHE BOOL "" FORCE)
if(LEOTA_LVGL_DIR)
  add_subdirectory(${LEOTA_LVGL_DIR} ${CMAKE_CURRENT_BINARY_DIR}/lvgl EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.2.2
    GIT_SHALLOW TRUE)
  FetchContent_GetProperties(lvgl)
  if(NOT lvgl_POPULATED)
    FetchContent_Populate(lvgl)
    add_subdirectory(${lvgl_SOURCE_DIR} ${lvgl_BINARY_DIR} EXCLUDE_FROM_ALL)
  endif()
endif()

add_library(leota_ui
  src/ui.cpp
  fonts/leota_inter_14.c
  fonts/leota_inter_16.c
  fonts/leota_inter_16_semi.c
  fonts/leota_inter_20_bold.c
  fonts/leota_inter_28_bold.c)
target_include_directories(leota_ui PUBLIC include)
target_link_libraries(leota_ui PUBLIC leota_core lvgl)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(leota_ui PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra>)
endif()

option(LEOTA_UI_TESTS "Build the UI tests" ON)
if(LEOTA_UI_TESTS)
  enable_testing()
  add_executable(leota_ui_tests tests/test_ui.cpp)
  target_include_directories(leota_ui_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core/third_party/doctest ${CMAKE_CURRENT_SOURCE_DIR}/../core/tests)
  target_link_libraries(leota_ui_tests PRIVATE leota_ui leota_plant)
  add_test(NAME leota_ui_tests COMMAND leota_ui_tests)
endif()
