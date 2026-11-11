# The plug-in "Zederia": VST3 and Standalone, plus CLAP through the community extensions
# (docs/ENVIRONMENT.md). The processor wraps zederia_core; the editor here is temporary.
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

set(ZED_PLUGIN_SOURCES PluginProcessor.cpp PluginProcessor.h PluginEditor.cpp PluginEditor.h)
target_sources(Zederia PRIVATE ${ZED_PLUGIN_SOURCES})
zed_strict_sources(PluginProcessor.cpp PluginEditor.cpp)

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
