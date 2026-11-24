// XPackage: package identity, install chunks and mounting.

#include "hle/xgameruntime/runtime_api.h"

// --- interface tables ----------------------------------------------------------------------------

static const GUID k_XPackage_provider = {
    0xaf406016, 0xe850, 0x4aa8, {0xa8, 0x8d, 0x2f, 0x3d, 0xcb, 0x9d, 0xac, 0x7e}};
static const GUID k_XPackage_iids[] = {
    {0x0b9d556e, 0xab70, 0x43f4, {0xba, 0xa0, 0x99, 0x99, 0xac, 0x4c, 0xb0, 0x40}},
};
static const RuntimeMethod k_XPackage_methods[] = {
    {3, "XPackageGetCurrentProcessPackageIdentifier", nullptr},
    {4, "XPackageIsPackagedProcess", nullptr},
    {5, "XPackageCreateInstallationMonitor", nullptr},
    {6, "XPackageCloseInstallationMonitorHandle", nullptr},
    {7, "XPackageGetInstallationProgress", nullptr},
    {8, "XPackageUpdateInstallationMonitor", nullptr},
    {9, "XPackageRegisterInstallationProgressChanged", nullptr},
    {10, "XPackageUnregisterInstallationProgressChanged", nullptr},
    {11, "XPackageGetUserLocale", nullptr},
    {12, "XPackageFindChunkAvailability", nullptr},
    {13, "XPackageEnumerateChunkAvailability", nullptr},
    {14, "XPackageChangeChunkInstallOrder", nullptr},
    {15, "XPackageInstallChunks", nullptr},
    {16, "XPackageInstallChunksAsync", nullptr},
    {17, "XPackageInstallChunksResult", nullptr},
    {18, "XPackageEstimateDownloadSize", nullptr},
    {19, "XPackageUninstallChunks", nullptr},
    {22, "XPackageUnregisterPackageInstalled", nullptr},
    {24, "XPackageGetMountPathSize", nullptr},
    {25, "XPackageGetMountPath", nullptr},
    {26, "XPackageCloseMountHandle", nullptr},
    {27, "XPackageGetIdentifier", nullptr},
    {30, "XPackageGetWriteStats", nullptr},
    {32, "XPackageUninstallUWPInstance", nullptr},
    {33, "XPackageEnumerateFeatures", nullptr},
    {34, "XPackageUninstallPackage", nullptr},
    {37, "XPackageMountWithUiAsync", nullptr},
    {38, "XPackageMountWithUiResult", nullptr},
    {39, "XPackageEnumeratePackages", nullptr},
    {40, "XPackageRegisterPackageInstalled", nullptr},
    {41, "XPackageGetPackageKind", nullptr},
};
extern const RuntimeApi g_runtime_api_XPackage =
    RUNTIME_API("XPackage", &k_XPackage_provider, k_XPackage_iids, k_XPackage_methods);
