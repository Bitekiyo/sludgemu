// System-level runtime APIs: console identity, title identity, launching, display, feature
// availability, error reporting, game events, persistent local storage and networking.

#include "hle/xgameruntime/runtime_api.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "core/console_profile.h"
#include "core/title.h"

// --- XGame ---------------------------------------------------------------------------------------

static HRESULT WINAPI XGameGetXboxTitleId(RuntimeSelf, u32* title_id) {
    if (!title_id)
        return E_INVALIDARG;
    if (!g_title.title_id[0]) {
        LOG_W("xgr", "XGameGetXboxTitleId: the package does not declare a title id");
        return E_FAIL;
    }
    *title_id = (u32)strtoul(g_title.title_id, nullptr, 16);
    return S_OK;
}

// --- display -------------------------------------------------------------------------------------

enum HdrModeResult : u32 {
    HDR_MODE_UNKNOWN = 0,
    HDR_MODE_ENABLED = 1,
    HDR_MODE_DISABLED = 2,
};

// HDR output arrives with the swapchain work; until then titles render SDR.
static u32 WINAPI XDisplayTryEnableHdrMode(RuntimeSelf, u32 preference, void* info) {
    LOG_I("xgr", "XDisplayTryEnableHdrMode: HDR output not supported yet, staying SDR");
    return HDR_MODE_DISABLED;
}

// --- persistent local storage --------------------------------------------------------------------
// A per-title directory under the user's local app data, with the quota the package declares.

struct PlsSpaceInfo {
    u64 available_free_bytes;
    u64 total_free_bytes;
};

static bool pls_path(char* out, usize cap, usize* used) {
    wchar_t dir[MAX_PATH];
    if (!title_data_dir(L"PersistentLocalStorage", dir, MAX_PATH))
        return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, dir, -1, out, (int)cap, nullptr, nullptr);
    if (n <= 0)
        return false;
    if (used)
        *used = (usize)n;
    return true;
}

static u64 directory_size(const wchar_t* dir) {
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%s*", dir);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    u64 total = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            wchar_t sub[MAX_PATH];
            _snwprintf(sub, MAX_PATH, L"%s%s\\", dir, fd.cFileName);
            sub[MAX_PATH - 1] = 0;
            total += directory_size(sub);
        } else {
            total += ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

static HRESULT WINAPI XPersistentLocalStorageGetPathSize(RuntimeSelf, usize* size) {
    char path[MAX_PATH * 3];
    if (!size || !pls_path(path, sizeof(path), size))
        return E_FAIL;
    return S_OK;
}

static HRESULT WINAPI XPersistentLocalStorageGetPath(RuntimeSelf, usize size, char* path, usize* used) {
    char buf[MAX_PATH * 3];
    usize n = 0;
    if (!path || !pls_path(buf, sizeof(buf), &n))
        return E_FAIL;
    if (size < n)
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(path, buf, n);
    if (used)
        *used = n;
    LOG_I("xgr", "persistent local storage: %s", buf);
    return S_OK;
}

static HRESULT WINAPI XPersistentLocalStorageGetSpaceInfo(RuntimeSelf, PlsSpaceInfo* info) {
    wchar_t dir[MAX_PATH];
    if (!info || !title_data_dir(L"PersistentLocalStorage", dir, MAX_PATH))
        return E_FAIL;
    u64 used = directory_size(dir);
    u64 free_bytes = g_title.pls_quota > used ? g_title.pls_quota - used : 0;
    info->available_free_bytes = free_bytes;
    info->total_free_bytes = free_bytes;
    return S_OK;
}

// --- XSystem ------------------------------------------------------------------------------------

struct RuntimeVersion {
    u16 major;
    u16 minor;
    u16 build;
    u16 revision;
};

struct SystemAnalyticsInfo {
    RuntimeVersion os_version;
    RuntimeVersion hosting_os_version;
    char family[64];
    char form[64];
};

// Runtime interfaces are compiled C++ classes, so a struct return comes back through a hidden
// pointer passed after the object.
static SystemAnalyticsInfo* WINAPI XSystemGetAnalyticsInfo(RuntimeSelf, SystemAnalyticsInfo* ret) {
    memset(ret, 0, sizeof(*ret));
    const RuntimeVersion console_os = {10, 0, 25398, 0}; // the console OS's Windows core version
    ret->os_version = console_os;
    ret->hosting_os_version = console_os;
    strcpy(ret->family, "Xbox");
    strcpy(ret->form, console_profile().name);
    return ret;
}

// Retail consoles are in the retail Xbox services sandbox, which titles check before going online.
static HRESULT WINAPI XSystemGetXboxLiveSandboxId(RuntimeSelf, usize size, char* sandbox, usize* used) {
    static const char k_retail[] = "RETAIL";
    if (!sandbox)
        return E_INVALIDARG;
    if (size < sizeof(k_retail))
        return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    memcpy(sandbox, k_retail, sizeof(k_retail));
    if (used)
        *used = sizeof(k_retail);
    return S_OK;
}

// --- feature availability ------------------------------------------------------------------------

// Runtime feature areas, numbered as titles pass them.
enum RuntimeFeature : u32 {
    FEATURE_GAME_STREAMING = 21,
};

// A console has every feature area. Game streaming is the exception: it only exists when the
// title runs in a cloud streaming session, which never applies here.
static bool WINAPI XGameRuntimeIsFeatureAvailable(RuntimeSelf, u32 feature) {
    return feature != FEATURE_GAME_STREAMING;
}

// --- interface tables ----------------------------------------------------------------------------

static const GUID k_XSystem_provider = {
    0xe349bd1a, 0xfc20, 0x4e40, {0xb9, 0x9c, 0x41, 0x78, 0xcc, 0x6b, 0x40, 0x9f}};
static const GUID k_XSystem_iids[] = {
    {0x1861cf2e, 0xe18b, 0x4834, {0xa9, 0xf5, 0xb4, 0xa4, 0xe6, 0xef, 0xb4, 0xcf}},
};
static const RuntimeMethod k_XSystem_methods[] = {
    {3, "XSystemGetConsoleId", nullptr},
    {4, "XSystemGetXboxLiveSandboxId", (void*)&XSystemGetXboxLiveSandboxId},
    {5, "XSystemGetAppSpecificDeviceId", nullptr},
    {6, "XSystemHandleTrack", nullptr},
    {7, "XSystemIsHandleValid", nullptr},
    {8, "XSystemAllowFullDownloadBandwidth", nullptr},
};
extern const RuntimeApi g_runtime_api_XSystem =
    RUNTIME_API("XSystem", &k_XSystem_provider, k_XSystem_iids, k_XSystem_methods);

static const GUID k_XSystemAnalytics_provider = {
    0xb884675d, 0xb738, 0x4a9c, {0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b}};
static const GUID k_XSystemAnalytics_iids[] = {
    {0xb884675d, 0xb738, 0x4a9c, {0x81, 0x5d, 0x9a, 0x9a, 0x1e, 0x0c, 0x6c, 0x9b}},
};
static const RuntimeMethod k_XSystemAnalytics_methods[] = {
    {3, "XSystemGetAnalyticsInfo", (void*)&XSystemGetAnalyticsInfo},
};
extern const RuntimeApi g_runtime_api_XSystemAnalytics = RUNTIME_API(
    "XSystemAnalytics", &k_XSystemAnalytics_provider, k_XSystemAnalytics_iids, k_XSystemAnalytics_methods);

static const GUID k_XGame_provider = {
    0x973a344e, 0x24bf, 0x4d0f, {0x84, 0x57, 0x56, 0xc5, 0x34, 0x89, 0x2b, 0x29}};
static const GUID k_XGame_iids[] = {
    {0x2549f142, 0x6419, 0x4a06, {0x97, 0xb5, 0x93, 0x1a, 0xab, 0x7c, 0x2f, 0x34}},
};
static const RuntimeMethod k_XGame_methods[] = {
    {3, "XGameGetXboxTitleId", (void*)&XGameGetXboxTitleId},
    {4, "XLaunchNewGame", nullptr},
    {5, "XLaunchRestartOnCrash", nullptr},
};
extern const RuntimeApi g_runtime_api_XGame =
    RUNTIME_API("XGame", &k_XGame_provider, k_XGame_iids, k_XGame_methods);

static const GUID k_XLaunch_provider = {
    0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}};
static const GUID k_XLaunch_iids[] = {
    {0x1b339674, 0x328d, 0x4283, {0xa2, 0x00, 0x31, 0x71, 0xf1, 0x8d, 0x36, 0x39}},
};
static const RuntimeMethod k_XLaunch_methods[] = {
    {3, "XLaunchUri", nullptr},
    {4, "XDisplayAcquireTimeoutDeferral", nullptr},
    {5, "XDisplayCloseTimeoutDeferralHandle", nullptr},
};
extern const RuntimeApi g_runtime_api_XLaunch =
    RUNTIME_API("XLaunch", &k_XLaunch_provider, k_XLaunch_iids, k_XLaunch_methods);

static const GUID k_XDisplay_provider = {
    0x03f0fe74, 0xfdd9, 0x4e5c, {0xb6, 0x30, 0xf9, 0x33, 0x9c, 0x47, 0xac, 0xc5}};
static const GUID k_XDisplay_iids[] = {
    {0x35f07670, 0x706e, 0x4bfb, {0x94, 0x76, 0x09, 0x07, 0x98, 0xc5, 0xeb, 0xf3}},
};
static const RuntimeMethod k_XDisplay_methods[] = {
    {4, "XDisplayTryEnableHdrMode", (void*)&XDisplayTryEnableHdrMode},
};
extern const RuntimeApi g_runtime_api_XDisplay =
    RUNTIME_API("XDisplay", &k_XDisplay_provider, k_XDisplay_iids, k_XDisplay_methods);

static const GUID k_XGameRuntimeFeature_provider = {
    0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}};
static const GUID k_XGameRuntimeFeature_iids[] = {
    {0x8836fe87, 0xedb9, 0x4fe3, {0x8d, 0xad, 0x05, 0xf0, 0xd2, 0xcd, 0x5b, 0x40}},
};
static const RuntimeMethod k_XGameRuntimeFeature_methods[] = {
    {3, "XGameRuntimeIsFeatureAvailable", (void*)&XGameRuntimeIsFeatureAvailable},
};
extern const RuntimeApi g_runtime_api_XGameRuntimeFeature =
    RUNTIME_API("XGameRuntimeFeature", &k_XGameRuntimeFeature_provider, k_XGameRuntimeFeature_iids,
                k_XGameRuntimeFeature_methods);

static const GUID k_XError_provider = {
    0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}};
static const GUID k_XError_iids[] = {
    {0x8ca467f7, 0x22e8, 0x4096, {0x84, 0x56, 0xbb, 0x8a, 0xa1, 0x3f, 0x79, 0xd8}},
};
static const RuntimeMethod k_XError_methods[] = {
    {3, "XErrorReport", nullptr},
    {4, "XErrorSetCallback", nullptr},
    {5, "XErrorSetOptions", nullptr},
};
extern const RuntimeApi g_runtime_api_XError =
    RUNTIME_API("XError", &k_XError_provider, k_XError_iids, k_XError_methods);

static const GUID k_XGameEvent_provider = {
    0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}};
static const GUID k_XGameEvent_iids[] = {
    {0xbbfbdcc7, 0xbfe7, 0x409b, {0xa5, 0xca, 0xed, 0xf0, 0x54, 0x96, 0x0b, 0x4d}},
};
static const RuntimeMethod k_XGameEvent_methods[] = {
    {3, "XGameEventWrite", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameEvent =
    RUNTIME_API("XGameEvent", &k_XGameEvent_provider, k_XGameEvent_iids, k_XGameEvent_methods);

static const GUID k_XPersistentLocalStorage_provider = {
    0xf4faf4d4, 0x2d04, 0x4fce, {0xb3, 0xe0, 0x47, 0x4a, 0x71, 0x3a, 0x3e, 0x84}};
static const GUID k_XPersistentLocalStorage_iids[] = {
    {0x41a4e10c, 0x5a7e, 0x41d9, {0x8c, 0x37, 0x37, 0xbd, 0xe6, 0x2a, 0x07, 0xd6}},
};
static const RuntimeMethod k_XPersistentLocalStorage_methods[] = {
    {3, "XPersistentLocalStorageGetPathSize", (void*)&XPersistentLocalStorageGetPathSize},
    {4, "XPersistentLocalStorageGetPath", (void*)&XPersistentLocalStorageGetPath},
    {5, "XPersistentLocalStorageGetSpaceInfo", (void*)&XPersistentLocalStorageGetSpaceInfo},
    {6, "XPersistentLocalStoragePromptUserForSpaceAsync", nullptr},
    {7, "XPersistentLocalStoragePromptUserForSpaceResult", nullptr},
    {8, "XPersistentLocalStorageMountForPackage", nullptr},
};
extern const RuntimeApi g_runtime_api_XPersistentLocalStorage =
    RUNTIME_API("XPersistentLocalStorage", &k_XPersistentLocalStorage_provider,
                k_XPersistentLocalStorage_iids, k_XPersistentLocalStorage_methods);

static const GUID k_XNetworking_provider = {
    0x37e56907, 0x2f10, 0x41e8, {0xb7, 0x2f, 0x36, 0xed, 0xb1, 0x85, 0x33, 0x1a}};
static const GUID k_XNetworking_iids[] = {
    {0xbf2346b2, 0x39af, 0x4658, {0xb5, 0xea, 0x44, 0x71, 0x3c, 0x7e, 0x83, 0xb3}},
};
static const RuntimeMethod k_XNetworking_methods[] = {
    {3, "XNetworkingQueryPreferredLocalUdpMultiplayerPort", nullptr},
    {4, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsync", nullptr},
    {5, "XNetworkingQueryPreferredLocalUdpMultiplayerPortAsyncResult", nullptr},
    {6, "XNetworkingRegisterPreferredLocalUdpMultiplayerPortChanged", nullptr},
    {7, "XNetworkingUnregisterPreferredLocalUdpMultiplayerPortChanged", nullptr},
    {8, "XNetworkingQuerySecurityInformationForUrlAsync", nullptr},
    {9, "XNetworkingQuerySecurityInformationForUrlAsyncResultSize", nullptr},
    {10, "XNetworkingQuerySecurityInformationForUrlAsyncResult", nullptr},
    {11, "XNetworkingQuerySecurityInformationForUrlUtf16Async", nullptr},
    {12, "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResultSize", nullptr},
    {13, "XNetworkingQuerySecurityInformationForUrlUtf16AsyncResult", nullptr},
    {14, "XNetworkingVerifyServerCertificate", nullptr},
    {15, "XNetworkingGetConnectivityHint", nullptr},
    {16, "XNetworkingRegisterConnectivityHintChanged", nullptr},
    {17, "XNetworkingUnregisterConnectivityHintChanged", nullptr},
    {18, "XNetworkingQueryConfigurationSetting", nullptr},
    {19, "XNetworkingSetConfigurationSetting", nullptr},
    {20, "XNetworkingQueryStatistics", nullptr},
};
extern const RuntimeApi g_runtime_api_XNetworking =
    RUNTIME_API("XNetworking", &k_XNetworking_provider, k_XNetworking_iids, k_XNetworking_methods);
