#pragma once

// PlatformServicesABI.h - C ABI for managed (C#) access to PlatformServices.
// Thin flat-function layer over GameEngine::PlatformServices::Get(); the
// managed facade lives in Managed/Platform.ABI. Return values are
// GE_PS_Result codes (see PlatformServices/PlatformProviderV1.h).

#include "Scripting/ScriptingABI.h"

#ifdef __cplusplus
extern "C" {
#endif

GE_API int32_t GE_CDECL GE_Platform_Initialize(const char* appNameUtf8, const char* saveRootUtf8);
GE_API void GE_CDECL GE_Platform_Shutdown(void);
GE_API void GE_CDECL GE_Platform_Tick(void);
GE_API int32_t GE_CDECL GE_Platform_IsInitialized(void);
GE_API uint32_t GE_CDECL GE_Platform_GetCapabilities(void);

// Strings are written as UTF-8 into caller buffers; *outLength receives the
// full length even when truncated to bufferSize.
GE_API int32_t GE_CDECL GE_Platform_GetPrimaryAccount(
    uint8_t* idBuffer, int32_t idBufferSize, int32_t* outIdLength,
    uint8_t* nameBuffer, int32_t nameBufferSize, int32_t* outNameLength,
    int32_t* outSignedIn, int32_t* outGuestOrOffline);
GE_API int32_t GE_CDECL GE_Platform_RequestSignIn(void);
GE_API int32_t GE_CDECL GE_Platform_RequestSignOut(void);

GE_API int32_t GE_CDECL GE_Platform_WriteSaveFile(
    const char* slotUtf8, const char* nameUtf8, const uint8_t* data, int32_t size);
// Two-call pattern: pass buffer == NULL to query the size via *outSize.
GE_API int32_t GE_CDECL GE_Platform_ReadSaveFile(
    const char* slotUtf8, const char* nameUtf8, uint8_t* buffer, int32_t bufferSize, int32_t* outSize);
GE_API int32_t GE_CDECL GE_Platform_DeleteSaveFile(const char* slotUtf8, const char* nameUtf8);
GE_API int32_t GE_CDECL GE_Platform_CommitSave(const char* slotUtf8);

// List results are written as newline-joined UTF-8 (save names cannot
// contain newlines); same buffer semantics as the string getters above,
// with buffer == NULL acting as a size query via *outLength.
GE_API int32_t GE_CDECL GE_Platform_ListSlots(uint8_t* buffer, int32_t bufferSize, int32_t* outLength);
GE_API int32_t GE_CDECL GE_Platform_ListFiles(
    const char* slotUtf8, uint8_t* buffer, int32_t bufferSize, int32_t* outLength);
GE_API int32_t GE_CDECL GE_Platform_ListEntitlements(uint8_t* buffer, int32_t bufferSize, int32_t* outLength);

GE_API int32_t GE_CDECL GE_Platform_UnlockAchievement(const char* idUtf8);
GE_API int32_t GE_CDECL GE_Platform_SetAchievementProgress(const char* idUtf8, uint32_t current, uint32_t target);
GE_API int32_t GE_CDECL GE_Platform_GetAchievementState(
    const char* idUtf8, int32_t* outUnlocked, uint32_t* outCurrent, uint32_t* outTarget);

GE_API int32_t GE_CDECL GE_Platform_HasEntitlement(const char* idUtf8, int32_t* outHas);

#ifdef __cplusplus
}
#endif
