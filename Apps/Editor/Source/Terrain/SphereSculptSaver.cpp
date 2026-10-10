#include "Terrain/SphereSculptSaver.h"

#include "Scene/SceneBackupRecovery.h"

#include "Assets/AssetRegistry.h"
#include "CBTTerrainECS/TerrainProvisioning.h"
#include "Components/Terrain/Terrain.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Logger/Logger.h"
#include "TerrainECS/TerrainService.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
#include <system_error>
#include <vector>

namespace GameEngine::Editor
{
namespace
{

std::string LowerExtension(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

uint32_t FlushDirtySphereSculpt(ECS::World& world, const std::filesystem::path& scenePath,
                                AssetRegistry& registry, TerrainECS::TerrainService& service,
                                ZonePayloadFlushMode mode, ZonePayloadStagingLedger* stagingLedger)
{
    const bool staging = mode == ZonePayloadFlushMode::BackupExistingOnly;
    if (staging && !stagingLedger)
    {
        Logger::Log::Warning("Sphere sculpt payload: backup staging requires a ledger — skipped");
        return 0;
    }

    if (!service.SphereSculptNeedsSave())
        return 0;

    // The sculpt belongs to the active spherical planet (the store is service-global, one live
    // planet at a time — the same resolver the renderer and the brush use). Orphaned edits
    // (planet deleted since the stroke) have no owner to re-reference and are not flushed.
    Components::Terrain terrain{};
    const ECS::EntityHandle entity = CBTTerrainECS::FindActiveTerrainEntity(world);
    if (!entity.IsValid() || !CBTTerrainECS::FindActiveTerrain(world, terrain) ||
        terrain.Domain != Components::TerrainDomain::Spherical)
        return 0;

    const GUID guid = terrain.SphereSculptGuid.ToGuid();

    // An existing file-backed .tsculpt for the component's GUID is the write anchor (keeps the
    // GUID stable across re-saves), exactly like the zones' .tzone anchor.
    std::filesystem::path liveTarget;
    bool hasExisting = false;
    AssetMetadata meta{};
    if (!guid.IsNull() && registry.TryGetAssetMetadata(guid, meta) && !meta.Path.empty()
        && LowerExtension(meta.Path) == ".tsculpt")
    {
        liveTarget = meta.Path;
        hasExisting = true;
    }

    if (!hasExisting)
    {
        // The backup timer never mints a file (nor the ref-swap it would force); a sculpt
        // unsaved since planet creation waits for the next explicit Save.
        if (staging)
            return 0;
        const std::filesystem::path zonesDir = SceneZonesFolderFor(scenePath);
        std::error_code ec;
        std::filesystem::create_directories(zonesDir, ec);
        liveTarget = zonesDir / (GUID::Generate().ToString() + ".tsculpt");
    }

    // Idempotent per stroke: an idle autosave interval re-stages nothing — checked BEFORE
    // the encode so a quiet timer tick costs a version read, not a full-store snapshot.
    if (staging && stagingLedger->StagedSphereSculptVersion == service.SphereSculptVersion())
        return 0;

    uint64 version = 0;
    const auto encodeStart = std::chrono::steady_clock::now();
    const std::vector<uint8> blob = service.EncodeSphereSculptBlob(version);

    if (staging)
    {
        if (!WriteBlobAtomic(StagedZonePayloadPathFor(liveTarget), blob))
            return 0;
        // NeedsSave semantics stay untouched: the live .tsculpt is still unwritten and the
        // next explicit Save must persist it.
        stagingLedger->StagedSphereSculptVersion = version;
        Logger::Log::Info("Sphere sculpt: staged backup ({} KiB) next to {}", blob.size() / 1024,
                          scenePath.filename().string());
        return 1;
    }

    if (!WriteBlobAtomic(liveTarget, blob))
        return 0;
    const auto writeEnd = std::chrono::steady_clock::now();

    // The live bytes supersede any staged autosave sibling; a stale sibling left behind would
    // be offered as recovery after a later crash.
    {
        std::error_code ec;
        std::filesystem::remove(StagedZonePayloadPathFor(liveTarget), ec);
    }
    if (stagingLedger)
        stagingLedger->StagedSphereSculptVersion = 0;

    GUID savedGuid = guid;
    // RegisterAssetByPath normalizes + registers + resolves in one step: a raw
    // GetAssetGUID on the absolute live path returns null on web, which would
    // leave the component on the minted GUID no reload can resolve.
    const auto registered = registry.RegisterAssetByPath(liveTarget);
    const GUID fileGuid = registered.IsOk() ? registered.Value() : GUID::Null();
    if (!fileGuid.IsNull() && fileGuid != guid)
    {
        // Swap the component reference to the file-backed (derived) GUID so a reload
        // resolves it from the path — the zones' mint-and-swap contract.
        if (const auto* c = world.GetComponent<Components::Terrain>(entity))
        {
            Components::Terrain u = *c;
            u.SphereSculptGuid.Set(fileGuid);
            world.AddComponentImmediate(entity, u);
        }
        savedGuid = fileGuid;
    }
    service.MarkSphereSculptSaved(savedGuid, version);

    Logger::Log::Info(
        "Sphere sculpt: wrote {} ({} KiB) in {:.1f} ms next to {}", liveTarget.filename().string(),
        blob.size() / 1024,
        std::chrono::duration<float, std::milli>(writeEnd - encodeStart).count(),
        scenePath.filename().string());
    return 1;
}

} // namespace GameEngine::Editor
