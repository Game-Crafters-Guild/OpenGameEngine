#pragma once

// CollectPackageShaderDirs: derive the material pipeline's package shader
// roots from the asset mount table — the mounts are the single authority for
// what packages exist and in which priority order; nothing package-specific
// is plumbed in parallel. A package "declares" shaders simply by shipping a
// Shaders/ directory inside its mounted assets dir (<pkg>/Assets/Shaders/),
// the same convention the editor mount uses for its own Shaders/ tree.

#include "Rendering/Materials/MaterialBuildContext.h"

#include <filesystem>
#include <vector>

namespace GameEngine
{

class AssetManager;

// Returns the Shaders/ directory of every registered source EXCEPT the
// 'editor' source, in mount priority order (project first when it has one,
// then packages highest-priority-first). Only existing directories are
// returned. The editor source is excluded because when it exists its Shaders/
// dir IS the MaterialBuildContext's AdapterShaderDir — the final fallback of
// the composer's root chain. A packaged game has no editor source and the
// adapter templates ship inside the project mount instead, so there that one
// directory is both this list's first entry and the AdapterShaderDir; it
// resolves the same files either way.
//
// Callers snapshot the result into MaterialBuildContext (value copy: compiles
// run on workers and must not touch live mount state) and re-derive when
// AssetManager::GetSourceSetVersion() moves.
std::vector<std::filesystem::path> CollectPackageShaderDirs(const AssetManager& assetManager);

// Returns the source ROOT of the open project mount (empty vector when no
// project source is registered) for MaterialBuildContext::ProjectRoots.
// Same snapshot/re-derive contract as CollectPackageShaderDirs.
std::vector<std::filesystem::path> CollectProjectRoots(const AssetManager& assetManager);

// Returns the alias and root of every registered source, the editor source and
// packages included, in mount priority order, for
// MaterialBuildContext::AssetSourceRoots. Same snapshot/re-derive contract as
// CollectPackageShaderDirs.
std::vector<Rendering::MaterialBuildContext::AssetSourceRoot> CollectAssetSourceRoots(
    const AssetManager& assetManager);

// Editor/Player staging copies module surfaces under the adapter shader tree
// as `<adapter>/<module>/Surfaces`. The offline cook's `--package-shaders`
// roots have the same relative layout (`<module>/Shaders/Surfaces`). Without
// those staged folders as package roots, `Surfaces/*.glsl` resolves to the
// engine Surfaces/ tree — which for GPU fog is a one-line forwarder — and a
// compiler-less runtime hashes a different include closure than the cook.
void AppendStagedModuleShaderDirs(const std::filesystem::path& adapterShaderDir,
                                  std::vector<std::filesystem::path>& packageShaderDirs);

// Returns MaterialBuildContext's AdapterShaderDir: the 'editor' mount's
// Shaders/ tree when that source is registered and has one, else the same
// relative path resolved through implicit mount priority
// (AssetManager::ResolveAssetPathPreferringSource, the shared spelling of this
// prefer-a-mount idiom — Engine.cpp's engine-shader resolver is the same call).
//
// The fallback is what lets a SHIPPED game compile materials at all. A
// manifest-packaged build registers no 'editor' source — the build pipeline
// fuses editor-owned content flat into the project mount and stages the
// adapter templates at <content>/Assets/Shaders — so an alias-pinned
// resolution alone yields an empty path, and an empty AdapterShaderDir makes
// every material compile bail before it starts.
//
// The returned path is NEVER empty and is NOT guaranteed to exist: the
// implicit fallback yields the project candidate even when nothing is there.
// Callers must therefore treat existence, not emptiness, as readiness, and
// must re-derive when the source set moves — a call made before the 'editor'
// source registers returns a placeholder that only a re-derive corrects
// (MaterialSystem::EnsureMaterialBuildContextReady does both).
std::filesystem::path ResolveAdapterShaderDir(const AssetManager& assetManager);

} // namespace GameEngine
