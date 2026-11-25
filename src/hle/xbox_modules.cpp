// Xbox system DLLs claimed by sludgemu that don't have any implemented exports yet. Every import
// from these resolves to a logging stub. Modules move into their own files as they get real
// implementations.

#include "hle/hle.h"

// Bundled with titles, but it drives console-only GPU features, so we replace it.
HLE_EMPTY_MODULE(g_hle_module_ffx_loader, "amd_fidelityfx_loader_xbox.dll");
