cmake_minimum_required(VERSION 3.16)
project(flying_leota LANGUAGES CXX)

# Everything that builds on a PC: the motion core (with its tests) and the simulator.
add_subdirectory(core)
add_subdirectory(sim)
