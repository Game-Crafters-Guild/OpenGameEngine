#pragma once

#include "Assets/Packages/PackageManifest.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace GameEngine
{

// Priority band for package mounts (formalized bands: project = 100,
// packages = 30..69 in dependency-topo order, editor = 50 in the editor /
// 0 conceptually below packages once bands are fully formalized).
//
// Direction (design call, documented per the package plan §7.4): DEPENDENTS
// get HIGHER priority than their dependencies. A pack that consumes
// water-core sits above water-core, so the consumer can override/shadow its
// dependency's assets at the same logical path — mirroring how the project
// (priority 100) shadows every package. Priorities are assigned base+index
// walking the topo order (dependencies first), clamped to the band max.
inline constexpr int32_t kPackageMountPriorityBase = 30;
inline constexpr int32_t kPackageMountPriorityMax = 69;

// Where a resolved package came from — decides its mount shape (embedded/file
// packages are mutable + watched; git packages live in the global immutable
// cache and mount read-only) and what the Package Manager panel displays.
// Engine packages are first-party packages staged next to the executable
// (<exe dir>/Packages/<name>, authored in the repo's Packages/):
// resolved implicitly for every project, identified by their committed
// .assetmanifest (stored identity), with native module builds redirected to the
// managed .native cache (under TEMP on Windows, the package-cache root elsewhere).
enum class PackageSourceKind
{
    Embedded,
    File,
    Git,
    Engine,
};

// The directory beside an executable that its build stages every engine package
// into (ge_stage_packages, cmake/Packages.cmake), and the resolver's default
// engine-package root: a development copy of the repository's Packages/ with the
// packages' sources, their editor-only modules and the authoring-root marker
// below. A packaged game carries only the packages the build pipeline stages
// under its content root's Packages/ with packages.index. On macOS the content
// root is Contents/Resources, so the bundle's Contents/MacOS/<this> is only ever
// the Player's development copy, and a game bundle leaves it out.
inline constexpr std::string_view kStagedEnginePackagesDirName = "Packages";

// A developer build stages the engine packages from the repository tree they
// are authored in and drops this file beside them naming that tree, so the
// editor writes a package's own asset metadata back to where it is committed
// rather than into the staged copy the next build overwrites. A shipped or
// packaged tree has no such file and mounts read-only. The build writes it in
// cmake/Packages.cmake, which names this constant.
inline constexpr std::string_view kEnginePackageAuthoringRootFile =
    "EnginePackageAuthoringRoot.txt";

// One package located and validated by the resolver.
struct ResolvedPackage
{
    PackageManifest Manifest;
    std::filesystem::path RootDir;   // directory containing package.json
    std::filesystem::path AssetsDir; // RootDir / Manifest.AssetsDir
    // Mount alias (sanitized package name; see SanitizePackageAlias).
    std::string Alias;
    // Mount priority within [kPackageMountPriorityBase, kPackageMountPriorityMax].
    int32_t Priority = 0;
    PackageSourceKind SourceKind = PackageSourceKind::Embedded;
    // The package's directory in the tree it is AUTHORED in, when the staged
    // tree names one (see kEnginePackageAuthoringRootFile). Engine packages in
    // a developer build only; empty everywhere else, which is what keeps a
    // shipped tree read-only. RootDir stays the staged directory: the staged
    // tree is what runs, this is only where its metadata is committed.
    std::filesystem::path AuthoringRootDir;
    // Full resolved commit sha (git packages only).
    std::string GitCommit;
};

// Result of resolving a project's package set. Resolution is degraded, never
// all-or-nothing: a package that fails validation (missing dependency,
// version conflict, cycle, disabled dependency) is skipped together with its
// transitive dependents, with one loud error each; every other package still
// resolves and mounts.
struct PackageResolution
{
    // Enabled packages in mount order = dependency-topo order (dependencies
    // first), priorities assigned. Mount these.
    std::vector<ResolvedPackage> MountOrder;
    // Resolved but disabled via the project manifest: validated, never
    // mounted, never counted for shadowing.
    std::vector<ResolvedPackage> Disabled;
    // Per-package loud errors (missing package dir, malformed manifest, name
    // mismatch, missing/conflicting dependency, cycle, alias collision).
    std::vector<std::string> Errors;
    // Non-fatal notices (disabled dependency skipping dependents, priority
    // band overflow).
    std::vector<std::string> Warnings;
};

// Map a package name to its asset-source mount alias (charset [a-z0-9-]:
// one separator, because the define below is the alias with that separator
// rewritten).
//
// Mapping, deterministic by construction:
//   - leading '@' of a scope is dropped,
//   - '/' (scope separator), '.' and '_' become '-',
//   - every other character passes through (npm names are already lowercase).
// Examples: "ocean-pack" -> "ocean-pack"; "@studio/water.core" -> "studio-water-core";
// "grid_tools" -> "grid-tools".
// The mapping is lossy ("@a/b", "a.b" and "a_b" all collide with "a-b"), so
// the resolver REJECTS any two packages whose sanitized aliases collide, and
// any alias equal to a reserved source alias ("project", "editor") — a loud
// per-package error, the later package is skipped.
std::string SanitizePackageAlias(std::string_view packageName);

// Map a package name to the ONE compile define it exports to every consumer's
// build graph, so game code and dependent packages can feature-gate on the
// package being enabled.
//
// The rule: "GE_PACKAGE_" followed by the sanitized mount alias
// (SanitizePackageAlias) upper-cased, with '-' written as '_'. The fixed
// prefix both says where the symbol came from and keeps a digit-leading
// package name a valid identifier. Examples:
//   "ocean-pack"           -> "GE_PACKAGE_OCEAN_PACK"
//   "@studio/water.core"   -> "GE_PACKAGE_STUDIO_WATER_CORE"
//   "2d-tools"             -> "GE_PACKAGE_2D_TOOLS"
//   "grid_tools"           -> "GE_PACKAGE_GRID_TOOLS"
// Alias and define are one bijection: an alias holds no '_', so every '_' in
// a define came from exactly one '-'. Two packages therefore cannot share a
// define without sharing an alias, and the alias uniqueness check above is
// the whole enforcement. Both derivations live here because the resolver is
// what enforces uniqueness.
std::string PackageDefine(std::string_view packageName);

// Resolves <projectRoot>/Packages/manifest.json into a mountable package set:
// locate each dependency (file:/embedded/git), parse + validate its
// package.json, run flat semver validation over the local set, topo-order by
// dependency edges, and assign mount priorities. No registry mutation; feed
// the result to MountResolvedPackages (PackageMounts.h).
//
// Git dependencies (P3): a "git+https://...#ref" spec is acquired into the
// global immutable cache (PackageGitSource.h) and pinned in
// Packages/packages-lock.json. An unchanged spec with a warm cache resolves
// offline; acquisition shells out to git (PackageGitFetcher.h) and updates
// the lock.
// Engine packages (first-party, staged next to the executable) resolve
// IMPLICITLY: every <enginePackagesRoot>/<dir>/package.json joins the set as
// if the project manifest declared it, unless the project declares the same
// package name itself (the project's spec wins — e.g. a file: checkout for
// package development) or lists it in "disabled". An empty enginePackagesRoot
// means the default, <executable dir>/Packages; tests inject a fixture
// root.
class PackageResolver
{
  public:
    static PackageResolution Resolve(const std::filesystem::path& projectRoot,
                                     const std::filesystem::path& enginePackagesRoot = {});
};

} // namespace GameEngine
