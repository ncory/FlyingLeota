cmake_minimum_required(VERSION 3.16)
project(flying_leota LANGUAGES C CXX)

# Everything that builds on a PC: the motion core (with its tests), the touchscreen UI
# (LVGL is fetched with git unless LEOTA_LVGL_DIR points at a copy), and the simulator.
option(LEOTA_UI "Build the touchscreen UI and put it in the simulator" ON)
add_subdirectory(core)
if(LEOTA_UI)
  add_subdirectory(ui)
endif()
add_subdirectory(sim)
