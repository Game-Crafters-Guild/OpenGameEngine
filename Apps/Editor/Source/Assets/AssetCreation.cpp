#include "Assets/AssetCreation.h"

#include "AssetCore/Asset.h"
#include "UndoRedo/UndoRedoService.h"
#include "UndoRedo/CreateTextFileCommand.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/MaterialAsset.h"
#include "Assets/ModelAsset.h"
#include "Assets/NavGridAsset.h"
#include "Assets/NavMeshAsset.h"
#include "Assets/Textures/TextureMimeType.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Engine/Rendering/ModelMaterialBridge.h"
#include "Core/Engine.h"
#include "Engine/Rendering/MaterialCompiler.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"
#include "Pathfinding/PathfindingTypes.h"
#include "Editor/Vcs/EditorVcsProviderRegistry.h"
#include "VCSIntegration/IVCSIntegration.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

// Session-level cache for ExportModelMaterials. Once a model has been fully
// exported (textures extracted, .material files written, assets registered),
// subsequent drags of the same model only need to swap entity material GUIDs
// from the ephemeral derived GUID to the persistent file-backed GUID — all the
// disk I/O + asset-registry work is already done.
//
// Keyed by source model GUID. Cleared on editor shutdown (process lifetime).
// Invalidation on source file change is deliberately not handled here: the
// worst case is that a manual .material delete silently misses a re-export
// until the editor restarts. Users hit by this can force a reimport.
struct ExportedMaterialMapping
{
    GUID derivedGuid;      // from ModelMaterialBridge::ConvertAll
    GUID fileBackedGuid;   // from AssetRegistry after .material is on disk
};

struct ModelExportCacheEntry
{
    std::vector<ExportedMaterialMapping> materials;
};

static std::mutex s_ExportCacheMutex;
static std::unordered_map<GUID, ModelExportCacheEntry> s_ExportCache;

static std::filesystem::path MaterialFilePathForModelSlot(const std::filesystem::path& modelFilePath,
                                                        const ModelAsset& modelAsset,
                                                        uint32_t slotIdx)
{
    const auto& importedMat = modelAsset.GetMaterial(slotIdx);
    const std::string matName = importedMat.Name.empty()
        ? (modelFilePath.stem().string() + "_material_" + std::to_string(slotIdx))
        : std::string(importedMat.Name.c_str());
    return modelFilePath.parent_path() / "Materials" / (matName + ".material");
}

static bool IsPermutationOfSlotIndices(const std::vector<uint32_t>& order, uint32_t n)
{
    if (order.size() != static_cast<size_t>(n))
        return false;
    std::vector<char> seen(n, 0);
    for (uint32_t x : order)
    {
        if (x >= n)
            return false;
        if (seen[x])
            return false;
        seen[x] = 1;
    }
    return true;
}

} // namespace


std::filesystem::path MakeUniqueFilePath(const std::filesystem::path& dir,
                                         const std::string& baseStem,
                                         const std::string& extensionWithDot)
{
    std::error_code ec;
    for (int i = 0; i < 4096; ++i)
    {
        const std::string stem = (i == 0) ? baseStem : (baseStem + std::to_string(i));
        const std::filesystem::path candidate = (dir / (stem + extensionWithDot)).lexically_normal();
        if (!std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}

AssetFileResult CreateAssetFile(const std::filesystem::path& dir,
                                const std::string& baseName,
                                const std::string& extension,
                                const std::function<std::string(const std::string& resolvedStem)>& contentFn,
                                const std::string& undoLabel,
                                UndoRedoService* undo,
                                AssetManager* assets)
{
    if (dir.empty())
        return {};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec))
        return {};

    const std::filesystem::path target = MakeUniqueFilePath(dir, baseName, extension);
    if (target.empty())
        return {};

    const std::string contents = contentFn(target.stem().string());

    if (undo)
    {
        undo->Execute(std::make_unique<CreateTextFileCommand>(undoLabel, target, contents));
    }
    else
    {
        std::filesystem::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (out.is_open())
        {
            out << contents;
            out.close();
        }
    }

    if (assets)
    {
        try
        {
            (void)assets->GetRegistry().RegisterAsset(target);
        }
        catch (...)
        {
        }
    }

    // Wake the VCS poll so the new file shows up in the panel on the next
    // tick instead of waiting for the next polling interval.
    if (auto* vcs = Editor::EditorVcsProviderRegistry::Get().ActiveIntegration())
        vcs->RefreshStatus();

    return {target};
}

AssetFileResult CreateDefaultMaterialFile(const std::filesystem::path& dir,
                                          const std::string& baseName,
                                          UndoRedoService* undo,
                                          AssetManager* assets)
{
    return CreateAssetFile(dir, baseName, ".material",
        [](const std::string& name) {
            return SerializeMaterialDocument(MaterialDocument::CreateDefaultPBR(name)).dump(2) + "\n";
        },
        "Create Material", undo, assets);
}

AssetFileResult CreateDefaultNavGridFile(const std::filesystem::path& dir,
                                         const std::string& baseName,
                                         UndoRedoService* undo,
                                         AssetManager* assets)
{
    if (dir.empty())
        return {};

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec))
        return {};

    const std::filesystem::path target = MakeUniqueFilePath(dir, baseName, ".navgrid");
    if (target.empty())
        return {};

    // Write a default binary navgrid with default GridSettings and zeroed cost/blocked data.
    Pathfinding::GridSettings settings;
    const uint32_t cellCount = settings.Width * settings.Depth;
    std::vector<float32> costs(cellCount, 1.0f);
    std::vector<uint8> blocked(cellCount, 0);

    bool ok = NavGridAsset::SaveToFile(target, settings, costs.data(), blocked.data(), cellCount);
    if (!ok)
        return {};

    // Undo support for binary files is not trivial; skip undo for now.
    (void)undo;

    if (assets)
    {
        try
        {
            (void)assets->GetRegistry().RegisterAsset(target);
        }
        catch (...)
        {
        }
    }

    return {target};
}

AssetFileResult CreateDefaultNavMeshFile(const std::filesystem::path& dir,
                                         const std::string& baseName,
                                         UndoRedoService* undo,
                                         AssetManager* assets)
{
    return CreateAssetFile(dir, baseName, ".navmesh",
        [](const std::string& /*name*/) {
            Pathfinding::NavMeshSettings settings;
            nlohmann::json json;
            json["version"] = 1;

            nlohmann::json s;
            s["cellSize"] = settings.CellSize;
            s["cellHeight"] = settings.CellHeight;
            s["agentRadius"] = settings.AgentRadius;
            s["agentHeight"] = settings.AgentHeight;
            s["agentMaxClimb"] = settings.AgentMaxClimb;
            s["agentMaxSlope"] = settings.AgentMaxSlope;
            s["regionMinSize"] = settings.RegionMinSize;
            s["regionMergeSize"] = settings.RegionMergeSize;
            s["edgeMaxLen"] = settings.EdgeMaxLen;
            s["edgeMaxError"] = settings.EdgeMaxError;
            s["detailSampleDist"] = settings.DetailSampleDist;
            s["detailSampleMaxError"] = settings.DetailSampleMaxError;
            s["vertsPerPoly"] = settings.VertsPerPoly;
            json["settings"] = s;
            json["sourceGeometry"] = nlohmann::json::array();

            return json.dump(4) + "\n";
        },
        "Create Navigation Mesh", undo, assets);
}

// Extract embedded images from a model to disk, returning a map from
// embedded index to the written file path (relative to the material file location).
static std::unordered_map<uint32, std::filesystem::path> ExtractEmbeddedTextures(
    const std::filesystem::path& modelFilePath,
    const ModelAsset& modelAsset)
{
    std::unordered_map<uint32, std::filesystem::path> extracted;
    const auto& images = modelAsset.GetEmbeddedImages();
    if (images.empty())
        return extracted;

    // A glTF ships its external images in its own lowercase "textures/" folder.
    // On a case-insensitive filesystem that is the same directory as the
    // "Textures/" written here, so the extracted copies join the originals. On a
    // case-sensitive filesystem (OPFS on web) the two names are distinct
    // directories and the import leaves two texture folders side by side —
    // reuse the model's existing folder so both platforms end up with one.
    // Single exists() probes only: iterating the directory proxy-walks OPFS
    // from the main thread and can stall the frame loop.
    const std::filesystem::path modelDir = modelFilePath.parent_path();
    std::error_code ec;
    std::filesystem::path texDir = modelDir / "textures";
    if (!std::filesystem::is_directory(texDir, ec))
        texDir = modelDir / "Textures";
    std::filesystem::create_directories(texDir, ec);

    const std::string modelStem = modelFilePath.stem().string();

    for (uint32 i = 0; i < static_cast<uint32>(images.size()); ++i)
    {
        const auto& img = images[i];
        if (img.Data.empty())
            continue;

        std::string filename = modelStem + "_tex" + std::to_string(i) +
                               std::string(TextureExtensionFromMime(img.MimeType));
        std::filesystem::path texPath = texDir / filename;

        if (!std::filesystem::exists(texPath, ec))
        {
            std::ofstream out(texPath, std::ios::binary | std::ios::trunc);
            if (out.is_open())
                out.write(reinterpret_cast<const char*>(img.Data.data()),
                          static_cast<std::streamsize>(img.Data.size()));
        }

        extracted[i] = texPath;
    }

    return extracted;
}

// Rewrite "__embedded:N" texture references in a MaterialDocument to point
// to extracted texture asset GUIDs so RegisterMaterialFromDocument can resolve them.
static void RewriteEmbeddedTexturePaths(
    MaterialDocument& doc,
    const std::unordered_map<uint32, std::filesystem::path>& extractedTextures,
    AssetRegistry& registry)
{
    for (auto& [slot, texRef] : doc.textures)
    {
        if (texRef.rfind(kEmbeddedTexturePrefix, 0) != 0)
            continue;

        uint32 idx = 0;
        try
        {
            idx = static_cast<uint32>(std::stoul(texRef.substr(kEmbeddedTexturePrefixLen)));
        }
        catch (...)
        {
            continue;
        }

        auto it = extractedTextures.find(idx);
        if (it == extractedTextures.end())
            continue;

        // RegisterAssetByPath registers the extracted file when nothing has yet
        // and resolves it in one step: a raw GetAssetGUID finds nothing for an
        // unregistered path, and the reference would stay on its __embedded:N
        // placeholder.
        const auto registered = registry.RegisterAssetByPath(it->second);
        const GUID texGuid = registered.IsOk() ? registered.Value() : GUID::Null();
        if (!texGuid.IsNull())
            texRef = texGuid.ToString();
    }
}

void ExportModelMaterials(const std::filesystem::path& modelFilePath,
                          const GUID& modelGuid,
                          const std::vector<ECS::EntityHandle>& entities,
                          ECS::World& world,
                          AssetManager& assets)
{
    using Clock = std::chrono::high_resolution_clock;
    const auto tStart = Clock::now();
    auto msElapsed = [](Clock::time_point t) {
        return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
    };

    // Fast path: if this model's materials have already been exported this
    // session, skip all the disk I/O (texture extraction, .material writes,
    // asset registry calls) and do only the entity GUID swap. The swap is
    // required every time because CreateFromModel hands newly-created entities
    // the ephemeral derived GUID — we need to rewrite that to the persistent
    // file-backed GUID so the scene saves correctly.
    {
        std::lock_guard<std::mutex> lk(s_ExportCacheMutex);
        auto it = s_ExportCache.find(modelGuid);
        if (it != s_ExportCache.end())
        {
            const auto& cached = it->second;
            auto& registry = assets.GetRegistry();
            for (const auto& mapping : cached.materials)
            {
                // Re-assert provenance in the fast path. The durable SQLite
                // row may have been wiped (cache rebuild after a v5->v6
                // schema bump, manual delete of AssetDbCache.sqlite) while
                // the in-process s_ExportCache survives — re-running the
                // idempotent RegisterProvenance call here keeps the durable
                // row in sync with the session cache. The wrapper validates
                // both GUIDs are registered, so a mapping with a missing
                // file-backed asset (file deleted off-disk between runs)
                // fails silently rather than persisting a dangling row.
                (void)registry.RegisterProvenance(mapping.fileBackedGuid, modelGuid, "ExportModelMaterials");

                for (const auto& entity : entities)
                {
                    auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
                    if (!mr)
                        continue;

                    if (mr->materialAssetGuid.ToGuid() != mapping.derivedGuid)
                        continue;

                    Components::MeshRenderer updated = *mr;
                    updated.materialAssetGuid.Set(mapping.fileBackedGuid);
                    world.AddComponentImmediate(entity, updated);
                }
            }
            Logger::Log::Trace(
                "[ModelLoad] ExportModelMaterials '{}': TOTAL {:.1f}ms (cached, swap-only)",
                modelFilePath.filename().string(), msElapsed(tStart));
            return;
        }
    }

    auto asset = assets.GetAsset(modelGuid);
    if (!asset)
        asset = assets.LoadAssetAsync(modelGuid, AssetLoadPriority::High).get();
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return;

    const auto tConvert = Clock::now();
    auto convertedMats = Engine::Renderer::ModelMaterialBridge::ConvertAll(
        modelGuid, modelAsset->GetMaterials());
    const double convertMs = msElapsed(tConvert);
    if (convertedMats.empty())
        return;

    // Extract embedded textures to disk so .material files can reference them.
    const auto tExtract = Clock::now();
    auto extractedTextures = ExtractEmbeddedTextures(modelFilePath, *modelAsset);
    const double extractMs = msElapsed(tExtract);

    const auto tDirCreate = Clock::now();
    std::filesystem::path matDir = modelFilePath.parent_path() / "Materials";
    std::error_code ec;
    std::filesystem::create_directories(matDir, ec);
    const double dirCreateMs = msElapsed(tDirCreate);

    // Register extracted textures with the asset system. Also record
    // importer-time provenance back to the model — the model.glb produced
    // these texture files during ExtractEmbeddedTextures.
    const auto tTexReg = Clock::now();
    auto& registry = assets.GetRegistry();
    for (const auto& [idx, texPath] : extractedTextures)
    {
        // Single register-and-resolve entry point; the RegisterAsset + raw
        // GetAssetGUID pair it replaces performed the same sequence.
        const GUID texGuid = assets.ResolveAssetGuid(texPath);
        if (!texGuid.IsNull())
            (void)registry.RegisterProvenance(texGuid, modelGuid, "ExportModelMaterials");
    }
    const double texRegMs = msElapsed(tTexReg);

    double matWriteMs = 0.0;
    double matRegMs = 0.0;
    double matFileRegMs = 0.0;
    double entityUpdateMs = 0.0;

    // Collected to populate the session cache once all materials have been
    // successfully exported. Partial populations are discarded — the next
    // call will retry the full export.
    std::vector<ExportedMaterialMapping> successfulMappings;
    successfulMappings.reserve(convertedMats.size());
    bool exportFullySucceeded = true;

    for (auto& cm : convertedMats)
    {
        std::string matName = cm.document.materialName.empty()
            ? (modelFilePath.stem().string() + "_material_" + std::to_string(cm.materialIndex))
            : cm.document.materialName;

        std::filesystem::path matPath = matDir / (matName + ".material");
        bool needsWrite = !std::filesystem::exists(matPath, ec);

        // If the file exists but still has __embedded: texture refs, rewrite it.
        if (!needsWrite && !extractedTextures.empty())
        {
            std::ifstream in(matPath);
            if (in.is_open())
            {
                std::string content((std::istreambuf_iterator<char>(in)),
                                     std::istreambuf_iterator<char>());
                if (content.find(kEmbeddedTexturePrefix) != std::string::npos)
                    needsWrite = true;
            }
        }

        // Save original document before rewriting paths — needed for
        // ResolveEmbeddedTextures which expects __embedded__:N references.
        const MaterialDocument originalDoc = cm.document;

        if (needsWrite)
        {
            const auto tWrite = Clock::now();
            RewriteEmbeddedTexturePaths(cm.document, extractedTextures, registry);

            std::ofstream out(matPath, std::ios::binary | std::ios::trunc);
            if (out.is_open())
                out << SerializeMaterialDocumentForSave(cm.document) << "\n";
            matWriteMs += msElapsed(tWrite);
        }

        const auto tMatFileReg = Clock::now();
        // Single register-and-resolve entry point; the RegisterAsset + raw
        // GetAssetGUID pair it replaces performed the same sequence.
        const GUID fileMatGuid = assets.ResolveAssetGuid(matPath);
        matFileRegMs += msElapsed(tMatFileReg);
        if (fileMatGuid.IsNull())
        {
            exportFullySucceeded = false;
            continue;
        }

        successfulMappings.push_back({cm.derivedGuid, fileMatGuid});

        // Record importer-time provenance: model.glb -> material_X.material.
        // Durable lineage for delete-confirm UX, reimport flow, asset-panel
        // "produced by X" badges. Idempotent (INSERT OR REPLACE) so re-running
        // ExportModelMaterials on the same model overwrites the row.
        (void)registry.RegisterProvenance(fileMatGuid, modelGuid, "ExportModelMaterials");

        // Register the file-backed material with the renderer so the entity
        // renders correctly immediately after the GUID swap (the derived GUID
        // was already registered during CreateFromModel).
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
        {
            if (!rs->Materials().Registry().Find(fileMatGuid))
            {
                const auto tReg = Clock::now();
                auto* mat = rs->RegisterAndPrewarmMaterial(fileMatGuid, cm.document);
                // Bind embedded textures — the document's texture refs are either
                // __embedded__:N (pre-rewrite) or file paths (post-rewrite), neither
                // of which RegisterMaterialFromDocument can resolve as GUIDs. Use
                // the same embedded image upload path that CreateFromModel uses.
                rs->Textures().ResolveEmbeddedTextures(mat, originalDoc, modelGuid,
                                            modelAsset->GetEmbeddedImages());
                matRegMs += msElapsed(tReg);
            }
        }

        const auto tEntity = Clock::now();
        for (const auto& entity : entities)
        {
            auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
            if (!mr)
                continue;

            if (mr->materialAssetGuid.ToGuid() != cm.derivedGuid)
                continue;

            Components::MeshRenderer updated = *mr;
            updated.materialAssetGuid.Set(fileMatGuid);
            world.AddComponentImmediate(entity, updated);
        }
        entityUpdateMs += msElapsed(tEntity);
    }

    // Commit to the session cache only if every material got a valid
    // file-backed GUID. A partial export leaves some entities with derived
    // GUIDs; the next ExportModelMaterials call will re-run the full import
    // and pick up the pieces. This avoids caching a broken state that would
    // make future drags silently wrong.
    if (exportFullySucceeded && successfulMappings.size() == convertedMats.size())
    {
        std::lock_guard<std::mutex> lk(s_ExportCacheMutex);
        s_ExportCache[modelGuid] = ModelExportCacheEntry{std::move(successfulMappings)};
    }

    Logger::Log::Info(
        "[ModelLoad] ExportModelMaterials '{}': TOTAL {:.1f}ms | convert {:.1f}ms | extractTex {:.1f}ms | dirCreate {:.1f}ms | texReg {:.1f}ms | matWrite {:.1f}ms | matFileReg {:.1f}ms | matRegister {:.1f}ms | entityUpdate {:.1f}ms",
        modelFilePath.filename().string(), msElapsed(tStart),
        convertMs, extractMs, dirCreateMs, texRegMs, matWriteMs, matFileRegMs, matRegMs, entityUpdateMs);
}

bool ApplyModelMaterialSlotDisplayOrderToFiles(const std::filesystem::path& modelFilePath,
                                               const GUID& modelGuid,
                                               const std::vector<uint32_t>& displayOrderTopToBottom,
                                               AssetManager& assets)
{
    SharedPtr<Asset> asset = assets.GetAsset(modelGuid);
    if (!asset)
        asset = assets.LoadAssetAsync(modelGuid, AssetLoadPriority::High).get();
    auto* modelAsset = dynamic_cast<ModelAsset*>(asset.get());
    if (!modelAsset || !modelAsset->IsLoaded())
        return false;

    const uint32_t n = modelAsset->GetMaterialCount();
    if (n == 0)
        return false;
    if (!IsPermutationOfSlotIndices(displayOrderTopToBottom, n))
        return false;

    bool identity = true;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (displayOrderTopToBottom[i] != i)
        {
            identity = false;
            break;
        }
    }
    if (identity)
        return true;

    std::vector<std::filesystem::path> paths(n);
    std::vector<std::optional<std::string>> oldText(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        paths[i] = MaterialFilePathForModelSlot(modelFilePath, *modelAsset, i);
        std::error_code ec;
        if (!std::filesystem::exists(paths[i], ec))
        {
            Logger::Log::Warning(
                "ApplyModelMaterialSlotDisplayOrderToFiles: missing material file for slot {} (expected '{}'); "
                "use New Material / export first",
                i,
                paths[i].string());
            return false;
        }
        std::ifstream in(paths[i], std::ios::binary);
        if (!in.is_open())
        {
            Logger::Log::Warning("ApplyModelMaterialSlotDisplayOrderToFiles: cannot read '{}'", paths[i].string());
            return false;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        oldText[i] = std::move(content);
    }

    std::vector<std::optional<std::string>> newText(n);
    for (uint32_t i = 0; i < n; ++i)
        newText[i] = oldText[displayOrderTopToBottom[i]];

    bool anyWrite = false;
    for (uint32_t i = 0; i < n; ++i)
    {
        if (!newText[i].has_value())
            continue;
        if (oldText[i] == newText[i])
            continue;
        std::ofstream out(paths[i], std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Logger::Log::Warning("ApplyModelMaterialSlotDisplayOrderToFiles: failed to open '{}' for write",
                                 paths[i].string());
            return false;
        }
        out << *newText[i];
        anyWrite = true;
    }

    if (!anyWrite)
        return true;

    for (uint32_t i = 0; i < n; ++i)
    {
        // Resolve through AssetManager, which registers the rewritten file when
        // nothing has yet: a raw registry lookup finds nothing for an unregistered
        // path, and the file would never be reloaded.
        const GUID g = assets.ResolveAssetGuid(paths[i]);
        if (g.IsNull())
            continue;
        SharedPtr<Asset> matAsset = assets.GetAsset(g);
        if (!matAsset)
            matAsset = assets.LoadAssetAsync(g, AssetLoadPriority::High).get();
        if (matAsset)
            (void)matAsset->Reload();
        if (auto* rs = EngineCore::GetInstance().GetRenderServices())
            rs->Materials().Compiler().Clear(g);
    }

    return true;
}

} // namespace GameEngine::Editor
