# Unit and integration tests (plan 6.1). The test program replaces the global allocation
# functions so the audio-thread guard can see every allocation (support/TestMain.cpp).
add_executable(zederia_tests
    support/TestMain.cpp
    core/test_registry.cpp
    core/test_patch.cpp
    core/test_patch_fuzz.cpp
    core/test_voice_allocator.cpp
    core/test_engine.cpp
    core/test_pitch.cpp
    core/test_render.cpp
    core/test_rt_guard.cpp
    core/test_osc_quality.cpp
    core/test_osc.cpp
    core/test_wavetable.cpp
    core/test_toolkit.cpp
    core/test_formula.cpp
    core/test_import.cpp
    core/test_sources.cpp
    core/test_content.cpp
    core/test_filter.cpp
    core/test_modulators.cpp
    core/test_tuning.cpp
    core/test_matrix.cpp
    core/test_modulation.cpp
    core/test_voice.cpp
    core/test_patches.cpp
)
target_link_libraries(zederia_tests PRIVATE zederia_core Catch2::Catch2 zederia_warnings)
target_compile_definitions(zederia_tests PRIVATE
    ZED_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    ZED_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content")

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(zederia_tests TEST_PREFIX "core." PROPERTIES TIMEOUT 600)

# Command-line tools, end to end.
add_test(NAME cli.version COMMAND zederia-cli --version)
set_tests_properties(cli.version PROPERTIES PASS_REGULAR_EXPRESSION "zederia-cli ${PROJECT_VERSION}")
add_test(NAME mcp.version COMMAND zederia-mcp --version)
set_tests_properties(mcp.version PROPERTIES PASS_REGULAR_EXPRESSION "zederia-mcp ${PROJECT_VERSION}")
add_test(NAME cli.params_doc_is_current
         COMMAND zederia-cli params --format md --check ${CMAKE_SOURCE_DIR}/docs/PARAMETERS.md)
add_test(NAME cli.validate_good_patch
         COMMAND zederia-cli validate-patch ${CMAKE_CURRENT_SOURCE_DIR}/data/patches/glass_pluck.zpreset)
add_test(NAME cli.validate_bad_patch
         COMMAND zederia-cli validate-patch ${CMAKE_CURRENT_SOURCE_DIR}/data/patches/broken.zpreset)
set_tests_properties(cli.validate_bad_patch PROPERTIES WILL_FAIL TRUE)
add_test(NAME cli.render_is_deterministic
         COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}
                 -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/data/patches/glass_pluck.zpreset
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RenderTwice.cmake)
# The hand-written test patches render through the command-line tool, the same every time.
foreach(patch bass pad pluck lead)
    add_test(NAME cli.render_test_patch_${patch}
             COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/render_${patch}
                     -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/data/patches/${patch}.zpreset
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RenderTwice.cmake)
    add_test(NAME cli.validate_test_patch_${patch}
             COMMAND zederia-cli validate-patch ${CMAKE_CURRENT_SOURCE_DIR}/data/patches/${patch}.zpreset)
    set_tests_properties(cli.validate_test_patch_${patch} PROPERTIES FAIL_REGULAR_EXPRESSION "[1-9][0-9]* warning")
endforeach()
add_test(NAME cli.bench_runs COMMAND zederia-cli bench --seconds 0.5 --runs 1 --report-only)
add_test(NAME cli.table_commands
         COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/table_commands
                 -DDATA=${CMAKE_CURRENT_SOURCE_DIR}/data -DCONTENT=${CMAKE_SOURCE_DIR}/content
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TableCommands.cmake)
add_test(NAME core.no_raw_locks
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}/core -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NoRawLocks.cmake)

if(ZED_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()
