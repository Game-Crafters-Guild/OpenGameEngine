#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace GameEngine::PlatformServices
{

enum class Result
{
    Ok = 0,
    Unsupported,
    NotInitialized,
    AlreadyInitialized,
    InvalidArgument,
    NotFound,
    IOError,
    ProviderError,
    FileTooLarge
};

// Portability limits chosen as the lowest common denominator across shipping
// save-data backends, so a title that validates against one backend cannot
// exceed a stricter one. Game code can size buffers / chunk large state
// against these.
namespace Limits
{
// Smallest per-file write ceiling among shipping backends (one of them caps a
// single save blob at this size). State larger than this must be split across
// multiple files.
inline constexpr uint64_t kMaxSaveFileBytes = 16ull * 1024 * 1024; // 16 MiB
// A slot maps to a backend's save container / mount identifier; the tightest
// of those is very short, so keep slot names terse.
inline constexpr size_t kMaxSlotNameLength = 15;
// A file maps to a blob / entry within a slot; this is well under every
// backend's entry-name limit, on top of the strict charset providers enforce.
inline constexpr size_t kMaxFileNameLength = 64;
} // namespace Limits

enum class Capability : uint32_t
{
    Accounts = 1u << 0,
    LocalSaves = 1u << 1,
    CloudSaves = 1u << 2,
    Achievements = 1u << 3,
    Entitlements = 1u << 4,
    PlatformUI = 1u << 5,
    OfflineMode = 1u << 6
};

using CapabilityMask = uint32_t;

struct ProviderInfo
{
    std::string Name;
    std::string Version;
};

struct Account
{
    std::string Id; // stable opaque id, never a vendor SDK identifier format guarantee
    std::string DisplayName;
    bool SignedIn = false;
    bool GuestOrOffline = true;
};

struct AchievementState
{
    bool Unlocked = false;
    uint32_t Current = 0;
    uint32_t Target = 0;
};

struct Config
{
    // Required. Providers without an explicit SaveRootOverride derive their
    // save location from this (Local uses StandardPaths::GameSavesRoot).
    std::string AppName;
    std::filesystem::path SaveRootOverride;
    // Deterministic entitlement set for providers without a real backend.
    std::vector<std::string> MockEntitlements;
};

} // namespace GameEngine::PlatformServices
