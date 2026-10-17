cmake_minimum_required(VERSION 3.20)

project(ExplicitlyDesktop VERSION 1.0.0)

# C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JUCE Path (adjust if needed)
set(JUCE_DIR "C:/JUCE" CACHE PATH "Path to JUCE framework")

if(NOT EXISTS "${JUCE_DIR}/modules")
    message(FATAL_ERROR "JUCE not found at ${JUCE_DIR}. Please set JUCE_DIR to your JUCE installation path.")
endif()

# Add JUCE to the build
add_subdirectory(${JUCE_DIR} JUCE EXCLUDE_FROM_ALL)

# whisper.cpp source checkout and its build folder (adjust if needed).
# The app runs Whisper on the CPU (use_gpu = false), so a plain CPU build works:
#   cmake -B build -A x64 -DBUILD_SHARED_LIBS=ON && cmake --build build --config Release
set(WHISPER_SDK_DIR "C:/whisper.cpp" CACHE PATH "Path to whisper.cpp checkout")
set(WHISPER_BUILD_DIR "${WHISPER_SDK_DIR}/build" CACHE PATH "Path to whisper.cpp build folder")
# Runtime DLLs: the official CUDA build (same commit as the headers/import lib)
# from github.com/ggml-org/whisper.cpp releases, whisper-bin-win-cuda-12.4.0-x64.zip
if(EXISTS "C:/whisper-cuda/Release/whisper.dll")
    set(WHISPER_DLL_DIR_DEFAULT "C:/whisper-cuda/Release")
else()
    set(WHISPER_DLL_DIR_DEFAULT "${WHISPER_BUILD_DIR}/bin/Release")
endif()
set(WHISPER_DLL_DIR "${WHISPER_DLL_DIR_DEFAULT}" CACHE PATH "Folder with whisper/ggml runtime DLLs")

if(NOT EXISTS "${WHISPER_SDK_DIR}/include/whisper.h")
    message(FATAL_ERROR "Whisper SDK not found at ${WHISPER_SDK_DIR}. Please see WHISPER_SETUP.md")
endif()

# Include directories (whisper.h includes ggml headers)
include_directories(${WHISPER_SDK_DIR}/include ${WHISPER_SDK_DIR}/ggml/include)

# Source files
set(SOURCES
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/AudioEngine.cpp
    Source/WhisperThread.cpp
    Source/LyricsAlignment.cpp
    Source/VocalFilter.cpp
    Source/TimestampRefiner.cpp
    Source/QualityAnalyzer.cpp
    Source/SongRecognition.cpp
    Source/WindowsMediaInfo.cpp
    Source/ControlServer.cpp
    Source/LyricsGuide.cpp
)

# Create executable
juce_add_gui_app(ExplicitlyDesktop
    PRODUCT_NAME "Explicitly Desktop"
    COMPANY_NAME "Explicitly Audio Systems"
    BUNDLE_ID "com.explicitly.desktop"
    ICON_BIG ""
)

# Add source files
target_sources(ExplicitlyDesktop PRIVATE ${SOURCES})

# Add header files
target_include_directories(ExplicitlyDesktop PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
)

# Link JUCE modules
target_link_libraries(ExplicitlyDesktop
    PRIVATE
        juce::juce_core
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_gui_basics
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# Link Whisper library
if(WIN32)
    target_link_libraries(ExplicitlyDesktop PRIVATE
        ${WHISPER_BUILD_DIR}/src/Release/whisper.lib
        windowsapp.lib  # Windows Runtime for Media Control
        ws2_32.lib      # Sockets for the local control server
    )
    
    # Copy whisper/ggml DLLs (CUDA build if present, else the CPU build) next to the exe
    file(GLOB WHISPER_DLLS "${WHISPER_DLL_DIR}/*.dll")
    add_custom_command(TARGET ExplicitlyDesktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${WHISPER_DLLS}
            $<TARGET_FILE_DIR:ExplicitlyDesktop>
        COMMENT "Copying Whisper DLLs to output directory"
    )
elseif(APPLE)
    target_link_libraries(ExplicitlyDesktop PRIVATE
        ${WHISPER_SDK_DIR}/lib/libwhisper.dylib
    )
elseif(UNIX)
    target_link_libraries(ExplicitlyDesktop PRIVATE
        ${WHISPER_SDK_DIR}/lib/libwhisper.so
    )
endif()

# Copy Models folder to output directory
add_custom_command(TARGET ExplicitlyDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/Models
        $<TARGET_FILE_DIR:ExplicitlyDesktop>/Models
    COMMENT "Copying Models folder to output directory"
)

# Copy lexicons folder to output directory
add_custom_command(TARGET ExplicitlyDesktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        $<TARGET_FILE_DIR:ExplicitlyDesktop>/lexicons
    COMMAND ${CMAKE_COMMAND} -E copy
        ${CMAKE_CURRENT_SOURCE_DIR}/Models/profanity_en.txt
        $<TARGET_FILE_DIR:ExplicitlyDesktop>/lexicons/profanity_en.txt
    COMMENT "Copying lexicons to output directory"
)

# Compiler definitions
target_compile_definitions(ExplicitlyDesktop
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:ExplicitlyDesktop,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:ExplicitlyDesktop,JUCE_VERSION>"
)

# Windows-specific settings
if(WIN32)
    target_compile_definitions(ExplicitlyDesktop PRIVATE
        _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
    )
endif()

# Output directory
set_target_properties(ExplicitlyDesktop PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Phase 3: Standalone Whisper test executable
add_executable(WhisperTest EXCLUDE_FROM_ALL Source/WhisperTest.cpp)

target_include_directories(WhisperTest PRIVATE
    ${WHISPER_SDK_DIR}/include
    ${WHISPER_SDK_DIR}/ggml/include
)

if(WIN32)
    target_link_libraries(WhisperTest PRIVATE
        ${WHISPER_BUILD_DIR}/src/Release/whisper.lib
    )
endif()

# Copy Models folder to WhisperTest output directory
add_custom_command(TARGET WhisperTest POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${CMAKE_CURRENT_SOURCE_DIR}/Models
        $<TARGET_FILE_DIR:WhisperTest>/Models
    COMMENT "Copying Models folder to WhisperTest directory"
)

set_target_properties(WhisperTest PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

message(STATUS "===========================================")
message(STATUS "Explicitly Desktop Configuration")
message(STATUS "===========================================")
message(STATUS "JUCE Directory: ${JUCE_DIR}")
message(STATUS "Whisper SDK Directory: ${WHISPER_SDK_DIR}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Targets: ExplicitlyDesktop, WhisperTest")
message(STATUS "==========================================")
