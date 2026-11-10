cmake_minimum_required(VERSION 3.25)
project(ZedSmoke VERSION 0.0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(${CMAKE_CURRENT_LIST_DIR}/../Versions.cmake)
include(FetchContent)
FetchContent_Declare(juce GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG ${ZED_JUCE_TAG} GIT_SHALLOW TRUE)
FetchContent_Declare(catch2 GIT_REPOSITORY https://github.com/catchorg/Catch2.git GIT_TAG ${ZED_CATCH2_TAG} GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(juce catch2)

juce_add_plugin(ZedSmokePlugin
  PRODUCT_NAME "ZedSmoke" COMPANY_NAME "Zederia" PLUGIN_MANUFACTURER_CODE Zedr PLUGIN_CODE Zsmk
  FORMATS VST3 Standalone IS_SYNTH TRUE NEEDS_MIDI_INPUT TRUE COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(ZedSmokePlugin PRIVATE plugin.cpp)
target_compile_definitions(ZedSmokePlugin PUBLIC JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_VST3_CAN_REPLACE_VST2=0)
target_link_libraries(ZedSmokePlugin PRIVATE juce::juce_audio_utils juce::juce_recommended_config_flags)

add_executable(zed_smoke_console console.cpp)
add_executable(zed_smoke_test test.cpp)
target_link_libraries(zed_smoke_test PRIVATE Catch2::Catch2WithMain)
enable_testing()
add_test(NAME smoke COMMAND zed_smoke_test)
