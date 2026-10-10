#pragma once

#include "ECS/Entity.h"
#include "Mathematics/Types.h"

#include <filesystem>

namespace GameEngine { class AssetManager; }
namespace GameEngine::ECS { class World; }
namespace GameEngine::Engine::Renderer { class RenderServices; }

namespace GameEngine::Editor
{

// Creates a "sprite" entity for a dropped texture: a plane primitive whose
// X/Z scale matches the texture's pixel aspect ratio (longest side = 1 world
// unit). A fresh .material file is written under <project>/Assets/Materials/
// with the texture bound to the albedoMap slot so the sprite survives scene
// serialization.
//
// When `orientForCamera2D` is true, the mesh is the PlaneSpriteUv primitive with
// +90° roll so bitmap rows line up upright for 2D scene cameras (with correct
// single-sided shading).
//
// The texture must already be loaded: the factory reads it from the asset cache
// and never loads it, so it never waits on the main thread. Callers load it first
// (RunWhenAssetLoaded) and create the sprite once it lands.
//
// Returns the new entity handle; invalid on failure (bad path, null GUID, texture
// not loaded, entity allocation failure). The factory does not assign a Parent;
// callers parent the result after construction.
ECS::EntityHandle CreateSpriteEntityFromTexture(
    ECS::World& world,
    Engine::Renderer::RenderServices& rs,
    AssetManager& assets,
    const std::filesystem::path& texturePath,
    const Mathematics::Vector3& worldPos,
    bool orientForCamera2D = false);

} // namespace GameEngine::Editor
