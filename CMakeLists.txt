cmake_minimum_required(VERSION 3.25)
project(Zederia VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Static C runtime: the plug-in must load on machines without the compiler's redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ZED_BUILD_PLUGIN "Build the plug-in (VST3, Standalone, CLAP)" ON)
option(ZED_BUILD_CLAP   "Add the CLAP format to the plug-in" ON)
option(ZED_BUILD_TESTS  "Build tests and benchmarks" ON)

if(MSVC)
    add_compile_options(/utf-8 /Zc:__cplusplus)
endif()

include(cmake/Versions.cmake)
include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(core)
if(ZED_BUILD_PLUGIN)
    add_subdirectory(plugin) # before the command-line tool: its ui-snapshot command uses the editor
endif()
add_subdirectory(cli)
add_subdirectory(mcp)
if(ZED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
