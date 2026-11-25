// Xbox system DLLs claimed by sludgemu that don't have any implemented exports yet. Every import
// from these resolves to a logging stub. Modules move into their own files as they get real
// implementations.

#include "hle/hle.h"

HLE_EMPTY_MODULE(g_hle_module_chad, "chad.dll");             // audio DSP
HLE_EMPTY_MODULE(g_hle_module_xapu, "xapu.dll");             // audio processing unit
HLE_EMPTY_MODULE(g_hle_module_acphal, "acphal.dll");         // audio control processor HAL

// Bundled with titles, but they drive console-only GPU features, so we replace them.
HLE_EMPTY_MODULE(g_hle_module_xboxbvhbuilder, "xboxbvhbuilder.dll");
HLE_EMPTY_MODULE(g_hle_module_ffx_loader, "amd_fidelityfx_loader_xbox.dll");
