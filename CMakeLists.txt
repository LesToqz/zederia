cmake_minimum_required(VERSION 3.25)
project(Zederia VERSION 1.0.0 LANGUAGES C CXX)
# The release is 1.0.0 plus this suffix ("-rc1": the first release candidate; "" for the release).
# Binaries carry the numeric version in their version resources and the full one in their text.
set(ZED_VERSION_SUFFIX "-rc1")
set(ZED_VERSION_FULL "${PROJECT_VERSION}${ZED_VERSION_SUFFIX}")

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
include(cmake/Resources.cmake)

add_subdirectory(core)
add_subdirectory(link) # the local pipe protocol shared by the plug-in and the agent server
if(ZED_BUILD_PLUGIN)
    add_subdirectory(plugin) # before the command-line tool: its ui-snapshot command uses the editor
endif()
add_subdirectory(cli)
add_subdirectory(mcp)
if(ZED_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
