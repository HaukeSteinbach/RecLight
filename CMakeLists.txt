cmake_minimum_required(VERSION 3.22)

project(RecLight VERSION 1.0.0 LANGUAGES C CXX)
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Local dev checkouts symlink JUCE/ to a shared checkout (see the repo's JUCE
# symlink); CI machines won't have that path, so fetch a pinned JUCE release
# from GitHub instead when the symlink/checkout isn't present.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/JUCE/CMakeLists.txt")
    add_subdirectory(JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        8.0.14
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(JUCE)
endif()

# AU is Apple-only; Windows/Linux builds are VST3 + Standalone.
if(APPLE)
    set(RECLIGHT_FORMATS VST3 AU Standalone)
else()
    set(RECLIGHT_FORMATS VST3 Standalone)
endif()

juce_add_plugin(RecLight
    COMPANY_NAME                "Steinbach Audio"
    COMPANY_WEBSITE             ""
    COMPANY_EMAIL               ""
    LOCAL_NETWORK_PERMISSION_ENABLED TRUE
    LOCAL_NETWORK_PERMISSION_TEXT     "RecLight sends and receives UDP messages on your local WiFi network to find and control the RecLight ESP device."
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    COPY_PLUGIN_AFTER_BUILD     TRUE
    PLUGIN_MANUFACTURER_CODE    Stbh
    PLUGIN_CODE                 Onar
    FORMATS                     ${RECLIGHT_FORMATS}
    PRODUCT_NAME                "RecLight"
    BUNDLE_ID                   "com.steinbach-audio.reclight"
    VST3_CATEGORIES             "Fx" "Tools"
)

target_sources(RecLight
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_include_directories(RecLight
    PRIVATE
        Source
)

target_compile_definitions(RecLight
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(RecLight
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

juce_generate_juce_header(RecLight)
