#include "Terrain/TerrainZonePayloadSaver.h"

#include "Scene/SceneBackupRecovery.h"

#include "Assets/AssetRegistry.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "FileSystem/FileSystem.h"
#include "Logger/Logger.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <system_error>
#include <tuple>
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

// The autosave backup runs this on a timer inside the very crash window the
// payloads guard against, so a plain truncating write would let a hard-kill
// mid-write zero the last good file (worse than not flushing at all). The temp
// is removed on any failure so a crashed write leaves the good target intact.
bool WriteBlobAtomic(const std::filesystem::path& target, const std::vector<uint8>& bytes)
{
    const std::filesystem::path temp = target.string() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            Logger::Log::Warning("Terrain zone payload: failed to open {} for write", temp.string());
            return false;
        }
        if (!bytes.empty())
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good())
        {
            Logger::Log::Warning("Terrain zone payload: write to {} failed", temp.string());
            out.close();
            std::error_code rmEc;
            std::filesystem::remove(temp, rmEc);
            return false;
        }
    } // flush + close before the file is put in place

    if (!GameEngine::FileSystem::PublishFile(temp, target))
    {
        Logger::Log::Warning("Terrain zone payload: publishing {} as {} failed",
                             temp.string(), target.string());
        return false;
    }
    return true;
}

uint32_t FlushDirtyZonePayloads(ECS::World& world, const std::filesystem::path& scenePath,
                                AssetRegistry& registry, TerrainECS::TerrainService& service,
                                ZonePayloadFlushMode mode, ZonePayloadStagingLedger* stagingLedger)
{
    const bool staging = mode == ZonePayloadFlushMode::BackupExistingOnly;
    if (staging && !stagingLedger)
    {
        Logger::Log::Warning("Terrain zone payload: backup staging requires a ledger — skipped");
        return 0;
    }

    // Companion payload folder next to the scene (under the asset root, so the
    // scene's [resource] paths resolve relative to it).
    const std::filesystem::path zonesDir = SceneZonesFolderFor(scenePath);

    // Collect zones first — the swap below mutates components, so don't iterate
    // and mutate in the same pass. (entity, payload GUID, isPaint).
    std::vector<std::tuple<ECS::EntityHandle, GUID, bool>> zones;
    world.Query<ECS::Read<Components::TerrainSculptZone>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle e, const Components::TerrainSculptZone& z)
        { zones.emplace_back(e, z.PayloadRef.ToGuid(), false); });
    world.Query<ECS::Read<Components::TerrainPaintZone>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle e, const Components::TerrainPaintZone& z)
        { zones.emplace_back(e, z.PayloadRef.ToGuid(), true); });

    uint32_t written = 0;
    for (const auto& [entity, guid, isPaint] : zones)
    {
        if (guid.IsNull())
            continue;
        TerrainECS::TerrainZonePayload* payload = service.GetZonePayload(guid);
        if (!payload || !payload->NeedsSave)
            continue;

        // An existing file-backed .tzone for this GUID is the write anchor
        // (keeps the GUID stable across re-saves).
        std::filesystem::path liveTarget;
        bool hasExisting = false;
        AssetMetadata meta{};
        if (registry.TryGetAssetMetadata(guid, meta) && !meta.Path.empty()
            && LowerExtension(meta.Path) == ".tzone")
        {
            liveTarget = meta.Path;
            hasExisting = true;
        }

        if (!hasExisting)
        {
            // The backup timer never mints a file (nor the ref-swap it would
            // force); an unsaved zone waits for the next explicit Save.
            if (staging)
                continue;
            std::error_code ec;
            std::filesystem::create_directories(zonesDir, ec);
            liveTarget = zonesDir / (guid.ToString() + ".tzone");
        }

        if (staging)
        {
            // Idempotent per stroke: skip zones whose payload hasn't changed
            // since the last staging tick.
            const auto it = stagingLedger->StagedDataVersions.find(guid);
            if (it != stagingLedger->StagedDataVersions.end() && it->second == payload->DataVersion)
                continue;

            if (!WriteBlobAtomic(StagedZonePayloadPathFor(liveTarget),
                                 TerrainECS::EncodeZonePayload(*payload)))
                continue;

            // NeedsSave stays set: the live .tzone is still unwritten and the
            // next explicit Save must persist it.
            stagingLedger->StagedDataVersions[guid] = payload->DataVersion;
            ++written;
            continue;
        }

        if (!WriteBlobAtomic(liveTarget, TerrainECS::EncodeZonePayload(*payload)))
            continue;

        // The live bytes now supersede any staged autosave sibling; a stale
        // sibling left behind would be offered as recovery after a later crash.
        {
            std::error_code ec;
            std::filesystem::remove(StagedZonePayloadPathFor(liveTarget), ec);
        }
        if (stagingLedger)
            stagingLedger->StagedDataVersions.erase(guid);

        if (mode == ZonePayloadFlushMode::SaveMintAndSwap)
        {
            // RegisterAssetByPath normalizes + registers + resolves in one step: a
            // raw GetAssetGUID on the absolute live path returns null on web, which
            // would leave the component on the minted GUID no reload can resolve.
            const auto registered = registry.RegisterAssetByPath(liveTarget);
            const GUID fileGuid = registered.IsOk() ? registered.Value() : GUID::Null();
            if (!fileGuid.IsNull() && fileGuid != guid)
            {
                // Swap the component reference + re-key the store to the file-backed
                // (derived) GUID so a reload resolves it from the path.
                if (isPaint)
                {
                    if (const auto* c = world.GetComponent<Components::TerrainPaintZone>(entity))
                    {
                        Components::TerrainPaintZone u = *c;
                        u.PayloadRef.Set(fileGuid);
                        world.AddComponentImmediate(entity, u);
                    }
                }
                else
                {
                    if (const auto* c = world.GetComponent<Components::TerrainSculptZone>(entity))
                    {
                        Components::TerrainSculptZone u = *c;
                        u.PayloadRef.Set(fileGuid);
                        world.AddComponentImmediate(entity, u);
                    }
                }
                TerrainECS::TerrainZonePayload moved = *payload;
                service.SetZonePayload(fileGuid, std::move(moved));
                service.EvictZonePayload(guid);
                payload = service.GetZonePayload(fileGuid);
            }
        }

        if (payload)
            payload->NeedsSave = false;
        ++written;
    }

    if (written > 0)
        Logger::Log::Info("Terrain zone payloads: {} {} .tzone {} next to {}",
                          staging ? "staged" : "wrote", written,
                          staging ? "backup(s)" : "file(s)", scenePath.filename().string());
    return written;
}

} // namespace GameEngine::Editor
