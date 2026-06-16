add_executable(test_sync_algorithm
    test_sync_algorithm.cpp
    ${CMAKE_SOURCE_DIR}/src/standalone/SegmentAnchoring.cpp
    ${CMAKE_SOURCE_DIR}/src/standalone/ReplayMetadata.cpp
)

target_include_directories(test_sync_algorithm PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external/json
)

target_compile_definitions(test_sync_algorithm PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)

# Tests rely on assert() to verify. MSVC defines NDEBUG in Release configs,
# which strips every assert — undefine it so the suite actually checks.
if(MSVC)
    target_compile_options(test_sync_algorithm PRIVATE /UNDEBUG)
else()
    target_compile_options(test_sync_algorithm PRIVATE -UNDEBUG)
endif()
