# The Flying Leota controller's firmware, for the ESP32-P4 (ESP-IDF 5.5).
# The motion core, the touchscreen and the settings are the same code as the PC
# simulator's: see components/.
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS components)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(flying-leota)
