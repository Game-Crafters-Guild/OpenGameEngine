#include "PlatformServices/PlatformServices.h"

#include "PlatformServices/PlatformProviderV1.h"

namespace GameEngine::PlatformServices
{
namespace
{

constexpr uint32_t kSupportedAbiMajor = 1;

Result MapResult(GE_PS_Result result)
{
    switch (result)
    {
    case GE_PS_Ok: return Result::Ok;
    case GE_PS_Unsupported: return Result::Unsupported;
    case GE_PS_NotInitialized: return Result::NotInitialized;
    case GE_PS_AlreadyInitialized: return Result::AlreadyInitialized;
    case GE_PS_InvalidArgument: return Result::InvalidArgument;
    case GE_PS_NotFound: return Result::NotFound;
    case GE_PS_IOError: return Result::IOError;
    case GE_PS_ProviderError: return Result::ProviderError;
    case GE_PS_FileTooLarge: return Result::FileTooLarge;
    }
    return Result::ProviderError;
}

// Enforced here, before any provider call, so the portability limits hold
// identically across every backend regardless of how each maps the
// slot/file model onto its native storage.
bool ValidSlotName(std::string_view slot)
{
    return !slot.empty() && slot.size() <= Limits::kMaxSlotNameLength;
}

bool ValidFileName(std::string_view name)
{
    return !name.empty() && name.size() <= Limits::kMaxFileNameLength;
}

GE_PS_Str ToStr(std::string_view view)
{
    return GE_PS_Str{view.data(), static_cast<uint32_t>(view.size())};
}

std::string ToString(GE_PS_Str str)
{
    return std::string(str.data ? str.data : "", str.length);
}

void AppendStringSink(GE_PS_Str value, void* userData)
{
    static_cast<std::vector<std::string>*>(userData)->emplace_back(ToString(value));
}

void AssignBytesSink(const uint8_t* data, uint64_t size, void* userData)
{
    static_cast<std::vector<uint8_t>*>(userData)->assign(data, data + size);
}

} // namespace

Result Services::Initialize(const Config& config)
{
    if (m_Initialized)
        return Result::AlreadyInitialized;

    const GE_PlatformProviderV1* provider = GE_GetPlatformProviderV1();
    if (provider == nullptr)
        return Result::ProviderError;
    if (provider->abiMajor != kSupportedAbiMajor || provider->sizeBytes < sizeof(GE_PlatformProviderV1))
        return Result::ProviderError;
    if (provider->Initialize == nullptr || provider->Shutdown == nullptr || provider->Tick == nullptr ||
        provider->GetProviderInfo == nullptr || provider->GetCapabilities == nullptr)
        return Result::ProviderError;

    std::vector<GE_PS_Str> entitlements;
    entitlements.reserve(config.MockEntitlements.size());
    for (const std::string& id : config.MockEntitlements)
        entitlements.push_back(ToStr(id));

    const std::u8string saveRootU8 = config.SaveRootOverride.u8string();
    const std::string_view saveRoot(reinterpret_cast<const char*>(saveRootU8.data()), saveRootU8.size());

    GE_PS_ConfigV1 providerConfig{};
    providerConfig.appName = ToStr(config.AppName);
    providerConfig.saveRoot = ToStr(saveRoot);
    providerConfig.entitlements = entitlements.empty() ? nullptr : entitlements.data();
    providerConfig.entitlementCount = static_cast<uint32_t>(entitlements.size());

    const Result result = MapResult(provider->Initialize(&providerConfig));
    if (result != Result::Ok)
        return result;

    m_Provider = provider;
    m_Initialized = true;
    return Result::Ok;
}

void Services::Shutdown()
{
    if (!m_Initialized)
        return;
    m_Provider->Shutdown();
    m_Provider = nullptr;
    m_Initialized = false;
}

void Services::Tick()
{
    if (m_Initialized)
        m_Provider->Tick();
}

ProviderInfo Services::GetProviderInfo() const
{
    if (!m_Initialized)
        return {};
    GE_PS_ProviderInfoV1 info{};
    m_Provider->GetProviderInfo(&info);
    return ProviderInfo{ToString(info.name), ToString(info.version)};
}

CapabilityMask Services::GetCapabilities() const
{
    return m_Initialized ? m_Provider->GetCapabilities() : 0u;
}

bool Services::Supports(Capability capability) const
{
    return (GetCapabilities() & static_cast<CapabilityMask>(capability)) != 0;
}

Result Services::GetPrimaryAccount(Account& outAccount) const
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->GetPrimaryAccount == nullptr)
        return Result::Unsupported;
    GE_PS_AccountV1 account{};
    const Result result = MapResult(m_Provider->GetPrimaryAccount(&account));
    if (result != Result::Ok)
        return result;
    outAccount.Id = ToString(account.id);
    outAccount.DisplayName = ToString(account.displayName);
    outAccount.SignedIn = account.signedIn != 0;
    outAccount.GuestOrOffline = account.guestOrOffline != 0;
    return Result::Ok;
}

Result Services::RequestSignIn()
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->RequestSignIn == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->RequestSignIn());
}

Result Services::RequestSignOut()
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->RequestSignOut == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->RequestSignOut());
}

Result Services::ListSlots(std::vector<std::string>& outSlots) const
{
    outSlots.clear();
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->ListSlots == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ListSlots(&AppendStringSink, &outSlots));
}

Result Services::ListFiles(std::string_view slot, std::vector<std::string>& outFiles) const
{
    outFiles.clear();
    if (!m_Initialized)
        return Result::NotInitialized;
    if (!ValidSlotName(slot))
        return Result::InvalidArgument;
    if (m_Provider->ListFiles == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ListFiles(ToStr(slot), &AppendStringSink, &outFiles));
}

Result Services::ReadFile(std::string_view slot, std::string_view name, std::vector<uint8_t>& outBytes) const
{
    outBytes.clear();
    if (!m_Initialized)
        return Result::NotInitialized;
    if (!ValidSlotName(slot) || !ValidFileName(name))
        return Result::InvalidArgument;
    if (m_Provider->ReadFile == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ReadFile(ToStr(slot), ToStr(name), &AssignBytesSink, &outBytes));
}

Result Services::WriteFile(std::string_view slot, std::string_view name, std::span<const uint8_t> bytes)
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (!ValidSlotName(slot) || !ValidFileName(name))
        return Result::InvalidArgument;
    if (bytes.size() > Limits::kMaxSaveFileBytes)
        return Result::FileTooLarge;
    if (m_Provider->WriteFile == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->WriteFile(ToStr(slot), ToStr(name), bytes.data(), bytes.size()));
}

Result Services::DeleteFile(std::string_view slot, std::string_view name)
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (!ValidSlotName(slot) || !ValidFileName(name))
        return Result::InvalidArgument;
    if (m_Provider->DeleteFile == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->DeleteFile(ToStr(slot), ToStr(name)));
}

Result Services::Commit(std::string_view slot)
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (!ValidSlotName(slot))
        return Result::InvalidArgument;
    if (m_Provider->Commit == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->Commit(ToStr(slot)));
}

Result Services::UnlockAchievement(std::string_view id)
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->UnlockAchievement == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->UnlockAchievement(ToStr(id)));
}

Result Services::SetAchievementProgress(std::string_view id, uint32_t current, uint32_t target)
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->SetAchievementProgress == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->SetAchievementProgress(ToStr(id), current, target));
}

Result Services::GetAchievementState(std::string_view id, AchievementState& outState) const
{
    outState = {};
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->GetAchievementState == nullptr)
        return Result::Unsupported;
    GE_PS_AchievementStateV1 state{};
    const Result result = MapResult(m_Provider->GetAchievementState(ToStr(id), &state));
    if (result != Result::Ok)
        return result;
    outState.Unlocked = state.unlocked != 0;
    outState.Current = state.current;
    outState.Target = state.target;
    return Result::Ok;
}

Result Services::HasEntitlement(std::string_view id, bool& outHas) const
{
    outHas = false;
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->HasEntitlement == nullptr)
        return Result::Unsupported;
    int32_t has = 0;
    const Result result = MapResult(m_Provider->HasEntitlement(ToStr(id), &has));
    outHas = has != 0;
    return result;
}

Result Services::ListEntitlements(std::vector<std::string>& outIds) const
{
    outIds.clear();
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->ListEntitlements == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ListEntitlements(&AppendStringSink, &outIds));
}

Result Services::ShowAchievements()
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->ShowAchievements == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ShowAchievements());
}

Result Services::ShowAccountPicker()
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->ShowAccountPicker == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ShowAccountPicker());
}

Result Services::ShowStorePage(std::string_view productId)
{
    if (!m_Initialized)
        return Result::NotInitialized;
    if (m_Provider->ShowStorePage == nullptr)
        return Result::Unsupported;
    return MapResult(m_Provider->ShowStorePage(ToStr(productId)));
}

Services& Get()
{
    static Services s_Instance;
    return s_Instance;
}

} // namespace GameEngine::PlatformServices
