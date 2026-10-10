#pragma once

#include <filesystem>
#include <string>

namespace GameEngine
{
class AssetManager;
}

namespace GameEngine::Editor
{

struct AssetRenameValidation
{
    bool Ok = false;
    std::string Reason; // User-facing, states what to fix. Empty when Ok.
};

/// Validates `newStem` (the file name without extension) as the new name for the
/// asset at `currentPath`. Rejects empty names, path separators and characters no
/// supported filesystem accepts, hidden-file dots, names Windows reserves, over-long
/// names, and collisions with an existing sibling (a case-only change of the same
/// file is allowed). `Reason` is the message shown to the user.
AssetRenameValidation ValidateAssetRenameStem(const std::string& newStem,
                                              const std::filesystem::path& currentPath);

/// The `.material` that `Create -> Surface Shader` pairs with a shader: same stem,
/// same directory, and its `surfaceShader` names the shader file. Empty when the
/// shader has no such companion (or `shaderPath` is not a shader).
std::filesystem::path FindPairedSurfaceShaderMaterial(const std::filesystem::path& shaderPath);

/// Rewrites every reference the material holds to the shader file named
/// `oldShaderFilename` (bare filename, or a path whose filename matches) — `surfaceShader`
/// and `vertexModifier` — so it names `newShaderFilename` instead, and drops the GUID
/// stamped beside each (`surfaceShaderGuid`, `vertexModifierGuid`): derived from the
/// shader's old path, that GUID would come to name whatever file next claims it.
/// Returns true when the file was rewritten.
bool RewriteMaterialShaderReferences(const std::filesystem::path& materialPath,
                                     const std::string& oldShaderFilename,
                                     const std::string& newShaderFilename);

/// Rewrites the material's `materialName` when it still equals `oldName` (Create set it
/// to the file stem), so the name shown for the material follows the file. Returns true
/// when the file was rewritten.
bool RewriteMaterialName(const std::filesystem::path& materialPath,
                         const std::string& oldName,
                         const std::string& newName);

/// Everything a rename touches. A surface shader drags its paired material along so
/// the pair keeps one name and the material's `surfaceShader` path stays resolvable.
struct AssetRenamePlan
{
    std::filesystem::path From;
    std::filesystem::path To;
    std::filesystem::path CompanionFrom; // empty when the asset has no companion
    std::filesystem::path CompanionTo;

    bool HasCompanion() const { return !CompanionFrom.empty(); }
};

AssetRenamePlan PlanAssetRename(const std::filesystem::path& currentPath, const std::string& newStem);

/// The registry's canonical form of `path` — the absolute, registry-normalized
/// path its metadata carries — or `path` unchanged when it is not a registered
/// asset (or `assets` is null). Records keyed by an asset's registry path (the
/// shader compile error log) can only be matched in this form: a plan path from
/// the browser may be project-relative, and the compare-time fold
/// (AssetPaths::NormalizeForRegistryKey) folds case/NFC/slashes but cannot
/// re-base.
std::filesystem::path ResolveRegistryAssetPath(const AssetManager* assets,
                                               const std::filesystem::path& path);

} // namespace GameEngine::Editor
