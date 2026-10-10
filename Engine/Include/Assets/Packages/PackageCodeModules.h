#pragma once

// Package code modules — flattens a PackageResolution into the per-module
// descriptors the compile-graph builders consume: the C# ScriptManager (one
// generated csproj + compile-server request per CSharp module) and the
// NativeScriptManager (one NativeBuildConfig per Cpp module). The modules
// themselves are discovered under each package root (PackageCodeDiscovery);
// nothing here writes or compiles. Collection is pure data shaping; consumers
// acquire a process-lifetime lease only when they actually use a managed native
// cache directory.

#include "Assets/Packages/PackageCodeDiscovery.h"
#include "Assets/Packages/PackageResolver.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

// One code module of a resolved, enabled package.
struct PackageCodeModule
{
    std::string PackageName; // manifest name, e.g. "@studio/water.core"
    // The module's build identity: C# AssemblyName (=> <AssemblyName>.dll) and
    // native ModuleName. See PackageAssemblyName for the mapping.
    std::string AssemblyName;
    PackageModuleRecord::ModuleKind Kind = PackageModuleRecord::ModuleKind::Runtime;
    PackageModuleRecord::ModuleLang Lang = PackageModuleRecord::ModuleLang::CSharp;
    std::filesystem::path RootDir;        // absolute module source root
    std::filesystem::path PackageRootDir; // package dir
    // Where the package came from. Embedded and File packages are the
    // developer's own trees, so the editor watches their C# module roots for
    // hot reload; Git cache entries and staged Engine packages never change in
    // place, so their roots are not watched.
    PackageSourceKind SourceKind = PackageSourceKind::Embedded;
    // Writable derived-data root for native builds under NativeScripts/.
    // Embedded/file packages own <PackageRootDir>/.Cache; immutable git/engine
    // packages use PackageCacheDerivedNativeRoot, with short package and engine
    // identities. Windows uses the temp root to keep CMake intermediates short
    // even when LOCALAPPDATA is redirected. The asset-DB mount cache stays under
    // the host's asset cache root.
    std::filesystem::path CacheDir;
    // True only for desktop Git/Engine modules whose CacheDir is under the
    // machine-global managed .native namespace. Consumers must acquire its
    // lease before creating files or loading a module from it.
    bool UsesManagedNativeCache = false;
    // Absolute root of shipped prebuilt binaries (manifest `prebuilt` dir);
    // empty when the module builds from source only. Native loaders try the
    // host-fingerprint-matching DLL under it before compiling; a C# module
    // uses <PrebuiltDir>/<AssemblyName>.dll only when it has no sources
    // (ScriptManager::SetPackageCodeModules).
    std::filesystem::path PrebuiltDir;
    // Effective compile defines: the package's own derived define plus every
    // transitive dependency's (dependencies first, deduped) — so a consumer can
    // `#if GE_PACKAGE_OCEAN_PACK` against anything below it in the graph.
    // See PackageDefine (PackageResolver.h) for the name each package exports.
    std::vector<std::string> Defines;
    // AssemblyNames of dependency packages' CSharp modules this module compiles
    // against (transitive closure): Runtime assemblies always, plus dependency
    // Editor assemblies when this module is Editor-kind.
    std::vector<std::string> DependencyAssemblies;
    // Direct dependency PACKAGE names (for failure propagation: a package whose
    // dependency failed to compile is skipped too).
    std::vector<std::string> DependencyPackages;
};

// Deterministic package-name -> assembly/module-name mapping:
// drop a scope's '@'; treat '/', '.' and '-' as word separators; uppercase each
// word's first character and concatenate. '_' passes through verbatim here,
// where the mount alias folds it to '-' (SanitizePackageAlias). Examples:
//   "sample-pack"          -> "SamplePack"
//   "@studio/water.core"   -> "StudioWaterCore"
//   "a_b"                  -> "A_b"   (mount alias "a-b")
// Editor-kind modules (CSharp AND Cpp) append ".Editor" (see
// CollectPackageCodeModules), so a package's editor half never collides with
// its runtime half in assembly names, module ids, or build cache dirs.
// Uniqueness: the resolver rejects packages whose sanitized aliases collide,
// which covers every pair differing only in how a separator is spelled. It does
// NOT cover a trailing separator, which survives into the alias and vanishes
// here: "ab" and "ab-" are two aliases and one assembly name (issue #1783).
std::string PackageAssemblyName(std::string_view packageName);

// The code modules a resolved package delivers: those discovered under its
// root (DiscoverPackageCodeModules), plus a module its manifest still declares
// by `prebuilt` alone when the scan found no sources of that language and
// kind. A shipped binary is not an asset type, so a package shipping binaries
// without sources delivers that module with an empty RootDir; its PrebuiltDir
// is what the loaders read. A binary the manifest does not declare delivers
// nothing. Ordered like discovery: C# before C++, runtime before editor.
std::vector<DiscoveredPackageModule> ModulesDeliveredBy(const ResolvedPackage& package);

// Flatten resolution.MountOrder (already dependency-topo order) into code
// modules, preserving that order — compile them in sequence and dependencies
// are always built first. Each package root is walked once
// (ModulesDeliveredBy), which yields at most one module per (language, kind).
// Rules applied here:
//  - editorContext=false (Player): Editor-kind modules are excluded entirely.
//  - Editor-kind assemblies (CSharp and Cpp) get the ".Editor" suffix. Native
//    Editor-kind modules build against the staged EditorSDK (the editor
//    wiring fills EditorImportLib/EditorIncludeDirs on their build configs).
//  - a declared `prebuilt` dir resolves onto the matching module (PrebuiltDir);
//    native loaders probe it fingerprint-keyed before building from source,
//    and a C# module with no sources loads the assembly it holds.
std::vector<PackageCodeModule> CollectPackageCodeModules(const PackageResolution& resolution,
                                                         bool editorContext);

// Every enabled package's derived define, in mount (dependencies-first) order,
// deduped. The project's own compile graphs (project scripts + project native
// module) receive these — the project is an implicit dependent of every
// enabled package.
std::vector<std::string> CollectAllPackageDefines(const PackageResolution& resolution);

} // namespace GameEngine
