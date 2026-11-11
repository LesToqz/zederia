# Fetches each dependency at its pinned version (cmake/Versions.cmake) into one source cache that
# all build folders share, so a second preset does not download again. Each build folder still
# compiles its own copy.
set(ZED_DEPS_DIR "${CMAKE_SOURCE_DIR}/build/_deps" CACHE PATH "Shared source cache for dependencies")
find_package(Git REQUIRED)

function(zed_git)
    execute_process(COMMAND ${GIT_EXECUTABLE} ${ARGN} RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "git ${ARGN} failed: ${err}")
    endif()
endfunction()

# zed_fetch(<name> <url> <ref> [SUBMODULES]) sets <name>_SOURCE_DIR.
# <ref> may be a tag or a full commit id. The folder name carries the ref, so changing a pin fetches anew.
function(zed_fetch name url ref)
    cmake_parse_arguments(ARG "SUBMODULES" "" "" ${ARGN})
    string(SUBSTRING "${ref}" 0 12 short)
    set(dir "${ZED_DEPS_DIR}/${name}-${short}")
    if(NOT EXISTS "${dir}/.zed-fetched")
        message(STATUS "Fetching ${name} ${ref}")
        file(REMOVE_RECURSE "${dir}")
        file(MAKE_DIRECTORY "${dir}")
        zed_git(-C "${dir}" init -q)
        zed_git(-C "${dir}" remote add origin "${url}")
        zed_git(-C "${dir}" -c advice.detachedHead=false fetch -q --depth 1 origin "${ref}")
        zed_git(-C "${dir}" -c advice.detachedHead=false checkout -q FETCH_HEAD)
        if(ARG_SUBMODULES)
            zed_git(-C "${dir}" submodule update -q --init --recursive --depth 1)
        endif()
        file(WRITE "${dir}/.zed-fetched" "${ref}\n")
    endif()
    set(${name}_SOURCE_DIR "${dir}" PARENT_SCOPE)
endfunction()

zed_fetch(nlohmann_json https://github.com/nlohmann/json.git ${ZED_NLOHMANN_JSON_TAG})
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
add_subdirectory(${nlohmann_json_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/nlohmann_json EXCLUDE_FROM_ALL SYSTEM)

if(ZED_BUILD_TESTS)
    zed_fetch(catch2 https://github.com/catchorg/Catch2.git ${ZED_CATCH2_TAG})
    add_subdirectory(${catch2_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/catch2 EXCLUDE_FROM_ALL SYSTEM)
endif()

if(ZED_BUILD_PLUGIN)
    zed_fetch(juce https://github.com/juce-framework/JUCE.git ${ZED_JUCE_TAG})
    add_subdirectory(${juce_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/juce EXCLUDE_FROM_ALL SYSTEM)
    if(ZED_BUILD_CLAP)
        zed_fetch(clap_juce_extensions https://github.com/free-audio/clap-juce-extensions.git
                  ${ZED_CLAP_JUCE_EXT_REF} SUBMODULES)
        add_subdirectory(${clap_juce_extensions_SOURCE_DIR} ${CMAKE_BINARY_DIR}/_deps/clap_juce_extensions
                         EXCLUDE_FROM_ALL SYSTEM)
    endif()
endif()

# cpp-httplib (${ZED_CPP_HTTPLIB_TAG}) is pinned for the agent bridge and is fetched when milestone 6 adds it.
