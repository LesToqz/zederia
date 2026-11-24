# Tests of the plug-in processor and the editor, without a host. They link the plug-in's shared
# code, so the framework is not compiled a second time, and run under the same audio-thread guard
# as the core tests. The editor tests write their snapshots to build/<preset>/snapshots.
add_executable(zederia_plugin_tests ../support/TestMain.cpp test_processor.cpp test_editor.cpp test_editor_states.cpp test_agent_link.cpp test_host_keys.cpp)
zed_strict_sources(../support/TestMain.cpp test_processor.cpp test_editor.cpp test_editor_states.cpp test_agent_link.cpp test_host_keys.cpp)
target_compile_definitions(zederia_plugin_tests PRIVATE
    ZED_TEST_WITH_GUI=1
    ZED_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
    ZED_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content"
    ZED_SNAPSHOT_DIR="${CMAKE_BINARY_DIR}/snapshots"
    $<TARGET_PROPERTY:Zederia,COMPILE_DEFINITIONS>)
target_include_directories(zederia_plugin_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin
    $<TARGET_PROPERTY:Zederia,INCLUDE_DIRECTORIES>)
target_link_libraries(zederia_plugin_tests PRIVATE Zederia zederia_core zederia_mcp_lib Catch2::Catch2)
catch_discover_tests(zederia_plugin_tests TEST_PREFIX "plugin." TEST_SPEC "~[perf]" PROPERTIES TIMEOUT 600)
# The interface and workshop measurements run alone: tests running beside them would be counted
# as the interface's time.
catch_discover_tests(zederia_plugin_tests TEST_PREFIX "plugin." TEST_SPEC "[perf]" PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
