cmake_minimum_required(VERSION 3.22)
project(PD_Decelerating_Modulator VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    juce
    URL https://github.com/juce-framework/JUCE/archive/refs/tags/8.0.9.zip
)
FetchContent_MakeAvailable(juce)

juce_add_plugin(PD
    COMPANY_NAME "POURIA"
    BUNDLE_ID "com.pouria.pd"
    PLUGIN_MANUFACTURER_CODE Pour
    PLUGIN_CODE PdDc
    FORMATS VST3
    PRODUCT_NAME "PD - Decelerating Modulator"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
)

# juce_add_plugin() alone does NOT generate JuceHeader.h - must ask for it explicitly
# (same fix that was needed for PQ; #include <JuceHeader.h> has nothing to include otherwise).
juce_generate_juce_header(PD)

target_sources(PD PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
)

target_compile_definitions(PD PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(PD PRIVATE
    juce::juce_audio_utils
    juce::juce_dsp
)

if (MINGW)
    target_compile_options(PD PRIVATE -Wall -Wextra -Wpedantic)
endif()
