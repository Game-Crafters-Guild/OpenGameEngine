#pragma once

// Package code discovery — which C++ and C# modules a package delivers, read
// from the files under its root. Code is an asset like any other: the language
// comes from the extension table and the editor/runtime split from the Editor
// folder convention, so a package declares neither.

#include "Assets/Packages/PackageManifest.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace GameEngine
{

// One code module found under a package root.
struct DiscoveredPackageModule
{
    PackageModuleRecord::ModuleKind Kind = PackageModuleRecord::ModuleKind::Runtime;
    PackageModuleRecord::ModuleLang Lang = PackageModuleRecord::ModuleLang::CSharp;
    // Absolute source root: the deepest directory holding every source file of
    // this (Lang, Kind). The generated build globs it recursively, so it is a
    // glob root, not merely a hint.
    std::filesystem::path RootDir;
};

// Discover the code modules under a package root, ordered by language (C#
// first) and then by kind (Runtime first). The rule, whole:
//
//  - A file is source when the asset extension table classifies it as
//    NativeSource (C++, translation units and headers alike) or as a .cs
//    Script. Other Script languages compile into no assembly and are ignored.
//  - A file is editor-only when any segment of its package-relative path is a
//    directory named "Editor" (IsEditorOnlyAssetPath); everything else is
//    runtime. This is the same convention that keeps editor content out of a
//    shipped build.
//  - A module's root is the deepest directory containing every file of its
//    (language, kind) pair. A C++ module needs at least one translation unit:
//    headers alone build nothing, but they do widen the root so the module's
//    own include path reaches them.
//  - Directories the asset scan ignores are never visited — build output,
//    derived caches, dot-directories and the package's own .assetignore
//    entries (AssetIgnoreRules) — nor is the package's top-level Tests folder
//    (any case): test sources build into a test executable the engine's own
//    build defines, never into a module the package delivers. The generated
//    module build and its staleness digest skip it by the same rule
//    (AssetIgnoreRules::IgnorePackageTestsFolder).
//
// The runtime and editor roots of one language must not nest: one glob root
// per module cannot express "everything here except that subtree". A package
// that nests them loses both modules of that language, with an error naming
// the two roots — a silent half-right compile would be worse.
//
// packageName appears in those errors and is not otherwise read.
std::vector<DiscoveredPackageModule> DiscoverPackageCodeModules(
    const std::filesystem::path& packageRoot, std::string_view packageName);

} // namespace GameEngine
