#include "Assets/Packages/PackagesLockFile.h"

#include "AssetCore/SharedFileRead.h"

#include <cstdint>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

namespace GameEngine
{

namespace
{

constexpr int kLockFormatVersion = 1;

// Local FNV-1a 64 so the asset-side lock format has no NativeScripting
// dependency (that module owns an identical helper for build digests).
std::uint64_t Fnv1a64(std::string_view data)
{
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const char c : data)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::string ReadStringField(const nlohmann::json& node, const char* key)
{
    const auto it = node.find(key);
    return (it != node.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

bool TryLoadPackagesLock(const std::filesystem::path& lockFile,
                         PackagesLock& out,
                         std::string& outError)
{
    out = PackagesLock{};
    outError.clear();

    std::error_code ec;
    if (!std::filesystem::exists(lockFile, ec))
        return true; // no lock yet — first git resolve writes one

    String text;
    if (!ReadFileTextShared(lockFile, text))
    {
        outError = "packages-lock.json (" + lockFile.generic_string() + "): cannot open file";
        return false;
    }

    const nlohmann::json doc =
        nlohmann::json::parse(text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object())
    {
        outError = "packages-lock.json (" + lockFile.generic_string() + "): not a JSON object";
        return false;
    }
    const auto packagesIt = doc.find("packages");
    if (packagesIt == doc.end() || !packagesIt->is_object())
    {
        outError = "packages-lock.json (" + lockFile.generic_string() +
                   "): missing 'packages' object";
        return false;
    }

    for (const auto& [name, node] : packagesIt->items())
    {
        if (!node.is_object())
        {
            outError = "packages-lock.json (" + lockFile.generic_string() + "): entry '" + name +
                       "' must be an object";
            return false;
        }
        PackagesLockEntry entry;
        entry.Spec = ReadStringField(node, "spec");
        entry.Url = ReadStringField(node, "url");
        entry.Ref = ReadStringField(node, "ref");
        entry.Commit = ReadStringField(node, "commit");
        entry.Version = ReadStringField(node, "version");
        entry.Integrity = ReadStringField(node, "integrity");
        if (entry.Spec.empty() || entry.Commit.empty() || entry.Version.empty())
        {
            outError = "packages-lock.json (" + lockFile.generic_string() + "): entry '" + name +
                       "' needs at least 'spec', 'commit', and 'version'";
            return false;
        }
        out.Packages.emplace(name, std::move(entry));
    }
    return true;
}

bool SavePackagesLock(const std::filesystem::path& lockFile, const PackagesLock& lock)
{
    nlohmann::json packages = nlohmann::json::object();
    for (const auto& [name, entry] : lock.Packages)
    {
        nlohmann::json node = nlohmann::json::object();
        node["spec"] = entry.Spec;
        node["url"] = entry.Url;
        node["ref"] = entry.Ref;
        node["commit"] = entry.Commit;
        node["version"] = entry.Version;
        node["integrity"] = entry.Integrity;
        packages[name] = std::move(node);
    }
    nlohmann::json doc = nlohmann::json::object();
    doc["version"] = kLockFormatVersion;
    doc["packages"] = std::move(packages);

    std::error_code ec;
    std::filesystem::create_directories(lockFile.parent_path(), ec);
    std::ofstream out(lockFile, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << doc.dump(2) << '\n';
    return out.good();
}

bool RemovePackagesLockEntry(const std::filesystem::path& lockFile,
                             const std::string& packageName,
                             std::string& outError)
{
    outError.clear();
    PackagesLock lock;
    if (!TryLoadPackagesLock(lockFile, lock, outError))
        return false;
    if (lock.Packages.erase(packageName) == 0)
        return true; // no pin = nothing to drop
    if (!SavePackagesLock(lockFile, lock))
    {
        outError = "packages-lock.json (" + lockFile.generic_string() + "): cannot write file";
        return false;
    }
    return true;
}

bool UpsertPackagesLockEntry(const std::filesystem::path& lockFile,
                             const std::string& packageName,
                             const PackagesLockEntry& entry,
                             std::string& outError)
{
    outError.clear();
    PackagesLock lock;
    if (!TryLoadPackagesLock(lockFile, lock, outError))
        return false;
    lock.Packages[packageName] = entry;
    if (!SavePackagesLock(lockFile, lock))
    {
        outError = "packages-lock.json (" + lockFile.generic_string() + "): cannot write file";
        return false;
    }
    return true;
}

std::string HashPackageManifestIntegrity(std::string_view manifestText)
{
    constexpr char kHexDigits[] = "0123456789abcdef";
    const std::uint64_t hash = Fnv1a64(manifestText);
    std::string text = "fnv1a64-";
    for (int shift = 60; shift >= 0; shift -= 4)
        text += kHexDigits[(hash >> shift) & 0xF];
    return text;
}

} // namespace GameEngine
