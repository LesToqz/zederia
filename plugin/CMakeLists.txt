# The plug-in "Zederia": VST3 and Standalone, plus CLAP through the community extensions
# (docs/ENVIRONMENT.md). The processor wraps zederia_core; the interface is in ui/.
juce_add_plugin(Zederia
    PRODUCT_NAME "Zederia"
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "Zederia"
    COMPANY_COPYRIGHT "Copyright (C) 2026 LesToqz. Free software under the GNU AGPL v3 or later."
    DESCRIPTION "Wavetable synthesizer"
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

# The theme (Graphite, the only one) is a token file compiled in as text.
set(ZED_THEMES graphite)
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
    PluginProcessor.cpp PluginProcessor.h ProcessorParts.h ProcessorEditing.cpp AgentLink.cpp AgentLink.h
    Session.cpp Session.h Prefs.cpp Prefs.h
    PluginEditor.cpp PluginEditor.h
    ui/Theme.cpp ui/Theme.h
    ui/Context.cpp ui/Context.h
    ui/HostKeys.cpp ui/HostKeys.h
    ui/Widgets.cpp ui/Widgets.h
    ui/CurveEditor.cpp ui/CurveEditor.h
    ui/ContentMenu.cpp ui/ContentMenu.h
    ui/Brand.cpp ui/Brand.h
    ui/Displays.cpp ui/Displays.h
    ui/Audit.cpp ui/Audit.h
    ui/Shell.cpp ui/Shell.h
    ui/OutputMonitor.cpp ui/OutputMonitor.h
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
zed_strict_sources(PluginProcessor.cpp ProcessorEditing.cpp AgentLink.cpp Session.cpp Prefs.cpp PluginEditor.cpp
    ui/Theme.cpp ui/Context.cpp ui/HostKeys.cpp ui/Widgets.cpp ui/CurveEditor.cpp ui/ContentMenu.cpp ui/Brand.cpp ui/Displays.cpp ui/Audit.cpp ui/Shell.cpp ui/OutputMonitor.cpp
    ui/Dock.cpp ui/ModOverlay.cpp ui/Palette.cpp ui/SynthPage.cpp ui/FxPage.cpp ui/ModPage.cpp ui/TablePage.cpp ui/PlayPage.cpp
    ui/LibraryPage.cpp ui/AgentPage.cpp ui/KitPage.cpp)

target_compile_definitions(Zederia PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=0)
target_link_libraries(Zederia
    PRIVATE zederia_core zederia_link juce::juce_audio_utils
    PUBLIC juce::juce_recommended_config_flags)

if(ZED_BUILD_CLAP)
    clap_juce_extensions_plugin(TARGET Zederia
        CLAP_ID "com.zederia.zederia"
        CLAP_FEATURES instrument synthesizer)
endif()

# The logo as the icon of the Standalone (window, taskbar, Start menu) and of the plug-in files, and
# as the VST3 bundle's folder icon (Plugin.ico and desktop.ini at its root, as the VST3 bundle
# layout describes; the folder gets the system attribute, which makes Explorer read desktop.ini).
zed_add_icon_resource(Zederia_Standalone)
zed_add_icon_resource(Zederia_VST3)
zed_set_plugin_version_resource(Zederia "Zederia wavetable synthesizer")
if(TARGET Zederia_CLAP) # the framework writes no version resource for the CLAP file: this one has the icon too
    zed_add_version_resource(Zederia_CLAP "Zederia (CLAP plug-in)" "Zederia.clap" dll)
endif()
if(WIN32 AND TARGET Zederia_VST3)
    set(zed_vst3_bundle "$<TARGET_FILE_DIR:Zederia_VST3>/../..")
    add_custom_command(TARGET Zederia_VST3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "${ZED_ICON}" "${zed_vst3_bundle}/Plugin.ico"
        COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/brand/desktop.ini" "${zed_vst3_bundle}/desktop.ini"
        COMMAND attrib +s "$<SHELL_PATH:${zed_vst3_bundle}>"
        VERBATIM)
endif()
