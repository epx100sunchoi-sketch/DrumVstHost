cmake_minimum_required(VERSION 3.22)
project(DrumVstHost VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Link the VC++ runtime statically so the exe runs without any extra install.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(JUCE_DIR "E:/DevTools/JUCE" CACHE PATH "JUCE source folder")
add_subdirectory("${JUCE_DIR}" JUCE EXCLUDE_FROM_ALL)

juce_add_gui_app(DrumVstHost
    PRODUCT_NAME "DrumVstHost"
    COMPANY_NAME "DrumVstHost"
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE)

juce_generate_juce_header(DrumVstHost)

target_sources(DrumVstHost PRIVATE
    Source/Main.cpp
    Source/Engine.cpp
    Source/Take.cpp
    Source/TakeEditor.cpp
    Source/Look.cpp
    Source/MainComponent.cpp
    Source/Platform.cpp
    Source/SongTools.cpp
    Source/DrumModules.cpp
    Source/DrumPlugins.cpp
    Source/Mp3Writer.cpp
    Source/PadMap.cpp
    Source/Language.cpp
    Source/AnalysisTools.cpp)

target_compile_definitions(DrumVstHost PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_USE_MP3AUDIOFORMAT=1
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:DrumVstHost,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:DrumVstHost,JUCE_VERSION>")

if (MSVC)
    # Korean string literals in the sources are UTF-8.
    target_compile_options(DrumVstHost PRIVATE /utf-8)

    # Keep a .pdb next to the optimised exe so the crash report can name functions and lines.
    target_compile_options(DrumVstHost PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(DrumVstHost PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
endif()

option(DVH_ASAN "Diagnostics only: build with AddressSanitizer (no LTO)" OFF)

target_link_libraries(DrumVstHost
    PRIVATE
        juce::juce_audio_utils
        juce::juce_cryptography
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_audio_processors
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags)

if (DVH_ASAN AND MSVC)
    # Finds the exact line of a bad memory write (heap overflow, use after free).
    target_compile_options(DrumVstHost PRIVATE /fsanitize=address)
    target_link_options(DrumVstHost PRIVATE /INCREMENTAL:NO)
else()
    target_link_libraries(DrumVstHost PUBLIC juce::juce_recommended_lto_flags)
endif()
