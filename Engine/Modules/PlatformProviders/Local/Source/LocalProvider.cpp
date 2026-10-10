// Local fallback provider: offline account, file/slot saves on local disk,
// and deterministic mock achievements/entitlements. This is the provider for
// editor play mode, tests, and player builds without a vendor platform SDK.

#include "PlatformServices/PlatformProviderV1.h"
#include "PlatformServices/PlatformServicesTypes.h"

#include "Core/StandardPaths.h"
#include "FileSystem/FileSystem.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{

namespace Limits = GameEngine::PlatformServices::Limits;

constexpr std::string_view kProviderName = "Local";
constexpr std::string_view kProviderVersion = "1.0";
constexpr std::string_view kAccountId = "local-offline";
constexpr std::string_view kAccountDisplayName = "Local User";
constexpr std::string_view kWriteTempSuffix = ".gewrite";
// Lives directly in the save root; never collides with slot directories
// (ListSlots only reports directories) or save files (those live inside
// slot directories).
constexpr std::string_view kAchievementsFileName = "achievements.state";

struct LocalProviderState
{
    bool Initialized = false;
    bool SignedIn = false;
    std::filesystem::path SaveRoot;
    std::vector<std::string> Entitlements;
    std::unordered_map<std::string, GE_PS_AchievementStateV1> Achievements;
};

LocalProviderState& State()
{
    static LocalProviderState s_State;
    return s_State;
}

// Provider entry points may be called from game and script threads
// concurrently; one coarse lock keeps state and filesystem access coherent.
// Kept outside LocalProviderState so Shutdown can reset the state by
// assignment.
std::mutex& StateMutex()
{
    static std::mutex s_Mutex;
    return s_Mutex;
}

std::string_view ToView(GE_PS_Str str)
{
    return std::string_view(str.data ? str.data : "", str.length);
}

GE_PS_Str ToStr(std::string_view view)
{
    return GE_PS_Str{view.data(), static_cast<uint32_t>(view.size())};
}

std::filesystem::path ToPath(std::string_view utf8)
{
    return std::filesystem::path(
        std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

// Slot and file names form on-disk paths, so they are restricted to a strict
// portable subset: alphanumerics plus '.', '_', '-', no leading dot, bounded
// length. This rejects traversal ("..", separators) by construction. Length
// caps come from the shared portability limits (slots are tighter than
// files). The facade enforces the same caps; this is the filesystem-side
// guard.
bool IsValidName(std::string_view name, size_t maxLength)
{
    if (name.empty() || name.size() > maxLength)
        return false;
    if (name.front() == '.')
        return false;
    for (const char c : name)
    {
        const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!valid)
            return false;
    }
    return true;
}

bool IsValidSlot(std::string_view slot)
{
    return IsValidName(slot, Limits::kMaxSlotNameLength);
}

bool IsValidFile(std::string_view name)
{
    return IsValidName(name, Limits::kMaxFileNameLength);
}

// Achievements are mock state for tests/editor play, but they survive
// restarts so play-mode sessions behave like a real platform. One line per
// achievement: "<unlocked>|<current>|<target>|<id>" (id last — it may
// contain '|').
void LoadAchievements(LocalProviderState& state)
{
    state.Achievements.clear();
    std::ifstream stream(state.SaveRoot / kAchievementsFileName);
    if (!stream)
        return;
    std::string line;
    while (std::getline(stream, line))
    {
        uint32_t fields[3] = {};
        size_t offset = 0;
        bool valid = true;
        for (uint32_t& field : fields)
        {
            const size_t separator = line.find('|', offset);
            if (separator == std::string::npos)
            {
                valid = false;
                break;
            }
            field = static_cast<uint32_t>(std::strtoul(line.c_str() + offset, nullptr, 10));
            offset = separator + 1;
        }
        if (!valid || offset >= line.size())
            continue;
        state.Achievements[line.substr(offset)] =
            GE_PS_AchievementStateV1{fields[0] != 0 ? 1 : 0, fields[1], fields[2]};
    }
}

void SaveAchievements(const LocalProviderState& state)
{
    std::ostringstream content;
    for (const auto& [id, achievement] : state.Achievements)
    {
        content << achievement.unlocked << '|' << achievement.current << '|'
                << achievement.target << '|' << id << '\n';
    }

    const std::filesystem::path finalPath = state.SaveRoot / kAchievementsFileName;
    std::filesystem::path tempPath = finalPath;
    tempPath += kWriteTempSuffix;
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream)
            return;
        const std::string data = content.str();
        stream.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!stream)
            return;
    }
    GameEngine::FileSystem::PublishFile(tempPath, finalPath);
}

GE_PS_Result Local_Initialize(const GE_PS_ConfigV1* config)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    LocalProviderState& state = State();
    if (state.Initialized)
        return GE_PS_AlreadyInitialized;
    if (config == nullptr)
        return GE_PS_InvalidArgument;

    const std::string_view appName = ToView(config->appName);
    const std::string_view saveRoot = ToView(config->saveRoot);
    if (appName.empty() && saveRoot.empty())
        return GE_PS_InvalidArgument;

    state.SaveRoot = saveRoot.empty() ? GameEngine::StandardPaths::GameSavesRoot(appName)
                                      : ToPath(saveRoot);

    std::error_code ec;
    std::filesystem::create_directories(state.SaveRoot, ec);
    if (ec)
        return GE_PS_IOError;

    state.Entitlements.clear();
    for (uint32_t i = 0; i < config->entitlementCount; ++i)
        state.Entitlements.emplace_back(ToView(config->entitlements[i]));

    LoadAchievements(state);
    state.SignedIn = true;
    state.Initialized = true;
    return GE_PS_Ok;
}

void Local_Shutdown(void)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    State() = LocalProviderState{};
}

void Local_Tick(void)
{
}

void Local_GetProviderInfo(GE_PS_ProviderInfoV1* outInfo)
{
    outInfo->name = ToStr(kProviderName);
    outInfo->version = ToStr(kProviderVersion);
}

uint32_t Local_GetCapabilities(void)
{
    return GE_PS_Cap_Accounts | GE_PS_Cap_LocalSaves | GE_PS_Cap_Achievements |
           GE_PS_Cap_Entitlements | GE_PS_Cap_OfflineMode;
}

GE_PS_Result Local_GetPrimaryAccount(GE_PS_AccountV1* outAccount)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    outAccount->id = ToStr(kAccountId);
    outAccount->displayName = ToStr(kAccountDisplayName);
    outAccount->signedIn = state.SignedIn ? 1 : 0;
    outAccount->guestOrOffline = 1;
    return GE_PS_Ok;
}

GE_PS_Result Local_RequestSignIn(void)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    state.SignedIn = true;
    return GE_PS_Ok;
}

GE_PS_Result Local_RequestSignOut(void)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    state.SignedIn = false;
    return GE_PS_Ok;
}

GE_PS_Result Local_ListSlots(GE_PS_StringSinkFn sink, void* userData)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(state.SaveRoot, ec))
    {
        if (!entry.is_directory())
            continue;
        const std::u8string nameU8 = entry.path().filename().u8string();
        const std::string name(reinterpret_cast<const char*>(nameU8.data()), nameU8.size());
        if (IsValidSlot(name))
            sink(ToStr(name), userData);
    }
    return ec ? GE_PS_IOError : GE_PS_Ok;
}

GE_PS_Result Local_ListFiles(GE_PS_Str slot, GE_PS_StringSinkFn sink, void* userData)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    if (!IsValidSlot(ToView(slot)))
        return GE_PS_InvalidArgument;

    const std::filesystem::path slotPath = state.SaveRoot / ToView(slot);
    std::error_code ec;
    if (!std::filesystem::is_directory(slotPath, ec))
        return GE_PS_NotFound;

    for (const auto& entry : std::filesystem::directory_iterator(slotPath, ec))
    {
        if (!entry.is_regular_file())
            continue;
        const std::u8string nameU8 = entry.path().filename().u8string();
        const std::string name(reinterpret_cast<const char*>(nameU8.data()), nameU8.size());
        if (IsValidFile(name))
            sink(ToStr(name), userData);
    }
    return ec ? GE_PS_IOError : GE_PS_Ok;
}

GE_PS_Result Local_ReadFile(GE_PS_Str slot, GE_PS_Str name, GE_PS_BytesSinkFn sink, void* userData)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    if (!IsValidSlot(ToView(slot)) || !IsValidFile(ToView(name)))
        return GE_PS_InvalidArgument;

    const std::filesystem::path filePath = state.SaveRoot / ToView(slot) / ToView(name);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(filePath, ec))
        return GE_PS_NotFound;

    std::ifstream stream(filePath, std::ios::binary);
    if (!stream)
        return GE_PS_IOError;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(stream)),
                               std::istreambuf_iterator<char>());
    if (stream.bad())
        return GE_PS_IOError;

    sink(bytes.data(), bytes.size(), userData);
    return GE_PS_Ok;
}

GE_PS_Result Local_WriteFile(GE_PS_Str slot, GE_PS_Str name, const uint8_t* data, uint64_t size)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    if (!IsValidSlot(ToView(slot)) || !IsValidFile(ToView(name)))
        return GE_PS_InvalidArgument;
    if (data == nullptr && size > 0)
        return GE_PS_InvalidArgument;

    const std::filesystem::path slotPath = state.SaveRoot / ToView(slot);
    std::error_code ec;
    std::filesystem::create_directories(slotPath, ec);
    if (ec)
        return GE_PS_IOError;

    // Write through a temp file + rename so an interrupted write never
    // leaves a truncated save behind.
    const std::filesystem::path finalPath = slotPath / ToView(name);
    std::filesystem::path tempPath = finalPath;
    tempPath += kWriteTempSuffix;
    {
        std::ofstream stream(tempPath, std::ios::binary | std::ios::trunc);
        if (!stream)
            return GE_PS_IOError;
        if (size > 0)
            stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        if (!stream)
            return GE_PS_IOError;
    }
    if (!GameEngine::FileSystem::PublishFile(tempPath, finalPath))
        return GE_PS_IOError;
    return GE_PS_Ok;
}

GE_PS_Result Local_DeleteFile(GE_PS_Str slot, GE_PS_Str name)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    if (!IsValidSlot(ToView(slot)) || !IsValidFile(ToView(name)))
        return GE_PS_InvalidArgument;

    const std::filesystem::path filePath = state.SaveRoot / ToView(slot) / ToView(name);
    std::error_code ec;
    if (!std::filesystem::remove(filePath, ec) || ec)
        return ec ? GE_PS_IOError : GE_PS_NotFound;
    return GE_PS_Ok;
}

GE_PS_Result Local_Commit(GE_PS_Str slot)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    // Local writes are already durable (temp + rename); nothing to sync.
    if (!State().Initialized)
        return GE_PS_NotInitialized;
    if (!IsValidSlot(ToView(slot)))
        return GE_PS_InvalidArgument;
    return GE_PS_Ok;
}

GE_PS_Result Local_UnlockAchievement(GE_PS_Str id)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    const std::string_view idView = ToView(id);
    if (idView.empty())
        return GE_PS_InvalidArgument;

    GE_PS_AchievementStateV1& achievement = state.Achievements[std::string(idView)];
    achievement.unlocked = 1;
    if (achievement.target > 0)
        achievement.current = achievement.target;
    SaveAchievements(state);
    return GE_PS_Ok;
}

GE_PS_Result Local_SetAchievementProgress(GE_PS_Str id, uint32_t current, uint32_t target)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    const std::string_view idView = ToView(id);
    if (idView.empty() || target == 0)
        return GE_PS_InvalidArgument;

    GE_PS_AchievementStateV1& achievement = state.Achievements[std::string(idView)];
    achievement.current = current;
    achievement.target = target;
    if (current >= target)
        achievement.unlocked = 1;
    SaveAchievements(state);
    return GE_PS_Ok;
}

GE_PS_Result Local_GetAchievementState(GE_PS_Str id, GE_PS_AchievementStateV1* outState)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    const std::string_view idView = ToView(id);
    if (idView.empty())
        return GE_PS_InvalidArgument;

    // Untouched achievements report a default (locked, zero progress) state
    // rather than NotFound — the provider has no achievement schema to
    // validate ids against.
    const auto it = state.Achievements.find(std::string(idView));
    *outState = it != state.Achievements.end() ? it->second : GE_PS_AchievementStateV1{};
    return GE_PS_Ok;
}

GE_PS_Result Local_HasEntitlement(GE_PS_Str id, int32_t* outHas)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    const std::string_view idView = ToView(id);
    *outHas = 0;
    for (const std::string& entitlement : state.Entitlements)
    {
        if (entitlement == idView)
        {
            *outHas = 1;
            break;
        }
    }
    return GE_PS_Ok;
}

GE_PS_Result Local_ListEntitlements(GE_PS_StringSinkFn sink, void* userData)
{
    const std::lock_guard<std::mutex> lock(StateMutex());
    const LocalProviderState& state = State();
    if (!state.Initialized)
        return GE_PS_NotInitialized;
    for (const std::string& entitlement : state.Entitlements)
        sink(ToStr(entitlement), userData);
    return GE_PS_Ok;
}

const GE_PlatformProviderV1 kLocalProviderTable = {
    /*abiMajor*/ 1,
    /*abiMinor*/ 0,
    /*sizeBytes*/ sizeof(GE_PlatformProviderV1),
    &Local_Initialize,
    &Local_Shutdown,
    &Local_Tick,
    &Local_GetProviderInfo,
    &Local_GetCapabilities,
    &Local_GetPrimaryAccount,
    &Local_RequestSignIn,
    &Local_RequestSignOut,
    &Local_ListSlots,
    &Local_ListFiles,
    &Local_ReadFile,
    &Local_WriteFile,
    &Local_DeleteFile,
    &Local_Commit,
    &Local_UnlockAchievement,
    &Local_SetAchievementProgress,
    &Local_GetAchievementState,
    &Local_HasEntitlement,
    &Local_ListEntitlements,
    // No native platform UI: NULL maps to Unsupported in the facade.
    /*ShowAchievements*/ nullptr,
    /*ShowAccountPicker*/ nullptr,
    /*ShowStorePage*/ nullptr,
};

} // namespace

extern "C" const GE_PlatformProviderV1* GE_GetPlatformProviderV1(void)
{
    return &kLocalProviderTable;
}
