#include "Engine/Build/NativeScriptStaging.h"

#include "Logger/Logger.h"
#include "NativeScripting/BuildCacheRecord.h"

#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace GameEngine {

namespace {

// The editor shadow-copies built modules under a content-addressed name
// ("UserScripts_1a2b3c4d.dll"). Strip that suffix so the packaged module ships
// under its plain name — which is also the PDB name its debug directory embeds.
std::string StripShadowCopySuffix(const std::string& stem)
{
    constexpr size_t kHashLen = 8;
    if (stem.size() <= kHashLen + 1 || stem[stem.size() - kHashLen - 1] != '_')
        return stem;
    const std::string_view hash(stem.data() + stem.size() - kHashLen, kHashLen);
    for (char c : hash)
    {
        const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex)
            return stem;
    }
    return stem.substr(0, stem.size() - kHashLen - 1);
}

// Newest "<moduleName>.pdb" under the user build tree. The generator's output
// subdir varies by toolchain (Ninja vs VS), and the shadow copy carries no PDB,
// so the original build output is walked instead.
fs::path FindModulePdb(const fs::path& buildDir, const std::string& moduleName)
{
    fs::path newestPdb;
    fs::file_time_type newestTime{};
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(buildDir, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code entryEc;
        if (!it->is_regular_file(entryEc) || it->path().extension() != ".pdb" ||
            it->path().stem().generic_string() != moduleName)
            continue;
        const auto t = fs::last_write_time(it->path(), entryEc);
        if (!entryEc && (newestPdb.empty() || t > newestTime))
        {
            newestPdb = it->path();
            newestTime = t;
        }
    }
    return newestPdb;
}

} // namespace

NativeScriptStageOutcome StageNativeUserScriptsForPackage(const fs::path& projectRoot,
                                                          const fs::path& contentRoot,
                                                          const EngineAbiDigestSet& engineAbiDigests,
                                                          std::vector<std::string>& errors,
                                                          bool includeDebugSymbols)
{
    namespace ns = NativeScripting;
    const std::string& engineAbiDigest = engineAbiDigests.Current;

    // Two accepted layouts, mirroring LoadPrebuiltUserModule: a project root
    // (<root>/.Cache/NativeScripts/build) or a package module's writable cache
    // dir (<cacheDir>/NativeScripts/build — git packages build outside their
    // immutable cache entry).
    fs::path buildDir = projectRoot / ".Cache" / "NativeScripts" / "build";
    std::optional<ns::BuildCacheRecord> record = ns::ReadBuildCacheRecord(buildDir);
    if (!record)
    {
        buildDir = projectRoot / "NativeScripts" / "build";
        record = ns::ReadBuildCacheRecord(buildDir);
    }
    if (!record)
    {
        Logger::Log::Info("Build: no native C++ user scripts to package (no build record)");
        return NativeScriptStageOutcome::NoScripts;
    }

    // From here on the project HAS a native module; anything that prevents packaging
    // it correctly is a build error — a "successful" build that runs without the
    // user's C++ gameplay is worse than a failed one.
    std::error_code ec;
    fs::path srcDll = record->DllPath;
    if (srcDll.is_relative())
        srcDll = projectRoot / srcDll;
    if (!fs::exists(srcDll, ec))
    {
        errors.push_back("Native script module recorded but missing on disk: " + srcDll.generic_string() +
                         " — rebuild the scripts in the editor, then package again");
        return NativeScriptStageOutcome::Failed;
    }

    // Ship-safety C2: the record's abiDigest names the engine the module was built
    // against; engineAbiDigest names the engine this package ships. They must agree
    // NOW, at package time — and both are staged below so the Player re-checks them
    // at load time (someone swapping files after packaging).
    if (engineAbiDigest.empty())
    {
        errors.push_back("Cannot determine this SDK's engine ABI digest, so the native script module "
                         "cannot be validated for packaging (is the staged SDK manifest present?)");
        return NativeScriptStageOutcome::Failed;
    }
    if (record->AbiDigest.empty())
    {
        errors.push_back("Native script build record predates engine-ABI stamping — rebuild the scripts "
                         "in the editor, then package again");
        return NativeScriptStageOutcome::Failed;
    }
    if (record->AbiDigest != engineAbiDigest)
    {
        // Package defines are digest inputs. Attribute the mismatch: a record
        // matching a defines-variant of THIS engine means the package set
        // changed since the scripts were built, not that the engine's ABI did.
        if (!engineAbiDigests.WithAllPackageDefines.empty() &&
            record->AbiDigest == engineAbiDigests.WithAllPackageDefines)
        {
            errors.push_back(
                "Native user scripts were built while a now-DISABLED package's defines were "
                "active — the enabled-package set changed since the last script build, not the "
                "engine ABI (scripts abi=" + record->AbiDigest + ", engine abi=" +
                engineAbiDigest + "). Rebuild the scripts in the editor, then package again");
        }
        else if (!engineAbiDigests.WithoutPackageDefines.empty() &&
                 engineAbiDigests.WithoutPackageDefines != engineAbiDigest &&
                 record->AbiDigest == engineAbiDigests.WithoutPackageDefines)
        {
            errors.push_back(
                "Native user scripts were built before the current package defines existed — "
                "the enabled-package set changed since the last script build, not the engine "
                "ABI (scripts abi=" + record->AbiDigest + ", engine abi=" + engineAbiDigest +
                "). Rebuild the scripts in the editor, then package again");
        }
        else
        {
            errors.push_back("Native user scripts are stale relative to this engine (scripts abi=" +
                             record->AbiDigest + ", engine abi=" + engineAbiDigest +
                             ") — rebuild the scripts in the editor, then package again");
        }
        return NativeScriptStageOutcome::Failed;
    }

    const std::string moduleName = StripShadowCopySuffix(srcDll.stem().generic_string());
    const std::string dllName = moduleName + srcDll.extension().generic_string();
    const fs::path destDir = contentRoot / "NativeScripts";
    fs::create_directories(destDir, ec);
    fs::copy_file(srcDll, destDir / dllName, fs::copy_options::overwrite_existing, ec);
    if (ec)
    {
        errors.push_back("Failed to stage native script module '" + dllName + "': " + ec.message());
        return NativeScriptStageOutcome::Failed;
    }

    // PDB: the shadow copy never carries one, so take the newest matching PDB from
    // the build output. Missing symbols degrade crash reports, not the game — stage
    // without when absent (records built before user-DLL PDBs existed).
    bool stagedPdb = false;
    const fs::path srcPdb = includeDebugSymbols ? FindModulePdb(buildDir, moduleName) : fs::path{};
    if (!srcPdb.empty())
    {
        std::error_code pdbEc;
        fs::copy_file(srcPdb, destDir / (moduleName + ".pdb"), fs::copy_options::overwrite_existing, pdbEc);
        stagedPdb = !pdbEc;
    }
    if (includeDebugSymbols && !stagedPdb)
        Logger::Log::Info("Build: no PDB found for native script module '{}' — packaged without symbols", dllName);

    // Relocatable record: the DLL path is RELATIVE to the game root (= the Player's
    // project root), so the package survives being moved to another machine. The
    // engine_abi marker beside it is what LoadPrebuiltUserModule validates against.
    if (!ns::WriteBuildCacheRecord(destDir, ns::BuildCacheRecord{record->Digest, "NativeScripts/" + dllName,
                                                                 record->AbiDigest,
                                                                 /*engineBuildId=*/{}}) ||
        !ns::WriteEngineAbiMarker(destDir, engineAbiDigest))
    {
        errors.push_back("Failed to write the packaged native-script record; the game would load no user scripts");
        return NativeScriptStageOutcome::Failed;
    }

    Logger::Log::Info("Build: packaged native C++ user-script module '{}'{} into NativeScripts/", dllName,
                      stagedPdb ? " (+ PDB)" : "");
    return NativeScriptStageOutcome::Staged;
}

} // namespace GameEngine
