add_executable(test_sync_algorithm test_sync_algorithm.cpp)

target_include_directories(test_sync_algorithm PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/external/json
)

target_compile_definitions(test_sync_algorithm PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
)
