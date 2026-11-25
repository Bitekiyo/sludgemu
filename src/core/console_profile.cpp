#include "core/console_profile.h"

#include <windows.h>
#include <dxgi1_6.h>

#include "common/log.h"

#include "core/title.h"

static const ConsoleProfile k_profiles[] = {
    // Standard: 10 GB at full GPU bandwidth; Advanced adds 3 GB of standard-bandwidth memory.
    {CONSOLE_SERIES_X, "Xbox Series X", GiB(16), GiB(10), GiB(13), 0x64},
    // Standard: 7904 MiB GPU-optimal + 94 MiB standard; Advanced adds 574 MiB.
    {CONSOLE_SERIES_S, "Xbox Series S", GiB(10), MiB(7998), MiB(7998 + 574), 0x60},
};

static const ConsoleProfile* g_profile = &k_profiles[0];

void console_profile_select(ConsoleModel model) {
    g_profile = &k_profiles[model == CONSOLE_SERIES_S ? 1 : 0];
}

const ConsoleProfile& console_profile() {
    return *g_profile;
}

u64 console_title_memory() {
    bool advanced = g_profile->model == CONSOLE_SERIES_S ? g_title.advanced_memory_series_s
                                                         : g_title.advanced_memory_series_x;
    return advanced ? g_profile->title_memory_advanced : g_profile->title_memory_standard;
}

// Below these, a Series X title's working set doesn't fit beside the OS and our own memory.
#define HOST_RAM_FOR_SERIES_X  GiB(24)
#define HOST_VRAM_FOR_SERIES_X GiB(10)

// Dedicated memory of the GPU we will render on (the high-performance hardware adapter), or 0.
static u64 host_vram() {
    IDXGIFactory6* factory = nullptr;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        return 0;
    u64 vram = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
         i++) {
        DXGI_ADAPTER_DESC1 desc;
        bool hardware = SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        adapter->Release();
        if (hardware) {
            vram = desc.DedicatedVideoMemory;
            break;
        }
    }
    factory->Release();
    return vram;
}

ConsoleModel console_model_for_host() {
    MEMORYSTATUSEX ms = {sizeof(ms)};
    u64 ram = GlobalMemoryStatusEx(&ms) ? ms.ullTotalPhys : 0;
    u64 vram = host_vram();
    // Installed RAM reads a little under the module size (firmware and GPU reservations).
    bool big = ram + GiB(1) >= HOST_RAM_FOR_SERIES_X && vram + MiB(512) >= HOST_VRAM_FOR_SERIES_X;
    LOG_I("console", "host: %llu MiB RAM, %llu MiB VRAM -> %s", ram >> 20, vram >> 20,
          big ? "Xbox Series X" : "Xbox Series S");
    return big ? CONSOLE_SERIES_X : CONSOLE_SERIES_S;
}

bool console_model_parse(const wchar_t* name, ConsoleModel* out) {
    if (!_wcsicmp(name, L"series-x") || !_wcsicmp(name, L"x")) {
        *out = CONSOLE_SERIES_X;
        return true;
    }
    if (!_wcsicmp(name, L"series-s") || !_wcsicmp(name, L"s")) {
        *out = CONSOLE_SERIES_S;
        return true;
    }
    return false;
}
