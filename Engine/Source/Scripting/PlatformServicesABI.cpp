#include "Scripting/PlatformServicesABI.h"

#include "PlatformServices/PlatformServices.h"

#include <algorithm>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace
{

namespace PS = GameEngine::PlatformServices;

int32_t ToAbiResult(PS::Result result)
{
    return static_cast<int32_t>(result);
}

std::string_view ViewOrEmpty(const char* utf8)
{
    return utf8 ? std::string_view(utf8) : std::string_view();
}

// Writes as much of value as fits; *outLength always receives the full size.
void WriteUtf8(const std::string& value, uint8_t* buffer, int32_t bufferSize, int32_t* outLength)
{
    if (outLength != nullptr)
        *outLength = static_cast<int32_t>(value.size());
    if (buffer == nullptr || bufferSize <= 0)
        return;
    const size_t count = std::min(value.size(), static_cast<size_t>(bufferSize));
    std::memcpy(buffer, value.data(), count);
}

std::string JoinLines(const std::vector<std::string>& values)
{
    std::string joined;
    for (const std::string& value : values)
    {
        if (!joined.empty())
            joined += '\n';
        joined += value;
    }
    return joined;
}

} // namespace

extern "C" {

GE_API int32_t GE_CDECL GE_Platform_Initialize(const char* appNameUtf8, const char* saveRootUtf8)
{
    PS::Config config;
    config.AppName = ViewOrEmpty(appNameUtf8);
    const std::string_view saveRoot = ViewOrEmpty(saveRootUtf8);
    if (!saveRoot.empty())
        config.SaveRootOverride = std::filesystem::path(
            std::u8string_view(reinterpret_cast<const char8_t*>(saveRoot.data()), saveRoot.size()));
    return ToAbiResult(PS::Get().Initialize(config));
}

GE_API void GE_CDECL GE_Platform_Shutdown(void)
{
    PS::Get().Shutdown();
}

GE_API void GE_CDECL GE_Platform_Tick(void)
{
    PS::Get().Tick();
}

GE_API int32_t GE_CDECL GE_Platform_IsInitialized(void)
{
    return PS::Get().IsInitialized() ? 1 : 0;
}

GE_API uint32_t GE_CDECL GE_Platform_GetCapabilities(void)
{
    return PS::Get().GetCapabilities();
}

GE_API int32_t GE_CDECL GE_Platform_GetPrimaryAccount(
    uint8_t* idBuffer, int32_t idBufferSize, int32_t* outIdLength,
    uint8_t* nameBuffer, int32_t nameBufferSize, int32_t* outNameLength,
    int32_t* outSignedIn, int32_t* outGuestOrOffline)
{
    PS::Account account;
    const PS::Result result = PS::Get().GetPrimaryAccount(account);
    if (result != PS::Result::Ok)
        return ToAbiResult(result);
    WriteUtf8(account.Id, idBuffer, idBufferSize, outIdLength);
    WriteUtf8(account.DisplayName, nameBuffer, nameBufferSize, outNameLength);
    if (outSignedIn != nullptr)
        *outSignedIn = account.SignedIn ? 1 : 0;
    if (outGuestOrOffline != nullptr)
        *outGuestOrOffline = account.GuestOrOffline ? 1 : 0;
    return ToAbiResult(PS::Result::Ok);
}

GE_API int32_t GE_CDECL GE_Platform_RequestSignIn(void)
{
    return ToAbiResult(PS::Get().RequestSignIn());
}

GE_API int32_t GE_CDECL GE_Platform_RequestSignOut(void)
{
    return ToAbiResult(PS::Get().RequestSignOut());
}

GE_API int32_t GE_CDECL GE_Platform_WriteSaveFile(
    const char* slotUtf8, const char* nameUtf8, const uint8_t* data, int32_t size)
{
    if (size < 0)
        return ToAbiResult(PS::Result::InvalidArgument);
    return ToAbiResult(PS::Get().WriteFile(
        ViewOrEmpty(slotUtf8), ViewOrEmpty(nameUtf8),
        std::span<const uint8_t>(data, static_cast<size_t>(size))));
}

GE_API int32_t GE_CDECL GE_Platform_ReadSaveFile(
    const char* slotUtf8, const char* nameUtf8, uint8_t* buffer, int32_t bufferSize, int32_t* outSize)
{
    std::vector<uint8_t> bytes;
    const PS::Result result = PS::Get().ReadFile(ViewOrEmpty(slotUtf8), ViewOrEmpty(nameUtf8), bytes);
    if (result != PS::Result::Ok)
        return ToAbiResult(result);
    if (outSize != nullptr)
        *outSize = static_cast<int32_t>(bytes.size());
    if (buffer == nullptr)
        return ToAbiResult(PS::Result::Ok); // size query
    if (bufferSize < static_cast<int32_t>(bytes.size()))
        return ToAbiResult(PS::Result::InvalidArgument);
    std::memcpy(buffer, bytes.data(), bytes.size());
    return ToAbiResult(PS::Result::Ok);
}

GE_API int32_t GE_CDECL GE_Platform_ListSlots(uint8_t* buffer, int32_t bufferSize, int32_t* outLength)
{
    std::vector<std::string> slots;
    const PS::Result result = PS::Get().ListSlots(slots);
    if (result != PS::Result::Ok)
        return ToAbiResult(result);
    WriteUtf8(JoinLines(slots), buffer, bufferSize, outLength);
    return ToAbiResult(PS::Result::Ok);
}

GE_API int32_t GE_CDECL GE_Platform_ListFiles(
    const char* slotUtf8, uint8_t* buffer, int32_t bufferSize, int32_t* outLength)
{
    std::vector<std::string> files;
    const PS::Result result = PS::Get().ListFiles(ViewOrEmpty(slotUtf8), files);
    if (result != PS::Result::Ok)
        return ToAbiResult(result);
    WriteUtf8(JoinLines(files), buffer, bufferSize, outLength);
    return ToAbiResult(PS::Result::Ok);
}

GE_API int32_t GE_CDECL GE_Platform_ListEntitlements(uint8_t* buffer, int32_t bufferSize, int32_t* outLength)
{
    std::vector<std::string> entitlements;
    const PS::Result result = PS::Get().ListEntitlements(entitlements);
    if (result != PS::Result::Ok)
        return ToAbiResult(result);
    WriteUtf8(JoinLines(entitlements), buffer, bufferSize, outLength);
    return ToAbiResult(PS::Result::Ok);
}

GE_API int32_t GE_CDECL GE_Platform_DeleteSaveFile(const char* slotUtf8, const char* nameUtf8)
{
    return ToAbiResult(PS::Get().DeleteFile(ViewOrEmpty(slotUtf8), ViewOrEmpty(nameUtf8)));
}

GE_API int32_t GE_CDECL GE_Platform_CommitSave(const char* slotUtf8)
{
    return ToAbiResult(PS::Get().Commit(ViewOrEmpty(slotUtf8)));
}

GE_API int32_t GE_CDECL GE_Platform_UnlockAchievement(const char* idUtf8)
{
    return ToAbiResult(PS::Get().UnlockAchievement(ViewOrEmpty(idUtf8)));
}

GE_API int32_t GE_CDECL GE_Platform_SetAchievementProgress(const char* idUtf8, uint32_t current, uint32_t target)
{
    return ToAbiResult(PS::Get().SetAchievementProgress(ViewOrEmpty(idUtf8), current, target));
}

GE_API int32_t GE_CDECL GE_Platform_GetAchievementState(
    const char* idUtf8, int32_t* outUnlocked, uint32_t* outCurrent, uint32_t* outTarget)
{
    PS::AchievementState state;
    const PS::Result result = PS::Get().GetAchievementState(ViewOrEmpty(idUtf8), state);
    if (result != PS::Result::Ok)
        return ToAbiResult(result);
    if (outUnlocked != nullptr)
        *outUnlocked = state.Unlocked ? 1 : 0;
    if (outCurrent != nullptr)
        *outCurrent = state.Current;
    if (outTarget != nullptr)
        *outTarget = state.Target;
    return ToAbiResult(PS::Result::Ok);
}

GE_API int32_t GE_CDECL GE_Platform_HasEntitlement(const char* idUtf8, int32_t* outHas)
{
    bool has = false;
    const PS::Result result = PS::Get().HasEntitlement(ViewOrEmpty(idUtf8), has);
    if (outHas != nullptr)
        *outHas = has ? 1 : 0;
    return ToAbiResult(result);
}

} // extern "C"
