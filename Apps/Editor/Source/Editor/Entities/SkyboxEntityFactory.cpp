#include "Editor/Entities/SkyboxEntityFactory.h"

#include "Assets/AssetManager.h"
#include "Assets/PolyhavenDownloadManager.h"
#include "Assets/PolyhavenService.h"
#include "Components/Name.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Editor/Assets/AssetRelativePath.h"
#include "EditorChangeNotifications.h"
#include "EditorContext.h"
#include "Logger/Logger.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

namespace GameEngine::Editor
{
namespace
{
// The resolution fetched first when the preferred one is not on disk: small
// enough to arrive quickly and bind as a preview.
constexpr std::string_view kPreviewHdriResolution = "1k";

void NotifySkyboxCreated(ECS::World& world,
                         ECS::EntityHandle skyEntity,
                         EditorChangeNotifications* notifications)
{
    if (!notifications)
        return;

    EditorChangeNotifications::ComponentChangedEvent e{};
    e.world = &world;
    e.entity = skyEntity;
    e.componentType = ECS::GetComponentTypeId<Components::Skybox>();
    e.kind = EditorChangeNotifications::ChangeKind::Commit;
    notifications->NotifyComponentChanged(e);

    EditorChangeNotifications::WorldStructureChangedEvent wse{};
    wse.world = &world;
    wse.kind = EditorChangeNotifications::ChangeKind::Commit;
    notifications->NotifyWorldStructureChanged(wse);
}

void SetSkyboxName(ECS::World& world, ECS::EntityHandle skyEntity, const std::string& label)
{
    Components::Name name{};
    std::memset(name.value, 0, sizeof(name.value));
    std::strncpy(name.value, label.c_str(), sizeof(name.value) - 1);
    world.AddComponentImmediate(skyEntity, name);
}

void AddIdentityTransform(ECS::World& world, ECS::EntityHandle skyEntity)
{
    Components::Transform xf{};
    xf.SetIdentity();
    world.AddComponentImmediate(skyEntity, xf);
}

// A Skybox with no HDRI bound yet, waiting for a Polyhaven download.
ECS::EntityHandle CreatePendingHDRISkybox(ECS::World& world,
                                          std::string_view displayName,
                                          EditorChangeNotifications* notifications,
                                          const std::function<void()>& markDirty,
                                          std::string_view sourceSlug,
                                          std::string_view resolution)
{
    ECS::EntityHandle skyEntity = world.CreateEntity();
    if (!skyEntity.IsValid())
        return {};

    AddIdentityTransform(world, skyEntity);

    Components::Skybox sky{};
    sky.HDRIIntensity = 1.0f;
    sky.RotationDegrees = 0.0f;
    std::strncpy(sky.SourceSlug, std::string(sourceSlug).c_str(), sizeof(sky.SourceSlug) - 1);
    std::strncpy(sky.Resolution,
                 std::string(resolution.empty() ? kPreviewHdriResolution : resolution).c_str(),
                 sizeof(sky.Resolution) - 1);
    world.AddComponentImmediate(skyEntity, sky);

    SetSkyboxName(world, skyEntity,
                  displayName.empty() ? std::string("Skybox") : "Skybox - " + std::string(displayName));

    NotifySkyboxCreated(world, skyEntity, notifications);
    if (markDirty)
        markDirty();

    return skyEntity;
}
} // namespace

bool IsHdriPath(const std::filesystem::path& path)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".hdr";
}

ECS::EntityHandle CreateSkyboxEntityFromHdri(ECS::World& world,
                                             AssetManager& assets,
                                             const std::filesystem::path& hdriPath,
                                             EditorChangeNotifications* notifications,
                                             const std::function<void()>& markDirty,
                                             std::string_view sourceSlug,
                                             std::string_view resolution)
{
    if (hdriPath.empty())
        return {};

    const GUID guid = assets.ResolveAssetGuid(hdriPath);
    if (guid.IsNull())
        return {};

    ECS::EntityHandle skyEntity = world.CreateEntity();
    if (!skyEntity.IsValid())
        return {};

    AddIdentityTransform(world, skyEntity);

    Components::Skybox sky{};
    sky.HDRIIntensity = 1.0f;
    sky.RotationDegrees = 0.0f;

    // HDRIPath is persisted into the scene, so it must be project-relative.
    // The path can arrive as a registry path (folded on case-insensitive
    // platforms) while the root carries its own spelling, and a lexical
    // subtraction compares case — it would store an absolute path.
    std::string pathString = TryMakeAssetRelativePathString(assets, hdriPath);
    if (pathString.empty())
        pathString = hdriPath.generic_string();

    const std::string guidString = guid.ToString();
    std::memset(sky.HDRIAssetGuid, 0, sizeof(sky.HDRIAssetGuid));
    std::memset(sky.HDRIPath, 0, sizeof(sky.HDRIPath));
    std::memset(sky.SourceSlug, 0, sizeof(sky.SourceSlug));
    std::memset(sky.Resolution, 0, sizeof(sky.Resolution));
    std::strncpy(sky.HDRIAssetGuid, guidString.c_str(), sizeof(sky.HDRIAssetGuid) - 1);
    std::strncpy(sky.HDRIPath, pathString.c_str(), sizeof(sky.HDRIPath) - 1);
    std::strncpy(sky.SourceSlug, std::string(sourceSlug).c_str(), sizeof(sky.SourceSlug) - 1);
    std::strncpy(sky.Resolution,
                 std::string(resolution.empty() ? kPreviewHdriResolution : resolution).c_str(),
                 sizeof(sky.Resolution) - 1);
    world.AddComponentImmediate(skyEntity, sky);

    SetSkyboxName(world, skyEntity, "Skybox - " + hdriPath.stem().string());

    NotifySkyboxCreated(world, skyEntity, notifications);
    if (markDirty)
        markDirty();

    return skyEntity;
}

ECS::EntityHandle CreateSkyboxEntityFromPolyhavenHdri(ECS::World& world,
                                                      const EditorContext& context,
                                                      std::string_view slug,
                                                      std::string_view displayName,
                                                      EditorChangeNotifications* notifications)
{
    if (slug.empty())
        return {};

    const EditorContext* ctx = &context;
    const std::function<void()> markDirty = [ctx]()
    {
        if (ctx->OnSceneDirty)
            ctx->OnSceneDirty();
    };

    const std::string slugString(slug);
    const std::string preview(kPreviewHdriResolution);
    const std::string preferred = PolyhavenService::GetPreferredHDRIResolution();
    PolyhavenDownloadManager* downloads = context.DownloadManager;

    std::filesystem::path mainFilePath =
        PolyhavenService::FindOrMoveDownloadedHDRI(slugString, context.AssetsRoot, preferred);
    const bool preferredOnDisk = !mainFilePath.empty();
    std::string initialResolution = preferred;
    if (!preferredOnDisk && preferred != preview)
    {
        mainFilePath = PolyhavenService::FindOrMoveDownloadedHDRI(slugString, context.AssetsRoot, preview);
        if (!mainFilePath.empty())
            initialResolution = preview;
    }

    if (!mainFilePath.empty() && context.Assets)
    {
        const ECS::EntityHandle skyEntity = CreateSkyboxEntityFromHdri(world, *context.Assets, mainFilePath, notifications,
                                                                       markDirty, slug, initialResolution);
        if (skyEntity.IsValid() && !preferredOnDisk && downloads)
        {
            downloads->AssignHDRIToSkyboxOnComplete(slugString, preferred, skyEntity, notifications);
            downloads->StartHDRIDownload(slugString, preferred, context.AssetsRoot);
        }
        return skyEntity;
    }

    const ECS::EntityHandle pendingSkybox = CreatePendingHDRISkybox(
        world, displayName.empty() ? slug : displayName, notifications, markDirty, slug, preferred);
    if (pendingSkybox.IsValid() && downloads)
    {
        if (preferred != preview)
        {
            downloads->AssignHDRIToSkyboxOnComplete(slugString, preview, pendingSkybox, notifications,
                                                    /*previewFallback*/ true);
            downloads->StartHDRIDownload(slugString, preview, context.AssetsRoot);
        }
        downloads->AssignHDRIToSkyboxOnComplete(slugString, preferred, pendingSkybox, notifications);
        downloads->StartHDRIDownload(slugString, preferred, context.AssetsRoot);
    }
    Logger::Log::Info("CreateSkyboxEntityFromPolyhavenHdri: created pending skybox for Polyhaven HDRI '{}'", slugString);
    return pendingSkybox;
}

} // namespace GameEngine::Editor
