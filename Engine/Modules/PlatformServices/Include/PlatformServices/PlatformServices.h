#pragma once

#include "PlatformServices/PlatformServicesTypes.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

extern "C" {
struct GE_PlatformProviderV1;
}

namespace GameEngine::PlatformServices
{

// Platform-blind facade over the single platform provider linked into this
// build. Game code (C++ or, via the ABI, C#) calls only this API; vendor SDK
// types never appear here. Feature availability is queried through
// GetCapabilities() — game code must branch on capabilities, never on
// provider identity.
//
// Thread-safety: data calls (saves, achievements, entitlements, accounts)
// may come from any thread — providers synchronize internally. Lifecycle
// calls (Initialize/Shutdown/Tick) must be externally serialized against
// all other calls.
class Services
{
  public:
    Result Initialize(const Config& config);
    void Shutdown();
    void Tick();
    bool IsInitialized() const { return m_Initialized; }

    ProviderInfo GetProviderInfo() const;
    CapabilityMask GetCapabilities() const;
    bool Supports(Capability capability) const;

    Result GetPrimaryAccount(Account& outAccount) const;
    Result RequestSignIn();
    Result RequestSignOut();

    // Saves use a slot + file model. Slots are created implicitly by the
    // first WriteFile into them. Whether Commit syncs to cloud, flushes to
    // disk, or no-ops is the provider's decision.
    Result ListSlots(std::vector<std::string>& outSlots) const;
    Result ListFiles(std::string_view slot, std::vector<std::string>& outFiles) const;
    Result ReadFile(std::string_view slot, std::string_view name, std::vector<uint8_t>& outBytes) const;
    Result WriteFile(std::string_view slot, std::string_view name, std::span<const uint8_t> bytes);
    Result DeleteFile(std::string_view slot, std::string_view name);
    Result Commit(std::string_view slot);

    Result UnlockAchievement(std::string_view id);
    Result SetAchievementProgress(std::string_view id, uint32_t current, uint32_t target);
    Result GetAchievementState(std::string_view id, AchievementState& outState) const;

    Result HasEntitlement(std::string_view id, bool& outHas) const;
    Result ListEntitlements(std::vector<std::string>& outIds) const;

    Result ShowAchievements();
    Result ShowAccountPicker();
    Result ShowStorePage(std::string_view productId);

  private:
    friend Services& Get();
    Services() = default;

    const GE_PlatformProviderV1* m_Provider = nullptr;
    bool m_Initialized = false;
};

Services& Get();

} // namespace GameEngine::PlatformServices
