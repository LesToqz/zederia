# Tests of the plug-in processor and the temporary editor, without a host. They link the
# plug-in's shared code, so the framework is not compiled a second time, and run under the same
# audio-thread guard as the core tests.
add_executable(zederia_plugin_tests ../support/TestMain.cpp test_processor.cpp)
zed_strict_sources(../support/TestMain.cpp test_processor.cpp)
target_compile_definitions(zederia_plugin_tests PRIVATE
    ZED_TEST_WITH_GUI=1
    ZED_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../data"
    $<TARGET_PROPERTY:Zederia,COMPILE_DEFINITIONS>)
target_include_directories(zederia_plugin_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/plugin
    $<TARGET_PROPERTY:Zederia,INCLUDE_DIRECTORIES>)
target_link_libraries(zederia_plugin_tests PRIVATE Zederia zederia_core Catch2::Catch2)
catch_discover_tests(zederia_plugin_tests TEST_PREFIX "plugin." PROPERTIES TIMEOUT 300)
