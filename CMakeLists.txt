cmake_minimum_required(VERSION 3.21)
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(CMAKE_OSX_ARCHITECTURES "arm64")
else()
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(BasuInputBuffer VERSION 3.3.0)

# Recursive: src/mic/ holds the Mic Clicks engine (DSP, audio backend, glue).
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Mic Clicks: the engine runs on its own threads (std::thread + miniaudio's device thread).
# Geode's setup_geode_mod() uses the plain target_link_libraries signature, so keep it plain here.
find_package(Threads)
if (TARGET Threads::Threads)
    target_link_libraries(${PROJECT_NAME} Threads::Threads)
endif()

# miniaudio links the platform audio API at build time on Windows (WASAPI/DSound/WinMM).
if (WIN32)
    target_link_libraries(${PROJECT_NAME} ole32 winmm uuid)
endif()

# iOS: miniaudio's CoreAudio backend talks to AVAudioSession, which is Objective-C, so that
# translation unit is compiled as Objective-C++ there. Android (AAudio/OpenSL) is plain C.
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    set(MIC_AUDIO_BACKEND "${CMAKE_CURRENT_SOURCE_DIR}/src/mic/AudioBackend.cpp")
    if (EXISTS "${MIC_AUDIO_BACKEND}")
        # The mod's precompiled header is C++; an Objective-C++ source cannot reuse it.
        set_source_files_properties("${MIC_AUDIO_BACKEND}" PROPERTIES
            COMPILE_OPTIONS "-x;objective-c++"
            SKIP_PRECOMPILE_HEADERS ON)
    endif()
    # The mod is loaded into the game, so GD/cocos symbols are resolved at load time.
    target_link_options(${PROJECT_NAME} PRIVATE "-undefined" "dynamic_lookup")
    target_link_libraries(${PROJECT_NAME}
        "-framework AVFoundation"
        "-framework AudioToolbox"
        "-framework CoreAudio"
        "-framework CoreFoundation"
        "-framework Foundation")
endif()

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# iOS: Geode's generated bindings source uses FMOD types without including the FMOD header
# (fixed in the bindings repo later); force-include it so the iOS build compiles
if (("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS) AND TARGET GeodeBindings)
    target_compile_options(GeodeBindings PRIVATE -include Geode/fmod/fmod.hpp)
endif()

# iOS: the mod's dylib has to carry a code signature or some iOS launchers refuse to load it
# ("missing code signature"); sign it ad-hoc right after it's built, before it's packaged
if ("${CMAKE_SYSTEM_NAME}" STREQUAL "iOS" OR IOS)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND codesign --force --sign - $<TARGET_FILE:${PROJECT_NAME}>
        COMMENT "Signing the iOS dylib (ad-hoc)")
endif()

setup_geode_mod(${PROJECT_NAME})
