#include "PageStreaming/HeightStoreKey.h"

#include "PageStreaming/PageStoreFormat.h"

#include "AssetCore/SharedFileRead.h"
#include "FileSystem/FileSystem.h"
#include "NonInheritedFile.h"
#include "FileWatcher/FileIdentity.h"
#include "Types/Fnv1a.h"

#include <cstdio>
#include <cstring>
#include <system_error>

namespace GameEngine::PageStreaming
{
namespace
{

// "GEPS" read as a little-endian uint32.
constexpr uint32 kSourceMemoMagic = 0x53504547u;

// What a memo remembers the file by: its modification time, size and filesystem identity.
struct SourceStat
{
    int64 ModifiedTicks = 0;
    uint64 Size = 0;
    uint64 Volume = 0;
    uint64 Index = 0;

    bool operator==(const SourceStat&) const = default;
};

std::optional<SourceStat> StatSource(const std::filesystem::path& source)
{
    std::error_code ec;
    const uint64 size = std::filesystem::file_size(source, ec);
    if (ec)
        return std::nullopt;
    const auto modified = std::filesystem::last_write_time(source, ec);
    if (ec)
        return std::nullopt;
    const FileIdentity identity = ReadFileIdentity(source);
    if (!identity.Valid)
        return std::nullopt;
    return SourceStat{static_cast<int64>(modified.time_since_epoch().count()), size, identity.Volume, identity.Index};
}

std::filesystem::path MemoFile(const std::filesystem::path& directory, const GUID& asset)
{
    return directory / (asset.ToString() + ".source");
}

} // namespace

uint64 ComputeHeightStoreKey(const HeightStoreCookInputs& inputs)
{
    uint64 key = Hashing::Fnv1a64Value(Hashing::kFnv1a64OffsetBasis, kPageStoreFormatVersion);
    key = Hashing::Fnv1a64Value(key, inputs.SourceContentHash);
    key = Hashing::Fnv1a64Value(key, static_cast<uint8>(inputs.Format));
    key = Hashing::Fnv1a64Value(key, inputs.SamplesX);
    key = Hashing::Fnv1a64Value(key, inputs.SamplesZ);
    key = Hashing::Fnv1a64Value(key, static_cast<uint8>(PageFieldFilter::HeightTent));
    key = Hashing::Fnv1a64Value(key, kHeightStepMax);
    return key;
}

std::filesystem::path HeightStoreFile(const std::filesystem::path& directory, const GUID& asset, uint64 key)
{
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "-%016llx.gepage", static_cast<unsigned long long>(key));
    return directory / (asset.ToString() + suffix);
}

std::optional<HeightSourceIdentity> ReadHeightSourceMemo(const std::filesystem::path& directory, const GUID& asset,
                                                         const std::filesystem::path& source)
{
    const std::optional<SourceStat> now = StatSource(source);
    if (!now)
        return std::nullopt;
    // A shared, non-inherited read, so neither an external save nor a child process blocks the next
    // publish over the memo.
    SharedFileReader in;
    bool read = in.Open(MemoFile(directory, asset));
    const auto take = [&in, &read](auto& value) {
        read = read && in.Read(&value, sizeof(value)) == static_cast<int64>(sizeof(value));
    };
    uint32 magic = 0;
    uint32 version = 0;
    SourceStat stored;
    HeightSourceIdentity identity;
    take(magic);
    take(version);
    take(stored.ModifiedTicks);
    take(stored.Size);
    take(stored.Volume);
    take(stored.Index);
    take(identity.ContentHash);
    take(identity.MinHeight);
    take(identity.MaxHeight);
    if (!read || magic != kSourceMemoMagic || version != kPageStoreFormatVersion || stored != *now)
        return std::nullopt;
    return identity;
}

bool WriteHeightSourceMemo(const std::filesystem::path& directory, const GUID& asset,
                           const std::filesystem::path& source, const HeightSourceIdentity& identity)
{
    const std::optional<SourceStat> now = StatSource(source);
    if (!now)
        return false;
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    const std::filesystem::path target = MemoFile(directory, asset);
    const std::filesystem::path temp = FileSystem::MakeTemporarySiblingPath(target);
    std::FILE* out = OpenNonInheritedFile(temp, NonInheritedFileMode::CreateWrite);
    if (!out)
        return false;
    const uint32 version = kPageStoreFormatVersion;
    const auto put = [out](const auto& value) { return std::fwrite(&value, sizeof(value), 1, out) == 1; };
    const bool written = put(kSourceMemoMagic) && put(version) && put(now->ModifiedTicks) && put(now->Size) &&
                         put(now->Volume) && put(now->Index) && put(identity.ContentHash) &&
                         put(identity.MinHeight) && put(identity.MaxHeight);
    if (std::fclose(out) != 0 || !written)
    {
        std::filesystem::remove(temp, ec);
        return false;
    }
    return FileSystem::PublishFile(temp, target);
}

} // namespace GameEngine::PageStreaming
