cmake_minimum_required(VERSION 3.16)
project(leota_sim LANGUAGES C CXX)

# The PC simulator: the motion core and a simulated rig, on SCMP over UDP, with a viewer.

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET leota_core)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../core ${CMAKE_CURRENT_BINARY_DIR}/core)
endif()

set(WEB_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/web_page.hpp)
add_custom_command(
  OUTPUT ${WEB_HEADER}
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/web/index.html -DOUT=${WEB_HEADER}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/index.html ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
  COMMENT "Embedding the viewer page")

add_executable(leota-sim src/main.cpp src/app.cpp src/net.cpp ${WEB_HEADER})
target_include_directories(leota-sim PRIVATE
  src ${CMAKE_CURRENT_BINARY_DIR}/generated ${CMAKE_CURRENT_SOURCE_DIR}/../core/third_party/ArduinoJson)
target_link_libraries(leota-sim PRIVATE leota_core leota_plant)
if(TARGET leota_ui)
  find_package(ZLIB REQUIRED)
  target_sources(leota-sim PRIVATE src/screen.cpp)
  target_link_libraries(leota-sim PRIVATE leota_ui ZLIB::ZLIB)
  target_compile_definitions(leota-sim PRIVATE LEOTA_SIM_UI=1)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(leota-sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
