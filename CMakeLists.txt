cmake_minimum_required(VERSION 3.19)
project(SyncComms LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Build options
option(BUILD_PLUGIN     "Build the legacy BakkesMod plugin DLL" ON)
option(BUILD_STANDALONE "Build the standalone SyncCommsApp.exe" ON)

# BakkesMod SDK path
if(NOT DEFINED BAKKESMOD_SDK_PATH)
    set(BAKKESMOD_SDK_PATH "$ENV{APPDATA}/bakkesmod/bakkesmod/bakkesmodsdk" CACHE PATH "Path to BakkesMod SDK")
endif()

# Auto-disable plugin build if SDK not present
if(BUILD_PLUGIN AND NOT EXISTS "${BAKKESMOD_SDK_PATH}/include")
    message(STATUS "BakkesMod SDK not found at ${BAKKESMOD_SDK_PATH} — disabling plugin build.")
    set(BUILD_PLUGIN OFF)
endif()

# Source files
set(SOURCES
    src/SyncCommsPlugin.cpp
    src/SyncState.cpp
    src/BakkesModConfig.cpp
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

# ─────────────────────────────────────────────────────────────────────────────
# Legacy BakkesMod plugin DLL
# ─────────────────────────────────────────────────────────────────────────────
if(BUILD_PLUGIN)
    add_library(SyncComms SHARED ${SOURCES})

    target_include_directories(SyncComms PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external          # stb_vorbis.c
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

    target_compile_definitions(SyncComms PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )

    set_target_properties(SyncComms PROPERTIES
        OUTPUT_NAME "SyncComms"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins"
    )

    install(TARGETS SyncComms
        RUNTIME DESTINATION "$ENV{APPDATA}/bakkesmod/bakkesmod/plugins"
    )
    install(FILES data/synccomms.set
        DESTINATION "$ENV{APPDATA}/bakkesmod/bakkesmod/plugins/settings"
    )
    install(FILES data/synccomms.cfg
        DESTINATION "$ENV{APPDATA}/bakkesmod/bakkesmod/cfg"
    )

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
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Standalone SyncCommsApp.exe (post-BakkesMod, talks to RL Stats API)
# ─────────────────────────────────────────────────────────────────────────────
if(BUILD_STANDALONE)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)

    # webview library (modern C++ wrapper around WebView2 on Windows).
    # Its CMake project handles WebView2 SDK fetching (from the official NuGet)
    # automatically, so we don't have to vendor that.
    FetchContent_Declare(
        webview
        GIT_REPOSITORY https://github.com/webview/webview
        GIT_TAG        0.12.0
    )
    FetchContent_MakeAvailable(webview)

    add_executable(SyncCommsApp WIN32
        # Standalone-specific code
        src/standalone/main.cpp
        src/standalone/SettingsStore.cpp
        src/standalone/StatsApiClient.cpp
        src/standalone/StandaloneConfig.cpp
        src/standalone/CaptureOrchestrator.cpp
        src/standalone/PlaybackController.cpp
        src/standalone/ReplayMetadata.cpp
        src/standalone/ReplayLocator.cpp
        src/standalone/SegmentAnchoring.cpp
        src/standalone/RecordingMaintenance.cpp

        # Reused audio engine (was the BakkesMod plugin's working code)
        src/AudioSessionEnumerator.cpp
        src/AudioCaptureManager.cpp
        src/AudioPlaybackManager.cpp
        src/AudioCompressor.cpp
        src/AudioResampler.cpp
        src/SidecarManager.cpp
        src/SyncState.cpp
        src/WasapiCapture.cpp
        src/WavWriter.cpp
        src/miniaudio_impl.cpp

        # Vendored encoder / OGG container
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
    )

    target_include_directories(SyncCommsApp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external          # stb_vorbis.c
        ${CMAKE_CURRENT_SOURCE_DIR}/external/miniaudio
        ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/json/single_include/nlohmann
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libogg/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libogg/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libvorbis/include
        ${CMAKE_CURRENT_SOURCE_DIR}/external/libvorbis/lib
    )

    target_link_libraries(SyncCommsApp PRIVATE
        webview::core
        ole32
        oleaut32
        propsys
        advapi32
        version
        shell32
        ws2_32
        winmm
        avrt
        mmdevapi
    )

    target_compile_definitions(SyncCommsApp PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        UNICODE
        _UNICODE
    )

    set_target_properties(SyncCommsApp PROPERTIES
        OUTPUT_NAME "SyncCommsApp"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/standalone"
    )

    # After the exe builds, copy the built frontend (frontend/dist) next to it
    # as a `frontend/` directory so the exe can load index.html via file://.
    # Best-effort: skips silently if the user hasn't run `npm run build` yet.
    # The exe still launches — it'll either use SYNCCOMMS_DEV_URL (dev mode)
    # or display its built-in "frontend not found" fallback page.
    set(_frontend_dist "${CMAKE_CURRENT_SOURCE_DIR}/frontend/dist")
    set(_copy_script   "${CMAKE_CURRENT_BINARY_DIR}/copy_frontend.cmake")
    file(WRITE "${_copy_script}"
"set(SRC \"${_frontend_dist}\")
if(EXISTS \"\${SRC}/index.html\")
    file(REMOVE_RECURSE \"\${TARGET_DIR}/frontend\")
    file(MAKE_DIRECTORY \"\${TARGET_DIR}/frontend\")
    file(GLOB_RECURSE _files RELATIVE \"\${SRC}\" \"\${SRC}/*\")
    foreach(f \${_files})
        configure_file(\"\${SRC}/\${f}\" \"\${TARGET_DIR}/frontend/\${f}\" COPYONLY)
    endforeach()
    message(\"[SyncCommsApp] copied frontend -> \${TARGET_DIR}/frontend\")
else()
    message(\"[SyncCommsApp] frontend/dist not found - run 'cd frontend && npm run build' to populate\")
endif()
")
    add_custom_command(TARGET SyncCommsApp POST_BUILD
        COMMAND ${CMAKE_COMMAND}
                -DTARGET_DIR=$<TARGET_FILE_DIR:SyncCommsApp>
                -P "${_copy_script}"
        VERBATIM
    )
endif()

# Unit tests (optional, standalone executable — no BakkesMod SDK needed)
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    add_subdirectory(test)
endif()
