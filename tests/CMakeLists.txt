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
    core/test_factory.cpp
    core/test_filter.cpp
    core/test_modulators.cpp
    core/test_tuning.cpp
    core/test_matrix.cpp
    core/test_modulation.cpp
    core/test_voice.cpp
    core/test_patches.cpp
    core/test_fx.cpp
    core/test_rack.cpp
    core/test_output.cpp
    core/test_play.cpp
    core/test_golden.cpp
    core/test_editing.cpp
    core/test_analysis.cpp
    core/test_theory.cpp
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
foreach(patch bass pad pluck lead arp_fx)
    add_test(NAME cli.render_test_patch_${patch}
             COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/render_${patch}
                     -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/data/patches/${patch}.zpreset
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/RenderTwice.cmake)
    add_test(NAME cli.validate_test_patch_${patch}
             COMMAND zederia-cli validate-patch ${CMAKE_CURRENT_SOURCE_DIR}/data/patches/${patch}.zpreset)
    set_tests_properties(cli.validate_test_patch_${patch} PROPERTIES FAIL_REGULAR_EXPRESSION "[1-9][0-9]* warning")
endforeach()
add_test(NAME cli.bench_runs COMMAND zederia-cli bench --seconds 0.5 --runs 1 --report-only)
add_test(NAME cli.clip_commands
         COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/clip_commands
                 -DPATCH=${CMAKE_CURRENT_SOURCE_DIR}/data/patches/arp_fx.zpreset
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ClipCommands.cmake)
add_test(NAME cli.table_commands
         COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/table_commands
                 -DDATA=${CMAKE_CURRENT_SOURCE_DIR}/data -DCONTENT=${CMAKE_SOURCE_DIR}/content
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TableCommands.cmake)
add_test(NAME core.no_raw_locks
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}/core -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/NoRawLocks.cmake)

# The agent server (plan 6.1: protocol): scripted sessions per revision and transport, schemas,
# error paths, the sandbox, token and origin checks, cancellation, and the live link.
add_executable(zederia_mcp_tests support/TestMain.cpp mcp/test_mcp_protocol.cpp mcp/test_mcp_transport.cpp mcp/test_mcp_live.cpp)
target_link_libraries(zederia_mcp_tests PRIVATE zederia_mcp_lib Catch2::Catch2 zederia_warnings)
if(MSVC)
    target_compile_definitions(zederia_mcp_tests PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_definitions(zederia_mcp_tests PRIVATE ZED_MCP_EXE="$<TARGET_FILE:zederia-mcp>")
    target_compile_options(zederia_mcp_tests PRIVATE /bigobj)
endif()
if(ZED_BUILD_PLUGIN)
    target_compile_definitions(zederia_mcp_tests PRIVATE ZED_STANDALONE_EXE="$<TARGET_FILE:Zederia_Standalone>")
    add_dependencies(zederia_mcp_tests Zederia_Standalone)
endif()
add_dependencies(zederia_mcp_tests zederia-mcp)
catch_discover_tests(zederia_mcp_tests TEST_PREFIX "mcp." TEST_SPEC "~[standalone]" PROPERTIES TIMEOUT 600)
catch_discover_tests(zederia_mcp_tests TEST_PREFIX "mcp." TEST_SPEC "[standalone]" PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
# The real program over its standard streams, in both generations.
add_test(NAME mcp.stdio_process
         COMMAND ${CMAKE_COMMAND} -DMCP=$<TARGET_FILE:zederia-mcp> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/mcp_stdio
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/McpStdio.cmake)

if(ZED_BUILD_PLUGIN)
    add_subdirectory(plugin)
    # The snapshot tool end to end: one page at one scale, with the interface checks.
    add_test(NAME cli.ui_snapshot_audit
             COMMAND zederia-cli ui-snapshot --page synth --scale 100 --audit --out ${CMAKE_CURRENT_BINARY_DIR}/ui_snapshot)
endif()
# The factory content, end to end: every preset loads and plays within its limits (milestone 7).
# Renders take minutes in an unoptimised build, so the test runs in optimised builds only.
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_test(NAME cli.content_check
             COMMAND ${CMAKE_COMMAND} -DCLI=$<TARGET_FILE:zederia-cli> -DOUT=${CMAKE_CURRENT_BINARY_DIR}/content_check
                     -DCONTENT=${CMAKE_SOURCE_DIR}/content -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ContentCheck.cmake)
    set_tests_properties(cli.content_check PROPERTIES TIMEOUT 1800)
endif()
