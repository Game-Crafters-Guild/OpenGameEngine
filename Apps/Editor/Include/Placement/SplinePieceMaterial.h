#pragma once

#include "AssetCore/GUID.h"
#include "Components/AssetRef.h"
#include "Types/Types.h"

#include <unordered_set>

namespace GameEngine::Components { struct MeshRenderer; }

namespace GameEngine::Engine::Renderer
{
class RenderServices;
struct ModelRenderResources;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Editor
{

// Override material GUIDs a controller has already pushed into the runtime
// material registry. Rebuilds run per frame; registration runs once per GUID.
using OverrideMaterialCache = std::unordered_set<GUID>;

// Make a recipe's override material renderable, once per GUID, and report
// whether it is. The render extraction pass drops any mesh whose material GUID
// is absent from the runtime registry, and what that looks like depends on the
// piece: one with no GPUScene instance yet never appears, while one that
// already earned an instance under a registered material keeps drawing THAT
// material — the drop skips the instance update, and the GPU scatter draws
// every live instance. Either way the override never applies, so binding an
// unregistered one is a strictly worse failure than the registered fallback
// it displaces.
// Callers bind the override only on true and fall back on false — to the
// embedded model material, or to the extrude recipe's minted default (a
// failure also logs a warning, once per GUID).
//
// A null override returns false. Call once per rebuild, before binding pieces.
bool EnsureOverrideMaterialRegistered(Engine::Renderer::RenderServices& renderServices,
                                      const Components::MaterialRef& overrideMaterial,
                                      OverrideMaterialCache& registered);

// Bind one spawned piece's mesh handle, model GUID and material.
//
// `overrideMaterial` wins when set; null falls back to the submesh's own
// embedded model material, which reproduces MeshMaterialFill::FromModel exactly
// — a recipe carrying no override places what it always did.
void BindPieceMaterial(Components::MeshRenderer& renderer,
                       const Engine::Renderer::ModelRenderResources& resources,
                       uint32 submeshIndex,
                       const Components::MaterialRef& overrideMaterial);

} // namespace GameEngine::Editor
