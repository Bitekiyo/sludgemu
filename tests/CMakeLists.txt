# Unit tests for platform-independent parts of sludgemu. Run with ctest or directly.
add_executable(sludgemu_tests
    test.h
    memory_test.cpp
    audio_test.cpp
    dsp_test.cpp
    xma_test.cpp
    ${PROJECT_SOURCE_DIR}/src/common/log.cpp
    ${PROJECT_SOURCE_DIR}/src/core/memory.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xapu.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xdsp.cpp
    ${PROJECT_SOURCE_DIR}/src/hle/audio/xma.cpp
)
target_include_directories(sludgemu_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sludgemu_tests PRIVATE sludge_options opus)
add_test(NAME memory COMMAND sludgemu_tests)
