// XUser: local users, sign-in and privileges.

#include "hle/xgameruntime/runtime_api.h"

// --- interface tables ----------------------------------------------------------------------------

static const GUID k_XUser_provider = {
    0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID k_XUser_iids[] = {
    {0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}},
    {0xeb9bf948, 0x18dc, 0x4d82, {0xbb, 0xcc, 0x40, 0xe0, 0xa8, 0x09, 0xc4, 0xc0}},
    {0x1bf2f8c5, 0xd507, 0x4e52, {0xbb, 0x05, 0xf7, 0x26, 0xd0, 0xe7, 0x11, 0x61}},
    {0x5131d685, 0x4394, 0x4ee6, {0x8c, 0x18, 0xbf, 0xb5, 0xd4, 0xae, 0xf1, 0xff}},
    {0x079415e3, 0x6727, 0x437f, {0x8e, 0x9d, 0x8f, 0x8f, 0x9b, 0x24, 0x39, 0xf7}},
    {0x26f3c674, 0xa2fe, 0x44fa, {0xb6, 0xc4, 0xa3, 0x23, 0xbc, 0x94, 0xff, 0x53}},
};
static const RuntimeMethod k_XUser_methods[] = {
    {3, "XUserDuplicateHandle", nullptr},
    {4, "XUserCloseHandle", nullptr},
    {5, "XUserCompare", nullptr},
    {6, "XUserGetMaxUsers", nullptr},
    {7, "XUserAddAsync", nullptr},
    {8, "XUserAddResult", nullptr},
    {9, "XUserGetLocalId", nullptr},
    {10, "XUserFindUserByLocalId", nullptr},
    {11, "XUserGetId", nullptr},
    {12, "XUserFindUserById", nullptr},
    {13, "XUserGetIsGuest", nullptr},
    {14, "XUserGetState", nullptr},
    {16, "XUserGetGamerPictureAsync", nullptr},
    {17, "XUserGetGamerPictureResultSize", nullptr},
    {18, "XUserGetGamerPictureResult", nullptr},
    {19, "XUserGetAgeGroup", nullptr},
    {20, "XUserCheckPrivilege", nullptr},
    {21, "XUserResolvePrivilegeWithUiAsync", nullptr},
    {22, "XUserResolvePrivilegeWithUiResult", nullptr},
    {23, "XUserGetTokenAndSignatureAsync", nullptr},
    {24, "XUserGetTokenAndSignatureResultSize", nullptr},
    {25, "XUserGetTokenAndSignatureResult", nullptr},
    {26, "XUserGetTokenAndSignatureUtf16Async", nullptr},
    {27, "XUserGetTokenAndSignatureUtf16ResultSize", nullptr},
    {28, "XUserGetTokenAndSignatureUtf16Result", nullptr},
    {29, "XUserResolveIssueWithUiAsync", nullptr},
    {30, "XUserResolveIssueWithUiResult", nullptr},
    {31, "XUserResolveIssueWithUiUtf16Async", nullptr},
    {32, "XUserResolveIssueWithUiUtf16Result", nullptr},
    {33, "XUserRegisterForChangeEvent", nullptr},
    {34, "XUserUnregisterForChangeEvent", nullptr},
    {35, "XUserGetSignOutDeferral", nullptr},
    {36, "XUserCloseSignOutDeferralHandle", nullptr},
    {37, "XUserAddByIdWithUiAsync", nullptr},
    {38, "XUserAddByIdWithUiResult", nullptr},
    {39, "XUserGetMsaTokenSilentlyAsync", nullptr},
    {40, "XUserGetMsaTokenSilentlyResult", nullptr},
    {41, "XUserGetMsaTokenSilentlyResultSize", nullptr},
    {42, "XUserIsStoreUser", nullptr},
    {43, "XUserPlatformRemoteConnectSetEventHandlers", nullptr},
    {44, "XUserPlatformRemoteConnectCancelPrompt", nullptr},
    {45, "XUserPlatformSpopPromptSetEventHandlers", nullptr},
    {46, "XUserPlatformSpopPromptComplete", nullptr},
    {47, "XUserIsSignOutPresent", nullptr},
    {48, "XUserSignOutAsync", nullptr},
    {49, "XUserSignOutResult", nullptr},
};
extern const RuntimeApi g_runtime_api_XUser =
    RUNTIME_API("XUser", &k_XUser_provider, k_XUser_iids, k_XUser_methods);

static const GUID k_XUserGamertag_provider = {
    0x01acd177, 0x91f9, 0x4763, {0xa3, 0x8e, 0xcc, 0xbb, 0x55, 0xce, 0x32, 0xe0}};
static const GUID k_XUserGamertag_iids[] = {
    {0xcef4fac0, 0x7676, 0x4a94, {0xa1, 0x19, 0x4c, 0x43, 0xf9, 0xeb, 0x5b, 0x74}},
};
static const RuntimeMethod k_XUserGamertag_methods[] = {
    {3, "XUserGetGamertag", nullptr},
};
extern const RuntimeApi g_runtime_api_XUserGamertag =
    RUNTIME_API("XUserGamertag", &k_XUserGamertag_provider, k_XUserGamertag_iids, k_XUserGamertag_methods);

static const GUID k_XUserDevice_provider = {
    0x7d824997, 0x10dc, 0x45ab, {0x86, 0xb7, 0x27, 0x37, 0x76, 0x7c, 0x0b, 0xf1}};
static const GUID k_XUserDevice_iids[] = {
    {0x0cc6a956, 0xe7e1, 0x4fdf, {0x93, 0x41, 0x9d, 0x5d, 0xa6, 0x49, 0xeb, 0xc8}},
};
static const RuntimeMethod k_XUserDevice_methods[] = {
    {3, "XUserFindForDevice", nullptr},
    {4, "XUserRegisterForDeviceAssociationChanged", nullptr},
    {5, "XUserUnregisterForDeviceAssociationChanged", nullptr},
    {6, "XUserGetDefaultAudioEndpointUtf16", nullptr},
    {7, "XUserRegisterForDefaultAudioEndpointUtf16Changed", nullptr},
    {8, "XUserUnregisterForDefaultAudioEndpointUtf16Changed", nullptr},
    {9, "XUserFindControllerForUserWithUiAsync", nullptr},
    {10, "XUserFindControllerForUserWithUiResult", nullptr},
};
extern const RuntimeApi g_runtime_api_XUserDevice =
    RUNTIME_API("XUserDevice", &k_XUserDevice_provider, k_XUserDevice_iids, k_XUserDevice_methods);
