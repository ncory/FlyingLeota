# The motion core from core/, unchanged. The simulated rig (plant) comes too: the
# "simulated drives" option flies it on the board, for the bench.
set(core ${CMAKE_CURRENT_LIST_DIR}/../../../core)
idf_component_register(
    SRCS ${core}/src/config.cpp ${core}/src/geometry.cpp ${core}/src/profile.cpp
         ${core}/src/wind.cpp ${core}/src/rig.cpp ${core}/src/scmp.cpp
         ${core}/src/settings.cpp ${core}/src/calibration.cpp ${core}/src/line_test.cpp
         ${core}/src/plant.cpp
    INCLUDE_DIRS ${core}/include
    PRIV_INCLUDE_DIRS ${core}/third_party/ArduinoJson)
