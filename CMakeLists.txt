cmake_minimum_required(VERSION 3.21)

project(virtual-cable-microphone
        VERSION 0.1.0
        DESCRIPTION "Virtual cable microphone for ShadowPlay (Sound Blaster GC7)"
        LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "This project targets Windows only (WASAPI). Configure with MSVC, or cross-compile "
        "with a mingw-w64 toolchain file for syntax checking.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(VCMIC_STATIC_RUNTIME "Link the MSVC runtime statically (no VC++ redist needed)" ON)

if(MSVC AND VCMIC_STATIC_RUNTIME)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(vcmic
    src/main.cpp
    src/audio_format.cpp
    src/config.cpp
    src/console.cpp
    src/device_registry.cpp
    src/hresult.cpp
    src/logging.cpp
    src/paths.cpp
    src/strings.cpp
)

target_include_directories(vcmic PRIVATE src)

target_compile_definitions(vcmic PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
)

if(MSVC)
    target_compile_options(vcmic PRIVATE
        /W4 /permissive- /utf-8 /EHsc
        /Zc:__cplusplus /Zc:preprocessor /Zc:inline
        /MP
        $<$<CONFIG:Release>:/O2>
    )
    # Keep release builds debuggable: ship the .pdb next to the .exe.
    target_link_options(vcmic PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
else()
    target_compile_options(vcmic PRIVATE -Wall -Wextra)
endif()

if(MINGW)
    # MSVC picks wmainCRTStartup automatically; mingw needs to be told.
    target_link_options(vcmic PRIVATE -municode)
endif()

# ole32  : COM, CoTaskMemFree, PropVariantClear
# avrt   : AvSetMmThreadCharacteristics (MMCSS "Pro Audio" scheduling)
target_link_libraries(vcmic PRIVATE ole32 avrt)

set_target_properties(vcmic PROPERTIES
    OUTPUT_NAME "vcmic"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# The example config is handy right next to the executable while developing.
add_custom_command(TARGET vcmic POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/config.example.toml"
            "$<TARGET_FILE_DIR:vcmic>/config.example.toml"
    VERBATIM)
