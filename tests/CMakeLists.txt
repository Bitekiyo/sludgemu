# Unit tests for platform-independent parts of sludgemu. Run with ctest or directly.
add_executable(sludgemu_tests
    memory_test.cpp
    ${PROJECT_SOURCE_DIR}/src/common/log.cpp
    ${PROJECT_SOURCE_DIR}/src/core/memory.cpp
)
target_include_directories(sludgemu_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(sludgemu_tests PRIVATE sludge_options)
add_test(NAME memory COMMAND sludgemu_tests)
