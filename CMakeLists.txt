cmake_minimum_required(VERSION 3.22)

set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Universal binary (Live native or under Rosetta)")

project(MTC4LIVE VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_PATH "$ENV{HOME}/JUCE/JUCE" CACHE PATH "Path to the JUCE checkout")
add_subdirectory(${JUCE_PATH} JUCE)

# Pure timecode maths, shared by the plugin and the unit tests
add_library(mtc_core STATIC Source/TimecodeMath.cpp)
target_include_directories(mtc_core PUBLIC Source)
set_target_properties(mtc_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

juce_add_plugin(MTC4Live
    COMPANY_NAME "CitizenOfTomorrow"
    BUNDLE_ID "com.citizenoftomorrow.mtc4live"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD TRUE        # installs into ~/Library/Audio/Plug-Ins
    PLUGIN_MANUFACTURER_CODE Ctzt
    PLUGIN_CODE Mtc4
    AU_MAIN_TYPE kAudioUnitType_Effect
    VST3_CATEGORIES Fx Tools
    FORMATS AU VST3
    PRODUCT_NAME "MTC4Live")

target_sources(MTC4Live
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
        Source/MtcSender.cpp
        Source/CoreMidiOutput.cpp)

target_compile_definitions(MTC4Live
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(MTC4Live
    PRIVATE
        mtc_core
        juce::juce_audio_utils
        juce::juce_audio_devices
        "-framework CoreMIDI"
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Unit tests (no JUCE)
enable_testing()
add_executable(mtc_tests Tests/TimecodeTests.cpp)
target_link_libraries(mtc_tests PRIVATE mtc_core)
add_test(NAME mtc_tests COMMAND mtc_tests)

# Same tests under AddressSanitizer + UBSan
add_executable(mtc_tests_san Tests/TimecodeTests.cpp Source/TimecodeMath.cpp)
target_compile_options(mtc_tests_san PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(mtc_tests_san PRIVATE -fsanitize=address,undefined)
add_test(NAME mtc_tests_san COMMAND mtc_tests_san)

# Dev tool: editor PNG snapshot + end-to-end MTC test over a virtual port
juce_add_console_app(MTC4LiveTool PRODUCT_NAME "MTC4LiveTool")
target_sources(MTC4LiveTool PRIVATE
    Tools/MTC4LiveTool.cpp
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/MtcSender.cpp
    Source/CoreMidiOutput.cpp)
target_compile_definitions(MTC4LiveTool PRIVATE
    JucePlugin_Name="MTC4Live" JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(MTC4LiveTool PRIVATE
    mtc_core juce::juce_audio_utils juce::juce_audio_devices "-framework CoreMIDI"
    juce::juce_recommended_config_flags)
