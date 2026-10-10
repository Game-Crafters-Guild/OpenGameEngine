#include "UndoRedo/RenameAssetFileCommand.h"

#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Core/Engine.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <cctype>
#include <string>
#include <system_error>
#include <utility>

namespace GameEngine::Editor
{

namespace
{

bool IsMaterialPath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return GetAssetTypeFromExtension(ext) == AssetType::Material;
}

// std::filesystem::rename silently replaces an existing destination (POSIX
// rename), so a file at the target name — e.g. created externally before an
// undo — would be destroyed. Refuse instead, mirroring
// ValidateAssetRenameStem's sibling-collision check; equivalent() keeps
// case-only renames of the same file working on case-insensitive filesystems.
bool DestinationOccupied(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (from.empty() || to.empty() || from == to)
        return false;
    std::error_code ec;
    if (!std::filesystem::exists(to, ec) || std::filesystem::equivalent(from, to, ec))
        return false;
    Logger::Log::Warning("RenameAssetFileCommand: refusing to rename '{}' to '{}': the destination "
                         "already exists",
                         from.string(), to.string());
    return true;
}

} // namespace

RenameAssetFileCommand::RenameAssetFileCommand(AssetRenamePlan plan,
                                               AssetManager* assets,
                                               std::function<void(const std::filesystem::path& renamedTo)> onRenamed)
    : m_Plan(std::move(plan)), m_Assets(assets), m_OnRenamed(std::move(onRenamed))
{
}

void RenameAssetFileCommand::Do()
{
    Apply(m_Plan.From, m_Plan.To, m_Plan.CompanionFrom, m_Plan.CompanionTo);
}

void RenameAssetFileCommand::Undo()
{
    Apply(m_Plan.To, m_Plan.From, m_Plan.CompanionTo, m_Plan.CompanionFrom);
}

void RenameAssetFileCommand::Apply(const std::filesystem::path& from,
                                   const std::filesystem::path& to,
                                   const std::filesystem::path& companionFrom,
                                   const std::filesystem::path& companionTo)
{
    // Shader-error entries are keyed by the registry's canonical absolute path,
    // which a plan path from the browser need not match (it can be
    // project-relative). Resolved before the rename — afterwards the registry
    // no longer knows the old paths.
    const std::filesystem::path fromRegistryPath = ResolveRegistryAssetPath(m_Assets, from);
    const std::filesystem::path companionFromRegistryPath =
        ResolveRegistryAssetPath(m_Assets, companionFrom);

    // Pre-flight both destinations before renaming either: refusing only the
    // companion's rename after the primary's succeeded would half-apply the
    // pair — a renamed shader whose material still sits at the old stem with a
    // broken surfaceShader reference.
    if (DestinationOccupied(from, to) || DestinationOccupied(companionFrom, companionTo))
        return;

    if (!RenameOne(from, to))
        return;
    PurgeShaderErrors(fromRegistryPath);

    if (IsMaterialPath(to) && RewriteMaterialName(to, from.stem().string(), to.stem().string()))
        ReloadMaterial(to);

    if (!companionFrom.empty() && RenameOne(companionFrom, companionTo))
    {
        PurgeShaderErrors(companionFromRegistryPath);
        const bool referenceRewritten =
            RewriteMaterialShaderReferences(companionTo, from.filename().string(), to.filename().string());
        const bool nameRewritten =
            RewriteMaterialName(companionTo, companionFrom.stem().string(), companionTo.stem().string());
        if (referenceRewritten || nameRewritten)
            ReloadMaterial(companionTo);
    }

    if (m_OnRenamed)
        m_OnRenamed(to);
}

bool RenameAssetFileCommand::RenameOne(const std::filesystem::path& from, const std::filesystem::path& to)
{
    if (from.empty() || to.empty() || from == to)
        return false;

    // Re-checked here (Apply pre-flighted both pair members) so a destination
    // that appeared between pre-flight and rename is still refused.
    if (DestinationOccupied(from, to))
        return false;

    std::error_code ec;
    std::filesystem::rename(from, to, ec);
    if (ec)
    {
        Logger::Log::Warning("RenameAssetFileCommand: failed to rename '{}' to '{}': {}", from.string(),
                             to.string(), ec.message());
        return false;
    }

    if (m_Assets && !m_Assets->RenameAssetPath(from, to))
    {
        // Not a registered asset (or it crossed a source root): rebind by path so
        // the new name is at least known to the registry.
        AssetRegistry& registry = m_Assets->GetRegistry();
        (void)registry.TryUnregisterAssetByPath(from);
        (void)registry.RegisterAsset(to);
    }
    return true;
}

// The loaded material keeps the document it was parsed with, and pass-variant
// warming compiles it on worker threads: re-register it from the rewritten file
// now so no NEW compile is enqueued against the old shader name. Variants
// already queued with the old document still run to completion and fail against
// the moved file — ShaderCompilationCache drops those failures (vanished
// material file), and PurgeShaderErrors below clears entries that landed before
// the rename.
void RenameAssetFileCommand::ReloadMaterial(const std::filesystem::path& materialPath)
{
    if (!m_Assets)
        return;
    const GUID guid = m_Assets->GetRegistry().GetAssetGUID(materialPath);
    if (guid.IsNull() || m_Assets->ReloadAssetNow(guid) != ReloadOutcome::Reloaded)
        return;

    auto* material = dynamic_cast<MaterialAsset*>(m_Assets->GetAsset(guid).get());
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (!material || !renderServices)
        return;
    renderServices->Materials().Compiler().Clear(guid);
    renderServices->RegisterAndPrewarmMaterial(guid, material->GetDocument());
}

// A renamed file's shader-error entries are keyed to a path nothing can
// recompile: no later success can clear them, so they would sit in the Shader
// Errors panel pointing at a file that no longer exists. Genuine breakage
// re-registers under the new name on the next compile. `oldPath` must be the
// registry's canonical absolute form (ResolveRegistryAssetPath) — the log's
// compare folds but cannot re-base.
void RenameAssetFileCommand::PurgeShaderErrors(const std::filesystem::path& oldPath)
{
    auto* renderServices = EngineCore::GetInstance().GetRenderServices();
    if (renderServices)
        renderServices->Materials().ShaderErrors().RemoveMaterialAsset(oldPath);
}

} // namespace GameEngine::Editor
