#pragma once

#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Mathematics/Types.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace GameEngine
{

class AssetManager;
namespace Engine::Renderer { class RenderServices; }

// Creates placeholder billboard entities for not-yet-downloaded Polyhaven assets.
// The billboard uses a plane mesh textured with the Polyhaven thumbnail,
// oriented to face the camera. After download completes, the caller replaces
// the placeholder entity with the full model entity hierarchy.
class PolyhavenPlaceholderFactory
{
public:
    struct PlaceholderResult
    {
        ECS::EntityHandle entity{};
        uint32_t downloadId = 0;
    };

    // Create a placeholder billboard entity.
    // `worldPos` is applied as the entity's translation.
    // `cameraPos` is used to orient the billboard to face the camera.
    // `thumbnailPath` is the cached thumbnail PNG (for the billboard texture).
    // `parent` is an optional parent entity (invalid = root-level).
    // `addPlaceholderComponent` — when false, skips adding PolyhavenPlaceholder.
    //   Use false for temporary drag previews so the stale-sweep doesn't destroy them.
    static PlaceholderResult Create(
        ECS::World& world,
        Engine::Renderer::RenderServices& rs,
        AssetManager* assets,
        const std::string& slug,
        const std::string& assetType,
        const Mathematics::Vector3& worldPos,
        const Mathematics::Vector3& cameraPos,
        const std::filesystem::path& thumbnailPath = {},
        ECS::EntityHandle parent = {},
        bool addPlaceholderComponent = true);

    // Re-orient an existing billboard entity to face the camera (cylindrical billboard).
    static void OrientToFaceCamera(ECS::World& world, ECS::EntityHandle entity,
                                   const Mathematics::Vector3& worldPos,
                                   const Mathematics::Vector3& cameraPos);
};

} // namespace GameEngine
