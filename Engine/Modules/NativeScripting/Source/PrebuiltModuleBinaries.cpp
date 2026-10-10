#include "NativeScripting/PrebuiltModuleBinaries.h"

#include "NativeScripting/BuildCacheRecord.h"

#include <system_error>

namespace GameEngine
{
namespace NativeScripting
{

namespace
{

constexpr const char* kPlatformName =
#if defined(_WIN32)
    "windows";
#elif defined(__APPLE__)
    "macos";
#else
    "linux";
#endif

constexpr const char* kArchName =
#if defined(_M_X64) || defined(__x86_64__)
    "x64";
#elif defined(_M_ARM64) || defined(__aarch64__)
    "arm64";
#else
    "unknown";
#endif

constexpr const char* kModuleExtension =
#if defined(_WIN32)
    ".dll";
#elif defined(__APPLE__)
    ".dylib";
#else
    ".so";
#endif

} // namespace

std::string ToolchainFingerprintShortHash(const GE_ToolchainFingerprint& fingerprint)
{
    // Serialize the compared fields (never the padding) as text so the hash is
    // layout-independent and stable across engine versions.
    std::string text = "v1:";
    const uint32_t fields[] = {
        fingerprint.CompilerVendor,     fingerprint.CompilerVersionMajor,
        fingerprint.CompilerVersionMinor, fingerprint.CrtId,
        fingerprint.IteratorDebugLevel, fingerprint.ZcFlags,
        fingerprint.StdLibAbiTag,
    };
    for (const uint32_t field : fields)
    {
        text += std::to_string(field);
        text += ':';
    }
    return ToHexDigest(Fnv1aHash(text)).substr(0, 8);
}

std::string PrebuiltPlatformDirName(const GE_ToolchainFingerprint& fingerprint)
{
    std::string name = kPlatformName;
    name += '-';
    name += kArchName;
    name += '-';
    name += ToolchainFingerprintShortHash(fingerprint);
    return name;
}

std::string PrebuiltModuleFileName(const std::string& moduleName)
{
    return moduleName + kModuleExtension;
}

PrebuiltModuleLookup FindPrebuiltModule(const std::filesystem::path& prebuiltRoot,
                                        const GE_ToolchainFingerprint& hostFingerprint,
                                        const std::string& moduleName,
                                        const std::string& hostEngineAbiDigest)
{
    PrebuiltModuleLookup lookup;
    lookup.PlatformDir = prebuiltRoot / PrebuiltPlatformDirName(hostFingerprint);

    std::error_code ec;
    if (!std::filesystem::is_directory(prebuiltRoot, ec))
    {
        lookup.Reason = "prebuilt dir '" + prebuiltRoot.generic_string() + "' does not exist";
        return lookup;
    }
    if (!std::filesystem::is_directory(lookup.PlatformDir, ec))
    {
        // Name the dirs that ARE shipped so a fingerprint mismatch is diagnosable.
        std::string available;
        for (std::filesystem::directory_iterator it(prebuiltRoot, ec), end; !ec && it != end;
             it.increment(ec))
        {
            if (!it->is_directory())
                continue;
            if (!available.empty())
                available += ", ";
            available += it->path().filename().string();
        }
        lookup.Reason = "no binaries for this toolchain (need '" +
                        lookup.PlatformDir.filename().string() + "', package ships: " +
                        (available.empty() ? "none" : available) + ")";
        return lookup;
    }

    const std::filesystem::path dll = lookup.PlatformDir / PrebuiltModuleFileName(moduleName);
    if (!std::filesystem::exists(dll, ec))
    {
        lookup.Reason = "'" + dll.filename().string() + "' missing in '" +
                        lookup.PlatformDir.generic_string() + "'";
        return lookup;
    }

    // Optional engine-ABI marker: when the author pinned the engine identity,
    // a mismatch means the DLL was built against a different engine build —
    // do not use it (the source fallback will rebuild against this engine).
    const std::string marker = ReadEngineAbiMarker(lookup.PlatformDir);
    if (!marker.empty() && marker != hostEngineAbiDigest)
    {
        lookup.Reason = "engine_abi marker mismatch (prebuilt=" + marker +
                        ", host=" + hostEngineAbiDigest + ")";
        return lookup;
    }

    lookup.Dll = dll;
    return lookup;
}

} // namespace NativeScripting
} // namespace GameEngine
