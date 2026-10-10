#include "NativeScripting/BuildCacheRecord.h"

#include "NativeScripting/NativeBuildConfig.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <system_error>

namespace GameEngine
{
namespace NativeScripting
{

std::optional<BuildCacheRecord> ReadBuildCacheRecord(const std::filesystem::path& dir)
{
    std::ifstream in(dir / kBuildCacheFileName);
    if (!in)
        return std::nullopt;
    BuildCacheRecord record;
    std::getline(in, record.Digest);
    std::getline(in, record.DllPath);
    std::getline(in, record.AbiDigest);     // absent on older 2-line caches -> empty
    std::getline(in, record.EngineBuildId); // absent on older 3-line caches -> empty
    if (record.DllPath.empty())
        return std::nullopt;
    return record;
}

bool WriteBuildCacheRecord(const std::filesystem::path& dir, const BuildCacheRecord& record)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / kBuildCacheFileName, std::ios::trunc);
    if (!out)
        return false;
    out << record.Digest << '\n'
        << record.DllPath << '\n'
        << record.AbiDigest << '\n'
        << record.EngineBuildId << '\n';
    out.flush();
    return static_cast<bool>(out);
}

std::string ReadEngineAbiMarker(const std::filesystem::path& dir)
{
    std::ifstream in(dir / kEngineAbiMarkerFileName);
    if (!in)
        return {};
    std::string digest;
    std::getline(in, digest);
    return digest;
}

bool WriteEngineAbiMarker(const std::filesystem::path& dir, const std::string& abiDigest)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(dir / kEngineAbiMarkerFileName, std::ios::trunc);
    if (!out)
        return false;
    out << abiDigest << '\n';
    out.flush();
    return static_cast<bool>(out);
}

std::uint64_t Fnv1aHash(std::string_view data, std::uint64_t seed)
{
    for (unsigned char c : data)
    {
        seed ^= c;
        seed *= 0x00000100000001b3ULL;
    }
    return seed;
}

std::string ToHexDigest(std::uint64_t hash)
{
    char hex[17];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(hex);
}

std::uint64_t HashEngineAbiInputs(const NativeBuildConfig& config)
{
    // Config first, then the import-lib identity, then the compile definitions — the
    // exact input order the pre-C2 in-manager hash used, so existing build caches
    // stay valid across this refactor.
    const auto hashImportLib = [](const std::filesystem::path& lib, std::uint64_t seed) {
        std::error_code ec;
        const auto libSize = static_cast<std::uintmax_t>(std::filesystem::file_size(lib, ec));
        const auto libMtime = static_cast<long long>(
            std::filesystem::last_write_time(lib, ec).time_since_epoch().count());
        std::ostringstream stream;
        stream << lib.generic_string() << ':' << libSize << ':' << libMtime;
        return Fnv1aHash(stream.str(), seed);
    };

    std::uint64_t hash = Fnv1aHash(config.Config);
    hash = hashImportLib(config.EngineImportLib, hash);
    for (const std::string& def : config.CompileDefinitions)
        hash = Fnv1aHash(def, hash);
    // Editor-kind modules additionally fold the EditorSDK identity — appended
    // AFTER the legacy inputs so every Runtime-module digest (EditorImportLib
    // empty) is byte-identical to the pre-EditorSDK hash and existing caches
    // stay valid.
    if (!config.EditorImportLib.empty())
        hash = hashImportLib(config.EditorImportLib, hash);
    return hash;
}

std::string ComputeEngineAbiDigest(const NativeBuildConfig& config)
{
    return ToHexDigest(HashEngineAbiInputs(config));
}

std::string ComputeRuntimeEngineAbiDigest(NativeBuildConfig config)
{
    config.EditorImportLib.clear();
    config.EditorIncludeDirs.clear();
    return ComputeEngineAbiDigest(config);
}

} // namespace NativeScripting
} // namespace GameEngine
