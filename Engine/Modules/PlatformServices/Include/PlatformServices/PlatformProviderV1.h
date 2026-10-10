#pragma once

// PlatformProviderV1.h - Versioned C-compatible contract between the
// PlatformServices facade and the single platform provider linked into a
// build (selected via GE_PLATFORM_BACKEND at configure time).
//
// Game code never includes this header; it talks to the facade in
// PlatformServices.h. Only provider modules and the facade implementation
// include it. The contract deliberately uses engine-owned primitives only
// (UTF-8 views, byte spans, result codes, callbacks) so vendor SDK types
// never leak across it, and stays C-compatible so a provider can later be
// loaded dynamically without redesigning game-facing APIs.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GE_PS_Result {
    GE_PS_Ok = 0,
    GE_PS_Unsupported = 1,
    GE_PS_NotInitialized = 2,
    GE_PS_AlreadyInitialized = 3,
    GE_PS_InvalidArgument = 4,
    GE_PS_NotFound = 5,
    GE_PS_IOError = 6,
    GE_PS_ProviderError = 7,
    GE_PS_FileTooLarge = 8
} GE_PS_Result;

typedef enum GE_PS_Capability {
    GE_PS_Cap_Accounts     = 1 << 0,
    GE_PS_Cap_LocalSaves   = 1 << 1,
    GE_PS_Cap_CloudSaves   = 1 << 2,
    GE_PS_Cap_Achievements = 1 << 3,
    GE_PS_Cap_Entitlements = 1 << 4,
    GE_PS_Cap_PlatformUI   = 1 << 5,
    GE_PS_Cap_OfflineMode  = 1 << 6
} GE_PS_Capability;

// UTF-8, non-owning string view. data may be NULL when length == 0.
typedef struct GE_PS_Str {
    const char* data;
    uint32_t    length;
} GE_PS_Str;

// Views returned by the provider point at provider-owned storage that stays
// valid until the next call into the same provider function or Shutdown.
typedef struct GE_PS_ProviderInfoV1 {
    GE_PS_Str name;
    GE_PS_Str version;
} GE_PS_ProviderInfoV1;

typedef struct GE_PS_AccountV1 {
    GE_PS_Str id;
    GE_PS_Str displayName;
    int32_t   signedIn;
    int32_t   guestOrOffline;
} GE_PS_AccountV1;

typedef struct GE_PS_AchievementStateV1 {
    int32_t  unlocked;
    uint32_t current;
    uint32_t target;
} GE_PS_AchievementStateV1;

typedef struct GE_PS_ConfigV1 {
    GE_PS_Str appName;  // used to derive the default save root when saveRoot is empty
    GE_PS_Str saveRoot; // optional UTF-8 path override for save storage
    // Deterministic entitlement set for providers without a real backend
    // (the Local provider reads these; SDK providers ignore them).
    const GE_PS_Str* entitlements;
    uint32_t         entitlementCount;
} GE_PS_ConfigV1;

// Variable-size outputs are delivered through caller-supplied sinks so no
// allocation ownership crosses the provider boundary. Views passed to sinks
// are only valid for the duration of the callback.
typedef void (*GE_PS_StringSinkFn)(GE_PS_Str value, void* userData);
typedef void (*GE_PS_BytesSinkFn)(const uint8_t* data, uint64_t size, void* userData);

// Versioning rules: additive changes append function pointers to the tail
// and bump abiMinor; breaking changes bump abiMajor. The facade rejects a
// provider whose abiMajor differs from its own or whose sizeBytes is smaller
// than the table it was compiled against. Optional feature groups (accounts,
// UI, ...) may be NULL; the facade maps missing functions to Unsupported.
typedef struct GE_PlatformProviderV1 {
    uint32_t abiMajor;
    uint32_t abiMinor;
    uint64_t sizeBytes;

    // Lifecycle (required — facade fails Initialize if any of these are NULL)
    GE_PS_Result (*Initialize)(const GE_PS_ConfigV1* config);
    void (*Shutdown)(void);
    void (*Tick)(void);
    void (*GetProviderInfo)(GE_PS_ProviderInfoV1* outInfo);
    uint32_t (*GetCapabilities)(void); // GE_PS_Capability mask

    // Accounts
    GE_PS_Result (*GetPrimaryAccount)(GE_PS_AccountV1* outAccount);
    GE_PS_Result (*RequestSignIn)(void);
    GE_PS_Result (*RequestSignOut)(void);

    // Saves (file + slot model)
    GE_PS_Result (*ListSlots)(GE_PS_StringSinkFn sink, void* userData);
    GE_PS_Result (*ListFiles)(GE_PS_Str slot, GE_PS_StringSinkFn sink, void* userData);
    GE_PS_Result (*ReadFile)(GE_PS_Str slot, GE_PS_Str name, GE_PS_BytesSinkFn sink, void* userData);
    GE_PS_Result (*WriteFile)(GE_PS_Str slot, GE_PS_Str name, const uint8_t* data, uint64_t size);
    GE_PS_Result (*DeleteFile)(GE_PS_Str slot, GE_PS_Str name);
    GE_PS_Result (*Commit)(GE_PS_Str slot);

    // Achievements
    GE_PS_Result (*UnlockAchievement)(GE_PS_Str id);
    GE_PS_Result (*SetAchievementProgress)(GE_PS_Str id, uint32_t current, uint32_t target);
    GE_PS_Result (*GetAchievementState)(GE_PS_Str id, GE_PS_AchievementStateV1* outState);

    // Entitlements
    GE_PS_Result (*HasEntitlement)(GE_PS_Str id, int32_t* outHas);
    GE_PS_Result (*ListEntitlements)(GE_PS_StringSinkFn sink, void* userData);

    // Platform UI
    GE_PS_Result (*ShowAchievements)(void);
    GE_PS_Result (*ShowAccountPicker)(void);
    GE_PS_Result (*ShowStorePage)(GE_PS_Str productId);
} GE_PlatformProviderV1;

// Defined by exactly one provider module per build (CMake links the module
// matching GE_PLATFORM_BACKEND). The returned table must outlive the process.
const GE_PlatformProviderV1* GE_GetPlatformProviderV1(void);

#ifdef __cplusplus
}
#endif
