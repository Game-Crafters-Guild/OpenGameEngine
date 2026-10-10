#include "Assets/Packages/PackageCodeModules.h"

#include "Assets/Packages/PackageCodeDiscovery.h"
#include "Assets/Packages/PackageGitSource.h"
#include "NativeScripting/EngineBuildIdentity.h"

#include <algorithm>
#include <map>
#include <set>

namespace GameEngine
{

namespace
{

void AppendIfAbsent(std::vector<std::string>& out, std::string value)
{
    if (std::find(out.begin(), out.end(), value) == out.end())
        out.push_back(std::move(value));
}

void AppendUnique(std::vector<std::string>& out, const std::vector<std::string>& values)
{
    for (const std::string& v : values)
        AppendIfAbsent(out, v);
}

// Per-package derived data, computed in one topo pass over MountOrder (which is
// already dependencies-first, so a package's dependencies are always present in
// these maps by the time it is visited).
struct PackageDerived
{
    std::vector<std::string> EffectiveDefines;    // deps' first, then own
    std::set<std::string> DependencyClosure;      // transitive dependency package names
    std::string RuntimeCSharpAssembly;            // empty when the package has none
    std::string EditorCSharpAssembly;             // empty when the package has none
};

// The code modules under every enabled package's root, keyed by package name.
// Walked once per collection: both passes below need the same answer.
std::map<std::string, std::vector<DiscoveredPackageModule>> DiscoverAll(const PackageResolution& resolution)
{
    std::map<std::string, std::vector<DiscoveredPackageModule>> discovered;
    for (const ResolvedPackage& pkg : resolution.MountOrder)
        discovered.emplace(pkg.Manifest.Name, ModulesDeliveredBy(pkg));
    return discovered;
}

// The `prebuilt` directory a package declares for one of its modules, absolute,
// or empty when it declares none. This is the one module fact the scan cannot
// answer: a shipped .dll is not an asset type, so a binaries directory is
// invisible to discovery. The declaration is matched to a delivered module by
// (language, kind); a record the scan found no sources for is itself a
// binaries-only module (ModulesDeliveredBy), so every declaration lands.
std::filesystem::path DeclaredPrebuiltDir(const ResolvedPackage& pkg,
                                          PackageModuleRecord::ModuleLang lang,
                                          PackageModuleRecord::ModuleKind kind)
{
    for (const PackageModuleRecord& record : pkg.Manifest.Modules)
    {
        if (record.Lang != lang || record.Kind != kind || record.Prebuilt.empty())
            continue;
        // Manifest dirs conventionally end with '/' ("Binaries/"); normalize the
        // trailing separator away so downstream path comparisons are exact.
        std::filesystem::path dir = (pkg.RootDir / record.Prebuilt).lexically_normal();
        if (!dir.empty() && dir.filename().empty())
            dir = dir.parent_path();
        return dir;
    }
    return {};
}

std::map<std::string, PackageDerived> ComputeDerived(
    const PackageResolution& resolution,
    const std::map<std::string, std::vector<DiscoveredPackageModule>>& discovered)
{
    std::map<std::string, PackageDerived> derived;
    for (const ResolvedPackage& pkg : resolution.MountOrder)
    {
        PackageDerived d;
        for (const auto& [depName, range] : pkg.Manifest.Dependencies)
        {
            const auto it = derived.find(depName);
            if (it == derived.end())
                continue; // dep not in the enabled set (resolver already reported)
            AppendUnique(d.EffectiveDefines, it->second.EffectiveDefines);
            d.DependencyClosure.insert(depName);
            d.DependencyClosure.insert(it->second.DependencyClosure.begin(),
                                       it->second.DependencyClosure.end());
        }
        AppendIfAbsent(d.EffectiveDefines, PackageDefine(pkg.Manifest.Name));

        const std::string baseName = PackageAssemblyName(pkg.Manifest.Name);
        for (const DiscoveredPackageModule& module : discovered.at(pkg.Manifest.Name))
        {
            if (module.Lang != PackageModuleRecord::ModuleLang::CSharp)
                continue;
            if (module.Kind == PackageModuleRecord::ModuleKind::Runtime)
                d.RuntimeCSharpAssembly = baseName;
            else
                d.EditorCSharpAssembly = baseName + ".Editor";
        }
        derived.emplace(pkg.Manifest.Name, std::move(d));
    }
    return derived;
}

} // namespace

std::vector<DiscoveredPackageModule> ModulesDeliveredBy(const ResolvedPackage& package)
{
    std::vector<DiscoveredPackageModule> modules =
        DiscoverPackageCodeModules(package.RootDir, package.Manifest.Name);
    for (const PackageModuleRecord& record : package.Manifest.Modules)
    {
        if (record.Prebuilt.empty())
            continue;
        const bool hasSources =
            std::any_of(modules.begin(), modules.end(), [&record](const DiscoveredPackageModule& module) {
                return module.Lang == record.Lang && module.Kind == record.Kind;
            });
        if (hasSources)
            continue;
        DiscoveredPackageModule binariesOnly;
        binariesOnly.Kind = record.Kind;
        binariesOnly.Lang = record.Lang;
        modules.push_back(binariesOnly);
    }
    std::stable_sort(modules.begin(), modules.end(),
                     [](const DiscoveredPackageModule& a, const DiscoveredPackageModule& b) {
                         const auto rank = [](const DiscoveredPackageModule& m) {
                             return (m.Lang == PackageModuleRecord::ModuleLang::Cpp ? 2 : 0) +
                                    (m.Kind == PackageModuleRecord::ModuleKind::Editor ? 1 : 0);
                         };
                         return rank(a) < rank(b);
                     });
    return modules;
}

std::string PackageAssemblyName(std::string_view packageName)
{
    std::string out;
    out.reserve(packageName.size());
    bool startOfWord = true;
    for (char c : packageName)
    {
        if (c == '@')
            continue;
        if (c == '/' || c == '.' || c == '-')
        {
            startOfWord = true;
            continue;
        }
        if (startOfWord && c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
        out.push_back(c);
        startOfWord = false;
    }
    return out;
}

std::vector<PackageCodeModule> CollectPackageCodeModules(const PackageResolution& resolution,
                                                         bool editorContext)
{
    const std::map<std::string, std::vector<DiscoveredPackageModule>> discovered = DiscoverAll(resolution);
    const std::map<std::string, PackageDerived> derived = ComputeDerived(resolution, discovered);

    std::vector<PackageCodeModule> modules;
    for (const ResolvedPackage& pkg : resolution.MountOrder)
    {
        const PackageDerived& d = derived.at(pkg.Manifest.Name);

        for (const DiscoveredPackageModule& found : discovered.at(pkg.Manifest.Name))
        {
            const bool isEditorKind = found.Kind == PackageModuleRecord::ModuleKind::Editor;
            if (isEditorKind && !editorContext)
                continue; // Editor-kind modules never exist outside the editor.

            PackageCodeModule module;
            module.PackageName = pkg.Manifest.Name;
            module.AssemblyName = PackageAssemblyName(pkg.Manifest.Name);
            if (isEditorKind)
                module.AssemblyName += ".Editor";
            module.Kind = found.Kind;
            module.Lang = found.Lang;
            module.RootDir = found.RootDir;
            module.PackageRootDir = pkg.RootDir;
            module.SourceKind = pkg.SourceKind;
            // Git entries and installed Engine packages are immutable. Their
            // generated projects and binaries use the managed native cache,
            // isolated by package/cache identity and the running Engine build.
            // Its Windows root stays short independently of LOCALAPPDATA depth.
            module.CacheDir =
                pkg.SourceKind == PackageSourceKind::Git ||
                        pkg.SourceKind == PackageSourceKind::Engine
                    ? PackageCacheDerivedNativeRoot(GlobalPackageCacheRoot(),
                                                    pkg.RootDir.filename().string(),
                                                    NativeScripting::EngineBuildIdentity())
                    : pkg.RootDir / ".Cache";
#if !defined(__EMSCRIPTEN__)
            module.UsesManagedNativeCache =
                pkg.SourceKind == PackageSourceKind::Git || pkg.SourceKind == PackageSourceKind::Engine;
#endif
            module.PrebuiltDir = DeclaredPrebuiltDir(pkg, found.Lang, found.Kind);
            module.Defines = d.EffectiveDefines;
            for (const auto& [depName, range] : pkg.Manifest.Dependencies)
                module.DependencyPackages.push_back(depName);

            for (const std::string& depName : d.DependencyClosure)
            {
                const auto it = derived.find(depName);
                if (it == derived.end())
                    continue;
                if (!it->second.RuntimeCSharpAssembly.empty())
                    module.DependencyAssemblies.push_back(it->second.RuntimeCSharpAssembly);
                if (isEditorKind && !it->second.EditorCSharpAssembly.empty())
                    module.DependencyAssemblies.push_back(it->second.EditorCSharpAssembly);
            }

            modules.push_back(std::move(module));
        }
    }
    return modules;
}

std::vector<std::string> CollectAllPackageDefines(const PackageResolution& resolution)
{
    std::vector<std::string> defines;
    for (const ResolvedPackage& pkg : resolution.MountOrder)
        AppendIfAbsent(defines, PackageDefine(pkg.Manifest.Name));
    return defines;
}

} // namespace GameEngine
