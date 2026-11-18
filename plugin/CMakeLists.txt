# The plug-in "Zederia": VST3 and Standalone, plus CLAP through the community extensions
# (docs/ENVIRONMENT.md). The processor wraps zederia_core; the interface is in ui/.
juce_add_plugin(Zederia
    PRODUCT_NAME "Zederia"
    COMPANY_NAME "Zederia"
    BUNDLE_ID com.zederia.zederia
    PLUGIN_MANUFACTURER_CODE Zedr
    PLUGIN_CODE Zdra
    FORMATS VST3 Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Instrument Synth
    COPY_PLUGIN_AFTER_BUILD FALSE)

# Theme token files are compiled in as text (plan 3.13: "Themes are token files").
set(ZED_THEMES graphite paper contrast)
set(ZED_THEME_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/generated/ThemeData.cpp")
set(theme_text "// Generated from plugin/themes/*.json by plugin/CMakeLists.txt. Do not edit.\n#include <string_view>\nnamespace zui::themes {\n")
foreach(theme ${ZED_THEMES})
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/themes/${theme}.json theme_json)
    string(APPEND theme_text "extern const std::string_view ${theme};\nconst std::string_view ${theme} = R\"zedtheme(${theme_json})zedtheme\";\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/themes/${theme}.json)
endforeach()
string(APPEND theme_text "} // namespace zui::themes\n")
file(CONFIGURE OUTPUT ${ZED_THEME_SOURCE} CONTENT "${theme_text}")

set(ZED_PLUGIN_SOURCES
    PluginProcessor.cpp PluginProcessor.h ProcessorParts.h ProcessorEditing.cpp
    Session.cpp Session.h Prefs.cpp Prefs.h
    PluginEditor.cpp PluginEditor.h
    ui/Theme.cpp ui/Theme.h
    ui/Context.cpp ui/Context.h
    ui/Widgets.cpp ui/Widgets.h
    ui/CurveEditor.cpp ui/CurveEditor.h
    ui/Displays.cpp ui/Displays.h
    ui/Audit.cpp ui/Audit.h
    ui/Shell.cpp ui/Shell.h
    ui/Dock.cpp ui/Dock.h
    ui/ModOverlay.cpp ui/ModOverlay.h
    ui/Palette.cpp ui/Palette.h
    ui/SynthPage.cpp ui/SynthPage.h
    ui/FxPage.cpp ui/FxPage.h
    ui/ModPage.cpp ui/ModPage.h
    ui/TablePage.cpp ui/TablePage.h
    ui/PlayPage.cpp ui/PlayPage.h
    ui/LibraryPage.cpp ui/LibraryPage.h
    ui/AgentPage.cpp ui/AgentPage.h
    ui/KitPage.cpp ui/KitPage.h
    ${ZED_THEME_SOURCE})
target_sources(Zederia PRIVATE ${ZED_PLUGIN_SOURCES})
zed_strict_sources(PluginProcessor.cpp ProcessorEditing.cpp Session.cpp Prefs.cpp PluginEditor.cpp
    ui/Theme.cpp ui/Context.cpp ui/Widgets.cpp ui/CurveEditor.cpp ui/Displays.cpp ui/Audit.cpp ui/Shell.cpp
    ui/Dock.cpp ui/ModOverlay.cpp ui/Palette.cpp ui/SynthPage.cpp ui/FxPage.cpp ui/ModPage.cpp ui/TablePage.cpp ui/PlayPage.cpp
    ui/LibraryPage.cpp ui/AgentPage.cpp ui/KitPage.cpp)

target_compile_definitions(Zederia PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(Zederia
    PRIVATE zederia_core juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)

if(ZED_BUILD_CLAP)
    clap_juce_extensions_plugin(TARGET Zederia
        CLAP_ID "com.zederia.zederia"
        CLAP_FEATURES instrument synthesizer)
endif()
