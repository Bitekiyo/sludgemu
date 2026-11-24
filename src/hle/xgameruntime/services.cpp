// Shell and online services: system UI, app capture and broadcasting, speech synthesis, game
// invites and activation, protocol activation and game streaming.

#include "hle/xgameruntime/runtime_api.h"

// --- interface tables ----------------------------------------------------------------------------

static const GUID k_XGameUi_provider = {
    0xdfcd4649, 0x4ff8, 0x4043, {0xba, 0x07, 0x35, 0xd6, 0x07, 0xdf, 0x98, 0xb0}};
static const GUID k_XGameUi_iids[] = {
    {0xeaf669df, 0x5542, 0x4590, {0x99, 0xa3, 0x8d, 0xc0, 0x61, 0xf8, 0x37, 0xcc}},
};
static const RuntimeMethod k_XGameUi_methods[] = {
    {3, "XGameUiShowMessageDialogAsync", nullptr},
    {4, "XGameUiShowMessageDialogResult", nullptr},
    {5, "XGameUiShowSendGameInviteAsync", nullptr},
    {6, "XGameUiShowSendGameInviteResult", nullptr},
    {7, "XGameUiShowPlayerProfileCardAsync", nullptr},
    {8, "XGameUiShowPlayerProfileCardResult", nullptr},
    {9, "XGameUiShowAchievementsAsync", nullptr},
    {10, "XGameUiShowAchievementsResult", nullptr},
    {11, "XGameUiShowPlayerPickerAsync", nullptr},
    {12, "XGameUiShowPlayerPickerResultCount", nullptr},
    {13, "XGameUiShowPlayerPickerResult", nullptr},
    {14, "XGameUiShowErrorDialogAsync", nullptr},
    {15, "XGameUiShowErrorDialogResult", nullptr},
    {16, "XGameUiSetNotificationPositionHint", nullptr},
    {17, "XGameUiShowTextEntryAsync", nullptr},
    {18, "XGameUiShowTextEntryResultSize", nullptr},
    {19, "XGameUiShowTextEntryResult", nullptr},
    {24, "XGameUiShowWebAuthenticationAsync", nullptr},
    {25, "XGameUiShowWebAuthenticationResultSize", nullptr},
    {26, "XGameUiShowWebAuthenticationResult", nullptr},
    {27, "XGameUiShowWebAuthenticationWithOptionsAsync", nullptr},
    {28, "XGameUiShowManageSpaceAsync", nullptr},
    {29, "XGameUiShowManageSpaceResult", nullptr},
    {30, "XGameUiShowMultiplayerActivityGameInviteAsync", nullptr},
    {31, "XGameUiShowMultiplayerActivityGameInviteResult", nullptr},
    {34, "XGameUiTextEntryOpen", nullptr},
    {35, "XGameUiTextEntryClose", nullptr},
    {36, "XGameUiTextEntryGetState", nullptr},
    {37, "XGameUiTextEntryGetExtents", nullptr},
    {38, "XGameUiTextEntryUpdatePositionHint", nullptr},
    {39, "XGameUiTextEntryUpdateVisibility", nullptr},
    {40, "XGameUiShowStateShareAsync", nullptr},
    {41, "XGameUiShowStateShareResult", nullptr},
    {42, "XGameUiSetUiCallbacks", nullptr},
    {43, "XGameUiSetMessageDialogUiResponse", nullptr},
    {44, "XGameUiSetPlayerPickerUiResponse", nullptr},
    {45, "XGameUiSetTextEntryUiResponse", nullptr},
    {46, "XGameUiSetPlayerProfileCardUiResponse", nullptr},
    {47, "XGameUiSetSendGameInviteUiResponse", nullptr},
    {48, "XGameUiSetAchievementsUiResponse", nullptr},
    {49, "XGameUiSetMultiplayerActivityGameInviteUiResponse", nullptr},
    {50, "XGameUiSetErrorDialogUiResponse", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameUi =
    RUNTIME_API("XGameUi", &k_XGameUi_provider, k_XGameUi_iids, k_XGameUi_methods);

static const GUID k_XAppCapture_provider = {
    0xa4f1aee2, 0x4bf1, 0x4485, {0xb0, 0x08, 0xa7, 0xc2, 0x6d, 0x52, 0xac, 0x27}};
static const GUID k_XAppCapture_iids[] = {
    {0x22e672d7, 0xb4e3, 0x406c, {0xbd, 0x50, 0x8f, 0x0d, 0x25, 0x23, 0x6f, 0x9e}},
};
static const RuntimeMethod k_XAppCapture_methods[] = {
    {3, "XAppCaptureTakeDiagnosticScreenshot", nullptr},
    {4, "XAppCaptureRecordDiagnosticClip", nullptr},
    {5, "XAppCaptureTakeScreenshot", nullptr},
    {6, "XAppCaptureOpenScreenshotStream", nullptr},
    {7, "XAppCaptureReadScreenshotStream", nullptr},
    {8, "XAppCaptureCloseScreenshotStream", nullptr},
    {9, "XAppCaptureEnableRecord", nullptr},
    {10, "XAppCaptureDisableRecord", nullptr},
    {11, "XAppCaptureGetVideoCaptureSettings", nullptr},
    {12, "XAppCaptureRecordTimespan", nullptr},
    {13, "XAppCaptureReadLocalStream", nullptr},
    {14, "XAppCaptureCloseLocalStream", nullptr},
    {15, "XAppCaptureStartUserRecord", nullptr},
    {16, "XAppCaptureStopUserRecord", nullptr},
    {17, "XAppCaptureCancelUserRecord", nullptr},
};
extern const RuntimeApi g_runtime_api_XAppCapture =
    RUNTIME_API("XAppCapture", &k_XAppCapture_provider, k_XAppCapture_iids, k_XAppCapture_methods);

static const GUID k_XAppBroadcast_provider = {
    0x186d5592, 0xa72d, 0x45fb, {0x95, 0x60, 0x11, 0xae, 0xd0, 0xe6, 0x64, 0x7a}};
static const GUID k_XAppBroadcast_iids[] = {
    {0x186d5592, 0xa72d, 0x45fb, {0x95, 0x60, 0x11, 0xae, 0xd0, 0xe6, 0x64, 0x7a}},
};
static const RuntimeMethod k_XAppBroadcast_methods[] = {
    {3, "XAppBroadcastIsAppBroadcasting", nullptr},
    {4, "XAppBroadcastShowUI", nullptr},
    {5, "XAppBroadcastGetStatus", nullptr},
    {6, "XAppBroadcastRegisterIsAppBroadcastingChanged", nullptr},
    {7, "XAppBroadcastUnregisterIsAppBroadcastingChanged", nullptr},
    {8, "XAppCaptureMetadataAddStringEvent", nullptr},
    {9, "XAppCaptureMetadataAddInt32Event", nullptr},
    {10, "XAppCaptureMetadataAddDoubleEvent", nullptr},
    {11, "XAppCaptureMetadataStartStringState", nullptr},
    {12, "XAppCaptureMetadataStartInt32State", nullptr},
    {13, "XAppCaptureMetadataStartDoubleState", nullptr},
    {14, "XAppCaptureMetadataStopState", nullptr},
    {15, "XAppCaptureMetadataStopAllStates", nullptr},
    {16, "XAppCaptureMetadataRemainingStorageBytesAvailable", nullptr},
    {17, "XAppCaptureRegisterMetadataPurged", nullptr},
    {18, "XAppCaptureUnRegisterMetadataPurged", nullptr},
};
extern const RuntimeApi g_runtime_api_XAppBroadcast =
    RUNTIME_API("XAppBroadcast", &k_XAppBroadcast_provider, k_XAppBroadcast_iids, k_XAppBroadcast_methods);

static const GUID k_XSpeech_provider = {
    0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}};
static const GUID k_XSpeech_iids[] = {
    {0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}},
};
static const RuntimeMethod k_XSpeech_methods[] = {
    {3, "XClosedCaptionGetProperties", nullptr},
    {4, "XClosedCaptionSetEnabled", nullptr},
    {5, "XHighContrastGetMode", nullptr},
    {6, "XSpeechToTextSetPositionHint", nullptr},
    {7, "XSpeechToTextSendString", nullptr},
    {8, "XSpeechSynthesizerEnumerateInstalledVoices", nullptr},
    {9, "XSpeechSynthesizerCreate", nullptr},
    {10, "XSpeechSynthesizerCloseHandle", nullptr},
    {11, "XSpeechSynthesizerSetDefaultVoice", nullptr},
    {12, "XSpeechSynthesizerSetCustomVoice", nullptr},
    {13, "XSpeechSynthesizerCreateStreamFromText", nullptr},
    {14, "XSpeechSynthesizerCloseStreamHandle", nullptr},
    {15, "XSpeechSynthesizerGetStreamDataSize", nullptr},
    {16, "XSpeechSynthesizerGetStreamData", nullptr},
    {17, "XSpeechToTextBeginHypothesisString", nullptr},
    {18, "XSpeechToTextUpdateHypothesisString", nullptr},
    {19, "XSpeechToTextFinalizeHypothesisString", nullptr},
    {20, "XSpeechToTextCancelHypothesisString", nullptr},
    {21, "XSpeechSynthesizerCreateStreamFromSsml", nullptr},
};
extern const RuntimeApi g_runtime_api_XSpeech =
    RUNTIME_API("XSpeech", &k_XSpeech_provider, k_XSpeech_iids, k_XSpeech_methods);

static const GUID k_XGameInvite_provider = {
    0x0651aae2, 0x4012, 0x4077, {0xbf, 0x84, 0x8b, 0x90, 0x97, 0x09, 0x0e, 0x2c}};
static const GUID k_XGameInvite_iids[] = {
    {0x0651aae2, 0x4012, 0x4077, {0xbf, 0x84, 0x8b, 0x90, 0x97, 0x09, 0x0e, 0x2c}},
    {0x014d1cc3, 0xbcfe, 0x41ff, {0xb2, 0xf0, 0xe1, 0xef, 0x07, 0x15, 0x58, 0x28}},
};
static const RuntimeMethod k_XGameInvite_methods[] = {
    {3, "XGameInviteRegisterForEvent", nullptr},        {4, "XGameInviteUnregisterForEvent", nullptr},
    {5, "XGameInviteRegisterForPendingEvent", nullptr}, {6, "XGameInviteUnregisterForPendingEvent", nullptr},
    {7, "XGameInviteAcceptPendingInvite", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameInvite =
    RUNTIME_API("XGameInvite", &k_XGameInvite_provider, k_XGameInvite_iids, k_XGameInvite_methods);

static const GUID k_XGameActivation_provider = {
    0x7f0fe8b8, 0xe075, 0x49ab, {0x9a, 0xa7, 0xa1, 0xe0, 0x65, 0x48, 0x9a, 0x9e}};
static const GUID k_XGameActivation_iids[] = {
    {0x2e4f76fe, 0x0fc7, 0x461e, {0xab, 0x4d, 0xa4, 0x49, 0x94, 0x34, 0xc3, 0xcf}},
};
static const RuntimeMethod k_XGameActivation_methods[] = {
    {3, "XGameActivationRegisterForEvent", nullptr},
    {4, "XGameActivationUnregisterForEvent", nullptr},
    {5, "XGameActivationAcceptPendingInvite", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameActivation = RUNTIME_API(
    "XGameActivation", &k_XGameActivation_provider, k_XGameActivation_iids, k_XGameActivation_methods);

static const GUID k_XGameProtocol_provider = {
    0x95fd18d2, 0x74dd, 0x4d7c, {0xaa, 0x1b, 0x0b, 0x51, 0x82, 0x76, 0x65, 0xd6}};
static const GUID k_XGameProtocol_iids[] = {
    {0x026b010c, 0x06c3, 0x4cdd, {0xbb, 0xcb, 0x43, 0xf2, 0x29, 0xdb, 0x1c, 0xff}},
};
static const RuntimeMethod k_XGameProtocol_methods[] = {
    {3, "XGameProtocolRegisterForActivation", nullptr},
    {4, "XGameProtocolUnregisterForActivation", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameProtocol =
    RUNTIME_API("XGameProtocol", &k_XGameProtocol_provider, k_XGameProtocol_iids, k_XGameProtocol_methods);

static const GUID k_XGameStreaming_provider = {
    0x0a2192aa, 0xb2d5, 0x4d58, {0x83, 0xbe, 0x38, 0x3b, 0x6d, 0x80, 0x79, 0x9e}};
static const GUID k_XGameStreaming_iids[] = {
    {0x57786622, 0x6605, 0x46d0, {0xb9, 0x17, 0x0f, 0x22, 0xbb, 0xcd, 0x9c, 0x52}},
};
static const RuntimeMethod k_XGameStreaming_methods[] = {
    {3, "XGameStreamingInitialize", nullptr},
    {4, "XGameStreamingUninitialize", nullptr},
    {5, "XGameStreamingIsStreaming", nullptr},
    {6, "XGameStreamingRegisterClientPropertiesChanged", nullptr},
    {7, "XGameStreamingUnregisterClientPropertiesChanged", nullptr},
    {8, "XGameStreamingGetStreamPhysicalDimensions", nullptr},
    {9, "XGameStreamingGetClientCount", nullptr},
    {10, "XGameStreamingGetClients", nullptr},
    {11, "XGameStreamingGetConnectionState", nullptr},
    {12, "XGameStreamingRegisterConnectionStateChanged", nullptr},
    {13, "XGameStreamingUnregisterConnectionStateChanged", nullptr},
    {14, "XGameStreamingGetStreamAddedLatency", nullptr},
    {15, "XGameStreamingGetServerLocationNameSize", nullptr},
    {16, "XGameStreamingGetServerLocationName", nullptr},
    {17, "XGameStreamingHideTouchControls", nullptr},
    {18, "XGameStreamingShowTouchControlLayout", nullptr},
    {19, "XGameStreamingHideTouchControlsOnClient", nullptr},
    {20, "XGameStreamingShowTouchControlLayoutOnClient", nullptr},
    {21, "XGameStreamingIsTouchInputEnabled", nullptr},
    {22, "XGameStreamingGetLastFrameDisplayed", nullptr},
    {23, "XGameStreamingGetAssociatedFrame", nullptr},
    {24, "XGameStreamingGetGamepadPhysicality", nullptr},
    {25, "XGameStreamingUpdateTouchControlsState", nullptr},
    {26, "XGameStreamingUpdateTouchControlsStateOnClient", nullptr},
    {27, "XGameStreamingShowTouchControlsWithStateUpdate", nullptr},
    {28, "XGameStreamingShowTouchControlsWithStateUpdateOnClient", nullptr},
    {29, "XGameStreamingGetTouchBundleVersionNameSize", nullptr},
    {30, "XGameStreamingGetTouchBundleVersion", nullptr},
    {31, "XGameStreamingGetClientIPAddress", nullptr},
    {32, "XGameStreamingGetSessionId", nullptr},
    {33, "XGameStreamingGetDisplayDetails", nullptr},
    {34, "XGameStreamingSetResolution", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameStreaming = RUNTIME_API(
    "XGameStreaming", &k_XGameStreaming_provider, k_XGameStreaming_iids, k_XGameStreaming_methods);

static const GUID k_XGameStreamingExperimental_provider = {
    0x0a2192aa, 0xb2d5, 0x4d58, {0x83, 0xbe, 0x38, 0x3b, 0x6d, 0x80, 0x79, 0x9e}};
static const GUID k_XGameStreamingExperimental_iids[] = {
    {0xaebdfe78, 0x4394, 0x49ff, {0xba, 0x3a, 0x00, 0x19, 0x56, 0x45, 0x70, 0x44}},
};
static const RuntimeMethod k_XGameStreamingExperimental_methods[] = {
    {3, "EXPERIMENTAL_XGameStreamingGetClientOrientation", nullptr},
    {4, "EXPERIMENTAL_XGameStreamingGetMotionState", nullptr},
    {5, "EXPERIMENTAL_XGameStreamingGetMotionInfo", nullptr},
    {6, "XGameStreamingSendDebugMessageToClient", nullptr},
};
extern const RuntimeApi g_runtime_api_XGameStreamingExperimental =
    RUNTIME_API("XGameStreamingExperimental", &k_XGameStreamingExperimental_provider,
                k_XGameStreamingExperimental_iids, k_XGameStreamingExperimental_methods);
