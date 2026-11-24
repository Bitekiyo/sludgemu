// Xbox system DLLs claimed by sludgemu that don't have any implemented exports yet. Every import
// from these resolves to a logging stub. Modules move into their own files as they get real
// implementations.

#include "hle/hle.h"

HLE_EMPTY_MODULE(g_hle_module_xmem, "xmem.dll");             // title memory / physical pages
HLE_EMPTY_MODULE(g_hle_module_d3d12_xs, "d3d12_xs.dll");     // Scarlett D3D12 runtime
HLE_EMPTY_MODULE(g_hle_module_xg_xs, "xg_xs.dll");           // texture layout / tiling
HLE_EMPTY_MODULE(g_hle_module_chad, "chad.dll");             // audio DSP
HLE_EMPTY_MODULE(g_hle_module_xcrdapi, "xcrdapi.dll");       // DirectStorage (console)
HLE_EMPTY_MODULE(g_hle_module_xapu, "xapu.dll");             // audio processing unit
HLE_EMPTY_MODULE(g_hle_module_acphal, "acphal.dll");         // audio control processor HAL
HLE_EMPTY_MODULE(g_hle_module_gameinput, "gameinput.dll");   // GameInput (console flavour)

// Bundled with titles, but they drive console-only GPU features, so we replace them.
HLE_EMPTY_MODULE(g_hle_module_xboxbvhbuilder, "xboxbvhbuilder.dll");
HLE_EMPTY_MODULE(g_hle_module_ffx_loader, "amd_fidelityfx_loader_xbox.dll");
