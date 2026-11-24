// XGameSave: title save containers.

#include "hle/xgameruntime/runtime_api.h"

// --- interface tables ----------------------------------------------------------------------------

static const GUID k_XGameSave_provider = {
    0x704c3f58, 0xe629, 0x4cc2, {0xb1, 0x97, 0x30, 0x51, 0x1b, 0x99, 0x6f, 0xe2}};
static const GUID k_XGameSave_iids[] = {
    {0x1bfff3af, 0xf14a, 0x40a3, {0x8e, 0x35, 0x9a, 0xda, 0x90, 0x65, 0x93, 0xf9}},
    {0xab4ae4fb, 0x6508, 0x4950, {0xa0, 0x32, 0x45, 0xfd, 0x4b, 0xf8, 0xc4, 0x3b}},
};
static const RuntimeMethod k_XGameSave_methods[] = {
    {3, "XGameSaveInitializeProvider", nullptr},
    {4, "XGameSaveInitializeProviderAsync", nullptr},
    {5, "XGameSaveInitializeProviderResult", nullptr},
    {6, "XGameSaveCloseProvider", nullptr},
    {7, "XGameSaveGetRemainingQuota", nullptr},
    {8, "XGameSaveGetRemainingQuotaAsync", nullptr},
    {9, "XGameSaveGetRemainingQuotaResult", nullptr},
    {10, "XGameSaveDeleteContainer", nullptr},
    {11, "XGameSaveDeleteContainerAsync", nullptr},
    {12, "XGameSaveDeleteContainerResult", nullptr},
    {13, "XGameSaveGetContainerInfo", nullptr},
    {14, "XGameSaveEnumerateContainerInfo", nullptr},
    {15, "XGameSaveEnumerateContainerInfoByName", nullptr},
    {16, "XGameSaveCreateContainer", nullptr},
    {17, "XGameSaveCloseContainer", nullptr},
    {18, "XGameSaveEnumerateBlobInfo", nullptr},
    {19, "XGameSaveEnumerateBlobInfoByName", nullptr},
    {20, "XGameSaveReadBlobData", nullptr},
    {21, "XGameSaveReadBlobDataAsync", nullptr},
    {22, "XGameSaveReadBlobDataResult", nullptr},
    {23, "XGameSaveCreateUpdate", nullptr},
    {24, "XGameSaveCloseUpdate", nullptr},
    {25, "XGameSaveSubmitBlobWrite", nullptr},
    {26, "XGameSaveSubmitBlobDelete", nullptr},
    {27, "XGameSaveSubmitUpdate", nullptr},
    {28, "XGameSaveSubmitUpdateAsync", nullptr},
    {29, "XGameSaveSubmitUpdateResult", nullptr},
    {30, "XGameSaveFilesGetFolderWithUiAsync", nullptr},
    {31, "XGameSaveFilesGetFolderWithUiResult", nullptr},
    {32, "XGameSaveFilesGetRemainingQuota", nullptr},
    {33, "PFXGameSaveInitializeConfig", nullptr},
    {34, "PFXGameSaveFreeConfig", nullptr},
    {35, "PFXGameSaveFilesGetFolderWithUiAsync", nullptr},
    {36, "PFXGameSaveFilesGetFolderWithUiResult", nullptr},
    {37, "PFXGameSaveFilesGetRemainingQuota", nullptr},
    {38, "PFXGameSaveSetUiCallbacks", nullptr},
    {39, "PFXGameSaveProgressUiGetProgress", nullptr},
    {40, "PFXGameSaveSetProgressUiResponse", nullptr},
    {41, "PFXGameSaveSetSyncFailedUiResponse", nullptr},
    {42, "PFXGameSaveSetActiveDeviceContentionUiResponse", nullptr},
    {43, "PFXGameSaveSetConflictUiResponse", nullptr},
    {44, "PFXGameSaveSetOutOfStorageUiResponse", nullptr},
    {45, "PFXGameSaveFilesUploadWithUiAsync", nullptr},
    {46, "PFXGameSaveFilesUploadWithUiResult", nullptr},
    {47, "PFXGameSaveFilesSetSaveDescriptionAsync", nullptr},
    {48, "PFXGameSaveFilesSetSaveDescriptionResult", nullptr},
    {49, "PFXGameSaveSetActiveDeviceChangedCallback", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameSave =
    RUNTIME_API("XGameSave", &k_XGameSave_provider, k_XGameSave_iids, k_XGameSave_methods);
