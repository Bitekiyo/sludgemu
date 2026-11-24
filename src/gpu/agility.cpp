// D3D12 Agility SDK opt-in. The D3D12 loader looks these up in the process executable and loads
// D3D12Core.dll from D3D12SDKPath (relative to the executable) instead of the OS copy, as long as
// the OS supports that SDK version.

#include <windows.h>
#include <d3d12.h>

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
