#include "Editor/Entities/SpriteEntityFactory.h"

#include "AssetCore/Asset.h"
#include "AssetCore/AssetTypes.h"
#include "AssetCore/GUID.h"
#include "Assets/AssetManager.h"
#include "Assets/AssetRegistry.h"
#include "Assets/TextureAsset.h"
#include "Components/Name.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "Engine/Rendering/PrimitiveGenerator.h"
#include "Engine/Rendering/RenderServices.h"
#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::Editor
{

namespace
{

// The loaded texture's pixel dimensions, read from the asset cache only: the
// factory never loads, so it never waits for an import or a cook. Null when the
// texture is not loaded.
const TextureAsset* FindLoadedTexture(AssetManager& assets, const GUID& texGuid, SharedPtr<Asset>& outHolder)
{
    outHolder = assets.GetAsset(texGuid);
    return dynamic_cast<const TextureAsset*>(outHolder.get());
}

} // namespace

ECS::EntityHandle CreateSpriteEntityFromTexture(
    ECS::World& world,
    Engine::Renderer::RenderServices& rs,
    AssetManager& assets,
    const std::filesystem::path& texturePath,
    const Mathematics::Vector3& worldPos,
    bool orientForCamera2D)
{
    if (texturePath.empty())
        return {};

    const GUID texGuid = assets.ResolveAssetGuid(texturePath);
    if (texGuid.IsNull())
        return {};

    SharedPtr<Asset> textureHolder;
    const TextureAsset* texture = FindLoadedTexture(assets, texGuid, textureHolder);
    if (!texture)
    {
        Logger::Log::Warning("SpriteEntityFactory: texture '{}' is not loaded, so no sprite was made; load it first "
                             "(RunWhenAssetLoaded) and create the sprite once it lands",
                             texturePath.string());
        return {};
    }
    if (texture->GetWidth() == 0 || texture->GetHeight() == 0)
    {
        Logger::Log::Warning("SpriteEntityFactory: texture '{}' is loaded but has no size ({}x{}), so no sprite was "
                             "made; check that the image file decodes",
                             texturePath.string(), texture->GetWidth(), texture->GetHeight());
        return {};
    }
    const float texW = static_cast<float>(texture->GetWidth());
    const float texH = static_cast<float>(texture->GetHeight());

    ECS::EntityHandle e = world.CreateEntity();
    if (!e.IsValid())
        return {};

    Components::Name nm{};
    std::memset(nm.value, 0, sizeof(nm.value));
    const std::string stem = texturePath.stem().string();
    std::strncpy(nm.value, stem.c_str(), sizeof(nm.value) - 1);
    world.AddComponentImmediate(e, nm);

    // Plane primitive is 1x1 in XZ with Y+ normal. Use 1 world unit per texel
    // so dropped sprites render at native size in pixel-perfect 2D at 1x zoom
    // (1 world unit = 1 physical pixel).
    const float scaleX = texW;
    const float scaleZ = texH;

    Components::Transform xf{};
    if (orientForCamera2D)
    {
        // 2D Scene View / pixel-perfect cameras: plane sits in XZ with normal +Y;
        // +90° roll (Rx) orients the quad for visible front faces. PlaneSpriteUv
        // mirrors V so decoded PNG rows match scene screen-up alongside this roll.
        const Quaternion rot =
            Components::QuaternionFromEulerXYZDegrees(90.0f, 0.0f, 0.0f);
        xf = Components::Transform::FromTRS(
            worldPos, rot, Mathematics::Vector3{scaleX, 1.0f, scaleZ});
    }
    else
    {
        xf.SetIdentity();
        xf.SetScale(scaleX, 1.0f, scaleZ);
        xf.matrix[12] = worldPos.x;
        xf.matrix[13] = worldPos.y;
        xf.matrix[14] = worldPos.z;
    }
    world.AddComponentImmediate(e, xf);

    const GUID meshGuid = Engine::Renderer::PrimitiveGenerator::PlaneSpriteUvGuid();
    const GUID defaultMatGuid = Engine::Renderer::PrimitiveGenerator::DefaultMaterialGuid();
    auto mr = Engine::Renderer::PrimitiveGenerator::MakePrimitiveMeshRenderer(&rs, meshGuid, defaultMatGuid);
    world.AddComponentImmediate(e, mr);
    world.AddComponentImmediate(e,
        Engine::Renderer::PrimitiveGenerator::MakePrimitiveLocalBounds(&rs, meshGuid));

    // Bind the dropped texture to albedoMap. AssignTextureToEntityMaterialSlot
    // detects that DefaultMaterialGuid is not file-backed and writes a fresh
    // .material under <project>/Assets/Materials/, repoints the MeshRenderer,
    // and uploads the binding to the runtime Material.
    if (!AssignTextureToEntityMaterialSlot(world, e, texGuid, "albedoMap", nullptr))
    {
        Logger::Log::Warning(
            "SpriteEntityFactory: failed to bind texture '{}' to new sprite material",
            texturePath.string());
    }

    return e;
}

} // namespace GameEngine::Editor
