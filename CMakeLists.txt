cmake_minimum_required(VERSION 3.15)
project(SyncComms LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# BakkesMod SDK path
if(NOT DEFINED BAKKESMOD_SDK_PATH)
    set(BAKKESMOD_SDK_PATH "$ENV{APPDATA}/bakkesmod/bakkesmod/bakkesmodsdk" CACHE PATH "Path to BakkesMod SDK")
endif()

# Source files
set(SOURCES
    src/SyncCommsPlugin.cpp
    src/SyncState.cpp
    src/Config.cpp
    src/WavWriter.cpp
    src/AudioCaptureManager.cpp
    src/AudioPlaybackManager.cpp
    src/AudioResampler.cpp
    src/SidecarManager.cpp
    src/UIOverlay.cpp
    src/WasapiCapture.cpp
    src/AudioSessionEnumerator.cpp
    src/miniaudio_impl.cpp
    src/AudioCompressor.cpp
    external/libogg/src/bitwise.c
    external/libogg/src/framing.c
    external/libvorbis/lib/analysis.c
    external/libvorbis/lib/bitrate.c
    external/libvorbis/lib/block.c
    external/libvorbis/lib/codebook.c
    external/libvorbis/lib/envelope.c
    external/libvorbis/lib/floor0.c
    external/libvorbis/lib/floor1.c
    external/libvorbis/lib/info.c
    external/libvorbis/lib/lookup.c
    external/libvorbis/lib/lpc.c
    external/libvorbis/lib/lsp.c
    external/libvorbis/lib/mapping0.c
    external/libvorbis/lib/mdct.c
    external/libvorbis/lib/psy.c
    external/libvorbis/lib/registry.c
    external/libvorbis/lib/res0.c
    external/libvorbis/lib/sharedbook.c
    external/libvorbis/lib/smallft.c
    external/libvorbis/lib/synthesis.c
    external/libvorbis/lib/vorbisenc.c
    external/libvorbis/lib/window.c
    external/imgui/imgui.cpp
    external/imgui/imgui_draw.cpp
    external/imgui/imgui_tables.cpp
    external/imgui/imgui_widgets.cpp
)

# Build as shared library (DLL)
add_library(SyncComms SHARED ${SOURCES})

target_include_directories(SyncComms PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio
    ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include/nlohmann
    ${CMAKE_CURRENT_SOURCE_DIR}/external/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake
    ${CMAKE_CURRENT_SOURCE_DIR}/external/libogg/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/libogg/src
    ${CMAKE_CURRENT_SOURCE_DIR}/external/libvorbis/include
    ${CMAKE_CURRENT_SOURCE_DIR}/external/libvorbis/lib
    ${BAKKESMOD_SDK_PATH}/include
)

target_link_directories(SyncComms PRIVATE
    ${BAKKESMOD_SDK_PATH}/lib
)

target_link_libraries(SyncComms PRIVATE
    pluginsdk
    winmm
    ole32
    avrt
    mmdevapi
    propsys
)

# Compile definitions
target_compile_definitions(SyncComms PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Set output name and directory
set_target_properties(SyncComms PROPERTIES
    OUTPUT_NAME "SyncComms"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
)

# Install targets
install(TARGETS SyncComms
    RUNTIME DESTINATION "$ENV{APPDATA}/bakkesmod/bakkesmod/plugins"
)
install(FILES data/synccomms.set
    DESTINATION "$ENV{APPDATA}/bakkesmod/bakkesmod/plugins/settings"
)
install(FILES data/synccomms.cfg
    DESTINATION "$ENV{APPDATA}/bakkesmod/bakkesmod/cfg"
)

# Auto-register in plugins.cfg if not already present
set(BAKKESMOD_CFG_DIR "$ENV{APPDATA}/bakkesmod/bakkesmod/cfg")
file(TO_CMAKE_PATH "${BAKKESMOD_CFG_DIR}" BAKKESMOD_CFG_DIR)
install(CODE "
    set(_plugins_cfg \"${BAKKESMOD_CFG_DIR}/plugins.cfg\")
    if(EXISTS \"\${_plugins_cfg}\")
        file(READ \"\${_plugins_cfg}\" _plugins_content)
        string(FIND \"\${_plugins_content}\" \"synccomms\" _found)
        if(_found EQUAL -1)
            file(APPEND \"\${_plugins_cfg}\" \"plugin load synccomms\\n\")
            message(STATUS \"Added 'plugin load synccomms' to plugins.cfg\")
        else()
            message(STATUS \"synccomms already in plugins.cfg\")
        endif()
    else()
        file(WRITE \"\${_plugins_cfg}\" \"plugin load synccomms\\n\")
        message(STATUS \"Created plugins.cfg with synccomms\")
    endif()
")

# Unit tests (optional, standalone executable — no BakkesMod SDK needed)
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
