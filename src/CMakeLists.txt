# sludge_tls.dll: owns an implicit TLS slot for the manually mapped title (see tls/sludge_tls.h).
add_library(sludge_tls SHARED
    tls/sludge_tls.cpp
    tls/sludge_tls.h
)
target_compile_definitions(sludge_tls PRIVATE SLUDGE_TLS_BUILD)
target_include_directories(sludge_tls PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sludge_tls PRIVATE sludge_options)

add_executable(sludgemu WIN32
    main.cpp

    common/types.h
    common/log.h
    common/log.cpp
    common/str.h
    common/str.cpp

    core/title.h
    core/title.cpp

    loader/pe_image.h
    loader/pe_image.cpp
    loader/loader.h
    loader/loader.cpp
    loader/stubs.h
    loader/stubs.cpp
    loader/crash.h
    loader/crash.cpp

    hle/hle.h
    hle/hle.cpp
    hle/xbox_modules.cpp
    hle/kernel/overrides.h
    hle/kernel/module_overrides.cpp

    platform/window.h
    platform/window.cpp
)
target_include_directories(sludgemu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sludgemu PRIVATE sludge_options sludge_tls shell32 user32)

# Keep our own image far away from where titles want to live (0x140000000 and up), so the title
# can be mapped at its preferred base and addresses match the disassembler.
target_link_options(sludgemu PRIVATE /BASE:0x7FF600000000 /DYNAMICBASE /HIGHENTROPYVA /LARGEADDRESSAWARE)
