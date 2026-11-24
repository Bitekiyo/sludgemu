cmake_minimum_required(VERSION 3.24)
project(sludgemu LANGUAGES C CXX)

if(NOT MSVC)
    message(FATAL_ERROR "sludgemu currently requires MSVC (the title ABI is MSVC x64).")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "sludgemu must be built for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# We build without C++ exceptions; drop CMake's default /EHsc so it doesn't fight /EHs-c-.
string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

# Static CRT: our own code must not share CRT state with the title's (host) CRT DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Common compile options for everything we build.
add_library(sludge_options INTERFACE)
target_compile_options(sludge_options INTERFACE
    /W4 /WX- /permissive- /utf-8 /Zi /Zc:__cplusplus /Zc:preprocessor
    /GR-          # no RTTI
    /EHs-c-       # no C++ exceptions
    /wd4100       # unreferenced parameter (stubs have lots of these)
    /wd4201       # nameless struct/union
)
target_compile_definitions(sludge_options INTERFACE
    _HAS_EXCEPTIONS=0
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _CRT_SECURE_NO_WARNINGS
)
target_link_options(sludge_options INTERFACE /DEBUG:FULL)

add_subdirectory(src)
