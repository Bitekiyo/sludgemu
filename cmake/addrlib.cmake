# AMD's GPU address library (addrlib), from Mesa: computes texture layouts and swizzle patterns
# for AMD GPUs. Console texture layouts are RDNA2 layouts, and the console's own layout library is
# built on addrlib, so using it is what reproduces console layouts exactly. Only the addrlib
# directory of the Mesa tree is downloaded.

FetchContent_Declare(addrlib
    URL "https://gitlab.freedesktop.org/mesa/mesa/-/archive/mesa-25.1.0/mesa-mesa-25.1.0.tar.gz?path=src/amd/addrlib"
    URL_HASH SHA256=b8fbe221c6c06cbc58757c0f909cfd14ee1ca0bf30d70a459de52f62698d2c72
    DOWNLOAD_NAME addrlib-mesa-25.1.0.tar.gz)
FetchContent_MakeAvailable(addrlib)

set(ADDRLIB_DIR ${addrlib_SOURCE_DIR}/src/amd/addrlib)
file(GLOB ADDRLIB_SOURCES
    ${ADDRLIB_DIR}/src/*.cpp
    ${ADDRLIB_DIR}/src/core/*.cpp
    ${ADDRLIB_DIR}/src/gfx9/*.cpp
    ${ADDRLIB_DIR}/src/gfx10/*.cpp
    ${ADDRLIB_DIR}/src/gfx11/*.cpp
    ${ADDRLIB_DIR}/src/gfx12/*.cpp
    ${ADDRLIB_DIR}/src/r800/*.cpp)

add_library(addrlib STATIC ${ADDRLIB_SOURCES})
target_include_directories(addrlib
    PUBLIC ${ADDRLIB_DIR}/inc
    PRIVATE ${ADDRLIB_DIR}/src ${ADDRLIB_DIR}/src/core ${ADDRLIB_DIR}/src/chip/gfx9 ${ADDRLIB_DIR}/src/chip/r800
            ${ADDRLIB_DIR}/src/chip/gfx10 ${ADDRLIB_DIR}/src/chip/gfx11 ${ADDRLIB_DIR}/src/chip/gfx12)
# Same configuration as Mesa's build: no regparm calling convention, little endian, no asserts.
target_compile_definitions(addrlib PUBLIC ADDR_FASTCALL= LITTLEENDIAN_CPU PRIVATE DEBUG=0)
target_compile_options(addrlib PRIVATE /w)
