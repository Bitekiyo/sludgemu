add_executable(sludgemu WIN32
    main.cpp

    common/types.h
    common/log.h
    common/log.cpp
    common/str.h
    common/str.cpp
    common/thread_slot.h

    core/title.h
    core/title.cpp
    core/memory.h
    core/memory.cpp
    core/vfs.h
    core/vfs.cpp

    loader/pe_image.h
    loader/pe_image.cpp
    loader/loader.h
    loader/loader.cpp
    loader/tls_reserve.h
    loader/tls_reserve.cpp
    loader/stubs.h
    loader/stubs.cpp
    loader/crash.h
    loader/crash.cpp

    hle/hle.h
    hle/hle.cpp
    hle/xbox_modules.cpp
    hle/kernel/overrides.h
    hle/kernel/module_overrides.cpp
    hle/kernel/memory_overrides.cpp
    hle/kernel/cpu_overrides.cpp
    hle/kernel/file_overrides.cpp
    hle/xmem/xmem.h
    hle/xmem/xmem.cpp
    hle/xmem/xmem_defs.h
    hle/d3d12/d3d12_xs.cpp
    hle/d3d12/d3d12_tables.h
    hle/d3d12/d3d12_tables.cpp
    hle/d3d12/console_object.h
    hle/d3d12/console_object.cpp
    hle/d3d12/device.h
    hle/d3d12/device.cpp
    hle/d3d12/d3d12_objects.h
    hle/d3d12/command_queue.cpp
    hle/d3d12/fence.cpp
    hle/d3d12/command_allocator.cpp
    hle/dstorage/dstorage.cpp

    gpu/agility.cpp
    gpu/gpu.h
    gpu/gpu.cpp
    hle/xgameruntime/runtime_api.h
    hle/xgameruntime/xgameruntime.h
    hle/xgameruntime/xgameruntime.cpp
    hle/xgameruntime/task_queue.h
    hle/xgameruntime/task_queue.cpp
    hle/xgameruntime/async.h
    hle/xgameruntime/async.cpp
    hle/xgameruntime/system.cpp
    hle/xgameruntime/user.cpp
    hle/xgameruntime/game_save.cpp
    hle/xgameruntime/package.cpp
    hle/xgameruntime/store.cpp
    hle/xgameruntime/services.cpp

    platform/window.h
    platform/window.cpp
)
target_include_directories(sludgemu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(sludgemu PRIVATE agility sludge_options shell32 user32 synchronization delayimp
    dstorage libdeflate::libdeflate_static d3d12 dxgi)

# Loaded only when a title uses DirectStorage.
target_link_options(sludgemu PRIVATE /DELAYLOAD:dstorage.dll)
add_custom_command(TARGET sludgemu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLUDGE_DSTORAGE_DLLS} $<TARGET_FILE_DIR:sludgemu>
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:sludgemu>/D3D12
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SLUDGE_AGILITY_DLLS} $<TARGET_FILE_DIR:sludgemu>/D3D12)

# sludgemu.exe's implicit TLS slot (index 0) belongs to the title exe (see loader/tls_reserve.h),
# so nothing else may use implicit TLS: no thread_local in our code, and no thread-safe static
# initialization (the CRT implements it with a thread_local epoch counter).
target_compile_options(sludgemu PRIVATE /Zc:threadSafeInit-)

# Keep our own image far away from where titles want to live (0x140000000 and up), so the title
# can be mapped at its preferred base and addresses match the disassembler.
target_link_options(sludgemu PRIVATE /BASE:0x7FF600000000 /DYNAMICBASE /HIGHENTROPYVA /LARGEADDRESSAWARE
    /INCREMENTAL:NO) # incremental linking pads section contributions, including the TLS reserve
